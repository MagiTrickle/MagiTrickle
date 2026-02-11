import { t } from "../../data/locale.svelte";
import { ChangeTracker } from "../../utils/change-tracker.svelte";

import { type Subscription, type SubscriptionRule } from "../../types";
import { randomId } from "../../utils/defaults";
import { overlay, toast } from "../../utils/events";
import { fetcher } from "../../utils/fetcher";

export const SUBSCRIPTIONS_STORE_CONTEXT = Symbol("subscriptions-store");

const SEARCH_DEBOUNCE_MS = 150 as const;

const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));

export type VisibleSubscription = {
  group_index: number;
  ruleIndices: number[] | null;
};

export type SubscriptionDragData = {
  group_id: string;
  group_index: number;
  name: string;
  count: number;
};

export type SubscriptionDropSlotData = {
  group_index: number;
  insert: "before" | "after";
};

type AddSubscriptionPayload = {
  url: string;
  name: string;
  rules: SubscriptionRule[];
  interface: string;
  interval: number;
};

type SubscriptionsStoreOptions = {
  onRenderComplete?: () => void;
};

export class SubscriptionsStore {
  onRenderComplete?: () => void;

  tracker = $state(new ChangeTracker<Subscription[]>([]));
  data = $derived.by(() => this.tracker.data);
  dataRevision = $state(0);
  valid_rules = $state(true);
  canSave = $derived(this.tracker.isDirty && this.valid_rules);

  open_state = $state<Record<string, boolean>>({});

  searchValue = $state("");
  visibleSubscriptions = $state<VisibleSubscription[]>([]);
  searchPending = $state(false);

  normalizedSearch = $derived(this.searchValue.trim().toLowerCase());
  searchActive = $derived(Boolean(this.normalizedSearch));

  visibilityMap = $derived(
    new Map(this.visibleSubscriptions.map((entry) => [entry.group_index, entry.ruleIndices])),
  );

  firstVisibleSubscriptionIndex = $derived(
    this.searchActive
      ? this.visibleSubscriptions.length
        ? this.visibleSubscriptions[0].group_index
        : -1
      : 0,
  );

  noVisibleSubscriptions = $derived(
    this.searchActive && !this.searchPending && this.visibleSubscriptions.length === 0,
  );

  fetchError = $state(false);
  dataLoaded = $state(false);
  finishedSubscriptionsCount = $state(0);

  isAllRendered = $derived(
    this.dataLoaded &&
      (this.data.length === 0 ||
        this.finishedSubscriptionsCount >= this.data.length ||
        this.searchActive),
  );

  isEmptyData = $derived(
    this.dataLoaded &&
      !this.fetchError &&
      !this.searchActive &&
      !this.searchPending &&
      this.data.length === 0,
  );

  renderSubscriptionsLimit = $state(1);
  renderSubscriptionsTimeout: number | null = null;

  #forcedSubscriptionIds = new Set<string>();
  #forcedRuleIdsBySubscription = new Map<string, Set<string>>();
  #forcedSearchKey = "";
  #debounceTimer: number | null = null;
  #dispose: (() => void) | null = null;

  constructor(options: SubscriptionsStoreOptions = {}) {
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
          this.visibleSubscriptions = this.data.map((_, index) => ({
            group_index: index,
            ruleIndices: null,
          }));
          this.searchPending = false;
          return;
        }

        this.searchPending = true;

        if (typeof window === "undefined") {
          this.performSearch();
          return;
        }

