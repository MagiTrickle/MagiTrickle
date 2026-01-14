<script lang="ts">
  import { scale } from "svelte/transition";
  import { onDestroy, onMount, tick } from "svelte";

  import { parseConfig, type Group, type Rule } from "../../types";
  import { defaultGroup, defaultRule, randomId } from "../../utils/defaults";
  import { fetcher } from "../../utils/fetcher";
  import { overlay, toast } from "../../utils/events";
  import { persistedState } from "../../utils/persisted-state.svelte";
  import { ChangeTracker } from "../../utils/change-tracker.svelte";
  import Button from "../../components/ui/Button.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { Add, Import, Export, Save } from "../../components/ui/icons";
  import { t } from "../../data/locale.svelte";
  import { droppable } from "../../lib/dnd";
  import GroupPanel from "./components/GroupPanel.svelte";
  import ImportRulesDialog from "./dialogs/ImportRulesDialog.svelte";
  import ImportConfigDialog from "./dialogs/ImportConfigDialog.svelte";

  function handleSaveShortcut(event: KeyboardEvent) {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") {
      if (canSave) {
        event.preventDefault();
        saveChanges();
      }
    }
  }

  const INITIAL_RULES_LIMIT = 30 as const;
  const INCREMENT_RULES_LIMIT = 40 as const;

  let tracker = $state(new ChangeTracker<Group[]>([]));
  let data = $derived(tracker.data);

  let showed_limit: number[] = $state([]);
  let valid_rules = $state(true);
  let canSave = $derived(tracker.isDirty && valid_rules);
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

  let searchQuery = $state("");
  let normalizedSearch = $derived(searchQuery.trim().toLowerCase());
  let searchActive = $derived(Boolean(normalizedSearch));
  let visibleGroups: VisibleGroup[] = $state([]);

  function recomputeVisibleGroups() {
    if (!normalizedSearch) {
      visibleGroups = data.map(
        (_, index): VisibleGroup => ({ group_index: index, ruleIndices: null }),
      );
      return;
    }

    visibleGroups = data
      .map<VisibleGroup | null>((group, index) => {
        if (!group) return null;
        const query = normalizedSearch;
        const groupMatch =
          (group.name?.toLowerCase() ?? "").includes(query) ||
          (group.interface?.toLowerCase() ?? "").includes(query);

        const matchedRuleIndices = group.rules
          .map((rule, ruleIndex) => {
            const ruleName = rule.name?.toLowerCase() ?? "";
            const rulePattern = rule.rule?.toLowerCase() ?? "";
            const ruleType = rule.type?.toLowerCase() ?? "";
            const match =
              ruleName.includes(query) || rulePattern.includes(query) || ruleType.includes(query);
            return match ? ruleIndex : -1;
          })
          .filter((idx) => idx !== -1);

        if (groupMatch || matchedRuleIndices.length > 0) {
          return {
            group_index: index,
            ruleIndices: groupMatch && matchedRuleIndices.length === 0 ? null : matchedRuleIndices,
          };
        }

        return null;
      })
      .filter(Boolean) as VisibleGroup[];
  }

  $effect(recomputeVisibleGroups);

  let noVisibleGroups = $derived(searchActive && visibleGroups.length === 0);

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

  function initOpenState() {
    for (const group of data) {
      if (!open_state.current[group.id]) {
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
    const fetched =
      (await fetcher.get<{ groups: Group[] }>("/groups?with_rules=true"))?.groups ?? [];
    tracker = new ChangeTracker(fetched);

    showed_limit = data.map((group) =>
      group.rules.length > INITIAL_RULES_LIMIT ? INITIAL_RULES_LIMIT : group.rules.length,
    );
    initOpenState();
    setTimeout(cleanOrphanedOpenState, 5000);
    window.addEventListener("keydown", handleSaveShortcut);
  });

  onDestroy(() => {
    window.removeEventListener("keydown", handleSaveShortcut);
  });

  $effect(() => {
    if (typeof window === "undefined" || !canSave) return;

    const handleBeforeUnload = (event: BeforeUnloadEvent) => {
      event.preventDefault();
    };

    window.addEventListener("beforeunload", handleBeforeUnload);
    return () => window.removeEventListener("beforeunload", handleBeforeUnload);
  });

  $effect(() => {
    const value = $state.snapshot(data);
    setTimeout(checkRulesValidityState, 10);
  });

  async function addRuleToGroup(group_index: number, rule: Rule, focus = false) {
    data[group_index].rules.unshift(rule);
    showed_limit[group_index]++;
    recomputeVisibleGroups();

    if (!rule.rule || !rule.name) {
      valid_rules = false;
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
    data[group_index].rules.splice(rule_index, 1);
    recomputeVisibleGroups();
  }

  function changeRuleIndex(
    from_group_index: number,
    from_rule_index: number,
    to_group_index: number,
    to_rule_index: number,
    to_rule_id?: string,
    insert: "before" | "after" = "before",
  ) {
    const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));

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

    let anchorIndex =
      to_rule_id && to_rule_id.length > 0 ? targetRules.findIndex((r) => r.id === to_rule_id) : -1;

    if (anchorIndex === -1 && targetRules.length > 0) {
      anchorIndex = clamp(to_rule_index, 0, targetRules.length - 1);
      if (isSameGroup && fromIndex < anchorIndex) {
        anchorIndex--;
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

    if (!isSameGroup) {
      showed_limit[from_group_index] = Math.min(showed_limit[from_group_index], sourceRules.length);
    }

    const ensureVisibleCount = isSameGroup
      ? Math.min(insertIndex + 1, targetRules.length)
      : Math.min(targetRules.length, Math.max(insertIndex + 1, showed_limit[to_group_index] + 1));

    if (showed_limit[to_group_index] < ensureVisibleCount) {
      showed_limit[to_group_index] = ensureVisibleCount;
    }

    showed_limit = [...showed_limit];
    recomputeVisibleGroups();
  }

  function changeGroupIndex(
    from_index: number,
    to_index: number,
    insert: "before" | "after" = "before",
  ) {
    if (from_index === to_index && insert !== "after") return;

    if (from_index < 0 || from_index >= data.length) return;

    const g = data[from_index];
    const lim = showed_limit[from_index];
    if (!g) return;

    data.splice(from_index, 1);
    showed_limit.splice(from_index, 1);

    let target = insert === "after" ? to_index + 1 : to_index;

    if (from_index < target) target -= 1;

    if (target < 0) target = 0;
    if (target > data.length) target = data.length;

    data.splice(target, 0, g);
    showed_limit.splice(target, 0, lim);
    recomputeVisibleGroups();
  }

  async function addGroup() {
    data.unshift(defaultGroup());
    showed_limit.unshift(INITIAL_RULES_LIMIT);
    open_state.current[data[0].id] = true;
    recomputeVisibleGroups();
    await addRuleToGroup(0, defaultRule(), false);
    await tick();
    const el = document.querySelector(`.group-header[data-group-index="0"]`);
    el?.querySelector<HTMLInputElement>("input.group-name")?.focus();
  }

  function deleteGroup(index: number) {
    if (!confirm(t("Delete this group?"))) return;
    data.splice(index, 1);
    showed_limit.splice(index, 1);
    recomputeVisibleGroups();
  }

  function exportConfig() {
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

  async function loadMore(group_index: number): Promise<void> {
    const group = data[group_index];
    if (!group) return;
    const totalRules = group.rules.length;
    if (showed_limit[group_index] >= totalRules) return;
    showed_limit[group_index] = Math.min(
      showed_limit[group_index] + INCREMENT_RULES_LIMIT,
      totalRules,
    );
  }

  function openImportRulesModal(groupIndex: number) {
    importRulesModal = { open: true, groupIndex };
  }

  function closeImportRulesModal() {
    importRulesModal = { open: false, groupIndex: null };
  }
</script>

<div class="groups-page">
  <div class="group-controls">
    <div class="group-controls-search">
      <input
        type="search"
        placeholder={t("Search groups and rules...")}
        class="group-search-input"
        bind:value={searchQuery}
      />
    </div>
    <div class="group-controls-actions">
      <Tooltip value={t("Save Changes")}>
        <Button
          onclick={saveChanges}
          id="save-changes"
          class={canSave ? "accent" : ""}
          inactive={!canSave}
        >
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

  {#if noVisibleGroups}
    <div class="no-groups">{t("No matches found")}</div>
  {/if}

  {#each visibleGroups as visible, index (data[visible.group_index]?.id)}
    {#if data[visible.group_index]}
      <div class="group-wrapper">
        {#if index === 0}
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
          bind:showed_limit={showed_limit[visible.group_index]}
          bind:open={open_state.current[data[visible.group_index].id]}
          {deleteGroup}
          {addRuleToGroup}
          {deleteRuleFromGroup}
          {changeRuleIndex}
          {loadMore}
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

<ImportRulesDialog
  open={importRulesModal.open}
  group_index={importRulesModal.groupIndex}
  on:close={closeImportRulesModal}
  on:import={(e) => {
    const { group_index, rules } = e.detail;
    data[group_index].rules.unshift(...rules);
    if (rules.length > 500) {
      showed_limit[group_index] = Math.max(showed_limit[group_index], 30);
    } else {
      showed_limit[group_index] = Math.min(
        showed_limit[group_index] + rules.length,
        data[group_index].rules.length,
      );
    }
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
      showed_limit.unshift(
        group.rules.length > INITIAL_RULES_LIMIT ? INITIAL_RULES_LIMIT : group.rules.length,
      );
      open_state.current[group.id] = true;
    }
    toast.success(`${t("Config imported")}: ${imported.length}`);
  }}
/>

<style>
  .group-wrapper {
    position: relative;
    margin: 1rem 0;
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
    flex-wrap: wrap;
    gap: 0.75rem;
    padding: 0.75rem 0.25rem;
    margin-bottom: 0.75rem;
    position: sticky;
    top: 0;
    z-index: 5;
    background: color-mix(in oklab, var(--bg-dark) 92%, var(--bg-dark-extra) 8%);
  }

  .group-controls-search {
    flex: 1 1 58px;
  }

  .group-search-input {
    width: 100%;
    padding: 0.7rem 0.85rem;
    border-radius: 0.5rem;
    border: 1px solid var(--bg-light-extra);
    background-color: var(--bg-light);
    color: var(--text);
    font: inherit;
    font-size: 1rem;
    line-height: 1.3;
    min-height: 2.7rem;
    transition:
      border-color 0.12s ease,
      box-shadow 0.18s ease,
      background-color 0.12s ease,
      color 0.12s ease;
  }

  .group-search-input:hover {
    background-color: color-mix(in oklab, var(--bg-light) 92%, var(--bg-light-extra) 8%);
    border-color: color-mix(in oklab, var(--bg-light-extra) 90%, transparent);
    color: var(--text);
  }

  .group-search-input:focus-visible {
    outline: none;
    border-color: var(--accent);
    box-shadow:
      0 0 0 1px color-mix(in oklab, var(--accent) 45%, transparent),
      0 6px 18px -14px color-mix(in oklab, var(--accent) 35%, transparent);
    background-color: var(--bg-light);
    color: var(--text);
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
