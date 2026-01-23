<script lang="ts">
  import { t } from "../../../data/locale.svelte";

  import { Search } from "../../../components/ui/icons";
  import type { Subscription } from "../../../types";

  type VisibleSubscription = {
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
    subscriptions: Subscription[];
    dataRevision?: number;
    visibleSubscriptions?: VisibleSubscription[];
    searchActive?: boolean;
    searchPending?: boolean;
    controls?: SearchControls | null;
    [key: string]: any;
  };

  let {
    value = $bindable(""),
    subscriptions = [],
    dataRevision = 0,
    visibleSubscriptions = $bindable([]),
    searchActive = $bindable(false),
    searchPending = $bindable(false),
    controls = $bindable(null),
    ...rest
  }: Props = $props();

  const SEARCH_DEBOUNCE_MS = 150 as const;

  const forcedSubIds = new Set<string>();
  const forcedRuleIdsBySub = new Map<string, Set<string>>();
  let forcedSearchKey = "";

  let normalizedSearch = $derived(value.trim().toLowerCase());

  let searchIndex = $derived.by(() => {
    dataRevision;
    return subscriptions.map((s) => ({
      id: s.id,
      nameLower: (s.name || "").toLowerCase(),
      urlLower: (s.url || "").toLowerCase(),
      rules: s.rules.map((r) => ({
        id: r.id,
        // For subscriptions, rules don't have names, so we search only by rule pattern
        searchBlob: (r.rule || "").toLowerCase(),
      })),
    }));
  });

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
      forcedSubIds.clear();
      forcedRuleIdsBySub.clear();
      forcedSearchKey = "";
    }
  }

  function markGroupOrderChanged() {}

  function forceVisibleGroup(groupId: string) {
    if (!normalizedSearch) return;
    forcedSubIds.add(groupId);
    forcedSearchKey = normalizedSearch;
  }

  function forceVisibleRule(groupId: string, ruleId: string) {
    if (!normalizedSearch) return;
    let forced = forcedRuleIdsBySub.get(groupId);
    if (!forced) {
      forced = new Set<string>();
      forcedRuleIdsBySub.set(groupId, forced);
    }
    forced.add(ruleId);
    forcedSearchKey = normalizedSearch;
  }

  function removeForcedGroup(groupId: string) {
    forcedSubIds.delete(groupId);
    forcedRuleIdsBySub.delete(groupId);
  }

  function removeForcedRule(groupId: string, ruleId: string) {
    const forced = forcedRuleIdsBySub.get(groupId);
    if (!forced) return;
    forced.delete(ruleId);
    if (!forced.size) forcedRuleIdsBySub.delete(groupId);
  }

  function moveForcedRule(sourceGroupId: string, targetGroupId: string, ruleId: string) {
    if (sourceGroupId === targetGroupId) return;
    const forced = forcedRuleIdsBySub.get(sourceGroupId);
    if (!forced?.has(ruleId)) return;
    forced.delete(ruleId);
    if (!forced.size) forcedRuleIdsBySub.delete(sourceGroupId);
    let targetForced = forcedRuleIdsBySub.get(targetGroupId);
    if (!targetForced) {
      targetForced = new Set<string>();
      forcedRuleIdsBySub.set(targetGroupId, targetForced);
    }
    targetForced.add(ruleId);
  }

  function performSearch() {
    const query = normalizedSearch;
    resetForcedVisibility(query);

    if (!query) {
      visibleSubscriptions = subscriptions.map((_, index) => ({
        group_index: index,
        ruleIndices: null,
      }));
      searchPending = false;
      return;
    }

    const nextVisible: VisibleSubscription[] = [];
    const len = searchIndex.length;

    for (let i = 0; i < len; i++) {
      const indexedSub = searchIndex[i];
      const isForcedSub = forcedSubIds.has(indexedSub.id);

      if (
        isForcedSub ||
        indexedSub.nameLower.includes(query) ||
        indexedSub.urlLower.includes(query)
      ) {
        nextVisible.push({ group_index: i, ruleIndices: null });
        continue;
      }

      const matchedRuleIndices: number[] = [];
      const forcedRules = forcedRuleIdsBySub.get(indexedSub.id);
      const rules = indexedSub.rules;
      const rulesLen = rules.length;

      for (let r = 0; r < rulesLen; r++) {
        const indexedRule = rules[r];
        if (forcedRules?.has(indexedRule.id) || indexedRule.searchBlob.includes(query)) {
          matchedRuleIndices.push(r);
        }
      }

      if (matchedRuleIndices.length > 0) {
        nextVisible.push({ group_index: i, ruleIndices: matchedRuleIndices });
      }
    }

    visibleSubscriptions = nextVisible;
    searchPending = false;
  }

  let debounceTimer: number;
  $effect(() => {
    normalizedSearch;
    searchIndex;

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

<div class="subscription-controls-search" {...rest}>
  <!-- svelte-ignore a11y_click_events_have_key_events -->
  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <div class="subscription-search-container" class:active={isActive} onclick={handleContainerClick}>
    <span class="icon-wrapper">
      <Search />
    </span>

    <div class="input-wrapper">
      <input
        bind:this={inputRef}
        type="search"
        class="subscription-search-input"
        placeholder={isActive ? t("Search subscriptions and rules...") : ""}
        bind:value
        onfocus={() => (isFocused = true)}
        onblur={() => (isFocused = false)}
      />
    </div>
  </div>
</div>

<style>
  .subscription-controls-search {
    display: flex;
    align-items: center;
    flex: 0 1 auto;
    transition: flex-grow 0.3s ease;
    min-width: 0;
  }

  .subscription-search-container {
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

  .subscription-search-container:hover {
    background-color: var(--bg-light-extra);
    color: var(--text);
  }

  .subscription-search-container.active {
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

  .subscription-search-container.active .input-wrapper {
    margin-left: 0.3rem;
    width: clamp(500px, 50vw, 700px);
  }

  .subscription-search-input {
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

  .subscription-search-container.active .subscription-search-input {
    opacity: 1;
  }
</style>
