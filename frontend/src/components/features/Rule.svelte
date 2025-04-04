<script lang="ts">
  import { droppable, draggable } from "../actions/dnd";
  import { RULE_TYPES, type Rule } from "../../types";
  import Switch from "../common/Switch.svelte";
  import Tooltip from "../common/Tooltip.svelte";
  import { Delete, Grip } from "../common/icons";
  import { VALIDATOP_MAP } from "../../utils/rule-validators";
  import Button from "../common/Button.svelte";
  import Select from "../common/Select.svelte";

  type Props = {
    rule: Rule;
    rule_index: number;
    group_index: number;
    rule_id: string;
    group_id: string;
    onChangeIndex?: (
      from_group_index: number,
      from_rule_index: number,
      to_group_index: number,
      to_rule_index: number,
    ) => void;
    onDelete?: (from_group_index: number, from_rule_index: number) => void;
    [key: string]: any;
  };

  let {
    rule = $bindable(),
    rule_index,
    group_index,
    rule_id,
    group_id,
    onChangeIndex,
    onDelete,
    ...rest
  }: Props = $props();

  let input: HTMLInputElement;

  function patternValidation() {
    if (
      input.value.length === 0 ||
      (VALIDATOP_MAP[rule.type] && !VALIDATOP_MAP[rule.type](input.value))
    ) {
      input.classList.add("invalid");
    } else {
      input.classList.remove("invalid");
    }
  }

  type DnDTransferData = {
    rule_id: string;
    group_id: string;
    rule_index: number;
    group_index: number;
  };

  function handlerDrop(source: DnDTransferData, target: DnDTransferData) {
    window.dispatchEvent(
      new CustomEvent("rule_drop", {
        detail: {
          from_group_index: source.group_index,
          from_rule_index: source.rule_index,
          to_group_index: target.group_index,
          to_rule_index: target.rule_index,
        },
      }),
    );
  }

  function onFocusInput(event: FocusEvent) {
    const target = event.target as HTMLElement;
    if (!target) return;
    target.closest(".rule")?.setAttribute("draggable", "false");
  }

  function onBlurInput(event: FocusEvent) {
    const target = event.target as HTMLElement;
    if (!target) return;
    target.closest(".rule")?.setAttribute("draggable", "true");
  }
</script>

<div
  class="container rule"
  data-index={rule_index}
  data-group-index={group_index}
  data-uuid={rule_id}
  data-group-uuid={group_id}
  {...rest}
  use:draggable={{
    data: { rule_id, rule_index, group_id, group_index },
    scope: "rule",
    onDrop: handlerDrop,
  }}
  use:droppable={{
    data: { rule_id, rule_index, group_id, group_index },
    scope: "rule",
  }}
>
  <div class="grip" data-index={rule_index} data-group-index={group_index}><Grip /></div>
  <div class="name">
    <div class="label">Name</div>
    <input
      type="text"
      placeholder="rule name..."
      class="table-input"
      bind:value={rule.name}
      onfocus={onFocusInput}
      onblur={onBlurInput}
    />
  </div>
  <div class="type">
    <div class="label">Type</div>
    <Select options={RULE_TYPES} bind:selected={rule.type} onSelectedChange={patternValidation} />
  </div>
  <div class="pattern">
    <div class="label">Pattern</div>
    <input
      type="text"
      placeholder="rule pattern..."
      class="table-input pattern-input"
      bind:value={rule.rule}
      bind:this={input}
      oninput={patternValidation}
      onfocusout={patternValidation}
      onfocus={onFocusInput}
      onblur={onBlurInput}
    />
  </div>
  <div class="actions">
    <Tooltip value="Enable Rule">
      <Switch bind:checked={rule.enable} />
    </Tooltip>
    <Tooltip value="Delete Rule">
      <Button
        small
        onclick={() => onDelete?.(group_index, rule_index)}
        data-index={rule_index}
        data-group-index={group_index}
      >
        <Delete size={20} />
      </Button>
    </Tooltip>
  </div>
</div>

<style>
  .container {
    display: grid;
    grid-template-columns: 1.1rem 2.5fr 1fr 3fr 1fr;
    gap: 0.5rem;
    padding: 0.1rem;
  }

  .rule:global(.dragover) {
    outline: 1px solid var(--accent);
    box-shadow: inset 0 0 5px 0 var(--accent);
  }

  .table-input {
    & {
      border: none;
      background-color: transparent;
      font-size: 1rem;
      font-family: var(--font);
      color: var(--text);
      top: 0.1rem;
      border-bottom: 1px solid transparent;
      width: 100%;
      position: relative;
    }

    &:focus-visible {
      outline: none;
      border-bottom: 1px solid var(--accent);
    }
  }

  .name,
  .type,
  .pattern {
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 0.1rem;
  }

  .actions {
    display: flex;
    align-items: center;
    justify-content: end;
    gap: 0.5rem;
  }

  .grip {
    & {
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: grab;
      color: var(--text-2);
      position: relative;
      top: -0.05rem;
      left: 0.1rem;
    }

    &:hover {
      color: var(--text);
    }
  }

  :global(.pattern-input.invalid),
  :global(.pattern-input.invalid:focus-visible) {
    border-bottom: 1px solid var(--red);
  }

  .label {
    font-size: 0.9rem;
    color: var(--text-2);
    width: 4.2rem;
    text-align: right;
    padding-right: 0.2rem;
    display: none;
  }

  @media (max-width: 700px) {
    .container {
      display: flex;
      flex-direction: row;
      flex-wrap: wrap;
      padding-top: 0.5rem;
    }
    .label {
      display: block;
    }
    .pattern .label,
    .name .label {
      position: relative;
      top: 0.1rem;
      right: 0.3rem;
    }
    .grip {
      display: none;
    }
    .name {
      order: 1;
      width: 100%;
    }
    .pattern {
      order: 2;
      width: 100%;
    }
    .type {
      order: 3;
      margin-right: auto;
    }
    .actions {
      order: 4;
      margin-left: auto;
      justify-content: end;
    }
  }
</style>
