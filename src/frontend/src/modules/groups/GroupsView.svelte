<script lang="ts">
  import { onDestroy, onMount, tick } from "svelte";
  import { scale } from "svelte/transition";

  import Button from "../../components/ui/Button.svelte";
  import Placeholder from "../../components/ui/Placeholder.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { t } from "../../data/locale.svelte";
  import { smoothReflow } from "../../lib/smooth-reflow.svelte";
  import { ChangeTracker } from "../../utils/change-tracker.svelte";
  import GroupPanel from "./components/GroupPanel.svelte";
  import Search from "./components/Search.svelte";
  import ImportConfigDialog from "./dialogs/ImportConfigDialog.svelte";
  import ImportRulesDialog from "./dialogs/ImportRulesDialog.svelte";

  import { Add, Export, Import, Save } from "../../components/ui/icons";
  import { droppable } from "../../lib/dnd";
  import { parseConfig, type Group, type Rule } from "../../types";
  import { defaultGroup, defaultRule, randomId } from "../../utils/defaults";
  import { overlay, toast } from "../../utils/events";
  import { fetcher } from "../../utils/fetcher";

  const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));

  type Props = {
    onRenderComplete?: () => void;
  };

  let { onRenderComplete }: Props = $props();

  let tracker = $state(new ChangeTracker<Group[]>([]));
  let data = $derived(tracker.data);
  let dataRevision = $state(0);
  let valid_rules = $state(true);
  let canSave = $derived(tracker.isDirty && valid_rules);
  let open_state = $state<Record<string, boolean>>({});

  let importRulesModal = $state<{ open: boolean; groupIndex: number | null }>({
    open: false,
    groupIndex: null,
  });

  let importConfigModal = $state<{ open: boolean; fileName: string }>({
    open: false,
    fileName: "",
  });
  let importedGroups: Group[] = [];

  function resetImportConfigModal() {
    importConfigModal = { open: false, fileName: "" };
    importedGroups = [];
  }

  function cloneGroupWithNewIds(group: Group): Group {
    return {
      ...group,
      id: randomId(),
      rules: group.rules.map((rule) => ({
        ...rule,
        id: randomId(),
      })),
    };
  }

  type VisibleGroup = {
    group_index: number;
    ruleIndices: number[] | null;
  };

  type SearchControls = {
    markGroupOrderChanged: () => void;
    forceVisibleGroup: (groupId: string) => void;
    forceVisibleRule: (groupId: string, ruleId: string) => void;
    removeForcedGroup: (groupId: string) => void;
    removeForcedRule: (groupId: string, ruleId: string) => void;
    moveForcedRule: (sourceGroupId: string, targetGroupId: string, ruleId: string) => void;
    syncRuleDeletion: (groupIndex: number, ruleIndex: number) => void;
  };

  type GroupDragData = {
    group_id: string;
    group_index: number;
    name: string;
    color: string;
    count: number;
  };

  type GroupDropSlotData = {
    group_index: number;
    insert: "before" | "after";
  };

  function handleGroupSlotDrop(source: GroupDragData, target: GroupDropSlotData) {
    const { group_index: from_index } = source;
    const { group_index: to_index, insert } = target;
    if (from_index === to_index && insert !== "after") return;
    changeGroupIndex(from_index, to_index, insert);
  }

  let searchControls = $state<SearchControls | null>(null);
  let visibleGroups: VisibleGroup[] = $state([]);
  let searchActive = $state(false);
  let searchPending = $state(false);

  function handleSaveShortcut(event: KeyboardEvent) {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") {
      if (canSave) {
        event.preventDefault();
        saveChanges();
      }
    }
  }

  function markGroupOrderChanged() {
    searchControls?.markGroupOrderChanged();
  }

  function forceVisibleGroup(groupId: string) {
    searchControls?.forceVisibleGroup(groupId);
  }

  function forceVisibleRule(groupId: string, ruleId: string) {
    searchControls?.forceVisibleRule(groupId, ruleId);
  }

  function removeForcedGroup(groupId: string) {
    searchControls?.removeForcedGroup(groupId);
  }

  function removeForcedRule(groupId: string, ruleId: string) {
    searchControls?.removeForcedRule(groupId, ruleId);
  }

  function moveForcedRule(sourceGroupId: string, targetGroupId: string, ruleId: string) {
    searchControls?.moveForcedRule(sourceGroupId, targetGroupId, ruleId);
  }

  let noVisibleGroups = $derived(searchActive && !searchPending && visibleGroups.length === 0);

  let visibilityMap = $derived(new Map(visibleGroups.map((v) => [v.group_index, v.ruleIndices])));

  let firstVisibleGroupIndex = $derived(
    searchActive ? (visibleGroups.length ? visibleGroups[0].group_index : -1) : 0,
  );

  function saveChanges() {
    if (!tracker.isDirty) return;
    overlay.show(t("saving changes..."));

    const rawData = $state.snapshot(data);

    fetcher
      .put("/groups?save=true", { groups: rawData })
      .then(() => {
        tracker.reset(rawData);
        overlay.hide();
        toast.success(t("Saved"));
      })
      .catch(() => {
        overlay.hide();
      });
  }

  function checkRulesValidityState() {
    valid_rules = !document.querySelector(".rule input.invalid");
  }

  function bumpDataRevision() {
    dataRevision += 1;
  }

  function markDataRevision() {
    bumpDataRevision();
  }

  onMount(async () => {
    finishedGroupsCount = 0;
    fetchError = false;
    try {
      const fetched =
        (await fetcher.get<{ groups: Group[] }>("/groups?with_rules=true"))?.groups ?? [];
      tracker = new ChangeTracker(fetched);
      dataRevision = 0;
      setTimeout(checkRulesValidityState, 10);
    } catch (error) {
      fetchError = true;
      console.error("Failed to load groups:", error);
    } finally {
      dataLoaded = true;
    }
    if (typeof window !== "undefined") {
      window.addEventListener("keydown", handleSaveShortcut);
    }

  });

  $effect(() => {
    if (typeof window === "undefined" || !canSave) return;

    const handleBeforeUnload = (event: BeforeUnloadEvent) => {
      event.preventDefault();
    };

    window.addEventListener("beforeunload", handleBeforeUnload);
    return () => window.removeEventListener("beforeunload", handleBeforeUnload);
  });

  onDestroy(() => {
    if (typeof window !== "undefined") {
      window.removeEventListener("keydown", handleSaveShortcut);
    }
  });

  $effect(() => {
    $state.snapshot(data);
    setTimeout(checkRulesValidityState, 10);
  });

  async function addRuleToGroup(group_index: number, rule: Rule, focus = false) {
    const group = data[group_index];
    if (!group) return;
    group.rules.unshift(rule);
    markDataRevision();
    if (!rule.rule || !rule.name) {
      valid_rules = false;
    }
    if (searchActive) {
      forceVisibleRule(group.id, rule.id);
    }
    if (!focus) return;
    await tick();
    const el = document.querySelector(`.rule[data-group-index="${group_index}"][data-index="0"]`);
    if (el) {
      requestAnimationFrame(() => {
        el.querySelector<HTMLInputElement>("div.name input")?.focus();
        el.querySelector<HTMLInputElement>("div.pattern input")?.classList.add("invalid");
        checkRulesValidityState();
      });
    }
  }

  function deleteRuleFromGroup(group_index: number, rule_index: number) {
    const group = data[group_index];
    if (!group) return;
    const removed = group.rules[rule_index];
    group.rules.splice(rule_index, 1);
    if (removed) {
      removeForcedRule(group.id, removed.id);
    }
    searchControls?.syncRuleDeletion(group_index, rule_index);
    markDataRevision();
  }

  function changeRuleIndex(
    from_group_index: number,
    from_rule_index: number,
    to_group_index: number,
    to_rule_index: number,
    to_rule_id?: string,
    insert: "before" | "after" = "before",
  ) {
    const sourceGroup = data[from_group_index];
    const targetGroup = data[to_group_index];

    if (!sourceGroup || !targetGroup) return;

    const isSameGroup = from_group_index === to_group_index;

    const sourceRules = sourceGroup.rules;
    const targetRules = targetGroup.rules;

    if (!sourceRules.length) return;

    const fromIndex = clamp(from_rule_index, 0, sourceRules.length - 1);
    const [movedRule] = sourceRules.splice(fromIndex, 1);
    if (!movedRule) return;

    if (!isSameGroup) {
      moveForcedRule(sourceGroup.id, targetGroup.id, movedRule.id);
    }

    let anchorIndex =
      to_rule_id && to_rule_id.length > 0 ? targetRules.findIndex((r) => r.id === to_rule_id) : -1;

    if (anchorIndex === -1 && targetRules.length > 0) {
      anchorIndex = clamp(to_rule_index, 0, targetRules.length - 1);
      if (isSameGroup && fromIndex < anchorIndex) {
        anchorIndex -= 1;
      }
    }

    let insertIndex: number;
    if (anchorIndex === -1) {
      insertIndex = insert === "after" ? targetRules.length : 0;
    } else {
      insertIndex = insert === "after" ? anchorIndex + 1 : anchorIndex;
    }

    insertIndex = clamp(insertIndex, 0, targetRules.length);
    targetRules.splice(insertIndex, 0, movedRule);

    markDataRevision();
  }

  function changeGroupIndex(
    from_index: number,
    to_index: number,
    insert: "before" | "after" = "before",
  ) {
    if (from_index === to_index && insert !== "after") return;

    if (from_index < 0 || from_index >= data.length) return;

    const g = data[from_index];
    if (!g) return;

    data.splice(from_index, 1);

    let target = insert === "after" ? to_index + 1 : to_index;

    if (from_index < target) target -= 1;

    if (target < 0) target = 0;
    if (target > data.length) target = data.length;

    data.splice(target, 0, g);
    markGroupOrderChanged();
    markDataRevision();
  }

  async function addGroup() {
    const group = defaultGroup();
    data.unshift(group);
    open_state[group.id] = true;
    markGroupOrderChanged();
    markDataRevision();
    if (searchActive) {
      forceVisibleGroup(group.id);
    }
    await addRuleToGroup(0, defaultRule(), false);
    await tick();
    const el = document.querySelector(`.group-header[data-group-index="0"]`);
    el?.querySelector<HTMLInputElement>("input.group-name")?.focus();
  }

  function deleteGroup(index: number) {
    if (!confirm(t("Delete this group?"))) return;
    const removed = data[index];
    data.splice(index, 1);
    if (removed) {
      removeForcedGroup(removed.id);
      delete open_state[removed.id];
    }
    markGroupOrderChanged();
    markDataRevision();
  }

  function exportConfig() {
    if (data.length === 0) {
      toast.warning(t("Empty config exported"));
    }
    const blob = new Blob([JSON.stringify({ groups: data })], { type: "application/json" });
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = "config.mtrickle";
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
  }

  function importConfig() {
    const input = document.getElementById("import-config") as HTMLInputElement;
    const file = input.files?.[0];
    if (!file) {
      alert(t("Please select a CONFIG file to load."));
      return;
    }

    const reader = new FileReader();
    reader.onload = (event) => {
      try {
        const { groups } = parseConfig(event.target?.result as string);

        if (!groups?.length) {
          toast.error(t("Invalid config file"));
          return;
        }

        importedGroups = groups;
        importConfigModal = {
          open: true,
          fileName: file.name,
        };
      } catch (error) {
        console.error("Error parsing CONFIG:", error);
        toast.error(t("Invalid config file"));
      }
    };
    reader.onerror = (event) => {
      console.error("Error reading file:", event.target?.error);
      toast.error(t("Invalid config file"));
    };
    reader.readAsText(file);
    input.value = "";
  }

  async function copyToClipboard(text: string) {
    if (typeof navigator !== "undefined" && navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text);
      return;
    }

    const textarea = document.createElement("textarea");
    textarea.value = text;
    textarea.setAttribute("readonly", "true");
    textarea.style.position = "fixed";
    textarea.style.top = "-1000px";
    textarea.style.opacity = "0";
    document.body.appendChild(textarea);
    textarea.focus();
    textarea.select();
    const ok = document.execCommand("copy");
    document.body.removeChild(textarea);
    if (!ok) throw new Error("Clipboard copy failed");
  }

  function openImportRulesModal(groupIndex: number) {
    importRulesModal = { open: true, groupIndex };
  }

  function closeImportRulesModal() {
    importRulesModal = { open: false, groupIndex: null };
  }

  let finishedGroupsCount = $state(0);
  let fetchError = $state(false);
  let dataLoaded = $state(false);
  function handleGroupFinished() {
    finishedGroupsCount++;
  }

  let isAllRendered = $derived(
    dataLoaded && (data.length === 0 || finishedGroupsCount >= data.length || searchActive),
  );
  let isEmptyData = $derived(
    dataLoaded && !fetchError && !searchActive && !searchPending && data.length === 0,
  );

  $effect(() => {
    if (isAllRendered) {
      onRenderComplete?.();
    }
  });

  let renderGroupsLimit = $state(1);
  let renderGroupsTimeout: number | null = null;

  function scheduleGroupsNext() {
    if (typeof window === "undefined") return;
    if (renderGroupsTimeout) return;
    if (renderGroupsLimit >= data.length) return;

    renderGroupsTimeout = window.setTimeout(() => {
      renderGroupsTimeout = null;
      if (searchActive) {
        renderGroupsLimit = data.length;
        return;
      }
      renderGroupsLimit = Math.min(renderGroupsLimit + 2, data.length);
      scheduleGroupsNext();
    }, 15);
  }

  $effect(() => {
    if (searchActive) {
      renderGroupsLimit = data.length;
      if (renderGroupsTimeout) {
        clearTimeout(renderGroupsTimeout);
        renderGroupsTimeout = null;
      }
    } else {
      scheduleGroupsNext();
    }
  });

  $effect(() => {
    data.length;
    scheduleGroupsNext();
  });
