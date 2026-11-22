<script lang="ts">
  import { getContext, onDestroy, onMount, setContext } from "svelte";

  import BulkActions from "../../components/bulk/BulkActions.svelte";
  import SelectionFrame from "../../components/bulk/SelectionFrame.svelte";
  import PageControls from "../../components/layout/PageControls.svelte";
  import Placeholder from "../../components/ui/Placeholder.svelte";
  import VirtualList from "../../components/ui/VirtualList.svelte";
  import { t } from "../../data/locale.svelte";
  import { SizeEstimate, type VirtualMeasurement } from "../../lib/virtual/size-estimate.svelte";
  import SubscriptionPanel from "./components/SubscriptionPanel.svelte";
  import SubscriptionSearch from "./components/SubscriptionSearch.svelte";
  import AddSubscriptionDialog from "./dialogs/AddSubscriptionDialog.svelte";
  import {
    SUBSCRIPTIONS_STORE_CONTEXT,
    SubscriptionsStore,
    type SubscriptionDragData,
    type SubscriptionDropSlotData,
  } from "./subscriptions.svelte";

  import { droppable } from "../../lib/dnd";
  import { copyRulePatternsToClipboard } from "../../utils/copy-rule-patterns";
  import { CONFIG_TRANSFER_CONTEXT, type ConfigTransferControls } from "../config-transfer/context";

  type Props = {
    active?: boolean;
    onRenderComplete?: () => void;
  };

  type AddSubscriptionEvent = {
    url: string;
    name: string;
    interface: string;
    profile?: string;
    interval: number;
  };

  let { active = true, onRenderComplete }: Props = $props();

  const store = new SubscriptionsStore({
    onRenderComplete: () => onRenderComplete?.(),
    isActive: () => active,
  });
  setContext(SUBSCRIPTIONS_STORE_CONTEXT, store);
  const transfer = getContext<ConfigTransferControls>(CONFIG_TRANSFER_CONTEXT);
  const unregisterTransfer = transfer.registerSubscriptions(store);

  let selectedIds = $state<string[]>([]);
  let list: VirtualList;
  let viewportWidth = $state(1280);
  const ruleSize = new SizeEstimate({ desktop: 33, mobile: 82 });
  const panelSize = new SizeEstimate({ desktop: 100, mobile: 100 });
  const ruleEstimate = $derived(ruleSize.forWidth(viewportWidth));
  function measureRules(measurements: readonly VirtualMeasurement[]) {
    ruleSize.learn(viewportWidth, measurements);
  }
  function measurePanels(measurements: readonly VirtualMeasurement[]) {
    panelSize.learn(
      viewportWidth,
      measurements.filter(({ index }) => {
        const subscription = store.data[subscriptionIndex(index)];
        return subscription && !store.open_state[subscription.id];
      }),
    );
  }
  const visibleIndices = $derived(
    store.searchActive ? store.visibleSubscriptions.map((entry) => entry.group_index) : undefined,
  );
  const visibleCount = $derived(visibleIndices?.length ?? store.data.length);
  const subscriptionIndex = (index: number) => visibleIndices?.[index] ?? index;
  function estimateSubscription(index: number) {
    const originalIndex = subscriptionIndex(index);
    const sub = store.data[originalIndex];
    if (!sub) return 100;
    const rules = store.searchActive
      ? (store.visibilityMap.get(originalIndex)?.length ?? sub.rules.length)
      : sub.rules.length;
    return (
      panelSize.forWidth(viewportWidth) +
      (store.open_state[sub.id] && rules ? 32 + rules * ruleEstimate : 0)
    );
  }
  let selectedItems = $derived(store.data.filter((item) => selectedIds.includes(item.id)));
  function toggleSelection(id: string) {
    selectedIds = selectedIds.includes(id)
      ? selectedIds.filter((value) => value !== id)
      : [...selectedIds, id];
  }
  function applyToSelected(update: { interface: string; profile?: string } | { enable: boolean }) {
    for (const item of selectedItems) Object.assign(item, update);
    store.markDataRevision();
  }
  async function deleteSelected() {
    if (!confirm(`${t("Delete selected items?")} (${selectedItems.length})`)) return;
    const ids = selectedItems.map((item) => item.id);
    for (const id of ids) {
      const index = store.data.findIndex((item) => item.id === id);
      if (index >= 0) await store.deleteSubscription(index, true);
    }
    selectedIds = selectedIds.filter((id) => store.data.some((item) => item.id === id));
  }

  let addSubscriptionModal = $state(false);

  async function handleAdd(event: CustomEvent<AddSubscriptionEvent>) {
    await store.addSubscription(event.detail);
    await list?.scrollToIndex(0, "start");
  }

  onMount(() => {
    void store.mount();
  });

  onDestroy(() => {
    unregisterTransfer();
    store.destroy();
  });
</script>

<svelte:window bind:innerWidth={viewportWidth} />

