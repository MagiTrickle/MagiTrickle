<script lang="ts">
  import { onDestroy, onMount, setContext } from "svelte";

  import Button from "../../components/ui/Button.svelte";
  import Placeholder from "../../components/ui/Placeholder.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
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

  import { Add, Save } from "../../components/ui/icons";
  import { droppable } from "../../lib/dnd";
  import type { SubscriptionRule } from "../../types";

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
  <div class="subscription-controls">
    <SubscriptionSearch />

    <div class="subscription-controls-actions">
      <Tooltip value={t("Save Changes")}>
        <Button
          onclick={() => store.saveChanges()}
          id="save-subscriptions"
          class="accent"
          inactive={!store.canSave}
        >
          <Save size={22} />
        </Button>
      </Tooltip>
      <Tooltip value={t("Add Subscription")}>
        <Button onclick={() => (addSubscriptionModal = true)}><Add size={22} /></Button>
      </Tooltip>
    </div>
  </div>

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

          <SubscriptionPanel subscription_index={sub_index} />

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
      display: block;
      padding: 0.3rem 0;
      padding-bottom: 0;
      transition: padding-bottom 220ms cubic-bezier(0.2, 0, 0.2, 1);
      --row-h: 48px;
      --gap: 10px;
      --actions-top: 0px;
      --actions-reserve: 108px;
    }

    .subscription-controls :global(.subscription-controls-search) {
      padding-right: var(--actions-reserve);
      transition: padding-right 220ms cubic-bezier(0.2, 0, 0.2, 1);
    }

    .subscription-controls-actions {
      position: absolute;
      right: 0;
      top: var(--actions-top);
      height: var(--row-h);
      transition: top 220ms cubic-bezier(0.2, 0, 0.2, 1);
    }

    .subscription-controls:has(:global(.search-container:focus-within)),
    .subscription-controls:has(:global(.search-input:not(:placeholder-shown))) {
      padding-bottom: calc(var(--row-h) + var(--gap));
      --actions-top: calc(var(--row-h) + var(--gap));
      --actions-reserve: 0px;
    }

    .subscription-controls:has(:global(.search-container:focus-within)) :global(.search-container),
    .subscription-controls:has(:global(.search-input:not(:placeholder-shown)))
      :global(.search-container) {
      width: 100%;
    }

    .subscription-controls:has(:global(.search-container:focus-within))
      :global(.search-container .input-wrapper),
    .subscription-controls:has(:global(.search-input:not(:placeholder-shown)))
      :global(.search-container .input-wrapper) {
      width: 100%;
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