        this.#debounceTimer = window.setTimeout(() => this.performSearch(), SEARCH_DEBOUNCE_MS);
      });

      $effect(() => {
        if (this.searchActive) {
          this.renderSubscriptionsLimit = this.data.length;
          if (this.renderSubscriptionsTimeout) {
            clearTimeout(this.renderSubscriptionsTimeout);
            this.renderSubscriptionsTimeout = null;
          }
        } else {
          this.scheduleSubscriptionsNext();
        }
      });

      $effect(() => {
        this.data.length;
        this.scheduleSubscriptionsNext();
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

        if (this.renderSubscriptionsTimeout) {
          clearTimeout(this.renderSubscriptionsTimeout);
          this.renderSubscriptionsTimeout = null;
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
    this.finishedSubscriptionsCount = 0;
    this.fetchError = false;
    try {
      const fetched =
        (await fetcher.get<{ subscriptions: Subscription[] }>("/subscriptions"))?.subscriptions ??
        [];
      this.tracker = new ChangeTracker(fetched);
      this.dataRevision = 0;
      if (typeof window !== "undefined") {
        setTimeout(() => this.checkRulesValidityState(), 10);
      }
    } catch (error) {
      this.fetchError = true;
      console.error("Failed to load subscriptions:", error);
    } finally {
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

  #resetForcedVisibility(searchKey: string) {
    if (!this.#forcedSearchKey) return;
    if (this.#forcedSearchKey !== searchKey) {
      this.#forcedSubscriptionIds.clear();
      this.#forcedRuleIdsBySubscription.clear();
      this.#forcedSearchKey = "";
    }
  }

  forceVisibleGroup(groupId: string) {
    if (!this.normalizedSearch) return;
    this.#forcedSubscriptionIds.add(groupId);
    this.#forcedSearchKey = this.normalizedSearch;
  }

  forceVisibleRule(groupId: string, ruleId: string) {
    if (!this.normalizedSearch) return;
    let forced = this.#forcedRuleIdsBySubscription.get(groupId);
    if (!forced) {
      forced = new Set<string>();
      this.#forcedRuleIdsBySubscription.set(groupId, forced);
    }
    forced.add(ruleId);
    this.#forcedSearchKey = this.normalizedSearch;
  }

  removeForcedGroup(groupId: string) {
    this.#forcedSubscriptionIds.delete(groupId);
    this.#forcedRuleIdsBySubscription.delete(groupId);
  }

  removeForcedRule(groupId: string, ruleId: string) {
    const forced = this.#forcedRuleIdsBySubscription.get(groupId);
    if (!forced) return;
    forced.delete(ruleId);
    if (!forced.size) this.#forcedRuleIdsBySubscription.delete(groupId);
  }

  moveForcedRule(sourceGroupId: string, targetGroupId: string, ruleId: string) {
    if (sourceGroupId === targetGroupId) return;
    const forced = this.#forcedRuleIdsBySubscription.get(sourceGroupId);
    if (!forced?.has(ruleId)) return;
    forced.delete(ruleId);
    if (!forced.size) this.#forcedRuleIdsBySubscription.delete(sourceGroupId);
    let targetForced = this.#forcedRuleIdsBySubscription.get(targetGroupId);
    if (!targetForced) {
      targetForced = new Set<string>();
      this.#forcedRuleIdsBySubscription.set(targetGroupId, targetForced);
    }
    targetForced.add(ruleId);
  }

  performSearch() {
    const query = this.normalizedSearch;
    this.#resetForcedVisibility(query);

    if (!query) {
      this.visibleSubscriptions = this.data.map((_, index) => ({
        group_index: index,
        ruleIndices: null,
      }));
      this.searchPending = false;
      return;
    }

    const subscriptions = this.data;
    const nextVisible: VisibleSubscription[] = [];

    for (let i = 0; i < subscriptions.length; i++) {
      const subscription = subscriptions[i];
      const isForcedSubscription = this.#forcedSubscriptionIds.has(subscription.id);

      const nameMatches = (subscription.name || "").toLowerCase().includes(query);
      const urlMatches = (subscription.url || "").toLowerCase().includes(query);

      if (isForcedSubscription || nameMatches || urlMatches) {
        nextVisible.push({ group_index: i, ruleIndices: null });
        continue;
      }

      const matchedRuleIndices: number[] = [];
      const forcedRules = this.#forcedRuleIdsBySubscription.get(subscription.id);
      const rules = subscription.rules;

      for (let ruleIndex = 0; ruleIndex < rules.length; ruleIndex++) {
        const rule = rules[ruleIndex];
        const isForcedRule = forcedRules?.has(rule.id);
        const patternMatches = (rule.rule || "").toLowerCase().includes(query);
        if (isForcedRule || patternMatches) {
          matchedRuleIndices.push(ruleIndex);
        }
      }

      if (matchedRuleIndices.length > 0) {
        nextVisible.push({ group_index: i, ruleIndices: matchedRuleIndices });
      }
    }

    this.visibleSubscriptions = nextVisible;
    this.searchPending = false;
  }

  saveChanges() {
    if (!this.tracker.isDirty) return;
    overlay.show(t("saving changes..."));

    const rawData = $state.snapshot(this.data);

    fetcher
      .put("/subscriptions", { subscriptions: rawData })
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
    this.valid_rules = !document.querySelector(".subscription-rule .invalid");
  };

  markDataRevision = () => {
    this.dataRevision += 1;
  };

  async syncSubscription(index: number) {
    const subscription = this.data[index];
    if (!subscription) return;

    overlay.show(t("syncing..."));
    try {
      const updated = await fetcher.patch<{ rules: SubscriptionRule[]; last_update: number }>(
        `/subscription?id=${subscription.id}`,
        {},
      );

      subscription.rules = updated.rules;
      subscription.last_update = updated.last_update;
      this.tracker.acknowledgeUpdate(subscription);

      this.markDataRevision();
      toast.success(t("Synced"));
    } catch (error) {
      console.error(error);
      toast.error(t("Failed to sync"));
    } finally {
      overlay.hide();
    }
  }

  async deleteSubscription(index: number) {
    if (!confirm(t("Delete this subscription?"))) return;

    const removed = this.data[index];
    if (!removed) return;

    overlay.show(t("deleting subscription..."));
    try {
      await fetcher.delete(`/subscription?id=${removed.id}`);
      this.data.splice(index, 1);
      this.tracker.acknowledgeDelete(this.data, removed.id);
      delete this.open_state[removed.id];
      this.removeForcedGroup(removed.id);
      this.markDataRevision();
      toast.success(t("Deleted"));
    } catch (error) {
      console.error(error);
      toast.error(t("Failed to delete subscription"));
    } finally {
      overlay.hide();
    }
  }

  async addSubscription(payload: AddSubscriptionPayload) {
    const nextSubscription: Subscription = {
      id: randomId(),
      name: payload.name,
      url: payload.url,
      rules: payload.rules,
      enable: true,
      interface: payload.interface || "",
      last_update: Date.now(),
      interval: payload.interval,
    };

    overlay.show(t("Adding..."));
    try {
      await fetcher.post("/subscription", nextSubscription);
      this.data.unshift(nextSubscription);
      this.tracker.acknowledgeNewItem(this.data, nextSubscription, "start");
      this.open_state[nextSubscription.id] = true;
      if (this.searchActive) {
        this.forceVisibleGroup(nextSubscription.id);
      }
      this.markDataRevision();
      toast.success(t("Added"));
    } catch (error) {
      console.error(error);
      toast.error(t("Failed to add subscription"));
    } finally {
      overlay.hide();
    }
  }

  changeSubscriptionIndex(
    from_index: number,
    to_index: number,
    insert: "before" | "after" = "before",
  ) {
    if (from_index === to_index && insert !== "after") return;
    if (from_index < 0 || from_index >= this.data.length) return;

    const subscription = this.data[from_index];
    if (!subscription) return;

    this.data.splice(from_index, 1);

    let target = insert === "after" ? to_index + 1 : to_index;
    if (from_index < target) target -= 1;

    target = clamp(target, 0, this.data.length);
    this.data.splice(target, 0, subscription);

    this.markDataRevision();
  }

  handleSubscriptionSlotDrop = (source: SubscriptionDragData, target: SubscriptionDropSlotData) => {
    const { group_index: from_index } = source;
    const { group_index: to_index, insert } = target;
    this.changeSubscriptionIndex(from_index, to_index, insert);
  };

  handleSubscriptionFinished = () => {
    this.finishedSubscriptionsCount += 1;
  };

  scheduleSubscriptionsNext() {
    if (typeof window === "undefined") return;
    if (this.renderSubscriptionsTimeout) return;
    if (this.renderSubscriptionsLimit >= this.data.length) return;

    this.renderSubscriptionsTimeout = window.setTimeout(() => {
      this.renderSubscriptionsTimeout = null;
      if (this.searchActive) {
        this.renderSubscriptionsLimit = this.data.length;
        return;
      }
      this.renderSubscriptionsLimit = Math.min(this.renderSubscriptionsLimit + 2, this.data.length);
      this.scheduleSubscriptionsNext();
    }, 15);
  }
}
