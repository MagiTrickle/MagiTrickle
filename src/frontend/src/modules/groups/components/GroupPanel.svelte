<script lang="ts">
  import { Collapsible } from "bits-ui";
  import { createEventDispatcher, getContext, onMount, tick, untrack } from "svelte";

  import Button from "../../../components/ui/Button.svelte";
  import DropdownMenu from "../../../components/ui/DropdownMenu.svelte";
  import EdgeNumberInput from "../../../components/ui/EdgeNumberInput.svelte";
  import RouteSelect from "../../../components/ui/RouteSelect.svelte";
  import Switch from "../../../components/ui/Switch.svelte";
  import Tooltip from "../../../components/ui/Tooltip.svelte";
  import VirtualList from "../../../components/ui/VirtualList.svelte";
  import { t } from "../../../data/locale.svelte";
  import type { VirtualMeasurement } from "../../../lib/virtual/size-estimate.svelte";
  import { GROUPS_STORE_CONTEXT, type GroupsStore } from "../groups.svelte";
  import GroupDuplicateMenu from "./GroupDuplicateMenu.svelte";
  import RuleRow from "./RuleRow.svelte";

  import {
    Add,
    Copy,
    Delete,
    Dots,
    Grip,
    GroupCollapse,
    GroupExpand,
    ImportList,
    SortAsc,
    SortDesc,
    SortNeutral,
  } from "../../../components/ui/icons";
  import { draggable, droppable } from "../../../lib/dnd";
  import { copyRulePatternsToClipboard } from "../../../utils/copy-rule-patterns";
  import { defaultRule } from "../../../utils/defaults";
  import { type SortDirection, type SortField } from "../../../utils/rule-sorter";

  type Props = {
    group_index: number;
    active?: boolean;
    ruleEstimate: number;
    onmeasureRules: (measurements: readonly VirtualMeasurement[]) => void;
    selectionActive?: boolean;
    selected?: boolean;
    ontoggleSelection?: () => void;
  };

  let {
    group_index,
    active = true,
    ruleEstimate,
    onmeasureRules,
    selectionActive = false,
    selected = false,
    ontoggleSelection,
  }: Props = $props();

  const store = getContext<GroupsStore>(GROUPS_STORE_CONTEXT);
  if (!store) {
    throw new Error("GroupsStore context is missing");
  }
  const dispatch = createEventDispatcher();

  let ruleList = $state<VirtualList>();
  const panelState = store.getPanelState(untrack(() => store.data[group_index].id));
  onMount(() => {
    const groupId = store.data[group_index].id;
    store.ruleScrollers.set(groupId, async (ruleId) => {
      await tick();
      const index = findRulePosition(ruleId);
      if (index >= 0) await ruleList?.scrollToIndex(index, "center");
    });
    return () => {
      store.ruleScrollers.delete(groupId);
    };
  });

  let client_width = $state<number>(Infinity);
  let is_desktop = $derived(client_width > 668);

  let group = $derived(store.data[group_index]);
  let searchActive = $derived(store.searchActive);
  let searchQuery = $derived(store.normalizedSearch);
  let isGroupSearchMatched = $derived(group ? store.searchMatchedGroupIds.has(group.id) : false);
  let groupNameHighlightParts = $derived(
    group && isGroupSearchMatched
      ? store.getSearchHighlightParts(group.name ?? "", searchQuery)
      : undefined,
  );
  let hasGroupNameSearchHighlight = $derived(Boolean(groupNameHighlightParts));
  let visibleRuleIndices = $derived(store.visibilityMap.get(group_index));
  let effectiveOpen = $derived(group ? (store.open_state[group.id] ?? false) : false);
  let hasDuplicateConflicts = $derived(group ? store.duplicateGroupIds.has(group.id) : false);

  function toggleOpen() {
    if (!group) return;
    store.open_state[group.id] = !effectiveOpen;
  }

  function copyRulePatterns() {
    if (group) copyRulePatternsToClipboard(group.rules);
  }

  type GroupDnD = {
    group_id: string;
    group_index: number;
    name: string;
    color: string;
    count: number;
  };

  function createGroupDragPreview(
    headerEl: HTMLElement,
    name: string,
    color: string,
    count: number,
  ) {
    const badge = document.createElement("div");
    badge.style.cssText =
      "position:fixed;top:-1000px;left:-1000px;pointer-events:none;z-index:2147483647;transform:translateZ(0);font:600 13px/1.2 var(--font, -apple-system, system-ui, Segoe UI, Roboto, sans-serif);color:var(--text,#e5e7eb);";

    const inner = document.createElement("div");
    inner.style.cssText =
      "display:flex;align-items:center;gap:.55rem;padding:.42rem .7rem;border-radius:.7rem;background:var(--bg-light,rgba(30,30,36,.92));border:1px solid var(--bg-light-extra,rgba(255,255,255,.12));box-shadow:0 6px 18px rgba(0,0,0,.35);backdrop-filter:saturate(120%) blur(6px);";

    const colorBadge = document.createElement("span");
    colorBadge.style.cssText =
      "display:inline-block;width:10px;height:10px;border-radius:999px;box-shadow:0 0 0 1px rgba(255,255,255,.25) inset;";
    colorBadge.style.background = color || "#888";
    inner.appendChild(colorBadge);

    const title = document.createElement("span");
    title.textContent = name || "group";
    title.style.cssText =
      "max-width:240px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;";
    inner.appendChild(title);

    const cnt = document.createElement("span");
    cnt.textContent = `• ${count}`;
    cnt.style.opacity = "0.8";
    inner.appendChild(cnt);

    const gripClone = headerEl.querySelector(".group-grip")?.cloneNode(true) as HTMLElement | null;
    if (gripClone) {
      gripClone.style.cssText += "opacity:.9;display:flex;align-items:center;margin-left:.25rem;";
      inner.appendChild(gripClone);
    }

    badge.appendChild(inner);
    document.body.appendChild(badge);
    return badge;
  }

  let totalRulesCount = $derived(
    searchActive && Array.isArray(visibleRuleIndices)
      ? visibleRuleIndices.length
      : (group?.rules.length ?? 0),
  );

  const originalIndexAt = (index: number) =>
    searchActive && Array.isArray(visibleRuleIndices) ? visibleRuleIndices[index] : index;

  function handleSort(field: SortField) {
    if (!group) return;
    if (!panelState.initialOrderIds) {
      panelState.initialOrderIds = group.rules.map((rule) => rule.id);
    }

    if (panelState.sortField === field && panelState.sortDirection === "desc") {
      panelState.sortField = null;
      panelState.sortDirection = "asc";

      if (panelState.initialOrderIds) {
        store.restoreGroupRulesOrder(group_index, panelState.initialOrderIds);
        void tick().then(() => ruleList?.refresh());
      }
      return;
    }

    if (panelState.sortField === field) {
      panelState.sortDirection = "desc";
    } else {
      panelState.sortField = field;
      panelState.sortDirection = "asc";
    }

    store.sortGroupRules(group_index, field, panelState.sortDirection);
    void tick().then(() => ruleList?.refresh());
  }

  function findRulePosition(ruleId: string) {
    if (!group) return -1;
    if (searchActive && Array.isArray(visibleRuleIndices)) {
      const visibleIndex = visibleRuleIndices.findIndex((idx) => group.rules[idx]?.id === ruleId);
      if (visibleIndex >= 0) return visibleIndex;
    }
    return group.rules.findIndex((rule) => rule.id === ruleId);
  }

  function handleDuplicateConflictClick(groupId: string, ruleId: string) {
    const highlighted = store.highlightRuleTemporarily(ruleId);
    if (!highlighted) return;
    store.requestDuplicateRuleFocus(groupId, ruleId);
  }

  $effect(() => {
    if (!group) return;
    const request = store.duplicateFocusRequest;
    if (!request || request.groupId !== group.id) return;

    void (async () => {
      const rulePosition = findRulePosition(request.ruleId);
      store.open_state[group.id] = true;

      await tick();
      if (rulePosition >= 0) await ruleList?.scrollToIndex(rulePosition, "center");
      await tick();

      if (typeof window !== "undefined") {
        requestAnimationFrame(() => {
          const row = document.querySelector<HTMLElement>(
            `.rule[data-group-uuid="${group.id}"][data-uuid="${request.ruleId}"]`,
          );
          if (row) {
            const rect = row.getBoundingClientRect();
            const inView = rect.top >= 0 && rect.bottom <= window.innerHeight;
            if (!inView) {
              row.scrollIntoView({ behavior: "smooth", block: "center" });
            }
          } else {
            const targetGroup = document.querySelector<HTMLElement>(
              `.group[data-uuid="${group.id}"]`,
            );
            if (targetGroup) {
              const rect = targetGroup.getBoundingClientRect();
              const inView = rect.top >= 0 && rect.bottom <= window.innerHeight;
              if (!inView) {
                targetGroup.scrollIntoView({ behavior: "smooth", block: "center" });
              }
            }
          }
        });
      }

      store.consumeDuplicateRuleFocus(request.groupId, request.ruleId, request.nonce);
    })();
  });

  $effect(() => {
    group?.rules.length;
    if (panelState.sortField === null) {
      panelState.initialOrderIds = null;
    }
  });
