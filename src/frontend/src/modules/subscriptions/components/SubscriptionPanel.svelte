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

  import Button from "../../../components/ui/Button.svelte";
  import DropdownMenu from "../../../components/ui/DropdownMenu.svelte";
  import EdgeNumberInput from "../../../components/ui/EdgeNumberInput.svelte";
  import RouteSelect from "../../../components/ui/RouteSelect.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import Switch from "../../../components/ui/Switch.svelte";
  import Tooltip from "../../../components/ui/Tooltip.svelte";
  import VirtualList from "../../../components/ui/VirtualList.svelte";
  import { t } from "../../../data/locale.svelte";
  import type { VirtualMeasurement } from "../../../lib/virtual/size-estimate.svelte";
  import { SUBSCRIPTIONS_STORE_CONTEXT, type SubscriptionsStore } from "../subscriptions.svelte";
  import SubscriptionRuleRow from "./SubscriptionRuleRow.svelte";

  import {
    CloudSync,
    Delete,
    Dots,
    Grip,
    GroupCollapse,
    GroupExpand,
    Link,
    Refresh,
  } from "../../../components/ui/icons";
  import { draggable } from "../../../lib/dnd";

  type Props = {
    subscription_index: number;
    active?: boolean;
    ruleEstimate: number;
    onmeasureRules: (measurements: readonly VirtualMeasurement[]) => void;
  };

  let { subscription_index, active = true, ruleEstimate, onmeasureRules }: Props = $props();

  const store = getContext<SubscriptionsStore>(SUBSCRIPTIONS_STORE_CONTEXT);
  if (!store) {
    throw new Error("SubscriptionsStore context is missing");
  }

  const lastUpdateDateFormatter = new Intl.DateTimeFormat("ru-RU", {
    day: "2-digit",
    month: "2-digit",
    year: "numeric",
  });
  const lastUpdateTimeFormatter = new Intl.DateTimeFormat("ru-RU", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  });

  let client_width = $state<number>(Infinity);
  let is_desktop = $derived(client_width > 668);

  let subscription = $derived(store.data[subscription_index]);
  let searchActive = $derived(store.searchActive);
  let visibleRuleIndices = $derived(store.visibilityMap.get(subscription_index));
  let effectiveOpen = $derived(subscription ? (store.open_state[subscription.id] ?? false) : false);
  let urlError = $derived(subscription ? store.subscriptionUrlErrors.get(subscription.id) : null);

  function toggleOpen() {
    if (!subscription) return;
    store.open_state[subscription.id] = !effectiveOpen;
  }

  let totalRulesCount = $derived(
    subscription && searchActive && Array.isArray(visibleRuleIndices)
      ? visibleRuleIndices.length
      : (subscription?.rules.length ?? 0),
  );

  // Keep the source collection in the store; only visible rows create components.
  function originalRuleIndex(index: number) {
    return searchActive && Array.isArray(visibleRuleIndices) ? visibleRuleIndices[index] : index;
  }

  function formatTime(timestamp: number | undefined | null) {
    if (!timestamp) return t("Never updated");
    const date = new Date(timestamp * 1000);
    return `${lastUpdateDateFormatter.format(date)} ${lastUpdateTimeFormatter.format(date)}`;
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
            <div class="subscription-url">
              <label class="url-line" for={`subscription-url-${subscription.id}`}>
                <span class="icon-wrap"><Link size={14} /></span>
                <input
                  id={`subscription-url-${subscription.id}`}
                  type="url"
                  class="subscription-url-input"
                  class:invalid={Boolean(urlError)}
                  bind:value={subscription.url}
                  placeholder="https://example.com/list.txt"
                  aria-label={t("URL")}
                  title={subscription.url}
                  onblur={() => store.normalizeSubscriptionUrl(subscription_index)}
                />
              </label>
              <span class="update-line">
                <span class="icon-wrap"><CloudSync size={14} /></span>
                <span class="update-text">{formatTime(subscription.lastUpdate)}</span>
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
            <RouteSelect
              bind:interface={subscription.interface}
              bind:profile={subscription.profile}
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
        <EdgeNumberInput
          title={t("Subscription priority")}
          label={`${t("Subscription priority")}: ${subscription.name}`}
          bind:value={subscription.priority}
        />
      </div>

      <Collapsible.Content>
        {#snippet child({ props })}
          {#if effectiveOpen}
            <div {...props}>
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
                  <VirtualList
                    recycle
                    count={totalRulesCount}
                    getKey={(index) => subscription.rules[originalRuleIndex(index)].id}
                    estimateSize={ruleEstimate}
                    measurementKey={subscription.rules}
                    onmeasure={onmeasureRules}
                    active={active && effectiveOpen}
                    revision={visibleRuleIndices ?? subscription.rules}
                  >
                    {#snippet children(index)}
                      {@const originalIndex = originalRuleIndex(index)}
                      <SubscriptionRuleRow
                        key={subscription.rules[originalIndex].id}
                        bind:rule={subscription.rules[originalIndex]}
                        rule_index={originalIndex}
                        {subscription_index}
                        style={index % 2 ? "" : "background-color: var(--bg-light)"}
                      />
                    {/snippet}
                  </VirtualList>
                {/if}
              </div>
            </div>
          {:else}
            <div id={typeof props.id === "string" ? props.id : undefined} hidden></div>
          {/if}
        {/snippet}
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
    padding: 0.5rem 2rem 0.5rem 0.5rem;
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
    width: 100%;
    max-width: 100%;
    overflow: hidden;
  }

  .subscription-url-input {
    min-width: 0;
    width: 100%;
    border: none;
    border-bottom: 1px solid transparent;
    background: transparent;
    color: var(--text-2);
    font: inherit;
    line-height: 1.3;
    padding: 0;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .subscription-url-input:focus-visible {
    outline: none;
    color: var(--text);
    border-bottom-color: var(--accent);
  }

  .subscription-url-input.invalid {
    color: var(--red);
    border-bottom-color: var(--red);
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
    box-sizing: border-box;
    min-width: 0;
    border: none;
    background-color: transparent;
    font-size: 1.3rem;
    font-weight: 600;
    font-family: var(--font);
    color: var(--text);
    border-bottom: 1px solid transparent;
    margin-left: 0.4rem;
    width: calc(100% - 0.4rem);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;

    &:focus-visible {
      outline: none;
      border-bottom: 1px solid var(--accent);
    }
  }

  .subscription-actions {
    min-height: 2.25rem;
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

  .update-interval :global([data-select-content]) {
    z-index: 1;
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

  @media (min-width: 701px) {
    .update-line {
      flex-wrap: nowrap;
    }
    .update-text {
      min-width: 0;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .update-interval {
      flex-shrink: 0;
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

    .subscription-url-input {
      max-width: 100%;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .subscription-actions .action.interface {
      flex: 0 1 auto;
      min-width: 100px;
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
