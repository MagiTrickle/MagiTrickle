<script lang="ts">
  import { onDestroy, untrack } from "svelte";

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
    visibleGroups?: VisibleGroup[];
    searchActive?: boolean;
    searchPending?: boolean;
    controls?: SearchControls | null;
    [key: string]: any;
  };

  let {
    value = $bindable(""),
    groups = [],
    visibleGroups = $bindable([]),
    searchActive = $bindable(false),
    searchPending = $bindable(false),
    controls = $bindable(null),
    ...rest
  }: Props = $props();

  const SEARCH_DEBOUNCE_MS = 60 as const;
  const SEARCH_YIELD_MS = 8 as const;
  const RULE_YIELD_BATCH = 80 as const;

  type GroupSearchCache = {
    name: string;
    blob: string;
    lowerBlob: string;
  };

  type RuleSearchCache = {
    name: string;
    pattern: string;
    blob: string;
    lowerBlob: string;
  };

  const groupSearchCache = new WeakMap<Group, GroupSearchCache>();
  const ruleSearchCache = new WeakMap<Rule, RuleSearchCache>();
  const forcedGroupIds = new Set<string>();
  const forcedRuleIdsByGroup = new Map<string, Set<string>>();

  let forcedSearchKey = "";
  let groupOrderRevision = $state(0);
  let lastGroupOrderRevision = 0;
  let lastSearchRevision = 0;
  let searchRunId = 0;
  let searchDebounceId: number | null = null;
  let lastSearchQuery = "";
  let lastVisibleGroups: VisibleGroup[] = [];

  let dataRevision = $state(0);
  let normalizedSearch = $derived(value.trim().toLowerCase());

  let isFocused = $state(false);
  let inputRef: HTMLInputElement;

  let isActive = $derived(isFocused || value.length > 0);

  $effect(() => {
    searchActive = Boolean(normalizedSearch);
  });

  $effect(() => {
    $state.snapshot(groups);
    dataRevision = untrack(() => dataRevision) + 1;
  });

  function handleContainerClick() {
    inputRef?.focus();
  }

  function getGroupSearchCache(group: Group): GroupSearchCache {
    const name = group.name ?? "";
    const cached = groupSearchCache.get(group);
    if (cached && cached.name === name) {
      return cached;
    }

    const blob = name;
    const next = {
      name,
      blob,
      lowerBlob: blob.toLowerCase(),
    };
    groupSearchCache.set(group, next);
    return next;
  }

  function getRuleSearchCache(rule: Rule): RuleSearchCache {
    const name = rule.name ?? "";
    const pattern = rule.rule ?? "";
    const cached = ruleSearchCache.get(rule);
    if (cached && cached.name === name && cached.pattern === pattern) {
      return cached;
    }

    const blob = `${name}\n${pattern}`;
    const next = {
      name,
      pattern,
      blob,
      lowerBlob: blob.toLowerCase(),
    };
    ruleSearchCache.set(rule, next);
    return next;
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
    groupOrderRevision += 1;
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

  async function runSearch(runId: number, query: string) {
    const now =
      typeof performance !== "undefined" && typeof performance.now === "function"
        ? () => performance.now()
        : () => Date.now();
    let lastYield = now();
    const canYield = typeof window !== "undefined";
    const shouldYield = () => canYield && now() - lastYield > SEARCH_YIELD_MS;
    const yieldControl = async () => {
      if (!shouldYield()) return true;
      await new Promise<void>((resolve) => setTimeout(resolve, 0));
      if (runId !== searchRunId) return false;
      lastYield = now();
      return true;
    };

    resetForcedVisibility(query);

    if (!query) {
      const allVisible: VisibleGroup[] = [];
      for (let index = 0; index < groups.length; index++) {
        if (groups[index]) {
          allVisible.push({ group_index: index, ruleIndices: null });
        }
      }
      visibleGroups = allVisible;
      lastSearchQuery = "";
      lastVisibleGroups = allVisible;
      lastGroupOrderRevision = groupOrderRevision;
      lastSearchRevision = dataRevision;
      searchPending = false;
      return;
    }

    const hasForced = forcedGroupIds.size > 0 || forcedRuleIdsByGroup.size > 0;
    const orderChanged = groupOrderRevision !== lastGroupOrderRevision;
    const dataChanged = dataRevision !== lastSearchRevision;
    const canUsePrevious =
      !orderChanged &&
      !dataChanged &&
      !hasForced &&
      lastSearchQuery.length > 0 &&
      query.startsWith(lastSearchQuery);

    const nextVisible: VisibleGroup[] = [];
    const groupsToScan = canUsePrevious ? lastVisibleGroups : null;
    const totalGroups = groupsToScan ? groupsToScan.length : groups.length;

    for (let i = 0; i < totalGroups; i++) {
      if (runId !== searchRunId) return;
      const group_index = groupsToScan ? groupsToScan[i].group_index : i;
      const group = groups[group_index];
      if (!group) continue;

      const groupCache = getGroupSearchCache(group);
      const groupMatch = groupCache.lowerBlob.includes(query);
      const isForcedGroup = forcedGroupIds.has(group.id);

      if (groupMatch || isForcedGroup) {
        nextVisible.push({ group_index, ruleIndices: null });
        if (!(await yieldControl())) return;
        continue;
      }

      const forcedRules = forcedRuleIdsByGroup.get(group.id);
      let matchedRuleIndices: number[] | null = null;
      const rules = group.rules;

      for (let ruleIndex = 0; ruleIndex < rules.length; ruleIndex++) {
        const rule = rules[ruleIndex];
        const isForced = forcedRules ? forcedRules.has(rule.id) : false;
        let ruleMatch = false;
        if (!isForced) {
          const ruleCache = getRuleSearchCache(rule);
          ruleMatch = ruleCache.lowerBlob.includes(query);
        }

        if (ruleMatch || isForced) {
          if (!matchedRuleIndices) matchedRuleIndices = [];
          matchedRuleIndices.push(ruleIndex);
        }

        if (ruleIndex % RULE_YIELD_BATCH === 0) {
          if (!(await yieldControl())) return;
        }
      }

      if (matchedRuleIndices) {
        nextVisible.push({ group_index, ruleIndices: matchedRuleIndices });
      }

      if (!(await yieldControl())) return;
    }

    if (runId !== searchRunId) return;
    visibleGroups = nextVisible;
    lastSearchQuery = query;
    lastVisibleGroups = nextVisible;
    lastGroupOrderRevision = groupOrderRevision;
    lastSearchRevision = dataRevision;
    searchPending = false;
  }

  function scheduleSearch() {
    if (searchDebounceId !== null) {
      clearTimeout(searchDebounceId);
      searchDebounceId = null;
    }

    searchRunId += 1;
    const runId = searchRunId;
    const query = normalizedSearch;

    if (!query) {
      searchPending = false;
      void runSearch(runId, query);
      return;
    }

    searchPending = true;
    if (typeof window === "undefined") {
      void runSearch(runId, query);
      return;
    }
    searchDebounceId = window.setTimeout(() => {
      searchDebounceId = null;
      void runSearch(runId, query);
    }, SEARCH_DEBOUNCE_MS);
  }

  $effect(() => {
    const query = normalizedSearch;
    groupOrderRevision;
    if (query) {
      dataRevision;
    } else {
      groups.length;
    }
    scheduleSearch();
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

  onDestroy(() => {
    if (typeof window === "undefined") return;
    if (searchDebounceId !== null) {
      window.clearTimeout(searchDebounceId);
      searchDebounceId = null;
    }
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
