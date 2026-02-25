import { tick } from "svelte";

import { t } from "../../data/locale.svelte";
import { ChangeTracker } from "../../utils/change-tracker.svelte";
import { defaultGroup, defaultRule } from "../../utils/defaults";
import { overlay, toast } from "../../utils/events";
import { fetcher } from "../../utils/fetcher";
import { type Group, type Rule } from "../../types";
import { type SortDirection, type SortField } from "../../utils/rule-sorter";
import {
  cloneGroupWithNewIds as cloneGroupWithNewIdsData,
  cloneGroupsWithNewIds as cloneGroupsWithNewIdsData,
  prependGroups as prependGroupsData,
  prependRules as prependRulesData,
  restoreGroupRulesOrder as restoreGroupRulesOrderData,
  sortGroupRules as sortGroupRulesData,
  toConfigPayload as toConfigPayloadData,
} from "./groups-data";

export const GROUPS_STORE_CONTEXT = Symbol("groups-store");

const SEARCH_DEBOUNCE_MS = 150 as const;
const IMPORT_RULES_CHUNK_SIZE = 300 as const;
const IMPORT_GROUPS_CLONE_CHUNK_SIZE = 20 as const;
const IMPORT_GROUPS_INSERT_CHUNK_SIZE = 25 as const;

const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));

export type VisibleGroup = {
  group_index: number;
  ruleIndices: number[] | null;
};

export type GroupDragData = {
  group_id: string;
  group_index: number;
  name: string;
  color: string;
  count: number;
};

export type GroupDropSlotData = {
  group_index: number;
  insert: "before" | "after";
};

export type GroupDuplicateConflict = {
  ruleId: string;
  ruleName: string;
  rulePattern: string;
  ruleType: string;
  groupId: string;
  groupName: string;
  groupColor: string;
  inCurrentGroup: boolean;
  duplicateKey: string;
  totalRulesWithSameKey: number;
};

export type DuplicateRuleFocusRequest = {
  groupId: string;
  ruleId: string;
  nonce: number;
};

type DuplicateComputationResult = {
  duplicateRules: Set<string>;
  duplicateGroups: Set<string>;
  duplicateRuleKeys: Map<string, string>;
  duplicateConflictsByGroup: Map<string, GroupDuplicateConflict[]>;
};

type SearchIndexRule = {
  id: string;
  searchBlob: string;
};

type SearchIndexGroup = {
  id: string;
  nameLower: string;
  rules: SearchIndexRule[];
};

type GroupsStoreOptions = {
  onRenderComplete?: () => void;
};

export class GroupsStore {
  onRenderComplete?: () => void;

  tracker = $state(new ChangeTracker<Group[]>([]));
  data = $derived.by(() => this.tracker.data);
  dataRevision = $state(0);
  valid_rules = $state(true);
  canSave = $derived(this.tracker.isDirty && this.valid_rules);

  open_state = $state<Record<string, boolean>>({});

  searchValue = $state("");
  visibleGroups = $state<VisibleGroup[]>([]);
  searchPending = $state(false);
  searchMatchedGroupIds = $state<Set<string>>(new Set());
  searchMatchedRuleIds = $state<Set<string>>(new Set());

  normalizedSearch = $derived(this.searchValue.trim().toLowerCase());
  searchActive = $derived(Boolean(this.normalizedSearch));

  searchIndex = $state<SearchIndexGroup[]>([]);
  searchIndexRevision = $state(-1);

  visibilityMap = $derived(new Map(this.visibleGroups.map((v) => [v.group_index, v.ruleIndices])));

  firstVisibleGroupIndex = $derived(
    this.searchActive ? (this.visibleGroups.length ? this.visibleGroups[0].group_index : -1) : 0,
  );

  noVisibleGroups = $derived(
    this.searchActive && !this.searchPending && this.visibleGroups.length === 0,
  );

  finishedGroupsCount = $state(0);
  fetchError = $state(false);
  dataLoaded = $state(false);

  isAllRendered = $derived(
    this.dataLoaded &&
      (this.data.length === 0 || this.finishedGroupsCount >= this.data.length || this.searchActive),
  );

