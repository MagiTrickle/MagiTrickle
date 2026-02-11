<script lang="ts">
  import { getContext } from "svelte";

  import Button from "../../../components/ui/Button.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import Switch from "../../../components/ui/Switch.svelte";
  import Tooltip from "../../../components/ui/Tooltip.svelte";
  import { t } from "../../../data/locale.svelte";

  import { Delete, Grip } from "../../../components/ui/icons";
  import { dnd_state, draggable, droppable } from "../../../lib/dnd";
  import { RULE_TYPES, type Rule } from "../../../types";
  import { VALIDATOP_MAP } from "../../../utils/rule-validators";
  import { GROUPS_STORE_CONTEXT, type GroupsStore } from "../groups.svelte";

  type Props = {
    rule: Rule;
    rule_index: number;
    group_index: number;
    rule_id: string;
    group_id: string;
    [key: string]: any;
  };

  let {
    rule = $bindable(),
    rule_index,
    group_index,
    rule_id,
    group_id,
    ...rest
  }: Props = $props();

  const store = getContext<GroupsStore>(GROUPS_STORE_CONTEXT);
  if (!store) {
    throw new Error("GroupsStore context is missing");
  }

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
    store.markDataRevision();
  }

  type DnDTransferData = {
    rule_id: string;
    group_id: string;
    rule_index: number;
    group_index: number;
    name?: string;
    drop_position?: "before" | "after";
  };

  let dropEdge = $state<"before" | "after">("before");

  const dropData = $derived<DnDTransferData>({
    rule_id,
    group_id,
    rule_index,
    group_index,
    drop_position: dropEdge,
  });

  function applyDropEdge(after: boolean) {
    dropEdge = after ? "after" : "before";
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
      dropEdge = "before";
    };
    if (delay) {
      setTimeout(reset, 0);
    } else {
      reset();
    }
  }

  $effect(() => {
    void rule_id;
    void group_id;
    dropEdge = "before";
  });

  function handlerDrop(source: DnDTransferData, target: DnDTransferData) {
    if (source.rule_id === target.rule_id && source.group_id === target.group_id) {
      return;
    }

    store.changeRuleIndex(
      source.group_index,
      source.rule_index,
      target.group_index,
      target.rule_index,
      target.rule_id,
      target.drop_position ?? (target.rule_id ? "before" : "after"),
    );
  }

  function createRuleDragPreview(rowEl: HTMLElement, title: string) {
    const badge = document.createElement("div");
    badge.style.cssText =
      "position:fixed;top:-1000px;left:-1000px;pointer-events:none;z-index:2147483647;transform:translateZ(0);font:600 13px/1.2 var(--font, -apple-system, system-ui, Segoe UI, Roboto, sans-serif);color:var(--text,#e5e7eb);";
    const inner = document.createElement("div");
    inner.style.cssText =
      "display:flex;align-items:center;gap:.5rem;padding:.35rem .6rem;border-radius:.6rem;background:var(--bg-light,rgba(30,30,36,.92));border:1px solid var(--bg-light-extra,rgba(255,255,255,.12));box-shadow:0 6px 18px rgba(0,0,0,.35);backdrop-filter:saturate(120%) blur(6px);";

    const gripClone = rowEl.querySelector(".grip")?.cloneNode(true) as HTMLElement | null;
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
  class="rule no-native-dnd"
  data-index={rule_index}
  data-group-index={group_index}
  data-uuid={rule_id}
  data-group-uuid={group_id}
  data-drop-edge={dropEdge}
  {...rest}
  use:draggable={{
    data: { rule_id, rule_index, group_id, group_index, name: rule.name } as DnDTransferData,
    scope: "rule",
    handle: ".grip",
    effects: { effectAllowed: "move", dropEffect: "move" },
    dragImage: (node) =>
      createRuleDragPreview((node.querySelector(".rule-row") ?? node) as HTMLElement, rule.name),
  }}
  use:droppable={{
    data: dropData,
    scope: "rule",
    canDrop: (src, tgt) => src.rule_id !== tgt.rule_id || src.group_id !== tgt.group_id,
    dropEffect: "move",
    onDrop: handlerDrop,
  }}
>
  <div
    class="rule-row"
    role="presentation"
    ondragenter={updateDropIntent}
    ondragover={updateDropIntent}
    ondragleave={() => resetDropIntent()}
    ondrop={() => resetDropIntent(true)}
  >
    <div class="grip" data-index={rule_index} data-group-index={group_index} title={t("Drag Rule")}>
      <Grip />
    </div>
    <div class="name">
      <div class="label">{t("Name")}</div>
      <input
        type="text"
        placeholder={t("rule name...")}
        class="table-input"
        bind:value={rule.name}
      />
    </div>
    <div class="type">
      <div class="label">{t("Type")}</div>
      <Select options={RULE_TYPES} bind:selected={rule.type} onValueChange={patternValidation} />
    </div>
    <div class="pattern">
      <div class="label">{t("Pattern")}</div>
      <input
        type="text"
        placeholder={t("rule pattern...")}
        class="table-input pattern-input"
        bind:value={rule.rule}
        bind:this={input}
        oninput={patternValidation}
        onfocusout={patternValidation}
      />
    </div>
    <div class="actions">
      <Tooltip value={t(rule.enable ? "Disable Rule" : "Enable Rule")}>
        <Switch bind:checked={rule.enable} />
      </Tooltip>
      <Tooltip value={t("Delete Rule")}>
        <Button
          small
          onclick={() => store.deleteRuleFromGroup(group_index, rule_index)}
          data-index={rule_index}
          data-group-index={group_index}
        >
          <Delete size={20} />
        </Button>
      </Tooltip>
    </div>
  </div>
</div>

<style>
  .rule {
    display: block;
  }

  .rule-row {
    display: grid;
    grid-template-columns: 1.1rem 2.5fr 1fr 3fr 1fr;
    gap: 0.5rem;
    padding: 0.1rem;
    background: inherit;
    border-radius: inherit;
  }

  .rule:global(.dragover) {
    outline: 1px solid var(--accent);
    box-shadow: inset 0 0 0 2px color-mix(in oklab, var(--accent) 50%, transparent);
    border-radius: 10px;
  }

  .table-input {
    border: none;
    background-color: transparent;
    font-size: 1rem;
    font-family: var(--font);
    color: var(--text);
    border-bottom: 1px solid transparent;
    width: 100%;
  }
  .table-input:focus-visible {
    outline: none;
    border-bottom: 1px solid var(--accent);
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
  }
  .grip:hover {
    color: var(--text);
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
    .rule-row {
      display: grid;
      grid-template-columns: minmax(0, 1fr) auto;
      column-gap: 0.4rem;
      row-gap: 0.35rem;
      padding: 0.5rem 0.35rem 0.45rem;
      align-items: start;
    }
    .label {
      display: block;
    }
    .name,
    .type,
    .pattern {
      display: grid;
      grid-template-columns: 3.2rem minmax(0, 1fr);
      align-items: center;
      gap: 0.35rem;
      padding: 0.05rem 0;
      grid-column: 1;
    }
    .name .label,
    .pattern .label,
    .type .label {
      justify-self: end;
      text-align: right;
      position: static;
    }
    .name .table-input,
    .pattern .table-input {
      width: 100%;
      min-width: 0;
    }
    .type :global([data-select-trigger]) {
      justify-content: flex-start;
    }
    .grip {
      display: none;
    }
    .actions {
      grid-column: 2;
      grid-row: 1 / span 3;
      display: flex;
      flex-direction: row;
      gap: 0.35rem;
      justify-content: center;
      align-items: center;
      align-self: stretch;
    }
  }
</style>
