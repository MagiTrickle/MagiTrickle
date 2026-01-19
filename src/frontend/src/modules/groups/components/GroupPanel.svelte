<script lang="ts">
  import { Collapsible } from "bits-ui";
  import { slide } from "svelte/transition";
  import { createEventDispatcher } from "svelte";

  import { type Group, type Rule } from "../../../types";
  import { defaultRule } from "../../../utils/defaults";
  import { interfaces } from "../../../data/interfaces.svelte";
  import { t } from "../../../data/locale.svelte";
  import { droppable, draggable } from "../../../lib/dnd";
  import Button from "../../../components/ui/Button.svelte";
  import DropdownMenu from "../../../components/ui/DropdownMenu.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import Switch from "../../../components/ui/Switch.svelte";
  import Tooltip from "../../../components/ui/Tooltip.svelte";
  import {
    Delete,
    Add,
    GroupExpand,
    GroupCollapse,
    Dots,
    ImportList,
    Grip,
  } from "../../../components/ui/icons";
  import RuleRow from "./RuleRow.svelte";

  type Props = {
    group: Group;
    group_index: number;
    total_groups: number;
    open: boolean;
    deleteGroup: (index: number) => void;
    addRuleToGroup: (group_index: number, rule: Rule, focus?: boolean) => void;
    deleteRuleFromGroup: (group_index: number, rule_index: number) => void;
    changeRuleIndex: (
      from_group_index: number,
      from_rule_index: number,
      to_group_index: number,
      to_rule_index: number,
    ) => void;
    searchActive?: boolean;
    visibleRuleIndices?: number[] | null;
    onFinished?: () => void;
    [key: string]: any;
  };

  let {
    group = $bindable(),
    group_index,
    total_groups = $bindable(),
    open = $bindable(),
    deleteGroup,
    addRuleToGroup,
    deleteRuleFromGroup,
    changeRuleIndex,
    searchActive = false,
    visibleRuleIndices = null,
    onFinished,
    ...rest
  }: Props = $props();

  const dispatch = createEventDispatcher();

  let client_width = $state<number>(Infinity);
  let is_desktop = $derived(client_width > 668);

  let effectiveOpen = $derived(open);

  function toggleOpen() {
    open = !open;
  }

  type GroupDnD = {
    group_id: string;
    group_index: number;
    name: string;
    color: string;
    count: number;
  };

  function createGroupDragPreview(
    headerEl: HTMLElement,
    name: string,
    color: string,
    count: number,
  ) {
    const badge = document.createElement("div");
    badge.style.cssText =
      "position:fixed;top:-1000px;left:-1000px;pointer-events:none;z-index:2147483647;transform:translateZ(0);font:600 13px/1.2 var(--font, -apple-system, system-ui, Segoe UI, Roboto, sans-serif);color:var(--text,#e5e7eb);";

    const inner = document.createElement("div");
    inner.style.cssText =
      "display:flex;align-items:center;gap:.55rem;padding:.42rem .7rem;border-radius:.7rem;background:var(--bg-light,rgba(30,30,36,.92));border:1px solid var(--bg-light-extra,rgba(255,255,255,.12));box-shadow:0 6px 18px rgba(0,0,0,.35);backdrop-filter:saturate(120%) blur(6px);";

    const colorBadge = document.createElement("span");
    colorBadge.style.cssText =
      "display:inline-block;width:10px;height:10px;border-radius:999px;box-shadow:0 0 0 1px rgba(255,255,255,.25) inset;";
    colorBadge.style.background = color || "#888";
    inner.appendChild(colorBadge);

    const title = document.createElement("span");
    title.textContent = name || "group";
    title.style.cssText =
      "max-width:240px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;";
    inner.appendChild(title);

    const cnt = document.createElement("span");
    cnt.textContent = `• ${count}`;
    cnt.style.opacity = "0.8";
    inner.appendChild(cnt);

    const gripClone = headerEl.querySelector(".group-grip")?.cloneNode(true) as HTMLElement | null;
    if (gripClone) {
      gripClone.style.cssText += "opacity:.9;display:flex;align-items:center;margin-left:.25rem;";
      inner.appendChild(gripClone);
    }

    badge.appendChild(inner);
    document.body.appendChild(badge);
    return badge;
  }

  let filteredRuleIndicesSet = $derived(
    Array.isArray(visibleRuleIndices) ? new Set(visibleRuleIndices) : null,
  );
  let displayedRulesCount = $state(0);

  $effect(() => {
    displayedRulesCount = Array.isArray(visibleRuleIndices)
      ? visibleRuleIndices.length
      : group.rules.length;
  });

  let renderLimit = $state(20);
  let renderTimeout: number | null = null;

  function scheduleNext() {
    if (typeof window === "undefined") return;
    if (renderTimeout) return;
    if (renderLimit >= group.rules.length) return;

    renderTimeout = window.setTimeout(() => {
      renderTimeout = null;
      if (searchActive) {
        renderLimit = group.rules.length;
        return;
      }
      renderLimit = Math.min(renderLimit + 15, group.rules.length);
      scheduleNext();
    }, 60);
  }

  $effect(() => {
    if (searchActive) {
      renderLimit = group.rules.length;
      if (renderTimeout) {
        clearTimeout(renderTimeout);
        renderTimeout = null;
      }
    } else {
      scheduleNext();
    }
  });

  $effect(() => {
    group.rules.length;
    scheduleNext();
  });

  let reportedFinished = false;
  $effect(() => {
    if (searchActive) return;
    if (!reportedFinished && renderLimit >= group.rules.length) {
      reportedFinished = true;
      onFinished?.();
    }
  });