<div class="subscriptions-page">
  <PageControls
    {active}
    actionsClass="subscription-controls-actions"
    controlsClass="subscription-controls"
    addLabel={t("Add Subscription")}
    hasUnsavedChanges={store.hasUnsavedChanges}
    saveBusy={store.saving || (transfer.pending() && transfer.busy())}
    canSave={transfer.pending() ? transfer.canSave("subscriptions") : store.canSave}
    exportLabel={t("Export Config")}
    importLabel={t("Import Config")}
    onExport={() => void transfer.exportConfig()}
    onImport={(event) => void transfer.importConfig(event)}
    onAdd={() => (addSubscriptionModal = true)}
    onSave={() => store.saveChanges()}
    saveButtonId="save-subscriptions"
    saveLabel={t("Save Changes")}
  >
    {#snippet search()}
      <SubscriptionSearch />
    {/snippet}
  </PageControls>

  {#if store.fetchError}
    <Placeholder variant="error" minHeight="auto" subtitle={t("Check connection or try again")}>
      {t("Failed to load subscriptions")}
    </Placeholder>
  {:else if !store.dataLoaded}
    <Placeholder variant="loading" minHeight="auto">
      {t("Loading subscriptions...")}
    </Placeholder>
  {:else if store.noVisibleSubscriptions}
    <Placeholder variant="empty" minHeight="auto">
      {t("No matches found")}
    </Placeholder>
  {:else if store.isEmptyData}
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
    class:visible={store.dataLoaded}
    style={store.dataLoaded ? "" : "display: none;"}
    oninput={store.markDataRevision}
    onchange={store.markDataRevision}
  >
    <VirtualList
      bind:this={list}
      count={visibleCount}
      getKey={(index) => store.data[subscriptionIndex(index)].id}
      estimateSize={estimateSubscription}
      {active}
      revision={visibleIndices ?? store.data}
      onmeasure={measurePanels}
    >
      {#snippet children(index)}
        {@const sub_index = subscriptionIndex(index)}
        {@const sub = store.data[sub_index]}

        <div class="subscription-wrapper">
          <div class="subscription-wrapper-inner">
            {#if sub_index === store.firstVisibleSubscriptionIndex}
              <div
                class="subscription-drop-slot subscription-drop-slot--top"
                aria-hidden="true"
                use:droppable={{
                  data: { group_index: sub_index, insert: "before" } as SubscriptionDropSlotData,
                  scope: "subscription",
                  canDrop: (source: SubscriptionDragData, target: SubscriptionDropSlotData) =>
                    source.group_index !== target.group_index,
                  dropEffect: "move",
                  onDrop: store.handleSubscriptionSlotDrop,
                }}
              ></div>
            {/if}

            <SelectionFrame
              selected={selectedIds.includes(sub.id)}
              name={sub.name}
              ontoggle={() => toggleSelection(sub.id)}
            >
              <SubscriptionPanel
                subscription_index={sub_index}
                {active}
                {ruleEstimate}
                onmeasureRules={measureRules}
              />
            </SelectionFrame>

            <div
              class="subscription-drop-slot subscription-drop-slot--bottom"
              aria-hidden="true"
              use:droppable={{
                data: { group_index: sub_index, insert: "after" } as SubscriptionDropSlotData,
                scope: "subscription",
                canDrop: () => true,
                dropEffect: "move",
                onDrop: store.handleSubscriptionSlotDrop,
              }}
            ></div>
          </div>
        </div>
      {/snippet}
    </VirtualList>
  </div>
</div>

<AddSubscriptionDialog
  open={addSubscriptionModal}
  existingUrls={store.data.map((subscription) => subscription.url)}
  on:close={() => (addSubscriptionModal = false)}
  on:add={handleAdd}
/>

{#if selectedItems.length}
  <BulkActions
    count={selectedItems.length}
    totalCount={store.data.length}
    currentInterface={selectedItems.every(
      (item) =>
        item.interface === selectedItems[0]?.interface &&
        item.profile === selectedItems[0]?.profile,
    )
      ? selectedItems[0]?.interface
      : undefined}
    onclear={() => (selectedIds = [])}
    onapply={(value) => applyToSelected(value)}
    currentProfile={selectedItems.every((item) => item.profile === selectedItems[0]?.profile)
      ? selectedItems[0]?.profile
      : undefined}
    currentEnabled={selectedItems.every((item) => item.enable === selectedItems[0]?.enable)
      ? selectedItems[0]?.enable
      : undefined}
    onenable={(enable) => applyToSelected({ enable })}
    ondelete={deleteSelected}
    oncopy={() => copyRulePatternsToClipboard(selectedItems.flatMap((item) => item.rules))}
    onselectall={() =>
      (selectedIds = store.data
        .filter((_, index) => !store.searchActive || store.visibilityMap.has(index))
        .map((item) => item.id))}
  />
{/if}

<style>
  .subscription-list {
    min-height: 1px;
    opacity: 0;
  }

  .subscription-list.visible {
    opacity: 1;
  }

  .subscription-wrapper {
    position: relative;
    margin: 0;
    padding-bottom: 1rem;
    display: grid;
    grid-template-rows: 1fr;
    opacity: 1;
    transition: none;
  }

  .subscription-wrapper-inner {
    min-height: 0;
    overflow: visible;
  }

  .subscription-wrapper:has(:global([data-select-trigger][data-state="open"])),
  .subscription-wrapper:has(:global([data-dropdown-menu-trigger][data-state="open"])) {
    z-index: 1;
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
  }

  .subscription-drop-slot--top {
    top: -1rem;
  }

  .subscription-drop-slot--bottom {
    bottom: 0;
  }

  :global(html[data-dnd-scope="subscription"]) .subscription-drop-slot {
    pointer-events: auto;
  }

  :global(html[data-dnd-scope="subscription"]) .subscription-drop-slot:global(.dragover) {
    opacity: 1;
  }
</style>
