<script lang="ts">
  import Button from "../../../components/ui/Button.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import Switch from "../../../components/ui/Switch.svelte";
  import Tooltip from "../../../components/ui/Tooltip.svelte";
  import { t } from "../../../data/locale.svelte";

  import { Delete, Grip } from "../../../components/ui/icons";
  import { dnd_state, draggable, droppable } from "../../../lib/dnd";
  import { RULE_TYPES, type SubscriptionRule } from "../../../types";
  import { VALIDATOP_MAP } from "../../../utils/rule-validators";

  type Props = {
    rule: SubscriptionRule;
    rule_index: number;
    subscription_index: number;
    onDelete?: (subscription_index: number, rule_index: number) => void;
    onChangeIndex?: (
      from_sub_index: number,
      from_rule_index: number,
      to_sub_index: number,
      to_rule_index: number,
      to_rule_id: string,
      insert?: "before" | "after",
    ) => void;
    [key: string]: any;
  };

  let {
    rule = $bindable(),
    rule_index,
    subscription_index,
    onDelete,
    onChangeIndex,
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
    subscription_index: number;
    rule_index: number;
    name?: string;
    drop_position?: "before" | "after";
  };

  const dropData: DnDTransferData = {
    rule_id: rule.id,
    // svelte-ignore state_referenced_locally
    subscription_index,
    // svelte-ignore state_referenced_locally
    rule_index,
    drop_position: "before",
  };

  let dropEdge = $state<"before" | "after">("before");

  function applyDropEdge(after: boolean) {
    const edge = after ? "after" : "before";
    dropData.drop_position = edge;
    dropEdge = edge;
  }

  function updateDropIntent(event: DragEvent) {
    if (dnd_state.source_scope !== "rule") return;
    const target = event.currentTarget as HTMLElement | null;
    if (!target) return;
    const rect = target.getBoundingClientRect();
    const after = event.clientY - rect.top > rect.height / 2;
    applyDropEdge(after);
  }

  function resetDropIntent(delay = false) {
    const reset = () => {
      dropData.drop_position = "before";
      dropEdge = "before";
    };
    if (delay) {
      setTimeout(reset, 0);
    } else {
      reset();
    }
  }

  $effect(() => {
    dropData.rule_id = rule.id;
    dropData.subscription_index = subscription_index;
    dropData.rule_index = rule_index;
    dropData.drop_position = "before";
    dropEdge = "before";
  });

  function handlerDrop(source: DnDTransferData, target: DnDTransferData) {
    if (
      source.rule_id === target.rule_id &&
      source.subscription_index === target.subscription_index
    ) {
      return;
    }

    onChangeIndex?.(
      source.subscription_index,
      source.rule_index,
      target.subscription_index,
      target.rule_index,
      target.rule_id,
      target.drop_position ?? "before",
    );
  }

  function createRuleDragPreview(rowEl: HTMLElement, title: string) {
    const badge = document.createElement("div");
    badge.style.cssText =
      "position:fixed;top:-1000px;left:-1000px;pointer-events:none;z-index:2147483647;transform:translateZ(0);font:600 13px/1.2 var(--font, -apple-system, system-ui, Segoe UI, Roboto, sans-serif);color:var(--text,#e5e7eb);";
    const inner = document.createElement("div");
    inner.style.cssText =
      "display:flex;align-items:center;gap:.5rem;padding:.35rem .6rem;border-radius:.6rem;background:var(--bg-light,rgba(30,30,36,.92));border:1px solid var(--bg-light-extra,rgba(255,255,255,.12));box-shadow:0 6px 18px rgba(0,0,0,.35);backdrop-filter:saturate(120%) blur(6px);";

    const gripClone = rowEl
      .querySelector(".subscription-rule-grip")
      ?.cloneNode(true) as HTMLElement | null;
    if (gripClone) {
      gripClone.style.cssText += "opacity:.85;display:flex;align-items:center;";
      inner.appendChild(gripClone);
    } else {
      const dots = document.createElement("span");
      dots.textContent = "⋮⋮";
      (dots.style as any).letterSpacing = "2px";
      dots.style.opacity = "0.85";
      inner.appendChild(dots);
    }

    const label = document.createElement("span");
    label.textContent = title || "rule";
    label.style.cssText =
      "max-width:260px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;";
    inner.appendChild(label);

    badge.appendChild(inner);
    document.body.appendChild(badge);
    return badge;
  }
</script>