</script>

<svelte:window bind:innerWidth={client_width} />

{#if group}
  <div
    class="group"
    role="listitem"
    data-uuid={group.id}
    use:draggable={{
      data: {
        group_id: group.id,
        group_index,
        name: group.name,
        color: group.color,
        count: group.rules.length,
      } as GroupDnD,
      scope: "group",
      handle: ".group-grip",
      effects: { effectAllowed: "move", dropEffect: "move" },
      dragImage: (node) =>
        createGroupDragPreview(
          (node.querySelector(".group-header") ?? node) as HTMLElement,
          group.name,
          group.color || "",
          group.rules.length,
        ),
    }}
  >
    <Collapsible.Root open={effectiveOpen} onOpenChange={toggleOpen}>
      <div
        class="group-header"
        data-group-index={group_index}
        use:droppable={{
          data: { rule_id: "", rule_index: 0, group_id: group.id, group_index },
          scope: "rule",
          canDrop: (src) => src.group_id !== group.id || src.rule_index !== 0,
          onDrop: (source) =>
            store.changeRuleIndex(source.group_index, source.rule_index, group_index, 0),
        }}
      >
        <div class="group-left">
          {#if selectionActive}
            <button
              class="group-color"
              style="background: {group.color}"
              type="button"
              aria-label={`${t("Toggle selection")}: ${group.name}`}
              aria-pressed={selected}
              title={t(selected ? "Deselect item" : "Select item")}
              onclick={ontoggleSelection}
            ></button>
          {:else}
            <label class="group-color" style="background: {group.color}">
              <input type="color" bind:value={group.color} />
            </label>
          {/if}

          <div class="group-grip" title={t("Drag Group")}>
            <Grip />
          </div>

          <div class="group-name-wrap">
            <div class="group-name-field">
              <input
                type="text"
                placeholder={t("group name...")}
                class="group-name"
                class:search-text-hidden={hasGroupNameSearchHighlight}
                class:has-warning={hasDuplicateConflicts}
                bind:value={group.name}
              />
              {#if hasGroupNameSearchHighlight && groupNameHighlightParts}
                <div
                  class="search-highlight-overlay group-name-search-overlay"
                  class:has-warning={hasDuplicateConflicts}
                  aria-hidden="true"
                >
                  {#each groupNameHighlightParts as part}
                    {#if part.matched}
                      <mark>{part.text}</mark>
                    {:else}
                      {part.text}
                    {/if}
                  {/each}
                </div>
              {/if}
            </div>
            {#if hasDuplicateConflicts}
              <GroupDuplicateMenu
                getConflicts={() => store.getDuplicateConflictsForGroup(group.id)}
                isRuleHighlighted={store.isRuleHighlighted}
                onConflictClick={handleDuplicateConflictClick}
              />
            {/if}
          </div>
        </div>

        <div class="group-actions">
          <RouteSelect bind:interface={group.interface} bind:profile={group.profile} />

          <Tooltip value={t(group.enable ? "Disable Group" : "Enable Group")}>
            <Switch class="enable-group" bind:checked={group.enable} />
          </Tooltip>

          {#if is_desktop}
            <Tooltip value={t("Delete Group")}>
              <Button small onclick={() => store.deleteGroup(group_index)}>
                <Delete size={20} />
              </Button>
            </Tooltip>
            <Tooltip value={t("Add Rule")}>
              <Button
                small
                onclick={() => {
                  store.addRuleToGroup(group_index, defaultRule(), true);
                  store.open_state[group.id] = true;
                }}
              >
                <Add size={20} />
              </Button>
            </Tooltip>
            <Tooltip value={t("Import Rule List")}>
              <Button small onclick={() => dispatch("importRules")}>
                <ImportList size={20} />
              </Button>
            </Tooltip>
            <Tooltip value={t("Copy to Clipboard")}>
              <Button small onclick={copyRulePatterns}>
                <Copy size={20} />
              </Button>
            </Tooltip>
          {:else}
            <DropdownMenu>
              {#snippet trigger()}
                <Dots size={20} />
              {/snippet}
              {#snippet item1()}
                <Button
                  general
                  onclick={() => {
                    store.addRuleToGroup(group_index, defaultRule(), true);
                    store.open_state[group.id] = true;
                  }}
                >
                  <div class="dd-icon"><Add size={20} /></div>
                  <div class="dd-label">{t("Add Rule")}</div>
                </Button>
              {/snippet}
              {#snippet item2()}
                <Button general onclick={() => dispatch("importRules")}>
                  <div class="dd-icon"><ImportList size={20} /></div>
                  <div class="dd-label">{t("Import Rule List")}</div>
                </Button>
              {/snippet}
              {#snippet item3()}
                <Button general onclick={copyRulePatterns}>
                  <div class="dd-icon"><Copy size={20} /></div>
                  <div class="dd-label">{t("Copy to Clipboard")}</div>
                </Button>
              {/snippet}
              {#snippet item4()}
                <Button general onclick={() => store.deleteGroup(group_index)}>
                  <div class="dd-icon"><Delete size={20} /></div>
                  <div class="dd-label">{t("Delete Group")}</div>
                </Button>
              {/snippet}
            </DropdownMenu>
          {/if}

          <Tooltip value={t(effectiveOpen ? "Collapse Group" : "Expand Group")}>
            <Collapsible.Trigger>
              {#if effectiveOpen}
                <GroupCollapse size={20} />
              {:else}
                <GroupExpand size={20} />
              {/if}
            </Collapsible.Trigger>
          </Tooltip>
        </div>
        <EdgeNumberInput
          title={t("Group priority")}
          label={`${t("Group priority")}: ${group.name}`}
          bind:value={group.priority}
        />
      </div>

      <Collapsible.Content>
        {#snippet child({ props })}
          {#if effectiveOpen}
            <div {...props}>
              {#if totalRulesCount > 0}
                <div class="group-rules-header">
                  <div class="group-rules-header-column total">
                    #{totalRulesCount}
                  </div>
                  <!-- svelte-ignore a11y_click_events_have_key_events -->
                  <!-- svelte-ignore a11y_no_static_element_interactions -->
                  <!-- svelte-ignore a11y_click_events_have_key_events -->
                  <!-- svelte-ignore a11y_no_static_element_interactions -->
                  <div
                    class="group-rules-header-column clickable"
                    onclick={() => handleSort("name")}
                  >
                    {t("Name")}
                    <div class="sort-icon">
                      {#if panelState.sortField === "name" && panelState.sortDirection === "desc"}
                        <SortAsc size={16} />
                      {:else if panelState.sortField === "name"}
                        <SortDesc size={16} />
                      {:else}
                        <SortNeutral size={16} />
                      {/if}
                    </div>
                  </div>
                  <div class="group-rules-header-column">{t("Type")}</div>
                  <!-- svelte-ignore a11y_click_events_have_key_events -->
                  <!-- svelte-ignore a11y_no_static_element_interactions -->
                  <div
                    class="group-rules-header-column clickable"
                    onclick={() => handleSort("pattern")}
                  >
                    {t("Pattern")}
                    <div class="sort-icon">
                      {#if panelState.sortField === "pattern" && panelState.sortDirection === "desc"}
                        <SortAsc size={16} />
                      {:else if panelState.sortField === "pattern"}
                        <SortDesc size={16} />
                      {:else}
                        <SortNeutral size={16} />
                      {/if}
                    </div>
                  </div>
                  <div class="group-rules-header-column">{t("Enabled")}</div>
                </div>
              {/if}
              <div class="group-rules">
                {#if totalRulesCount > 0}
                  <VirtualList
                    bind:this={ruleList}
                    recycle
                    count={totalRulesCount}
                    getKey={(index) => group.rules[originalIndexAt(index)].id}
                    estimateSize={ruleEstimate}
                    measurementKey={group.rules}
                    onmeasure={onmeasureRules}
                    active={active && effectiveOpen}
                    revision={visibleRuleIndices ?? group.rules}
                  >
                    {#snippet children(index)}
                      {@const originalIndex = originalIndexAt(index)}
                      {@const rule = group.rules[originalIndex]}
                      <RuleRow
                        key={rule.id}
                        bind:rule={group.rules[originalIndex]}
                        rule_index={originalIndex}
                        {group_index}
                        rule_id={rule.id}
                        group_id={group.id}
                        isDuplicate={store.isRuleDuplicate(rule.id)}
                        isHighlighted={store.isRuleHighlighted(rule.id)}
                        style={index % 2 ? "" : "background-color: var(--bg-light)"}
                      />
                    {/snippet}
                  </VirtualList>
                {/if}
              </div>
            </div>
          {:else}
            <!-- Keep the ARIA target without attaching Bits' measurement
                 observer or creating a nested rule list for closed panels. -->
            <div id={typeof props.id === "string" ? props.id : undefined} hidden></div>
          {/if}
        {/snippet}
      </Collapsible.Content>
    </Collapsible.Root>
  </div>
{/if}

<style>
  .group {
    & {
      background-color: var(--bg-medium);
      border-radius: 0.5rem;
      border: 1px solid var(--bg-light-extra);
      overflow: hidden;
      transition:
        transform 0.12s ease,
        opacity 0.12s ease,
        box-shadow 0.12s ease;
    }
  }

  .group-header {
    & {
      min-height: 2.25rem;
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 0.5rem 2rem 0.5rem 0.5rem;
      border-radius: 0.5rem;
      background-color: var(--bg-light);
      position: relative;
    }
  }

  .group-left {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    flex: 1 1 auto;
    min-width: 0;
  }

  .group-color {
    & {
      display: inline-block;
      padding: 0;
      border: 0;
      width: 2rem;
      height: 100%;
      border-top-left-radius: calc(0.5rem - 1px);
      border-bottom-left-radius: calc(0.5rem - 1px);
      position: absolute;
      left: 0;
      top: 0;
      overflow: hidden;
      cursor: pointer;
    }

    & input {
      margin-left: 0.5rem;
    }

    &:focus-visible {
      outline: 2px solid var(--accent);
      outline-offset: -2px;
    }
  }

  .group-grip {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    margin-left: 2.2rem;
    color: var(--text-2);
    cursor: grab;
    user-select: none;
    -webkit-user-select: none;
    -webkit-user-drag: none;
  }
  .group-grip:hover {
    color: var(--text);
  }

  .group-name {
    & {
      border: none;
      background-color: transparent;
      font-size: 1.3rem;
      font-weight: 600;
      font-family: var(--font);
      color: var(--text);
      border-bottom: 1px solid transparent;
      position: relative;
      top: 0.1rem;
      min-width: 0;
      width: 100%;
      box-sizing: border-box;
    }

    &.has-warning {
      padding-right: 2.2rem;
    }

    &:focus-visible {
      outline: none;
      border-bottom: 1px solid var(--accent);
    }
  }

  .group-name-wrap {
    display: flex;
    position: relative;
    align-items: center;
    margin-left: 0.4rem;
    min-width: 0;
    flex: 1 1 auto;
  }

  .group-name-wrap :global([data-dropdown-menu-trigger]) {
    position: absolute;
    right: 0;
    top: 0;
    bottom: 0;
    display: flex;
    align-items: center;
    z-index: 2;
  }

  .group-name-field {
    position: relative;
    width: 100%;
    min-width: 0;
    flex: 1 1 auto;
  }

  .search-highlight-overlay {
    position: absolute;
    inset: 0;
    width: 100%;
    pointer-events: none;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: pre;
    color: var(--text);
    padding: 0;
    margin: 0;
    box-sizing: border-box;
  }

  .search-highlight-overlay.has-warning {
    padding-right: 2.2rem;
  }

  .group-name-search-overlay {
    transform: translateY(0.1rem);
    font: 600 1.3rem var(--font);
  }

  .search-highlight-overlay mark {
    background: color-mix(in oklab, var(--yellow) 34%, transparent);
    box-shadow:
      inset 0 0 0 1px color-mix(in oklab, var(--yellow) 62%, transparent),
      0 1px 0 color-mix(in oklab, var(--yellow) 30%, transparent);
    color: inherit;
    border-radius: 0.22rem;
  }

  .group-name.search-text-hidden {
    color: transparent;
    -webkit-text-fill-color: transparent;
    caret-color: var(--text);
  }

  .group-name.search-text-hidden:focus,
  .group-name.search-text-hidden:focus-visible {
    color: var(--text);
    -webkit-text-fill-color: var(--text);
  }

  .group-name:focus + .search-highlight-overlay,
  .group-name:focus-visible + .search-highlight-overlay {
    display: none;
  }

  .group-actions {
    & {
      min-height: 2.25rem;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 0.2rem;
    }
    &:global([data-switch-root]) {
      margin: 0 0.3rem;
    }
  }

  .group-rules-header {
    display: grid;
    grid-template-columns: 4rem 2.1fr 1fr 3fr 1fr;
    justify-content: center;
    align-items: center;

    font-size: 0.9rem;
    color: var(--text-2);
    padding-top: 0.6rem;
    padding-bottom: 0.2rem;
    border-bottom: 1px solid var(--bg-light-extra);
  }

  .group-rules-header-column {
    & {
      display: flex;
      align-items: center;
      justify-content: center;
    }

    &.total {
      justify-content: start;
      margin-left: 0.5rem;
    }

    &.total :global(svg) {
      position: relative;
      top: -1px;
    }
  }

  .clickable {
    cursor: pointer;
    user-select: none;
    transition: color 0.12s ease;
  }

  .clickable:hover {
    color: var(--text);
  }

  .sort-icon {
    margin-left: 0.4rem;
    display: flex;
    align-items: center;
    color: var(--text-2);
  }

  :global {
    [data-collapsible-trigger] {
      & {
        color: var(--text-2);
        background-color: transparent;
        border: 1px solid transparent;
        display: inline-flex;
        align-items: center;
        justify-content: center;
        padding: 0.4rem;
        border-radius: 0.5rem;
        cursor: pointer;
      }

      &:hover {
        background-color: var(--bg-dark);
        color: var(--text);
        border: 1px solid var(--bg-light-extra);
      }
    }
  }

  input[type="color"] {
    -webkit-appearance: none;
    -moz-appearance: none;
    appearance: none;
    background: transparent;
    width: auto;
    height: 0;
    padding: 0;
    border: none;
    cursor: pointer;
  }

  @media (max-width: 700px) {
    .group-header {
      display: flex;
      flex-direction: column;
      align-items: start;
      justify-content: center;
      padding: 0.4rem 2rem 0.4rem 0.5rem;
    }

    .group-left {
      flex: none;
      width: 100%;
    }

    .group-name-wrap {
      width: calc(100% - 2rem);
      margin-left: 2rem;
    }

    .group-grip {
      display: none;
    }

    .group-actions {
      width: calc(100% - 2rem);
      justify-content: stretch;
      gap: 0.25rem;
      margin-left: 2rem;
    }

    :global(.group-actions > *:nth-child(1)) {
      margin-right: auto;
      min-width: 100px;
      flex: 0 1 auto;
    }

    :global(.group-actions > *:nth-child(2)) {
      margin-left: auto;
    }

    .group-rules-header {
      height: 1px;
      & .group-rules-header-column {
        display: none;
      }
    }
  }
  .clickable {
    cursor: pointer;
    user-select: none;
    transition: color 0.12s ease;
  }
  .clickable:hover {
    color: var(--text);
  }

  .sort-icon {
    margin-left: 0.4rem;
    display: flex;
    align-items: center;
    color: var(--text-2);
  }
</style>
