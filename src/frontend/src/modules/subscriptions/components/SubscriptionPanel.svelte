<script module lang="ts">
  export const intervals = [
    { value: 3600, labelKey: "hour" },
    { value: 21600, labelKey: "6 hours" },
    { value: 86400, labelKey: "day" },
    { value: 604800, labelKey: "week" },
  ];

  export function parseIntervalSeconds(value: string) {
    const next = Number(value);
    if (!Number.isFinite(next)) return null;
    return next;
  }

  export function handleIntervalChange(value: string, apply: (next: number) => void) {
    const next = parseIntervalSeconds(value);
    if (next === null) return;
    apply(next);
  }
</script>

<script lang="ts">
  import { Collapsible } from "bits-ui";
  import { getContext } from "svelte";
  import { slide } from "svelte/transition";

  import Pagination from "../../../components/Pagination.svelte";
  import Button from "../../../components/ui/Button.svelte";
  import DropdownMenu from "../../../components/ui/DropdownMenu.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import Switch from "../../../components/ui/Switch.svelte";
  import Tooltip from "../../../components/ui/Tooltip.svelte";
  import { interfaces } from "../../../data/interfaces.svelte";
  import { t } from "../../../data/locale.svelte";
  import { SUBSCRIPTIONS_STORE_CONTEXT, type SubscriptionsStore } from "../subscriptions.svelte";
  import SubscriptionRuleRow from "./SubscriptionRuleRow.svelte";

  import {
    Delete,
    Dots,
    Grip,
    GroupCollapse,
    GroupExpand,
    History,
    Link,
    Refresh,
  } from "../../../components/ui/icons";
  import { draggable } from "../../../lib/dnd";
  import { type SubscriptionRule } from "../../../types";

  type Props = {
    subscription_index: number;
  };

  let { subscription_index }: Props = $props();

  const store = getContext<SubscriptionsStore>(SUBSCRIPTIONS_STORE_CONTEXT);
  if (!store) {
    throw new Error("SubscriptionsStore context is missing");
  }

  const PAGE_SIZE = 50;
  let currentPage = $state(1);

  let client_width = $state<number>(Infinity);
  let is_desktop = $derived(client_width > 668);

  let subscription = $derived(store.data[subscription_index]);
  let searchActive = $derived(store.searchActive);
  let visibleRuleIndices = $derived(store.visibilityMap.get(subscription_index));
  let effectiveOpen = $derived(subscription ? (store.open_state[subscription.id] ?? false) : false);

  function toggleOpen() {
    if (!subscription) return;
    store.open_state[subscription.id] = !effectiveOpen;
  }

  let totalRulesCount = $derived(
    subscription && searchActive && Array.isArray(visibleRuleIndices)
      ? visibleRuleIndices.length
      : (subscription?.rules.length ?? 0),
  );

  let usePagination = $derived(totalRulesCount > PAGE_SIZE);

  $effect(() => {
    if (searchActive && visibleRuleIndices) {
      currentPage = 1;
    }
  });

  $effect(() => {
    const maxPage = Math.ceil(totalRulesCount / PAGE_SIZE);
    if (currentPage > maxPage && maxPage > 0) {
      currentPage = 1;
    }
  });

  let displayedRules = $derived.by(() => {
    if (!subscription) return [];

    let rulesToRender: { rule: SubscriptionRule; originalIndex: number }[] = [];

    let sourceIndices: number[] = [];
    if (searchActive && Array.isArray(visibleRuleIndices)) {
      sourceIndices = visibleRuleIndices;
    } else {
      sourceIndices = new Array(subscription.rules.length);
      for (let i = 0; i < subscription.rules.length; i++) sourceIndices[i] = i;
    }

    let startIndex = 0;
    let endIndex = sourceIndices.length;

    if (usePagination) {
      startIndex = (currentPage - 1) * PAGE_SIZE;
      endIndex = Math.min(startIndex + PAGE_SIZE, sourceIndices.length);
    }

    for (let i = startIndex; i < endIndex; i++) {
      const idx = sourceIndices[i];
      if (subscription.rules[idx]) {
        rulesToRender.push({ rule: subscription.rules[idx], originalIndex: idx });
      }
    }
    return rulesToRender;
  });

  let reportedFinished = false;
  $effect(() => {
    if (searchActive) return;
    if (!reportedFinished && (totalRulesCount === 0 || displayedRules.length > 0)) {
      reportedFinished = true;
      store.handleSubscriptionFinished();
    }
  });

  function formatTime(timestamp: number | undefined | null) {
    if (!timestamp) return t("Never updated");
    const date = new Date(timestamp);
    return `${date.toLocaleDateString()} ${date.toLocaleTimeString()}`;
  }

  type SubscriptionDnD = {
    group_id: string;
    group_index: number;
    name: string;
    count: number;
  };

  function createSubscriptionDragPreview(headerEl: HTMLElement, name: string, count: number) {
    const badge = document.createElement("div");
    badge.style.cssText =
      "position:fixed;top:-1000px;left:-1000px;pointer-events:none;z-index:2147483647;transform:translateZ(0);font:600 13px/1.2 var(--font, -apple-system, system-ui, Segoe UI, Roboto, sans-serif);color:var(--text,#e5e7eb);";

    const inner = document.createElement("div");
    inner.style.cssText =
      "display:flex;align-items:center;gap:.55rem;padding:.42rem .7rem;border-radius:.7rem;background:var(--bg-light,rgba(30,30,36,.92));border:1px solid var(--bg-light-extra,rgba(255,255,255,.12));box-shadow:0 6px 18px rgba(0,0,0,.35);backdrop-filter:saturate(120%) blur(6px);";

    const title = document.createElement("span");
    title.textContent = name || "subscription";
    title.style.cssText =
      "max-width:240px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;";
    inner.appendChild(title);

    const cnt = document.createElement("span");
    cnt.textContent = `• ${count}`;
    cnt.style.opacity = "0.8";
    inner.appendChild(cnt);

    const gripClone = headerEl
      .querySelector(".subscription-grip")
      ?.cloneNode(true) as HTMLElement | null;
    if (gripClone) {
      gripClone.style.cssText += "opacity:.9;display:flex;align-items:center;margin-left:.25rem;";
      inner.appendChild(gripClone);
    }

    badge.appendChild(inner);
    document.body.appendChild(badge);
    return badge;
  }