<div
  class="subscription-rule"
  data-index={rule_index}
  data-subscription-index={subscription_index}
  data-drop-edge={dropEdge}
  {...rest}
  use:draggable={{
    data: { rule_id: rule.id, rule_index, subscription_index, name: rule.rule } as DnDTransferData,
    scope: "subscription-rule",
    handle: ".subscription-rule-grip",
    effects: { effectAllowed: "move", dropEffect: "move" },
    dragImage: (node) =>
      createRuleDragPreview(
        (node.querySelector(".subscription-rule-row") ?? node) as HTMLElement,
        rule.rule,
      ),
  }}
  use:droppable={{
    data: dropData,
    scope: "subscription-rule",
    canDrop: (src, tgt) =>
      src.rule_id !== tgt.rule_id || src.subscription_index !== tgt.subscription_index,
    dropEffect: "move",
    onDrop: handlerDrop,
  }}
>
  <div
    class="subscription-rule-row"
    role="presentation"
    ondragenter={updateDropIntent}
    ondragover={updateDropIntent}
    ondragleave={() => resetDropIntent()}
    ondrop={() => resetDropIntent(true)}
  >
    <div
      class="subscription-rule-grip"
      data-index={rule_index}
      data-subscription-index={subscription_index}
      title={t("Drag Rule")}
    >
      <Grip />
    </div>
    <div class="subscription-rule-type">
      <div class="label">{t("Type")}</div>
      <Select options={RULE_TYPES} bind:selected={rule.type} onValueChange={patternValidation} />
    </div>
    <div class="subscription-rule-pattern">
      <div class="label">{t("Pattern")}</div>
      <input
        type="text"
        placeholder={t("rule pattern...")}
        class="subscription-rule-input pattern-input"
        bind:value={rule.rule}
        bind:this={input}
        oninput={patternValidation}
        onfocusout={patternValidation}
      />
    </div>
    <div class="subscription-rule-actions">
      <Tooltip value={t(rule.enable ? "Disable Rule" : "Enable Rule")}>
        <Switch bind:checked={rule.enable} />
      </Tooltip>
      <Tooltip value={t("Delete Rule")}>
        <Button
          small
          onclick={() => onDelete?.(subscription_index, rule_index)}
          data-index={rule_index}
          data-subscription-index={subscription_index}
        >
          <Delete size={20} />
        </Button>
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
    grid-template-columns: 1rem 1fr 4fr 1fr;
    gap: 0.5rem;
    padding: 0.1rem;
    background: inherit;
    border-radius: inherit;
  }

  .subscription-rule:global(.dragover) {
    outline: 1px solid var(--accent);
    box-shadow: inset 0 0 0 2px color-mix(in oklab, var(--accent) 50%, transparent);
    border-radius: 10px;
  }

  .subscription-rule-input {
    border: none;
    background-color: transparent;
    font-size: 1rem;
    font-family: var(--font);
    color: var(--text);
    border-bottom: 1px solid transparent;
    width: 100%;

    &:focus-visible {
      outline: none;
      border-bottom: 1px solid var(--accent);
    }
  }

  .subscription-rule-type,
  .subscription-rule-pattern {
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 0.1rem;
  }

  .subscription-rule-actions {
    display: flex;
    align-items: center;
    justify-content: end;
    gap: 0.5rem;
  }

  .subscription-rule-grip {
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: grab;
    color: var(--text-2);
    position: relative;
    left: 0.1rem;
    -webkit-user-drag: none;
    user-select: none;
    -webkit-user-select: none;

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
    width: 3.2rem;
    text-align: right;
    padding-right: 0.2rem;
    display: none;
  }

  :global(html.dnd-possible),
  :global(html.dnd-possible *) {
    user-select: none;
    -webkit-user-select: none;
  }

  :global(html.dnd-dragging),
  :global(html.dnd-dragging *) {
    user-select: none;
    -webkit-user-select: none;
    cursor: grabbing !important;
  }

  @media (max-width: 700px) {
    .subscription-rule-row {
      grid-template-columns: minmax(0, 1fr) auto;
      column-gap: 0.4rem;
      row-gap: 0.35rem;
      padding: 0.5rem 0.35rem 0.45rem;
      align-items: start;
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
    .subscription-rule-pattern .subscription-rule-input {
      width: 100%;
      min-width: 0;
    }
    .subscription-rule-type :global([data-select-trigger]) {
      justify-content: flex-start;
    }
    .subscription-rule-grip {
      display: none;
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
