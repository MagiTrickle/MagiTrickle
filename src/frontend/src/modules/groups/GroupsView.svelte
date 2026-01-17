<script lang="ts">
  import { scale } from "svelte/transition";
  import { onMount, onDestroy, tick } from "svelte";

  import { parseConfig, type Group, type Rule } from "../../types";
  import { defaultGroup, defaultRule, randomId } from "../../utils/defaults";
  import { fetcher } from "../../utils/fetcher";
  import { overlay, toast } from "../../utils/events";
  import { persistedState } from "../../utils/persisted-state.svelte";
  import Button from "../../components/ui/Button.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { Add, Import, Export, Save } from "../../components/ui/icons";
  import { t } from "../../data/locale.svelte";
  import { droppable } from "../../lib/dnd";
  import GroupPanel from "./components/GroupPanel.svelte";
  import ImportRulesDialog from "./dialogs/ImportRulesDialog.svelte";
  import ImportConfigDialog from "./dialogs/ImportConfigDialog.svelte";
  import Search from "./components/Search.svelte";
  import { smoothReflow } from "../../lib/smooth-reflow.svelte";
  import { createGroupsVirtualList } from "../../lib/virtualization.svelte";

  const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));

  let data: Group[] = $state([]);
  let counter = $state(0);
  let dataRevision = $state(0);
  let isInitialized = false;
  let dirtyRaf = 0;
  let validityTimeout: number | null = null;
  let valid_rules = $state(true);
  let canSave = $derived(counter > 0 && valid_rules);
  let open_state = persistedState<Record<string, boolean>>("group_open_state", {});

  let importRulesModal = $state<{ open: boolean; groupIndex: number | null }>({
    open: false,
    groupIndex: null,
  });

  let importConfigModal = $state<{ open: boolean; groups: Group[]; fileName: string }>({
    open: false,
    groups: [],
    fileName: "",
  });
  function resetImportConfigModal() {
    importConfigModal = { open: false, groups: [], fileName: "" };
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

  const groupsVirtual = createGroupsVirtualList<VisibleGroup>({
    items: () => visibleGroups,
    getKey: (visible, index) => data[visible.group_index]?.id ?? `group-${index}`,
  });

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

  function saveChanges() {
    if (counter === 0) return;
    overlay.show(t("saving changes..."));

    fetcher
      .put("/groups?save=true", { groups: data })
      .then(() => {
        counter = 0;
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

  function scheduleValidityCheck() {
    if (typeof window === "undefined") return;
    if (validityTimeout !== null) {
      window.clearTimeout(validityTimeout);
    }
    validityTimeout = window.setTimeout(() => {
      validityTimeout = null;
      checkRulesValidityState();
    }, 40);
  }

  function bumpDirty() {
    if (counter <= 0) {
      counter = 1;
    } else {
      counter += 1;
    }
    dataRevision += 1;
    scheduleValidityCheck();
  }

  function markDirty() {
    if (!isInitialized) return;
    if (dirtyRaf) return;
    if (typeof window === "undefined") {
      bumpDirty();
      return;
    }
    dirtyRaf = window.requestAnimationFrame(() => {
      dirtyRaf = 0;
      bumpDirty();
    });
  }

  function initOpenState() {
    for (const group of data) {
      if (open_state.current[group.id] === undefined) {
        open_state.current[group.id] = false;
      }
    }
  }

  function cleanOrphanedOpenState() {
    for (const key of Object.keys(open_state.current)) {
      if (!data.some((group) => group.id === key)) {
        delete open_state.current[key];
      }
    }
  }

  onMount(async () => {
    data = (await fetcher.get<{ groups: Group[] }>("/groups?with_rules=true"))?.groups ?? [];
    initOpenState();
    counter = 0;
    dataRevision = 0;
    isInitialized = true;
    setTimeout(checkRulesValidityState, 10);
    setTimeout(cleanOrphanedOpenState, 5000);
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
    if (typeof window !== "undefined" && dirtyRaf) {
      window.cancelAnimationFrame(dirtyRaf);
      dirtyRaf = 0;
    }
    if (typeof window !== "undefined" && validityTimeout !== null) {
      window.clearTimeout(validityTimeout);
      validityTimeout = null;
    }
    if (typeof window !== "undefined") {
      window.removeEventListener("keydown", handleSaveShortcut);
    }
  });

  async function addRuleToGroup(group_index: number, rule: Rule, focus = false) {
    const group = data[group_index];
    if (!group) return;
    group.rules.unshift(rule);
    markDirty();
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
    markDirty();
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

    const sourceRulesNext = [...sourceGroup.rules];
    if (!sourceRulesNext.length) return;

    const fromIndex = clamp(from_rule_index, 0, sourceRulesNext.length - 1);
    const [movedRule] = sourceRulesNext.splice(fromIndex, 1);
    if (!movedRule) return;

    const targetRulesNext = isSameGroup ? sourceRulesNext : [...targetGroup.rules];

    if (!isSameGroup) {
      moveForcedRule(sourceGroup.id, targetGroup.id, movedRule.id);
    }

    let anchorIndex =
      to_rule_id && to_rule_id.length > 0
        ? targetRulesNext.findIndex((r) => r.id === to_rule_id)
        : -1;

    if (anchorIndex === -1 && targetRulesNext.length > 0) {
      anchorIndex = clamp(to_rule_index, 0, targetRulesNext.length - 1);
    }

    let insertIndex: number;
    if (anchorIndex === -1) {
      insertIndex = insert === "after" ? targetRulesNext.length : 0;
    } else {
      insertIndex = insert === "after" ? anchorIndex + 1 : anchorIndex;
    }

    if (isSameGroup && insertIndex > fromIndex) {
      insertIndex -= 1;
    }

    insertIndex = clamp(insertIndex, 0, targetRulesNext.length);

    targetRulesNext.splice(insertIndex, 0, movedRule);

    const nextData = [...data];

    if (isSameGroup) {
      nextData[from_group_index] = { ...sourceGroup, rules: targetRulesNext };
    } else {
      nextData[from_group_index] = { ...sourceGroup, rules: sourceRulesNext };
      nextData[to_group_index] = { ...targetGroup, rules: targetRulesNext };
    }

    data = nextData;
    markDirty();
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
    markDirty();
  }

  async function addGroup() {
    const group = defaultGroup();
    data.unshift(group);
    open_state.current[group.id] = true;
    markGroupOrderChanged();
    markDirty();
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
      delete open_state.current[removed.id];
    }
    markGroupOrderChanged();
    markDirty();
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

        importConfigModal = {
          open: true,
          groups,
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
</script>
<div class="groups-page" use:smoothReflow>
  <div class="group-controls" use:smoothReflow>
    <Search
      groups={data}
      dataRevision={dataRevision}
      bind:visibleGroups
      bind:searchActive
      bind:searchPending
      bind:controls={searchControls}
    />

    <div class="group-controls-actions">
      {#if canSave}
        <div transition:scale>
          <Tooltip value={t("Save Changes")}>
            <Button onclick={saveChanges} id="save-changes">
              <Save size={22} />
            </Button>
          </Tooltip>
        </div>
      {/if}
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

  {#if noVisibleGroups}
    <div class="no-groups">{t("No matches found")}</div>
  {/if}

  <div
    class="group-list"
    use:groupsVirtual.list
    style={`height: ${groupsVirtual.totalHeight}px`}
    oninput={markDirty}
    onchange={markDirty}
  >
    {#each groupsVirtual.entries as entry (entry.key)}
      {@const visible = entry.item}
      {#if data[visible.group_index]}
        <div class="group-wrapper" style={entry.style} use:groupsVirtual.item={{ key: entry.key }}>
          {#if entry.index === 0}
            <div
              class="group-drop-slot group-drop-slot--top"
              aria-hidden="true"
              use:droppable={{
                data: { group_index: visible.group_index, insert: "before" } as GroupDropSlotData,
                scope: "group",
                canDrop: (source: GroupDragData, target: GroupDropSlotData) =>
                  source.group_index !== target.group_index,
                dropEffect: "move",
                onDrop: handleGroupSlotDrop,
              }}
            ></div>
          {/if}
          <GroupPanel
            bind:group={data[visible.group_index]}
            group_index={visible.group_index}
            bind:total_groups={data.length}
            bind:open={open_state.current[data[visible.group_index].id]}
            {deleteGroup}
            {addRuleToGroup}
            {deleteRuleFromGroup}
            {changeRuleIndex}
            {searchActive}
            visibleRuleIndices={visible.ruleIndices}
            on:importRules={() => openImportRulesModal(visible.group_index)}
          />
          <div
            class="group-drop-slot group-drop-slot--bottom"
            aria-hidden="true"
            use:droppable={{
              data: { group_index: visible.group_index, insert: "after" } as GroupDropSlotData,
              scope: "group",
              canDrop: () => true,
              dropEffect: "move",
              onDrop: handleGroupSlotDrop,
            }}
          ></div>
        </div>
      {/if}
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
    markDirty();
  }}
/>

<ImportConfigDialog
  open={importConfigModal.open}
  groups={importConfigModal.groups}
  fileName={importConfigModal.fileName}
  on:close={resetImportConfigModal}
  on:import={(e) => {
    const imported = e.detail.groups.map(cloneGroupWithNewIds);
    if (!imported.length) return;
    for (let i = imported.length - 1; i >= 0; i--) {
      const group = imported[i];
      data.unshift(group);
      open_state.current[group.id] = true;
    }
    markGroupOrderChanged();
    markDirty();
    toast.success(`${t("Config imported")}: ${imported.length}`);
  }}
/>


<style>
  .group-list {
    position: relative;
    min-height: 1px;
    padding: 1rem 0;
  }

  .group-wrapper {
    position: absolute;
    left: 0;
    right: 0;
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
    margin-bottom: 1.5rem;
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

  .no-groups {
    width: 100%;
    text-align: center;
    padding: 2rem 0;
    color: color-mix(in oklab, var(--text) 75%, transparent);
  }
</style>