</script>

<svelte:window bind:innerWidth={client_width} />

{#if subscription}
  <div
    class="subscription-panel"
    role="listitem"
    data-uuid={subscription.id}
    use:draggable={{
      data: {
        group_id: subscription.id,
        group_index: subscription_index,
        name: subscription.name,
        count: subscription.rules.length,
      } as SubscriptionDnD,
      scope: "subscription",
      handle: ".subscription-grip",
      effects: { effectAllowed: "move", dropEffect: "move" },
      dragImage: (node) =>
        createSubscriptionDragPreview(
          (node.querySelector(".subscription-header") ?? node) as HTMLElement,
          subscription.name,
          subscription.rules.length,
        ),
    }}
  >
    <Collapsible.Root open={effectiveOpen} onOpenChange={toggleOpen}>
      <div class="subscription-header">
        <div class="subscription-left">
          <div class="subscription-grip" title={t("Drag Subscription")}>
            <Grip />
          </div>
          <div class="subscription-info">
            <input
              type="text"
              placeholder={t("subscription name...")}
              class="subscription-name"
              bind:value={subscription.name}
            />
            <div class="subscription-url" title={subscription.url}>
              <span class="url-line">
                <span class="icon-wrap"><Link size={14} /></span>
                <span class="url-text">{subscription.url}</span>
              </span>
              <span class="update-line">
                <span class="icon-wrap"><History size={14} /></span>
                <span class="update-text">{formatTime(subscription.last_update)}</span>
                <span class="update-sep">•</span>
                <span class="update-interval">
                  <span class="interval-label">{t("Update every")}</span>
                  <Select
                    options={intervals.map((item) => ({
                      value: String(item.value),
                      label: t(item.labelKey),
                    }))}
                    selected={String(subscription.interval ?? 86400)}
                    onValueChange={(value) =>
                      handleIntervalChange(value, (next) => {
                        subscription.interval = next;
                      })}
                    class="subscription-interval"
                    ariaLabel={t("Update every")}
                  />
                </span>
              </span>
            </div>
          </div>
        </div>

        <div class="subscription-actions">
          <div class="action interface">
            <Select
              options={interfaces.list.map((item) => ({ value: item, label: item }))}
              bind:selected={subscription.interface}
              class="subscription-interface"
            />
          </div>

          <div class="action toggle">
            <Tooltip
              value={t(subscription.enable ? "Disable Subscription" : "Enable Subscription")}
            >
              <Switch class="enable-subscription" bind:checked={subscription.enable} />
            </Tooltip>
          </div>

          <div class="action sync">
            <Tooltip value={t("Sync Subscription")}>
              <Button small onclick={() => store.syncSubscription(subscription_index)}>
                <Refresh size={20} />
              </Button>
            </Tooltip>
          </div>

          <div class="action delete">
            {#if is_desktop}
              <Tooltip value={t("Delete Subscription")}>
                <Button small onclick={() => store.deleteSubscription(subscription_index)}>
                  <Delete size={20} />
                </Button>
              </Tooltip>
            {:else}
              <DropdownMenu>
                {#snippet trigger()}
                  <Dots size={20} />
                {/snippet}
                {#snippet item1()}
                  <Button general onclick={() => store.syncSubscription(subscription_index)}>
                    <div class="dd-icon"><Refresh size={20} /></div>
                    <div class="dd-label">{t("Sync")}</div>
                  </Button>
                {/snippet}
                {#snippet item2()}
                  <Button general onclick={() => store.deleteSubscription(subscription_index)}>
                    <div class="dd-icon"><Delete size={20} /></div>
                    <div class="dd-label">{t("Delete Subscription")}</div>
                  </Button>
                {/snippet}
              </DropdownMenu>
            {/if}
          </div>

          <div class="action collapse">
            <Tooltip value={t(effectiveOpen ? "Collapse" : "Expand")}>
              <Collapsible.Trigger>
                {#if effectiveOpen}
                  <GroupCollapse size={20} />
                {:else}
                  <GroupExpand size={20} />
                {/if}
              </Collapsible.Trigger>
            </Tooltip>
          </div>
        </div>
      </div>

      <Collapsible.Content>
        <div transition:slide={searchActive ? { duration: 0 } : {}}>
          {#if totalRulesCount > 0}
            <div class="subscription-rules-header">
              <div class="subscription-rules-header-column total">
                #{totalRulesCount}
              </div>
              <div class="subscription-rules-header-column pattern">
                {t("Pattern")}
              </div>
              <div class="subscription-rules-header-column">{t("Type")}</div>

              <div class="subscription-rules-header-column">{t("Enabled")}</div>
            </div>
          {/if}
          <div class="subscription-rules">
            {#if totalRulesCount > 0}
              {#each displayedRules as { rule, originalIndex }, i (rule.id)}
                <SubscriptionRuleRow
                  key={rule.id}
                  bind:rule={subscription.rules[originalIndex]}
                  rule_index={originalIndex}
                  {subscription_index}
                  style={i % 2 ? "" : "background-color: var(--bg-light)"}
                />
              {/each}
            {/if}
          </div>
          {#if usePagination}
            <Pagination totalItems={totalRulesCount} pageSize={PAGE_SIZE} bind:currentPage />
          {/if}
        </div>
      </Collapsible.Content>
    </Collapsible.Root>
  </div>
{/if}

<style>
  .subscription-panel {
    background-color: var(--bg-medium);
    border-radius: 0.5rem;
    border: 1px solid var(--bg-light-extra);
    transition:
      transform 0.12s ease,
      opacity 0.12s ease,
      box-shadow 0.12s ease;
  }

  .subscription-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 0.5rem;
    padding: 0.5rem;
    border-radius: 0.5rem;
    background-color: var(--bg-light);
    position: relative;
  }

  .subscription-left {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    flex: 1;
    min-width: 0;
  }

  .subscription-grip {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    margin-right: 0.1rem;
    margin-left: 0.1rem;
    color: var(--text-2);
    cursor: grab;
    user-select: none;
    -webkit-user-select: none;
    -webkit-user-drag: none;

    &:hover {
      color: var(--text);
    }
  }

  .subscription-info {
    display: flex;
    flex-direction: column;
    flex: 1;
    min-width: 0;
    gap: 0.2rem;
  }

  .subscription-url {
    font-size: 0.8rem;
    color: var(--text-2);
    margin-left: 0.4rem;
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 0.2rem;
    min-width: 0;
    width: 100%;
    max-width: 100%;
    overflow: hidden;
  }

  .url-line {
    display: flex;
    align-items: center;
    gap: 0.3rem;
    min-width: 0;
    max-width: 100%;
    overflow: hidden;
  }

  .url-text {
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .update-line {
    display: flex;
    align-items: center;
    gap: 0.3rem;
    flex-wrap: wrap;
    width: 100%;
    max-width: 100%;
    overflow: hidden;
  }

  .icon-wrap {
    display: flex;
    align-items: center;
    opacity: 0.7;
  }

  .subscription-name {
    border: none;
    background-color: transparent;
    font-size: 1.3rem;
    font-weight: 600;
    font-family: var(--font);
    color: var(--text);
    border-bottom: 1px solid transparent;
    margin-left: 0.4rem;
    width: 100%;

    &:focus-visible {
      outline: none;
      border-bottom: 1px solid var(--accent);
    }
  }

  .subscription-actions {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 0.35rem;
    flex-shrink: 0;

    &:global([data-switch-root]) {
      margin: 0 0.3rem;
    }
  }

  .subscription-actions .action {
    display: inline-flex;
    align-items: center;
  }

  .update-sep {
    opacity: 0.5;
  }

  .update-interval {
    display: inline-flex;
    align-items: center;
  }

  .update-interval :global([data-select-trigger]) {
    padding: 0.1rem 0.3rem;
    font-size: 0.85rem;
    color: var(--text-2);
  }

  .update-interval :global(.selected-value) {
    padding-left: 0;
  }

  .subscription-rules-header {
    display: grid;
    grid-template-columns: minmax(2.5rem, auto) 5.5fr 1fr 0.6fr;
    gap: 0.5rem;
    align-items: center;
    font-size: 0.9rem;
    color: var(--text-2);
    padding: 0.6rem 0 0.2rem 0;
    border-bottom: 1px solid var(--bg-light-extra);
  }

  .subscription-rules-header-column {
    display: flex;
    align-items: center;
    justify-content: center;
    min-width: 0;

    &.total {
      padding-left: 0.2rem;
      padding-right: 0.2rem;
    }
  }

  :global {
    [data-collapsible-trigger] {
      color: var(--text-2);
      background-color: transparent;
      border: 1px solid transparent;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      padding: 0.4rem;
      border-radius: 0.5rem;
      cursor: pointer;

      &:hover {
        background-color: var(--bg-dark);
        color: var(--text);
        border: 1px solid var(--bg-light-extra);
      }
    }
  }

  @media (max-width: 700px) {
    .subscription-header {
      display: flex;
      flex-direction: column;
      align-items: start;
      justify-content: center;
    }

    .subscription-left {
      width: 100%;
    }

    .subscription-grip {
      display: none;
    }

    .subscription-actions {
      width: 100%;
      justify-content: flex-start;
      flex-wrap: wrap;
      gap: 0.35rem;
      margin-left: 0;
      margin-top: 0;
    }

    .subscription-url {
      display: flex;
      flex-direction: column;
      width: 100%;
      gap: 0.2rem;
      align-items: start;
      max-width: 100%;
      overflow: hidden;
    }

    .url-line {
      max-width: 100%;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .url-text {
      max-width: 100%;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .subscription-actions .action.interface {
      flex: 1 1 160px;
      min-width: 140px;
    }

    .subscription-actions .action.toggle {
      margin-left: auto;
    }

    .subscription-actions .action.sync {
      display: none;
    }

    .subscription-rules-header {
      height: 1px;
      padding: 0;
      border: none;
      & .subscription-rules-header-column {
        display: none;
      }
    }
  }
</style>
