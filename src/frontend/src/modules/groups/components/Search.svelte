<script lang="ts">
  import { t } from "../../../data/locale.svelte";
  import { Search } from "../../../components/ui/icons";
  import type { Group, Rule } from "../../../types";

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

  type Props = {
    value?: string;
    groups: Group[];
    dataRevision?: number;
    visibleGroups?: VisibleGroup[];
    searchActive?: boolean;
    searchPending?: boolean;
    controls?: SearchControls | null;
    [key: string]: any;
  };

  let {
    value = $bindable(""),
    groups = [],
    dataRevision = 0,
    visibleGroups = $bindable([]),
    searchActive = $bindable(false),
    searchPending = $bindable(false),
    controls = $bindable(null),
    ...rest
  }: Props = $props();

  const SEARCH_DEBOUNCE_MS = 30 as const;

  const forcedGroupIds = new Set<string>();
  const forcedRuleIdsByGroup = new Map<string, Set<string>>();
  let forcedSearchKey = "";

  let normalizedSearch = $derived(value.trim().toLowerCase());
  let isFocused = $state(false);
  let inputRef: HTMLInputElement;
  let isActive = $derived(isFocused || value.length > 0);

  $effect(() => {
    searchActive = Boolean(normalizedSearch);
  });

  function handleContainerClick() {
    inputRef?.focus();
  }

  function resetForcedVisibility(searchKey: string) {
    if (!forcedSearchKey) return;
    if (forcedSearchKey !== searchKey) {
      forcedGroupIds.clear();
      forcedRuleIdsByGroup.clear();
      forcedSearchKey = "";
    }
  }

  function markGroupOrderChanged() {
    // No-op in simple search but kept for interface compatibility if needed
  }

  function forceVisibleGroup(groupId: string) {
    if (!normalizedSearch) return;
    forcedGroupIds.add(groupId);
    forcedSearchKey = normalizedSearch;
  }

  function forceVisibleRule(groupId: string, ruleId: string) {
    if (!normalizedSearch) return;
    let forced = forcedRuleIdsByGroup.get(groupId);
    if (!forced) {
      forced = new Set<string>();
      forcedRuleIdsByGroup.set(groupId, forced);
    }
    forced.add(ruleId);
    forcedSearchKey = normalizedSearch;
  }

  function removeForcedGroup(groupId: string) {
    forcedGroupIds.delete(groupId);
    forcedRuleIdsByGroup.delete(groupId);
  }

  function removeForcedRule(groupId: string, ruleId: string) {
    const forced = forcedRuleIdsByGroup.get(groupId);
    if (!forced) return;
    forced.delete(ruleId);
    if (!forced.size) forcedRuleIdsByGroup.delete(groupId);
  }

  function moveForcedRule(sourceGroupId: string, targetGroupId: string, ruleId: string) {
    if (sourceGroupId === targetGroupId) return;
    const forced = forcedRuleIdsByGroup.get(sourceGroupId);
    if (!forced?.has(ruleId)) return;
    forced.delete(ruleId);
    if (!forced.size) forcedRuleIdsByGroup.delete(sourceGroupId);
    let targetForced = forcedRuleIdsByGroup.get(targetGroupId);
    if (!targetForced) {
      targetForced = new Set<string>();
      forcedRuleIdsByGroup.set(targetGroupId, targetForced);
    }
    targetForced.add(ruleId);
  }

  function performSearch() {
    const query = normalizedSearch;
    resetForcedVisibility(query);

    if (!query) {
      visibleGroups = groups.map((_, index) => ({ group_index: index, ruleIndices: null }));
      searchPending = false;
      return;
    }

    const nextVisible: VisibleGroup[] = [];

    for (let i = 0; i < groups.length; i++) {
      const group = groups[i];
      if (!group) continue;

      const groupName = (group.name || "").toLowerCase();
      const groupMatch = groupName.includes(query);
      const isForcedGroup = forcedGroupIds.has(group.id);

      if (groupMatch || isForcedGroup) {
        nextVisible.push({ group_index: i, ruleIndices: null });
        continue;
      }

      const matchedRuleIndices: number[] = [];
      const forcedRules = forcedRuleIdsByGroup.get(group.id);

      for (let ruleIndex = 0; ruleIndex < group.rules.length; ruleIndex++) {
        const rule = group.rules[ruleIndex];
        const isForcedRule = forcedRules?.has(rule.id);
        const ruleBlob = `${rule.name || ""} ${rule.rule || ""}`.toLowerCase();

        if (isForcedRule || ruleBlob.includes(query)) {
          matchedRuleIndices.push(ruleIndex);
        }
      }

      if (matchedRuleIndices.length > 0) {
        nextVisible.push({ group_index: i, ruleIndices: matchedRuleIndices });
      }
    }

    visibleGroups = nextVisible;
    searchPending = false;
  }

  let debounceTimer: number;
  $effect(() => {
    normalizedSearch;
    dataRevision;
    groups.length;

    clearTimeout(debounceTimer);
    searchPending = true;
    debounceTimer = window.setTimeout(performSearch, SEARCH_DEBOUNCE_MS);
  });

  const controlsImpl: SearchControls = {
    markGroupOrderChanged,
    forceVisibleGroup,
    forceVisibleRule,
    removeForcedGroup,
    removeForcedRule,
    moveForcedRule,
  };

  $effect(() => {
    controls = controlsImpl;
  });
