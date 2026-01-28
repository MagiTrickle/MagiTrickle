<script lang="ts">
  import { onDestroy, onMount } from "svelte";

  import Button from "../../components/ui/Button.svelte";
  import Placeholder from "../../components/ui/Placeholder.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { t } from "../../data/locale.svelte";
  import { smoothReflow } from "../../lib/smooth-reflow.svelte";
  import { ChangeTracker } from "../../utils/change-tracker.svelte";
  import SubscriptionPanel from "./components/SubscriptionPanel.svelte";
  import SubscriptionSearch from "./components/SubscriptionSearch.svelte";
  import AddSubscriptionDialog from "./dialogs/AddSubscriptionDialog.svelte";

  import { Add, Save } from "../../components/ui/icons";
  import { droppable } from "../../lib/dnd";
  import type { Subscription, SubscriptionRule } from "../../types";
  import { randomId } from "../../utils/defaults";
  import { overlay, toast } from "../../utils/events";
  import { fetcher } from "../../utils/fetcher";

  type Props = {
    onRenderComplete?: () => void;
  };

  let { onRenderComplete }: Props = $props();

  let tracker = $state(new ChangeTracker<Subscription[]>([]));
  let data = $derived(tracker.data);
  let dataRevision = $state(0);
  let valid_rules = $state(true);
  let canSave = $derived(tracker.isDirty && valid_rules);
  let open_state = $state<Record<string, boolean>>({});

  let addSubscriptionModal = $state(false);

  let fetchError = $state(false);
  let dataLoaded = $state(false);
  let finishedSubscriptionsCount = $state(0);

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

  let searchControls = $state<SearchControls | null>(null);
  let visibleSubscriptions: VisibleSubscription[] = $state([]);
  let searchActive = $state(false);
  let searchPending = $state(false);

  let noVisibleSubscriptions = $derived(
    searchActive && !searchPending && visibleSubscriptions.length === 0,
  );

  let visibilityMap = $derived(
    new Map(visibleSubscriptions.map((v) => [v.group_index, v.ruleIndices])),
  );

  let firstVisibleSubscriptionIndex = $derived(
    searchActive ? (visibleSubscriptions.length ? visibleSubscriptions[0].group_index : -1) : 0,
  );

  let isAllRendered = $derived(
    dataLoaded && (data.length === 0 || finishedSubscriptionsCount >= data.length || searchActive),
  );
  let isEmptyData = $derived(
    dataLoaded && !fetchError && !searchActive && !searchPending && data.length === 0,
  );

  $effect(() => {
    if (isAllRendered) {
      onRenderComplete?.();
    }
  });

  let renderSubscriptionsLimit = $state(1);
  let renderSubscriptionsTimeout: number | null = null;
  function scheduleSubscriptionsNext() {
    if (typeof window === "undefined") return;
    if (renderSubscriptionsTimeout) return;
    if (renderSubscriptionsLimit >= data.length) return;

    renderSubscriptionsTimeout = window.setTimeout(() => {
      renderSubscriptionsTimeout = null;
      if (searchActive) {
        renderSubscriptionsLimit = data.length;
        return;
      }
      renderSubscriptionsLimit = Math.min(renderSubscriptionsLimit + 2, data.length);
      scheduleSubscriptionsNext();
    }, 15);
  }

  $effect(() => {
    if (searchActive) {
      renderSubscriptionsLimit = data.length;
      if (renderSubscriptionsTimeout) {
        clearTimeout(renderSubscriptionsTimeout);
        renderSubscriptionsTimeout = null;
      }
    } else {
      scheduleSubscriptionsNext();
    }
  });

  $effect(() => {
    data.length;
    scheduleSubscriptionsNext();
  });

  function bumpDataRevision() {
    dataRevision += 1;
  }

  function checkRulesValidityState() {
    valid_rules = !document.querySelector(".subscription-rule .invalid");
  }

  $effect(() => {
    $state.snapshot(data);
    setTimeout(checkRulesValidityState, 10);
  });

  $effect(() => {
    if (typeof window === "undefined" || !canSave) return;

    const handleBeforeUnload = (event: BeforeUnloadEvent) => {
      event.preventDefault();
    };

    window.addEventListener("beforeunload", handleBeforeUnload);
    return () => window.removeEventListener("beforeunload", handleBeforeUnload);
  });

  onMount(async () => {
    finishedSubscriptionsCount = 0;
    fetchError = false;
    try {
      const fetched =
        (await fetcher.get<{ subscriptions: Subscription[] }>("/subscriptions"))?.subscriptions ??
        [];
      tracker = new ChangeTracker(fetched);
      dataRevision = 0;
      setTimeout(checkRulesValidityState, 10);
    } catch (error) {
      fetchError = true;
      console.error("Failed to load subscriptions:", error);
    } finally {
      dataLoaded = true;
    }
    if (typeof window !== "undefined") {
      window.addEventListener("keydown", handleSaveShortcut);
    }
  });

  onDestroy(() => {
    if (typeof window !== "undefined") {
      window.removeEventListener("keydown", handleSaveShortcut);
    }
  });

  function handleSaveShortcut(event: KeyboardEvent) {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") {
      if (canSave) {
        event.preventDefault();
        saveChanges();
      }
    }
  }

  function saveChanges() {
    if (!tracker.isDirty) return;
    overlay.show(t("saving changes..."));

    const rawData = $state.snapshot(data);

    fetcher
      .put("/subscriptions", { subscriptions: rawData })
      .then(() => {
        tracker.reset(rawData);
        overlay.hide();
        toast.success(t("Saved"));
      })
      .catch(() => {
        overlay.hide();
      });
  }

  async function syncSubscription(index: number) {
    const sub = data[index];
    if (!sub) return;
    overlay.show(t("syncing..."));
    try {
      // Send PATCH to trigger sync on backend and get updated data
      const updatedSub = await fetcher.patch<{ rules: SubscriptionRule[]; last_update: number }>(
        `/subscription?id=${sub.id}`,
        {},
      );
      // Update local state with response
      sub.rules = updatedSub.rules;
      sub.last_update = updatedSub.last_update;

      // Acknowledge update in tracker so it's not marked as dirty
      tracker.acknowledgeUpdate(sub);

      toast.success(t("Synced"));
      bumpDataRevision();
    } catch (e) {
      console.error(e);
      toast.error(t("Failed to sync"));
    } finally {
      overlay.hide();
    }
  }

  async function deleteSubscription(index: number) {
    if (!confirm(t("Delete this subscription?"))) return;
    const removed = data[index];
    if (!removed) return;

    overlay.show(t("deleting subscription..."));
    try {
      await fetcher.delete(`/subscription?id=${removed.id}`);
      data.splice(index, 1);
      tracker.acknowledgeDelete(data, removed.id);

      if (removed) {
        delete open_state[removed.id];
        searchControls?.removeForcedGroup(removed.id);
      }
      bumpDataRevision();
      toast.success(t("Deleted"));
    } catch (e) {
      console.error(e);
      toast.error(t("Failed to delete subscription"));
    } finally {
      overlay.hide();
    }
  }

  async function handleAdd(e: CustomEvent) {
    const { url, name, rules, interface: iface, interval } = e.detail;
    const newSub: Subscription = {
      id: randomId(),
      name,
      url,
      rules,
      enable: true,
      interface: iface || "",
      last_update: Date.now(),
      interval,
    };

    overlay.show(t("Adding..."));
    try {
      await fetcher.post("/subscription", newSub);
      data.unshift(newSub);
      tracker.acknowledgeNewItem(data, newSub, "start");

      open_state[newSub.id] = true;
      if (searchActive) {
        searchControls?.forceVisibleGroup(newSub.id);
      }
      bumpDataRevision();
      toast.success(t("Added"));
    } catch (e) {
      console.error(e);
      toast.error(t("Failed to add subscription"));
    } finally {
      overlay.hide();
    }
  }

  function deleteRuleFromSubscription(sub_index: number, rule_index: number) {
    const sub = data[sub_index];
    if (!sub) return;
    const removed = sub.rules[rule_index];
    sub.rules.splice(rule_index, 1);
    if (removed) {
      searchControls?.removeForcedRule(sub.id, removed.id);
    }
    bumpDataRevision();
  }

  // DnD Logic
  type SubscriptionDragData = {
    group_id: string;
    group_index: number;
    name: string;
    count: number;
  };
  type SubscriptionDropSlotData = {
    group_index: number;
    insert: "before" | "after";
  };

  function handleSubscriptionSlotDrop(
    source: SubscriptionDragData,
    target: SubscriptionDropSlotData,
  ) {
    const { group_index: from_index } = source;
    const { group_index: to_index, insert } = target;
    if (from_index === to_index && insert !== "after") return;

    const sub = data[from_index];
    if (!sub) return;
    data.splice(from_index, 1);

    let targetIndex = insert === "after" ? to_index + 1 : to_index;
    if (from_index < targetIndex) targetIndex -= 1;

    data.splice(targetIndex, 0, sub);
    searchControls?.markGroupOrderChanged();
    bumpDataRevision();
  }

  function handleSubscriptionFinished() {
    finishedSubscriptionsCount++;
  }
