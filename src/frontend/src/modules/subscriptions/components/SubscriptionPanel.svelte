<script lang="ts">
  import { Collapsible } from "bits-ui";
  import { createEventDispatcher } from "svelte";
  import { slide } from "svelte/transition";

  import Pagination from "../../../components/Pagination.svelte";
  import Button from "../../../components/ui/Button.svelte";
  import DropdownMenu from "../../../components/ui/DropdownMenu.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import Switch from "../../../components/ui/Switch.svelte";
  import Tooltip from "../../../components/ui/Tooltip.svelte";
  import { interfaces } from "../../../data/interfaces.svelte";
  import { t } from "../../../data/locale.svelte";
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
    SortAsc,
    SortDesc,
    SortNeutral,
  } from "../../../components/ui/icons";
  import { draggable, droppable } from "../../../lib/dnd";
  import { type Subscription, type SubscriptionRule } from "../../../types";
  import { sortRules, type SortDirection, type SortField } from "../../../utils/rule-sorter";

  type Props = {
    subscription: Subscription;
    subscription_index: number;
    open: boolean;
    deleteSubscription: (index: number) => void;
    syncSubscription: (index: number) => void;
    deleteRuleFromSubscription: (subscription_index: number, rule_index: number) => void;
    changeRuleIndex: (
      from_sub_index: number,
      from_rule_index: number,
      to_sub_index: number,
      to_rule_index: number,
      to_rule_id: string,
      insert?: "before" | "after",
    ) => void;
    searchActive?: boolean;
    visibleRuleIndices?: number[] | null;
    onFinished?: () => void;
    [key: string]: any;
  };

  let {
    subscription = $bindable(),
    subscription_index,
    open = $bindable(),
    deleteSubscription,
    syncSubscription,
    deleteRuleFromSubscription,
    changeRuleIndex,
    searchActive = false,
    visibleRuleIndices = null,
    onFinished,
    ...rest
  }: Props = $props();

  const dispatch = createEventDispatcher();

  const PAGE_SIZE = 50;
  let currentPage = $state(1);

  let client_width = $state<number>(Infinity);
  let is_desktop = $derived(client_width > 668);

  let effectiveOpen = $derived(open);

  function toggleOpen() {
    open = !open;
  }

  let totalRulesCount = $derived(
    searchActive && Array.isArray(visibleRuleIndices)
      ? visibleRuleIndices.length
      : subscription.rules.length,
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
      onFinished?.();
    }
  });

  let sortField = $state<SortField | null>(null);
  let sortDirection = $state<SortDirection>("asc");
  let initialOrderIds = $state<string[] | null>(null);

  function handleSort(field: SortField) {
    if (!initialOrderIds) {
      initialOrderIds = subscription.rules.map((rule) => rule.id);
    }

    if (sortField === field && sortDirection === "desc") {
      sortField = null;
      sortDirection = "asc";

      if (initialOrderIds) {
        const ruleMap = new Map(subscription.rules.map((rule) => [rule.id, rule]));
        const orderedRules = initialOrderIds
          .map((id) => ruleMap.get(id))
          .filter((rule): rule is SubscriptionRule => Boolean(rule));
        subscription.rules.splice(0, subscription.rules.length, ...orderedRules);
      }
      return;
    }

    if (sortField === field) {
      sortDirection = "desc";
    } else {
      sortField = field;
      sortDirection = "asc";
    }

    const sorted = sortRules(subscription.rules as any, field, sortDirection);
    subscription.rules.splice(0, subscription.rules.length, ...sorted);
  }

  $effect(() => {
    subscription.rules.length;
    if (sortField === null) {
      initialOrderIds = null;
    }
  });

  function formatTime(timestamp: number | undefined | null) {
    if (!timestamp) return t("Never updated");
    return new Date(timestamp).toLocaleString();
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

  type DnDTransferData = {
    rule_id: string;
    subscription_index: number;
    rule_index: number;
  };

  function handleRuleDrop(source: DnDTransferData, target: any) {
    changeRuleIndex(
      source.subscription_index,
      source.rule_index,
      subscription_index,
      0,
      "",
      "before",
    );
  }
</script>

<svelte:window bind:innerWidth={client_width} />

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
    <div
      class="subscription-header"
      use:droppable={{
        data: { rule_id: "", rule_index: 0, subscription_index: subscription_index },
        scope: "subscription-rule",
        canDrop: () => true,
        onDrop: (src) => handleRuleDrop(src as DnDTransferData, {}),
      }}
    >
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
              <span class="update-text">({formatTime(subscription.last_update)})</span>
            </span>
          </div>
        </div>
      </div>

      <div class="subscription-actions">
        <Select
          options={interfaces.list.map((item) => ({ value: item, label: item }))}
          bind:selected={subscription.interface}
        />

        <Tooltip value={t(subscription.enable ? "Disable Subscription" : "Enable Subscription")}>
          <Switch class="enable-subscription" bind:checked={subscription.enable} />
        </Tooltip>

        <Tooltip value={t("Sync Subscription")}>
          <Button small onclick={() => syncSubscription(subscription_index)}>
            <Refresh size={20} />
          </Button>
        </Tooltip>

        {#if is_desktop}
          <Tooltip value={t("Delete Subscription")}>
            <Button small onclick={() => deleteSubscription(subscription_index)}>
              <Delete size={20} />
            </Button>
          </Tooltip>
        {:else}
          <DropdownMenu>
            {#snippet trigger()}
              <Dots size={20} />
            {/snippet}
            {#snippet item1()}
              <Button general onclick={() => syncSubscription(subscription_index)}>
                <div class="dd-icon"><Refresh size={20} /></div>
                <div class="dd-label">{t("Sync")}</div>
              </Button>
            {/snippet}
            {#snippet item2()}
              <Button general onclick={() => deleteSubscription(subscription_index)}>
                <div class="dd-icon"><Delete size={20} /></div>
                <div class="dd-label">{t("Delete Subscription")}</div>
              </Button>
            {/snippet}
          </DropdownMenu>
        {/if}

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

    <Collapsible.Content>
      <div transition:slide={searchActive ? { duration: 0 } : {}}>
        {#if totalRulesCount > 0}
          <div class="subscription-rules-header">
            <div class="subscription-rules-header-column total">
              #{totalRulesCount}
            </div>
            <div class="subscription-rules-header-column">{t("Type")}</div>
            <!-- svelte-ignore a11y_click_events_have_key_events -->
            <!-- svelte-ignore a11y_no_static_element_interactions -->
            <div
              class="subscription-rules-header-column clickable"
              onclick={() => handleSort("pattern")}
            >
              {t("Pattern")}
              <div class="sort-icon">
                {#if sortField === "pattern" && sortDirection === "desc"}
                  <SortAsc size={16} />
                {:else if sortField === "pattern"}
                  <SortDesc size={16} />
                {:else}
                  <SortNeutral size={16} />
                {/if}
              </div>
            </div>
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
                onDelete={deleteRuleFromSubscription}
                onChangeIndex={changeRuleIndex}
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
    padding: 0.5rem;
    border-radius: 0.5rem;
    background-color: var(--bg-light);
    position: relative;

    &:global(.dragover) {
      outline: 1px solid var(--accent);
      box-shadow: inset 0 0 5px 0 var(--accent);
    }
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
    align-items: center;
    gap: 1rem;
    flex-wrap: wrap;
  }

  .url-line,
  .update-line {
    display: flex;
    align-items: center;
    gap: 0.3rem;
    min-width: 0;
  }

  .url-text {
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .icon-wrap {
    display: flex;
    align-items: center;
    opacity: 0.7;
  }

  .update-text {
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
    gap: 0.2rem;
    flex-shrink: 0;

    &:global([data-switch-root]) {
      margin: 0 0.3rem;
    }
  }

  .subscription-rules-header {
    display: grid;
    grid-template-columns: 0rem 1.5fr 5.5fr 1fr;
    justify-content: center;
    align-items: center;
    font-size: 0.9rem;
    color: var(--text-2);
    padding-top: 0.6rem;
    padding-bottom: 0.2rem;
    border-bottom: 1px solid var(--bg-light-extra);
  }

  .subscription-rules-header-column {
    display: flex;
    align-items: center;
    justify-content: center;

    &.total {
      justify-content: start;
      margin-left: 0.5rem;
    }
  }

  .clickable {
    cursor: pointer;
    user-select: none;
    transition: color 0.12s ease;

    &:hover {
      color: var(--text);
    }
  }

  .sort-icon {
    margin-left: 0.4rem;
    display: flex;
    align-items: center;
    color: var(--text-2);
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
      justify-content: stretch;
      gap: 0.25rem;
      margin-left: 0rem;
      margin-top: 0rem;
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

    :global(.subscription-actions > *:nth-child(3)) {
      display: none;
    }

    :global(.subscription-actions > *:nth-child(1)) {
      margin-right: auto;
      width: 150px;
      min-width: 140px;
      flex: 1 1 auto;
    }

    :global(.subscription-actions > *:nth-child(2)) {
      margin-left: auto;
    }

    .subscription-rules-header {
      height: 1px;
      & .subscription-rules-header-column {
        display: none;
      }
    }
  }
</style>