  isEmptyData = $derived(
    this.dataLoaded &&
      !this.fetchError &&
      !this.searchActive &&
      !this.searchPending &&
      this.data.length === 0,
  );
  duplicateRuleIds = $state<Set<string>>(new Set());
  duplicateGroupIds = $state<Set<string>>(new Set());
  duplicateRuleKeys = $state<Map<string, string>>(new Map());
  duplicateConflictsByGroup = $state<Map<string, GroupDuplicateConflict[]>>(new Map());
  activeDuplicateKey = $state<string | null>(null);
  duplicateHighlightPinned = $state(false);
  highlightedRuleId = $state<string | null>(null);
  duplicateFocusRequest = $state<DuplicateRuleFocusRequest | null>(null);

  renderGroupsLimit = $state(1);
  renderGroupsTimeout: number | null = null;

  #forcedGroupIds = new Set<string>();
  #forcedRuleIdsByGroup = new Map<string, Set<string>>();
  #forcedSearchKey = "";
  #debounceTimer: number | null = null;
  #duplicateRuleIdsTimer: number | null = null;
  #highlightedRuleTimer: number | null = null;
  #searchIndexBuildToken = 0;
  #searchIndexBuilding = false;
  #dispose: (() => void) | null = null;

  constructor(options: GroupsStoreOptions = {}) {
    this.onRenderComplete = options.onRenderComplete;
    this.#setupEffects();
  }

  #setupEffects() {
    this.#dispose = $effect.root(() => {
      $effect(() => {
        if (typeof window === "undefined" || !this.canSave) return;

        const handleBeforeUnload = (event: BeforeUnloadEvent) => {
          event.preventDefault();
        };

        window.addEventListener("beforeunload", handleBeforeUnload);
        return () => window.removeEventListener("beforeunload", handleBeforeUnload);
      });

      $effect(() => {
        this.dataRevision;
        if (typeof window === "undefined") return;
        setTimeout(() => this.checkRulesValidityState(), 10);
      });

      $effect(() => {
        const query = this.normalizedSearch;
        this.dataRevision;
        this.data.length;

        if (this.#debounceTimer) {
          clearTimeout(this.#debounceTimer);
          this.#debounceTimer = null;
        }

        if (!query) {
          this.#cancelSearchIndexBuild();
          this.visibleGroups = this.data.map((_, index) => ({
            group_index: index,
            ruleIndices: null,
          }));
          this.#clearSearchMatches();
          this.searchPending = false;
          return;
        }

        this.searchPending = true;

        if (this.searchIndexRevision !== this.dataRevision) {
          this.#startSearchIndexBuild();
        }

        if (typeof window === "undefined") {
          this.performSearch();
          return;
        }

        this.#debounceTimer = window.setTimeout(() => this.performSearch(), SEARCH_DEBOUNCE_MS);
      });

      $effect(() => {
        if (this.searchActive) {
          this.renderGroupsLimit = this.data.length;
          if (this.renderGroupsTimeout) {
            clearTimeout(this.renderGroupsTimeout);
            this.renderGroupsTimeout = null;
          }
        } else {
          this.scheduleGroupsNext();
        }
      });

      $effect(() => {
        this.data.length;
        this.scheduleGroupsNext();
      });

      $effect(() => {
        if (this.isAllRendered) {
          this.onRenderComplete?.();
        }
      });

      return () => {
        if (this.#debounceTimer) {
          clearTimeout(this.#debounceTimer);
          this.#debounceTimer = null;
        }

        if (this.#duplicateRuleIdsTimer) {
          clearTimeout(this.#duplicateRuleIdsTimer);
          this.#duplicateRuleIdsTimer = null;
        }

        if (this.#highlightedRuleTimer) {
          clearTimeout(this.#highlightedRuleTimer);
          this.#highlightedRuleTimer = null;
        }

        if (this.renderGroupsTimeout) {
          clearTimeout(this.renderGroupsTimeout);
          this.renderGroupsTimeout = null;
        }
      };
    });
  }

  destroy() {
    if (typeof window !== "undefined") {
      window.removeEventListener("keydown", this.handleSaveShortcut);
    }

    if (this.#dispose) {
      this.#dispose();
      this.#dispose = null;
    }
  }

  mount = async () => {
    this.finishedGroupsCount = 0;
    this.fetchError = false;
    try {
      const fetched =
        (await fetcher.get<{ groups: Group[] }>("/groups?with_rules=true"))?.groups ?? [];
      this.tracker = new ChangeTracker(fetched);
      this.dataRevision = 0;
      if (typeof window !== "undefined") {
        setTimeout(() => this.checkRulesValidityState(), 10);
      }
    } catch (error) {
      this.fetchError = true;
      console.error("Failed to load groups:", error);
    } finally {
      this.refreshDuplicateRuleIds();
      this.dataLoaded = true;
    }

    if (typeof window !== "undefined") {
      window.addEventListener("keydown", this.handleSaveShortcut);
    }
  };

  handleSaveShortcut = (event: KeyboardEvent) => {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") {
      if (this.canSave) {
        event.preventDefault();
        this.saveChanges();
      }
    }
  };

