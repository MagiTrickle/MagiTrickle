<script lang="ts">
  import { onDestroy, onMount, setContext } from "svelte";

  import BulkActions from "../../components/bulk/BulkActions.svelte";
  import SelectionFrame from "../../components/bulk/SelectionFrame.svelte";
  import PageControls from "../../components/layout/PageControls.svelte";
  import Placeholder from "../../components/ui/Placeholder.svelte";
  import { t } from "../../data/locale.svelte";
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
  import type { SubscriptionRule } from "../../types";
  import { copyRulePatternsToClipboard } from "../../utils/copy-rule-patterns";

  type Props = {
    onRenderComplete?: () => void;
  };

  type AddSubscriptionEvent = {
    url: string;
    name: string;
    rules: SubscriptionRule[];
    interface: string;
    interval: number;
  };

  let { onRenderComplete }: Props = $props();

  const store = new SubscriptionsStore({ onRenderComplete: () => onRenderComplete?.() });
  setContext(SUBSCRIPTIONS_STORE_CONTEXT, store);

  let selectedIds = $state<string[]>([]);
  let selectedItems = $derived(store.data.filter((item) => selectedIds.includes(item.id)));
  function toggleSelection(id: string) {
    selectedIds = selectedIds.includes(id)
      ? selectedIds.filter((value) => value !== id)
      : [...selectedIds, id];
  }
  function applyToSelected(update: { interface: string } | { enable: boolean }) {
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
  }

  onMount(() => {
    void store.mount();
  });

  onDestroy(() => {
    store.destroy();
  });
</script>

<div class="subscriptions-page">
  <PageControls
    actionsClass="subscription-controls-actions"
    controlsClass="subscription-controls"
    addLabel={t("Add Subscription")}
    canSave={store.canSave}
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
  {:else if !store.isAllRendered}
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
    class:visible={store.isAllRendered}
    style={store.isAllRendered ? "" : "display: none;"}
    oninput={store.markDataRevision}
    onchange={store.markDataRevision}
  >
    {#each store.data.slice(0, store.renderSubscriptionsLimit) as sub, sub_index (sub.id)}
      {@const isVisible = !store.searchActive || store.visibilityMap.has(sub_index)}

      <div class="subscription-wrapper" class:is-hidden={!isVisible}>
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
            <SubscriptionPanel subscription_index={sub_index} />
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
    {/each}
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
    onclear={() => (selectedIds = [])}
    onapply={(value) => applyToSelected({ interface: value })}
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
    margin: 1rem 0;
    display: grid;
    grid-template-rows: 1fr;
    opacity: 1;
    transition: none;
  }

  .subscription-wrapper-inner {
    min-height: 0;
    overflow: hidden;
  }

  .subscription-wrapper:not(.is-hidden) .subscription-wrapper-inner {
    overflow: visible;
  }

  .subscription-wrapper.is-hidden {
    grid-template-rows: 0fr;
    opacity: 0;
    margin-top: 0;
    margin-bottom: 0;
    pointer-events: none;
  }

  .subscription-wrapper:has(:global([data-select-trigger][data-state="open"])),
  .subscription-wrapper:has(:global([data-dropdown-menu-trigger][data-state="open"])) {
    z-index: 1;
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
</style>
