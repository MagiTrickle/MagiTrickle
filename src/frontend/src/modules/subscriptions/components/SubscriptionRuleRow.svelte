<script lang="ts">
  import { getContext, tick } from "svelte";

  import Select from "../../../components/ui/Select.svelte";
  import Switch from "../../../components/ui/Switch.svelte";
  import Tooltip from "../../../components/ui/Tooltip.svelte";
  import { t } from "../../../data/locale.svelte";

  import { RULE_TYPES, type SubscriptionRule } from "../../../types";
  import { VALIDATOP_MAP } from "../../../utils/rule-validators";
  import {
    SUBSCRIPTIONS_STORE_CONTEXT,
    type SubscriptionsStore,
  } from "../subscriptions.svelte";

  type Props = {
    rule: SubscriptionRule;
    rule_index: number;
    subscription_index: number;
    [key: string]: any;
  };

  let { rule = $bindable(), rule_index, subscription_index, ...rest }: Props = $props();
  const store = getContext<SubscriptionsStore>(SUBSCRIPTIONS_STORE_CONTEXT);
  if (!store) {
    throw new Error("SubscriptionsStore context is missing");
  }

  function isPatternInvalid() {
    return (
      rule.rule.length === 0 || (VALIDATOP_MAP[rule.type] && !VALIDATOP_MAP[rule.type](rule.rule))
    );
  }

  async function handleTypeChange(value: string) {
    rule.type = value;
    store.markDataRevision();
    await tick();
    store.checkRulesValidityState();
  }
</script>

<div
  class="subscription-rule"
  data-index={rule_index}
  data-subscription-index={subscription_index}
  {...rest}
>
  <div class="subscription-rule-row">
    <div class="subscription-rule-number">{rule_index + 1}</div>
    <div class="subscription-rule-pattern">
      <div class="label">{t("Pattern")}</div>
      <div class="subscription-rule-value pattern-value" class:invalid={isPatternInvalid()}>
        {rule.rule}
      </div>
    </div>
    <div class="subscription-rule-type">
      <div class="label">{t("Type")}</div>
      <Select options={RULE_TYPES} bind:selected={rule.type} onValueChange={handleTypeChange} />
    </div>
    <div class="subscription-rule-actions">
      <Tooltip value={t(rule.enable ? "Disable Rule" : "Enable Rule")}>
        <Switch bind:checked={rule.enable} />
      </Tooltip>
    </div>
  </div>
</div>

<style>
  .subscription-rule {
    display: block;
  }

  .subscription-rule-row {
    display: grid;
    grid-template-columns: 2.5rem 5.5fr 1fr 0.6fr;
    gap: 0.5rem;
    padding: 0.1rem 0;
    background: inherit;
    border-radius: inherit;
  }

  .subscription-rule-number {
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 0.9rem;
    color: var(--text-2);
  }

  .subscription-rule-pattern {
    grid-column: 2;
  }
  .subscription-rule-type {
    grid-column: 3;
  }
  .subscription-rule-actions {
    grid-column: 4;
  }

  .subscription-rule-value {
    border: none;
    background-color: transparent;
    font-size: 1rem;
    font-family: var(--font);
    color: var(--text);
    border-bottom: 1px solid transparent;
    width: 100%;
    padding: 2px 0;
    min-height: 1.5rem;
    display: flex;
    align-items: center;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .subscription-rule-type,
  .subscription-rule-pattern,
  .subscription-rule-actions {
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 0 0.5rem;
    min-width: 0;
  }

  .subscription-rule-type {
    justify-content: flex-end;
  }

  .subscription-rule-actions {
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .pattern-value.invalid {
    border-bottom: 1px solid var(--red);
  }

  .label {
    font-size: 0.9rem;
    color: var(--text-2);
    width: 3.2rem;
    text-align: right;
    padding-right: 0.2rem;
    display: none;
  }

  @media (max-width: 700px) {
    .subscription-rule-row {
      grid-template-columns: minmax(0, 1fr) auto;
      column-gap: 0.4rem;
      row-gap: 0.35rem;
      padding: 0.5rem 0.35rem 0.45rem;
      align-items: start;
    }

    .subscription-rule-number {
      display: none;
    }

    .subscription-rule-type,
    .subscription-rule-pattern,
    .subscription-rule-actions {
      grid-column: auto;
    }

    .label {
      display: block;
    }

    .subscription-rule-type,
    .subscription-rule-pattern {
      display: grid;
      grid-template-columns: 3.2rem minmax(0, 1fr);
      align-items: center;
      gap: 0.35rem;
      padding: 0.05rem 0;
      grid-column: 1;
    }

    .subscription-rule-pattern .label,
    .subscription-rule-type .label {
      justify-self: end;
      text-align: right;
      position: static;
    }

    .subscription-rule-type :global([data-select-trigger]) {
      justify-content: flex-start;
    }

    .subscription-rule-actions {
      grid-column: 2;
      grid-row: 1 / span 2;
      display: flex;
      flex-direction: row;
      gap: 0.35rem;
      justify-content: center;
      align-items: center;
      align-self: stretch;
    }
  }
</style>