</script>

<div class="group-controls-search" {...rest}>
  <!-- svelte-ignore a11y_click_events_have_key_events -->
  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <div class="search-container" class:active={isActive} onclick={handleContainerClick}>
    <span class="icon-wrapper">
      <Search />
    </span>

    <div class="input-wrapper">
      <input
        bind:this={inputRef}
        type="search"
        class="search-input"
        placeholder={isActive ? t("Search groups and rules...") : ""}
        bind:value
        onfocus={() => (isFocused = true)}
        onblur={() => (isFocused = false)}
      />
    </div>
  </div>
</div>

<style>
  .group-controls-search {
    display: flex;
    align-items: center;
    flex: 0 1 auto;
    transition: flex-grow 0.3s ease;
    min-width: 0;
  }

  .search-container {
    background-color: var(--bg-light);
    padding: 0.6rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    color: var(--text-2);
    font: 400 1rem var(--font);
    display: flex;
    align-items: center;
    cursor: pointer;
    box-sizing: border-box;
    white-space: nowrap;
    width: auto;
    transition:
      background-color 0.1s ease-in-out,
      border-color 0.1s ease-in-out,
      box-shadow 0.1s ease-in-out,
      color 0.1s ease-in-out;
  }

  .search-container:hover {
    background-color: var(--bg-light-extra);
    color: var(--text);
  }

  .search-container.active {
    cursor: text;
    background-color: var(--bg-light);
    color: var(--text);
    border-color: var(--accent);
    max-width: 100%;
    box-shadow:
      0 0 0 1px color-mix(in oklab, var(--accent) 45%, transparent),
      0 6px 18px -14px color-mix(in oklab, var(--accent) 35%, transparent);
  }

  .icon-wrapper {
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }

  .input-wrapper {
    width: 0;
    overflow: hidden;
    transition: width 0.3s cubic-bezier(0.25, 1, 0.5, 1);
  }

  .search-container.active .input-wrapper {
    margin-left: 0.3rem;
    width: clamp(500px, 50vw, 700px);
  }

  .search-input {
    appearance: none;
    border: none;
    background: transparent;
    outline: none;
    margin: 0;
    padding: 0;
    font: inherit;
    color: inherit;
    width: 100%;
    margin-left: 0.8rem;
    opacity: 0;
    transition: opacity 0.2s ease;
  }

  .search-container.active .search-input {
    opacity: 1;
  }
</style>