</script>

<div class="subscriptions-page" use:smoothReflow>
  <div class="subscription-controls" use:smoothReflow data-no-smooth-reflow>
    <SubscriptionSearch
      subscriptions={data}
      {dataRevision}
      bind:visibleSubscriptions
      bind:searchActive
      bind:searchPending
      bind:controls={searchControls}
    />

    <div class="subscription-controls-actions">
      <Tooltip value={t("Save Changes")}>
        <Button onclick={saveChanges} id="save-subscriptions" class="accent" inactive={!canSave}>
          <Save size={22} />
        </Button>
      </Tooltip>
      <Tooltip value={t("Add Subscription")}>
        <Button onclick={() => (addSubscriptionModal = true)}><Add size={22} /></Button>
      </Tooltip>
    </div>
  </div>

  {#if fetchError}
    <Placeholder variant="error" minHeight="auto" subtitle={t("Check connection or try again")}>
      {t("Failed to load subscriptions")}
    </Placeholder>
  {:else if !isAllRendered}
    <Placeholder variant="loading" minHeight="auto">
      {t("Loading subscriptions...")}
    </Placeholder>
  {:else if noVisibleSubscriptions}
    <Placeholder variant="empty" minHeight="auto">
      {t("No matches found")}
    </Placeholder>
  {:else if isEmptyData}
    <Placeholder
      variant="empty"
      minHeight="auto"
      subtitle={t("Add a new subscription to get started")}
    >
      {t("No subscriptions yet")}
    </Placeholder>
  {/if}

  <div
    class="subscription-list"
    class:visible={isAllRendered}
    style={isAllRendered ? "" : "display: none;"}
    oninput={bumpDataRevision}
    onchange={bumpDataRevision}
  >
    {#each data.slice(0, renderSubscriptionsLimit) as sub, sub_index (sub.id)}
      {@const ruleIndices = visibilityMap.get(sub_index)}
      {@const isVisible = !searchActive || visibilityMap.has(sub_index)}

      <div class="subscription-wrapper" style={isVisible ? "" : "display: none"}>
        {#if sub_index === firstVisibleSubscriptionIndex}
          <div
            class="subscription-drop-slot subscription-drop-slot--top"
            aria-hidden="true"
            use:droppable={{
              data: { group_index: sub_index, insert: "before" } as SubscriptionDropSlotData,
              scope: "subscription",
              canDrop: (source: SubscriptionDragData, target: SubscriptionDropSlotData) =>
                source.group_index !== target.group_index,
              dropEffect: "move",
              onDrop: handleSubscriptionSlotDrop,
            }}
          ></div>
        {/if}

        <SubscriptionPanel
          bind:subscription={data[sub_index]}
          subscription_index={sub_index}
          bind:open={open_state[sub.id]}
          {deleteSubscription}
          {syncSubscription}
          {deleteRuleFromSubscription}
          {searchActive}
          visibleRuleIndices={ruleIndices}
          onFinished={handleSubscriptionFinished}
          data-no-smooth-reflow
        />

        <div
          class="subscription-drop-slot subscription-drop-slot--bottom"
          aria-hidden="true"
          use:droppable={{
            data: { group_index: sub_index, insert: "after" } as SubscriptionDropSlotData,
            scope: "subscription",
            canDrop: () => true,
            dropEffect: "move",
            onDrop: handleSubscriptionSlotDrop,
          }}
        ></div>
      </div>
    {/each}
  </div>
</div>

<AddSubscriptionDialog
  open={addSubscriptionModal}
  existingUrls={data.map((s) => s.url)}
  on:close={() => (addSubscriptionModal = false)}
  on:add={handleAdd}
/>

<style>
  .subscription-list {
    min-height: 1px;
    opacity: 0;
    transition: opacity 0.4s ease-in-out;
  }

  .subscription-list.visible {
    opacity: 1;
  }

  .subscription-wrapper {
    position: relative;
    margin: 1rem 0;
    animation: subscription-appear 0.15s ease-out forwards;
  }

  .subscription-wrapper:has(:global([data-select-trigger][data-state="open"])) {
    z-index: 1;
  }

  @keyframes subscription-appear {
    from {
      opacity: 0;
    }
    to {
      opacity: 1;
    }
  }

  .subscription-wrapper:first-of-type {
    margin-top: 1rem;
  }

  .subscription-wrapper:last-of-type {
    margin-bottom: 1rem;
  }

  .subscription-drop-slot {
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

  .subscription-drop-slot--top {
    top: -1rem;
  }

  .subscription-drop-slot--bottom {
    bottom: -1rem;
  }

  :global(html[data-dnd-scope="subscription"]) .subscription-drop-slot {
    pointer-events: auto;
  }

  :global(html[data-dnd-scope="subscription"]) .subscription-drop-slot:global(.dragover) {
    opacity: 1;
  }

  .subscription-controls {
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
    .subscription-controls {
      flex-wrap: wrap;
    }
    .subscription-controls-actions {
      justify-content: flex-start;
    }
  }

  @media (max-width: 700px) {
    .subscription-controls {
      margin-bottom: 1rem;
    }
  }

  .subscription-controls-actions {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }
</style>