  forceVisibleGroup(groupId: string) {
    if (!this.normalizedSearch) return;
    this.#forcedGroupIds.add(groupId);
    this.#forcedSearchKey = this.normalizedSearch;
  }

  forceVisibleRule(groupId: string, ruleId: string) {
    if (!this.normalizedSearch) return;
    let forced = this.#forcedRuleIdsByGroup.get(groupId);
    if (!forced) {
      forced = new Set<string>();
      this.#forcedRuleIdsByGroup.set(groupId, forced);
    }
    forced.add(ruleId);
    this.#forcedSearchKey = this.normalizedSearch;
  }

  removeForcedGroup(groupId: string) {
    this.#forcedGroupIds.delete(groupId);
    this.#forcedRuleIdsByGroup.delete(groupId);
  }

  removeForcedRule(groupId: string, ruleId: string) {
    const forced = this.#forcedRuleIdsByGroup.get(groupId);
    if (!forced) return;
    forced.delete(ruleId);
    if (!forced.size) this.#forcedRuleIdsByGroup.delete(groupId);
  }

  moveForcedRule(sourceGroupId: string, targetGroupId: string, ruleId: string) {
    if (sourceGroupId === targetGroupId) return;
    const forced = this.#forcedRuleIdsByGroup.get(sourceGroupId);
    if (!forced?.has(ruleId)) return;
    forced.delete(ruleId);
    if (!forced.size) this.#forcedRuleIdsByGroup.delete(sourceGroupId);
    let targetForced = this.#forcedRuleIdsByGroup.get(targetGroupId);
    if (!targetForced) {
      targetForced = new Set<string>();
      this.#forcedRuleIdsByGroup.set(targetGroupId, targetForced);
    }
    targetForced.add(ruleId);
  }

  syncRuleDeletion(groupIndex: number, ruleIndex: number) {
    if (!this.searchActive) return;
    const targetIndex = this.visibleGroups.findIndex((entry) => entry.group_index === groupIndex);
    if (targetIndex === -1) return;
    const target = this.visibleGroups[targetIndex];
    if (!target?.ruleIndices) return;

    const ruleIndices = target.ruleIndices;
    let write = 0;
    for (let i = 0; i < ruleIndices.length; i++) {
      const index = ruleIndices[i];
      if (index === ruleIndex) continue;
      ruleIndices[write] = index > ruleIndex ? index - 1 : index;
      write += 1;
    }

    if (write === 0) {
      this.visibleGroups.splice(targetIndex, 1);
      return;
    }

    ruleIndices.length = write;
    target.ruleIndices = ruleIndices;
    this.visibleGroups[targetIndex] = target;
  }

  #resetForcedVisibility(searchKey: string) {
    if (!this.#forcedSearchKey) return;
    if (this.#forcedSearchKey !== searchKey) {
      this.#forcedGroupIds.clear();
      this.#forcedRuleIdsByGroup.clear();
      this.#forcedSearchKey = "";
    }
  }