</script>

<svelte:window bind:innerWidth={client_width} />

<div
  class="group"
  role="listitem"
  data-uuid={group.id}
  use:draggable={{
    data: {
      group_id: group.id,
      group_index,
      name: group.name,
      color: group.color,
      count: group.rules.length,
    } as GroupDnD,
    scope: "group",
    handle: ".group-grip",
    effects: { effectAllowed: "move", dropEffect: "move" },
    dragImage: (node) =>
      createGroupDragPreview(
        (node.querySelector(".group-header") ?? node) as HTMLElement,
        group.name,
        group.color || "",
        group.rules.length,
      ),
  }}
>
  <Collapsible.Root open={effectiveOpen} onOpenChange={toggleOpen}>
    <div
      class="group-header"
      data-group-index={group_index}
      use:droppable={{
        data: { rule_id: "", rule_index: 0, group_id: group.id, group_index },
        scope: "rule",
        canDrop: (src) => src.group_id === group.id,
      }}
    >
      <div class="group-left">
        <label class="group-color" style="background: {group.color}">
          <input type="color" bind:value={group.color} />
        </label>

        <div class="group-grip" title={t("Drag Group")}>
          <Grip />
        </div>

        <input
          type="text"
          placeholder={t("group name...")}
          class="group-name"
          bind:value={group.name}
        />
      </div>

      <div class="group-actions">
        <Select
          options={interfaces.list.map((item) => ({ value: item, label: item }))}
          bind:selected={group.interface}
        />

        <Tooltip value={t(group.enable ? "Disable Group" : "Enable Group")}>
          <Switch class="enable-group" bind:checked={group.enable} />
        </Tooltip>

        {#if is_desktop}
          <Tooltip value={t("Delete Group")}>
            <Button small onclick={() => deleteGroup(group_index)}>
              <Delete size={20} />
            </Button>
          </Tooltip>
          <Tooltip value={t("Add Rule")}>
            <Button
              small
              onclick={() => {
                addRuleToGroup(group_index, defaultRule(), true);
                open = true;
              }}
            >
              <Add size={20} />
            </Button>
          </Tooltip>
          <Tooltip value={t("Import Rule List")}>
            <Button small onclick={() => dispatch("importRules")}>
              <ImportList size={20} />
            </Button>
          </Tooltip>
        {:else}
          <DropdownMenu>
            {#snippet trigger()}
              <Dots size={20} />
            {/snippet}
            {#snippet item1()}
              <Button
                general
                onclick={() => {
                  addRuleToGroup(group_index, defaultRule(), true);
                  open = true;
                }}
              >
                <div class="dd-icon"><Add size={20} /></div>
                <div class="dd-label">{t("Add Rule")}</div>
              </Button>
            {/snippet}
            {#snippet item2()}
              <Button general onclick={() => dispatch("importRules")}>
                <div class="dd-icon"><ImportList size={20} /></div>
                <div class="dd-label">{t("Import Rule List")}</div>
              </Button>
            {/snippet}
            {#snippet item3()}
              <Button general onclick={() => deleteGroup(group_index)}>
                <div class="dd-icon"><Delete size={20} /></div>
                <div class="dd-label">{t("Delete Group")}</div>
              </Button>
            {/snippet}
          </DropdownMenu>
        {/if}

        <Tooltip value={t(effectiveOpen ? "Collapse Group" : "Expand Group")}>
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
        {#if group.rules.length > 0}
          <div class="group-rules-header">
            <div class="group-rules-header-column total">
              #{displayedRulesCount}
            </div>
            <div class="group-rules-header-column">{t("Name")}</div>
            <div class="group-rules-header-column">{t("Type")}</div>
            <div class="group-rules-header-column">{t("Pattern")}</div>
            <div class="group-rules-header-column">{t("Enabled")}</div>
          </div>
        {/if}
        <div class="group-rules">
          {#if group.rules.length > 0}
            {#each group.rules.slice(0, renderLimit) as rule, rule_index (rule.id)}
              {@const isVisible =
                !searchActive ||
                filteredRuleIndicesSet === null ||
                filteredRuleIndicesSet.has(rule_index)}
              <div style={isVisible ? "" : "display: none"}>
                <RuleRow
                  key={rule.id}
                  bind:rule={group.rules[rule_index]}
                  {rule_index}
                  {group_index}
                  rule_id={rule.id}
                  group_id={group.id}
                  onChangeIndex={changeRuleIndex}
                  onDelete={deleteRuleFromGroup}
                  style={rule_index % 2 ? "" : "background-color: var(--bg-light)"}
                />
              </div>
            {/each}
          {/if}
        </div>
      </div>
    </Collapsible.Content>
  </Collapsible.Root>
</div>

<style>
  .group {
    & {
      background-color: var(--bg-medium);
      border-radius: 0.5rem;
      border: 1px solid var(--bg-light-extra);
      transition:
        transform 0.12s ease,
        opacity 0.12s ease,
        box-shadow 0.12s ease;
    }
  }

  .group-header {
    & {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 0.5rem;
      border-radius: 0.5rem;
      background-color: var(--bg-light);
      position: relative;
    }

    &:global(.dragover) {
      outline: 1px solid var(--accent);
      box-shadow: inset 0 0 5px 0 var(--accent);
    }
  }

  .group-left {
    display: flex;
    align-items: center;
    gap: 0.4rem;
  }

  .group-color {
    & {
      display: inline-block;
      width: 2rem;
      height: calc(100% + 1px);
      border-top-left-radius: 0.5rem;
      border-bottom-left-radius: 0.5rem;
      position: absolute;
      left: 0px;
      top: -1px;
      overflow: hidden;
      cursor: pointer;
    }

    & input {
      margin-left: 0.5rem;
    }
  }

  /* Ручка групп */
  .group-grip {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    margin-left: 2.2rem;
    color: var(--text-2);
    cursor: grab;
    user-select: none;
    -webkit-user-select: none;
    -webkit-user-drag: none;
  }
  .group-grip:hover {
    color: var(--text);
  }

  .group-name {
    & {
      border: none;
      background-color: transparent;
      font-size: 1.3rem;
      font-weight: 600;
      font-family: var(--font);
      color: var(--text);
      border-bottom: 1px solid transparent;
      position: relative;
      top: 0.1rem;
      margin-left: 0.4rem;
    }

    &:focus-visible {
      outline: none;
      border-bottom: 1px solid var(--accent);
    }
  }

  .group-actions {
    & {
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 0.2rem;
    }
    &:global([data-switch-root]) {
      margin: 0 0.3rem;
    }
  }

  .group-rules-header {
    display: grid;
    grid-template-columns: 4rem 2.1fr 1fr 3fr 1fr;
    justify-content: center;
    align-items: center;

    font-size: 0.9rem;
    color: var(--text-2);
    padding-top: 0.6rem;
    padding-bottom: 0.2rem;
    border-bottom: 1px solid var(--bg-light-extra);
  }

  .group-rules-header-column {
    & {
      display: flex;
      align-items: center;
      justify-content: center;
    }

    &.total {
      justify-content: start;
      margin-left: 0.5rem;
    }

    &.total :global(svg) {
      position: relative;
      top: -1px;
    }
  }

  :global {
    [data-collapsible-trigger] {
      & {
        color: var(--text-2);
        background-color: transparent;
        border: 1px solid transparent;
        display: inline-flex;
        align-items: center;
        justify-content: center;
        padding: 0.4rem;
        border-radius: 0.5rem;
        cursor: pointer;
      }

      &:hover {
        background-color: var(--bg-dark);
        color: var(--text);
        border: 1px solid var(--bg-light-extra);
      }
    }
  }

  input[type="color"] {
    -webkit-appearance: none;
    -moz-appearance: none;
    appearance: none;
    background: transparent;
    width: auto;
    height: 0;
    padding: 0;
    border: none;
    cursor: pointer;
  }

  @media (max-width: 700px) {
    .group-header {
      display: flex;
      flex-direction: column;
      align-items: start;
      justify-content: center;
    }

    .group-left {
      & {
        width: 100%;
      }
      & input[type="text"] {
        width: calc(100% - 2rem);
        margin-left: 2rem;
      }
      & label {
        height: calc(100% + 1px);
      }
    }

    .group-grip {
      display: none;
    }

    .group-actions {
      width: calc(100% - 2rem);
      justify-content: stretch;
      gap: 0.25rem;
      margin-left: 2rem;
    }

    :global(.group-actions > *:nth-child(1)) {
      margin-right: auto;
      width: 150px;
      min-width: 140px;
      flex: 1 1 auto;
    }

    :global(.group-actions > *:nth-child(2)) {
      margin-left: auto;
    }

    .group-rules-header {
      height: 1px;
      & .group-rules-header-column {
        display: none;
      }
    }
  }
</style>