</script>

<div class="groups-page" use:smoothReflow>
  <div class="group-controls" use:smoothReflow>
    <Search
      groups={data}
      {dataRevision}
      bind:visibleGroups
      bind:searchActive
      bind:searchPending
      bind:controls={searchControls}
    />

    <div class="group-controls-actions">
      <Tooltip value={t("Save Changes")}>
        <Button onclick={saveChanges} id="save-changes" class="accent" inactive={!canSave}>
          <Save size={22} />
        </Button>
      </Tooltip>
      <Tooltip value={t("Import Config")}>
        <input type="file" id="import-config" hidden accept=".mtrickle" onchange={importConfig} />
        <Button onclick={() => document.getElementById("import-config")!.click()}>
          <Import size={22} />
        </Button>
      </Tooltip>
      <Tooltip value={t("Export Config")}>
        <Button onclick={exportConfig}>
          <Export size={22} />
        </Button>
      </Tooltip>
      <Tooltip value={t("Add Group")}>
        <Button onclick={addGroup}><Add size={22} /></Button>
      </Tooltip>
    </div>
  </div>

  {#if fetchError}
    <Placeholder variant="error" minHeight="auto" subtitle={t("Check connection or try again")}>
      {t("Failed to load groups")}
    </Placeholder>
  {:else if !isAllRendered}
    <Placeholder variant="loading" minHeight="auto">
      {t("Loading groups...")}
    </Placeholder>
  {:else if noVisibleGroups}
    <Placeholder variant="empty" minHeight="auto">
      {t("No matches found")}
    </Placeholder>
  {:else if isEmptyData}
    <Placeholder variant="empty" minHeight="auto" subtitle={t("Create a new group to get started")}>
      {t("No groups yet")}
    </Placeholder>
  {/if}

  <div
    class="group-list"
    class:visible={isAllRendered}
    style={isAllRendered ? "" : "display: none;"}
    oninput={markDataRevision}
    onchange={markDataRevision}
  >
    {#each data.slice(0, renderGroupsLimit) as group, group_index (group.id)}
      {@const ruleIndices = visibilityMap.get(group_index)}
      {@const isVisible = !searchActive || visibilityMap.has(group_index)}

      <div class="group-wrapper" style={isVisible ? "" : "display: none"}>
        {#if group_index === firstVisibleGroupIndex}
          <div
            class="group-drop-slot group-drop-slot--top"
            aria-hidden="true"
            use:droppable={{
              data: { group_index, insert: "before" } as GroupDropSlotData,
              scope: "group",
              canDrop: (source: GroupDragData, target: GroupDropSlotData) =>
                source.group_index !== target.group_index,
              dropEffect: "move",
              onDrop: handleGroupSlotDrop,
            }}
          ></div>
        {/if}

        <GroupPanel
          bind:group={data[group_index]}
          {group_index}
          bind:total_groups={data.length}
          bind:open={open_state[group.id]}
          {deleteGroup}
          {addRuleToGroup}
          {deleteRuleFromGroup}
          {changeRuleIndex}
          {searchActive}
          visibleRuleIndices={ruleIndices}
          onFinished={handleGroupFinished}
          on:importRules={() => openImportRulesModal(group_index)}
        />

        <div
          class="group-drop-slot group-drop-slot--bottom"
          aria-hidden="true"
          use:droppable={{
            data: { group_index, insert: "after" } as GroupDropSlotData,
            scope: "group",
            canDrop: () => true,
            dropEffect: "move",
            onDrop: handleGroupSlotDrop,
          }}
        ></div>
      </div>
    {/each}
  </div>
</div>

<ImportRulesDialog
  open={importRulesModal.open}
  group_index={importRulesModal.groupIndex}
  on:close={closeImportRulesModal}
  on:import={(e) => {
    const { group_index, rules } = e.detail;
    const group = data[group_index];
    if (!group) return;
    group.rules.unshift(...rules);
    markDataRevision();
  }}
/>

<ImportConfigDialog
  open={importConfigModal.open}
  groups={importedGroups}
  fileName={importConfigModal.fileName}
  onclose={resetImportConfigModal}
  onimport={(e) => {
    const imported = e.groups.map(cloneGroupWithNewIds);
    if (!imported.length) return;

    if (e.replace) {
      data.splice(0, data.length);
      open_state = {};
    }

    for (let i = imported.length - 1; i >= 0; i--) {
      const group = imported[i];
      data.unshift(group);
      open_state[group.id] = true;
    }
    markGroupOrderChanged();
    markDataRevision();
    toast.success(`${t("Config imported")}: ${imported.length}`);
  }}
/>

<style>
  .group-list {
    min-height: 1px;
    opacity: 0;
    transition: opacity 0.4s ease-in-out;
  }

  .group-list.visible {
    opacity: 1;
  }

  .group-wrapper {
    position: relative;
    margin: 1rem 0;
    animation: group-appear 0.15s ease-out forwards;
  }

  .group-wrapper:has(:global([data-select-trigger][data-state="open"])) {
    z-index: 1;
  }

  @keyframes group-appear {
    from {
      opacity: 0;
    }
    to {
      opacity: 1;
    }
  }

  .group-wrapper:first-of-type {
    margin-top: 1rem;
  }

  .group-wrapper:last-of-type {
    margin-bottom: 1rem;
  }

  .group-drop-slot {
    position: absolute;
    left: 0;
    right: 0;
    height: 1rem;
    pointer-events: none;
    background: color-mix(in oklab, var(--accent) 28%, transparent);
    box-shadow: inset 0 0 0 2px color-mix(in oklab, var(--accent) 54%, transparent);
    opacity: 0;
    transition: opacity 0.12s ease;
  }

  .group-drop-slot--top {
    top: -1rem;
  }

  .group-drop-slot--bottom {
    bottom: -1rem;
  }

  :global(html[data-dnd-scope="group"]) .group-drop-slot {
    pointer-events: auto;
  }

  :global(html[data-dnd-scope="group"]) .group-drop-slot:global(.dragover) {
    opacity: 1;
  }

  .group-controls {
    display: flex;
    align-items: center;
    justify-content: space-between;
    flex-wrap: nowrap;
    gap: 0.75rem;
    padding: 0.3rem 0rem;
    margin-bottom: 1rem;
    position: sticky;
    top: 0;
    z-index: 5;
    background: color-mix(in oklab, var(--bg-dark) 92%, var(--bg-dark-extra) 8%);
  }

  @media (max-width: 570px) {
    .group-controls {
      flex-wrap: wrap;
    }
    .group-controls-actions {
      justify-content: flex-start;
    }
  }

  @media (max-width: 700px) {
    .group-controls {
      margin-bottom: 1rem;
    }
  }

  .group-controls-actions {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }
</style>