  #clearSearchMatches() {
    this.searchMatchedGroupIds = new Set();
    this.searchMatchedRuleIds = new Set();
  }

  #cancelSearchIndexBuild() {
    this.#searchIndexBuildToken += 1;
  }

  #startSearchIndexBuild(incrementToken = true) {
    if (incrementToken) {
      this.#searchIndexBuildToken += 1;
    }

    if (this.#searchIndexBuilding) return;
    void this.#buildSearchIndex(this.#searchIndexBuildToken);
  }

  async #buildSearchIndex(token: number) {
    if (!this.searchActive) return;

    this.#searchIndexBuilding = true;
    const revision = this.dataRevision;
    const groups = this.data;
    const total = groups.length;
    const nextIndex = new Array<SearchIndexGroup>(total);

    const now = () => (typeof performance !== "undefined" ? performance.now() : Date.now());
    let lastYield = now();

    const maybeYield = async () => {
      if (now() - lastYield < 8) return;
      await this.#yieldToMain();
      lastYield = now();
    };

    const abortIfStale = () => token !== this.#searchIndexBuildToken || !this.searchActive;

    for (let i = 0; i < total; i++) {
      if (abortIfStale()) {
        this.#searchIndexBuilding = false;
        if (this.searchActive && token !== this.#searchIndexBuildToken) {
          this.#startSearchIndexBuild(false);
        }
        return;
      }

      const group = groups[i];
      const rules = group.rules;
      const rulesCount = rules.length;
      const indexedRules = new Array<SearchIndexRule>(rulesCount);

      for (let r = 0; r < rulesCount; r++) {
        const rule = rules[r];
        indexedRules[r] = {
          id: rule.id,
          searchBlob: `${rule.name || ""} ${rule.rule || ""}`.toLowerCase(),
        };
        if ((r & 31) === 0) {
          await maybeYield();
          if (abortIfStale()) {
            this.#searchIndexBuilding = false;
            if (this.searchActive && token !== this.#searchIndexBuildToken) {
              this.#startSearchIndexBuild(false);
            }
            return;
          }
        }
      }

      nextIndex[i] = {
        id: group.id,
        nameLower: (group.name || "").toLowerCase(),
        rules: indexedRules,
      };

      if ((i & 7) === 0) {
        await maybeYield();
      }
    }

    if (abortIfStale()) {
      this.#searchIndexBuilding = false;
      if (this.searchActive && token !== this.#searchIndexBuildToken) {
        this.#startSearchIndexBuild(false);
      }
      return;
    }

    if (this.dataRevision !== revision) {
      this.#searchIndexBuilding = false;
      if (this.searchActive) {
        this.#startSearchIndexBuild(false);
      }
      return;
    }

    this.searchIndex = nextIndex;
    this.searchIndexRevision = revision;
    this.#searchIndexBuilding = false;

    if (this.normalizedSearch) {
      this.performSearch();
    } else {
      this.searchPending = false;
    }
  }

  performSearch() {
    const query = this.normalizedSearch;
    this.#resetForcedVisibility(query);

    if (!query) {
      this.visibleGroups = this.data.map((_, index) => ({
        group_index: index,
        ruleIndices: null,
      }));
      this.#clearSearchMatches();
      this.searchPending = false;
      return;
    }

    if (this.searchIndexRevision !== this.dataRevision) {
      this.#clearSearchMatches();
      this.searchPending = true;
      return;
    }

    const nextVisible: VisibleGroup[] = [];
    const matchedGroupIds = new Set<string>();
    const matchedRuleIds = new Set<string>();
    const searchIndex = this.searchIndex;
    const len = searchIndex.length;

    for (let i = 0; i < len; i++) {
      const indexedGroup = searchIndex[i];
      const isForcedGroup = this.#forcedGroupIds.has(indexedGroup.id);
      const isGroupMatchedByName = indexedGroup.nameLower.includes(query);

      if (isForcedGroup || isGroupMatchedByName) {
        if (isGroupMatchedByName) {
          matchedGroupIds.add(indexedGroup.id);
        }
        nextVisible.push({ group_index: i, ruleIndices: null });
        continue;
      }

      const matchedRuleIndices: number[] = [];
      const forcedRules = this.#forcedRuleIdsByGroup.get(indexedGroup.id);
      const rules = indexedGroup.rules;
      const rulesLen = rules.length;

      for (let r = 0; r < rulesLen; r++) {
        const indexedRule = rules[r];
        const isMatchedBySearch = indexedRule.searchBlob.includes(query);
        if (forcedRules?.has(indexedRule.id) || isMatchedBySearch) {
          matchedRuleIndices.push(r);
          if (isMatchedBySearch) {
            matchedRuleIds.add(indexedRule.id);
          }
        }
      }

      if (matchedRuleIndices.length > 0) {
        nextVisible.push({ group_index: i, ruleIndices: matchedRuleIndices });
      }
    }

    this.visibleGroups = nextVisible;
    this.searchMatchedGroupIds = matchedGroupIds;
    this.searchMatchedRuleIds = matchedRuleIds;
    this.searchPending = false;
  }

  saveChanges() {
    if (!this.tracker.isDirty) return;
    overlay.show(t("saving changes..."));

    const rawData = $state.snapshot(this.data);

    fetcher
      .put("/groups?save=true", { groups: rawData })
      .then(() => {
        this.tracker.reset(rawData);
        overlay.hide();
        toast.success(t("Saved"));
      })
      .catch(() => {
        overlay.hide();
      });
  }

  checkRulesValidityState = () => {
    if (typeof document === "undefined") return;
    this.valid_rules = !document.querySelector(".rule input.invalid");
  };

  #sortDuplicateConflicts(conflicts: GroupDuplicateConflict[]) {
    conflicts.sort((a, b) => {
      const byKey = a.duplicateKey.localeCompare(b.duplicateKey);
      if (byKey !== 0) return byKey;

      if (a.inCurrentGroup !== b.inCurrentGroup) {
        return a.inCurrentGroup ? 1 : -1;
      }
      const byGroup = (a.groupName || "").localeCompare(b.groupName || "");
      if (byGroup !== 0) return byGroup;
      const byName = (a.ruleName || "").localeCompare(b.ruleName || "");
      if (byName !== 0) return byName;
      const byPattern = (a.rulePattern || "").localeCompare(b.rulePattern || "");
      if (byPattern !== 0) return byPattern;
      return a.ruleId.localeCompare(b.ruleId);
    });
  }

  #computeDuplicates(): DuplicateComputationResult {
    const duplicateRules = new Set<string>();
    const duplicateGroups = new Set<string>();
    const duplicateRuleKeys = new Map<string, string>();
    const rulesByKey = new Map<
      string,
      { rule: Rule; groupId: string; groupName: string; groupColor: string }[]
    >();

    for (const group of this.data) {
      for (const rule of group.rules) {
        const normalizedPattern = rule.rule.trim();
        if (!normalizedPattern) continue;

        const key = `${rule.type}:${rule.rule}`;
        duplicateRuleKeys.set(rule.id, key);
        let rules = rulesByKey.get(key);
        if (!rules) {
          rules = [];
          rulesByKey.set(key, rules);
        }
        rules.push({
          rule,
          groupId: group.id,
          groupName: group.name,
          groupColor: group.color || "#ffffff",
        });
      }
    }

    const conflictsByKey = new Map<string, GroupDuplicateConflict[]>();
    const duplicateKeysByGroup = new Map<string, Set<string>>();

    for (const [key, rules] of rulesByKey) {
      if (rules.length < 2) continue;

      for (const entry of rules) {
        duplicateRules.add(entry.rule.id);
        duplicateGroups.add(entry.groupId);
        let keys = duplicateKeysByGroup.get(entry.groupId);
        if (!keys) {
          keys = new Set<string>();
          duplicateKeysByGroup.set(entry.groupId, keys);
        }
        keys.add(key);
      }

      conflictsByKey.set(
        key,
        rules.map((entry) => ({
          ruleId: entry.rule.id,
          ruleName: entry.rule.name,
          rulePattern: entry.rule.rule,
          ruleType: entry.rule.type,
          groupId: entry.groupId,
          groupName: entry.groupName,
          groupColor: entry.groupColor,
          inCurrentGroup: false,
          duplicateKey: key,
          totalRulesWithSameKey: rules.length,
        })),
      );
    }

    const duplicateConflictsByGroup = new Map<string, GroupDuplicateConflict[]>();

    for (const [groupId, keys] of duplicateKeysByGroup) {
      const conflicts: GroupDuplicateConflict[] = [];
      for (const key of keys) {
        const sameKeyConflicts = conflictsByKey.get(key);
        if (!sameKeyConflicts) continue;
        for (const conflict of sameKeyConflicts) {
          conflicts.push({
            ...conflict,
            inCurrentGroup: conflict.groupId === groupId,
          });
        }
      }
      this.#sortDuplicateConflicts(conflicts);
      duplicateConflictsByGroup.set(groupId, conflicts);
    }

    return { duplicateRules, duplicateGroups, duplicateRuleKeys, duplicateConflictsByGroup };
  }

  #applyDuplicateComputation(result: DuplicateComputationResult) {
    this.duplicateRuleIds = result.duplicateRules;
    this.duplicateGroupIds = result.duplicateGroups;
    this.duplicateRuleKeys = result.duplicateRuleKeys;
    this.duplicateConflictsByGroup = result.duplicateConflictsByGroup;
    this.#syncActiveDuplicateKey();
  }

  #syncActiveDuplicateKey() {
    if (this.highlightedRuleId && !this.duplicateRuleIds.has(this.highlightedRuleId)) {
      this.highlightedRuleId = null;
    }

    if (!this.activeDuplicateKey) return;

    for (const ruleId of this.duplicateRuleIds) {
      if (this.duplicateRuleKeys.get(ruleId) === this.activeDuplicateKey) {
        return;
      }
    }

    this.activeDuplicateKey = null;
    this.duplicateHighlightPinned = false;
  }

  #resolveDuplicateKey(ruleId: string) {
    const key = this.duplicateRuleKeys.get(ruleId);
    if (!key || !this.duplicateRuleIds.has(ruleId)) {
      return null;
    }
    return key;
  }

  refreshDuplicateRuleIds = () => {
    if (this.#duplicateRuleIdsTimer) {
      clearTimeout(this.#duplicateRuleIdsTimer);
      this.#duplicateRuleIdsTimer = null;
    }
    this.#applyDuplicateComputation(this.#computeDuplicates());
  };

  scheduleDuplicateRuleIdsRefresh = (delayMs = 120) => {
    if (typeof window === "undefined") {
      this.refreshDuplicateRuleIds();
      return;
    }

    if (this.#duplicateRuleIdsTimer) {
      clearTimeout(this.#duplicateRuleIdsTimer);
    }

    this.#duplicateRuleIdsTimer = window.setTimeout(() => {
      this.#duplicateRuleIdsTimer = null;
      this.#applyDuplicateComputation(this.#computeDuplicates());
    }, delayMs);
  };

  isRuleDuplicate = (ruleId: string) => this.duplicateRuleIds.has(ruleId);
  isRuleSearchMatched = (ruleId: string) => this.searchMatchedRuleIds.has(ruleId);
  isGroupSearchMatched = (groupId: string) => this.searchMatchedGroupIds.has(groupId);

  pinDuplicateByRuleId = (ruleId: string) => {
    const key = this.#resolveDuplicateKey(ruleId);
    if (!key) {
      this.activeDuplicateKey = null;
      this.duplicateHighlightPinned = false;
      return false;
    }
    this.activeDuplicateKey = key;
    this.duplicateHighlightPinned = true;
    return true;
  };

  setActiveDuplicateByRuleId = (ruleId: string) => {
    if (this.duplicateHighlightPinned) return;
    const key = this.#resolveDuplicateKey(ruleId);
    if (!key) {
      this.activeDuplicateKey = null;
      return;
    }
    this.activeDuplicateKey = key;
  };

  clearActiveDuplicateByRuleId = (ruleId: string) => {
    if (this.duplicateHighlightPinned || !this.activeDuplicateKey) return;
    const key = this.#resolveDuplicateKey(ruleId);
    if (!key || key === this.activeDuplicateKey) {
      this.activeDuplicateKey = null;
    }
  };

  togglePinnedDuplicateByRuleId = (ruleId: string) => {
    const key = this.#resolveDuplicateKey(ruleId);
    if (!key) {
      this.activeDuplicateKey = null;
      this.duplicateHighlightPinned = false;
      return;
    }
    if (this.duplicateHighlightPinned && this.activeDuplicateKey === key) {
      this.activeDuplicateKey = null;
      this.duplicateHighlightPinned = false;
      return;
    }
    this.pinDuplicateByRuleId(ruleId);
  };

  clearPinnedDuplicateHighlight = () => {
    if (!this.duplicateHighlightPinned) return;
    this.activeDuplicateKey = null;
    this.duplicateHighlightPinned = false;
  };

  highlightRuleTemporarily = (ruleId: string, durationMs = 2200) => {
    if (!this.duplicateRuleIds.has(ruleId)) return false;

    if (this.#highlightedRuleTimer) {
      clearTimeout(this.#highlightedRuleTimer);
      this.#highlightedRuleTimer = null;
    }

    const applyHighlight = () => {
      this.highlightedRuleId = ruleId;

      if (typeof window === "undefined") return;

      this.#highlightedRuleTimer = window.setTimeout(() => {
        this.#highlightedRuleTimer = null;
        if (this.highlightedRuleId === ruleId) {
          this.highlightedRuleId = null;
        }
      }, durationMs);
    };

    if (this.highlightedRuleId === ruleId) {
      this.highlightedRuleId = null;
      if (typeof window !== "undefined") {
        requestAnimationFrame(() => applyHighlight());
      } else {
        applyHighlight();
      }
      return true;
    }

    applyHighlight();

    return true;
  };

  isRuleHighlighted = (ruleId: string) => this.highlightedRuleId === ruleId;

  isRuleHighlightPinned = (ruleId: string) => this.isRuleHighlighted(ruleId);

  requestDuplicateRuleFocus = (groupId: string, ruleId: string) => {
    const groupIndex = this.data.findIndex((group) => group.id === groupId);
    if (groupIndex < 0) return false;

    if (this.searchActive) {
      this.forceVisibleRule(groupId, ruleId);
      this.performSearch();
    }

    this.renderGroupsLimit = Math.max(this.renderGroupsLimit, groupIndex + 1);
    this.open_state[groupId] = true;

    const nonce = (this.duplicateFocusRequest?.nonce ?? 0) + 1;
    this.duplicateFocusRequest = { groupId, ruleId, nonce };
    return true;
  };

  consumeDuplicateRuleFocus = (groupId: string, ruleId: string, nonce: number) => {
    const request = this.duplicateFocusRequest;
    if (!request) return;
    if (request.groupId !== groupId || request.ruleId !== ruleId || request.nonce !== nonce) return;
    this.duplicateFocusRequest = null;
  };

  getDuplicateConflictsForGroup = (groupId: string): GroupDuplicateConflict[] =>
    this.duplicateConflictsByGroup.get(groupId) ?? [];

  markDataRevision = () => {
    this.dataRevision += 1;
    this.scheduleDuplicateRuleIdsRefresh();
  };

  async addRuleToGroup(group_index: number, rule: Rule, focus = false) {
    const group = this.data[group_index];
    if (!group) return;
    group.rules.unshift(rule);
    this.markDataRevision();
    if (!rule.rule || !rule.name) {
      this.valid_rules = false;
    }
    if (this.searchActive) {
      this.forceVisibleRule(group.id, rule.id);
    }
    if (!focus) return;
    await tick();
    const el = document.querySelector(`.rule[data-group-index="${group_index}"][data-index="0"]`);
    if (el) {
      requestAnimationFrame(() => {
        el.querySelector<HTMLInputElement>("div.name input")?.focus();
        el
          .querySelector<HTMLInputElement>("div.pattern input")
          ?.classList.add("invalid");
        this.checkRulesValidityState();
      });
    }
  }

  deleteRuleFromGroup = (group_index: number, rule_index: number) => {
    const group = this.data[group_index];
    if (!group) return;
    const removed = group.rules[rule_index];
    group.rules.splice(rule_index, 1);
    if (removed) {
      this.removeForcedRule(group.id, removed.id);
    }
    this.syncRuleDeletion(group_index, rule_index);
    this.markDataRevision();
  };

  changeRuleIndex(
    from_group_index: number,
    from_rule_index: number,
    to_group_index: number,
    to_rule_index: number,
    to_rule_id?: string,
    insert: "before" | "after" = "before",
  ) {
    const sourceGroup = this.data[from_group_index];
    const targetGroup = this.data[to_group_index];

    if (!sourceGroup || !targetGroup) return;

    const isSameGroup = from_group_index === to_group_index;

    const sourceRules = sourceGroup.rules;
    const targetRules = targetGroup.rules;

    if (!sourceRules.length) return;

    const fromIndex = clamp(from_rule_index, 0, sourceRules.length - 1);
    const [movedRule] = sourceRules.splice(fromIndex, 1);
    if (!movedRule) return;

    if (!isSameGroup) {
      this.moveForcedRule(sourceGroup.id, targetGroup.id, movedRule.id);
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

    this.markDataRevision();
  }

  changeGroupIndex(
    from_index: number,
    to_index: number,
    insert: "before" | "after" = "before",
  ) {
    if (from_index === to_index && insert !== "after") return;

    if (from_index < 0 || from_index >= this.data.length) return;

    const group = this.data[from_index];
    if (!group) return;

    this.data.splice(from_index, 1);

    let target = insert === "after" ? to_index + 1 : to_index;

    if (from_index < target) target -= 1;

    if (target < 0) target = 0;
    if (target > this.data.length) target = this.data.length;

    this.data.splice(target, 0, group);
    this.markDataRevision();
  }

  handleGroupSlotDrop = (source: GroupDragData, target: GroupDropSlotData) => {
    const { group_index: from_index } = source;
    const { group_index: to_index, insert } = target;
    if (from_index === to_index && insert !== "after") return;
    this.changeGroupIndex(from_index, to_index, insert);
  };

  async addGroup() {
    const group = defaultGroup();
    this.data.unshift(group);
    this.open_state[group.id] = true;
    this.markDataRevision();
    if (this.searchActive) {
      this.forceVisibleGroup(group.id);
    }
    await this.addRuleToGroup(0, defaultRule(), false);
    await tick();
    const el = document.querySelector(`.group-header[data-group-index="0"]`);
    el?.querySelector<HTMLInputElement>("input.group-name")?.focus();
  }

  deleteGroup = (index: number) => {
    if (!confirm(t("Delete this group?"))) return;
    const removed = this.data[index];
    this.data.splice(index, 1);
    if (removed) {
      this.removeForcedGroup(removed.id);
      delete this.open_state[removed.id];
    }
    this.markDataRevision();
  };

  cloneGroupWithNewIds(group: Group): Group {
    return cloneGroupWithNewIdsData(group);
  }

  sortGroupRules(groupIndex: number, field: SortField, direction: SortDirection) {
    const group = this.data[groupIndex];
    if (!group) return false;
    sortGroupRulesData(group, field, direction);
    this.markDataRevision();
    return true;
  }

  restoreGroupRulesOrder(groupIndex: number, ruleIds: string[]) {
    const group = this.data[groupIndex];
    if (!group) return false;

    const restored = restoreGroupRulesOrderData(group, ruleIds);
    if (!restored) return false;
    this.markDataRevision();
    return true;
  }

  toConfigPayload() {
    return toConfigPayloadData($state.snapshot(this.data));
  }

  async cloneGroupsWithNewIds(groups: Group[]) {
    return cloneGroupsWithNewIdsData(
      groups,
      () => this.#yieldToMain(),
      IMPORT_GROUPS_CLONE_CHUNK_SIZE,
    );
  }

  async addGroups(groups: Group[]) {
    if (!groups.length) return;
    await prependGroupsData(
      this.data,
      this.open_state,
      groups,
      () => this.#yieldToMain(),
      IMPORT_GROUPS_INSERT_CHUNK_SIZE,
    );
    this.markDataRevision();
  }

  async overwriteGroups(groups: Group[]) {
    this.finishedGroupsCount = 0;
    this.renderGroupsLimit = 1;
    this.data.splice(0, this.data.length);
    this.open_state = {};
    await this.addGroups(groups);
    if (!groups.length) {
      this.markDataRevision();
    }
  }

  async addRulesToGroup(groupIndex: number, rules: Rule[]) {
    const group = this.data[groupIndex];
    if (!group || !rules.length) return;

    await prependRulesData(group, rules, () => this.#yieldToMain(), IMPORT_RULES_CHUNK_SIZE);
    this.markDataRevision();
  }

  async #yieldToMain() {
    if (typeof window === "undefined") return;
    await new Promise<void>((resolve) => {
      window.setTimeout(resolve, 0);
    });
  }

  handleGroupFinished = () => {
    this.finishedGroupsCount += 1;
  };

  scheduleGroupsNext() {
    if (typeof window === "undefined") return;
    if (this.renderGroupsTimeout) return;
    if (this.renderGroupsLimit >= this.data.length) return;

    this.renderGroupsTimeout = window.setTimeout(() => {
      this.renderGroupsTimeout = null;
      if (this.searchActive) {
        this.renderGroupsLimit = this.data.length;
        return;
      }
      this.renderGroupsLimit = Math.min(this.renderGroupsLimit + 2, this.data.length);
      this.scheduleGroupsNext();
    }, 15);
  }
}
