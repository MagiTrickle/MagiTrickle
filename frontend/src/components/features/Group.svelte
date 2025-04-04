<script lang="ts">
  import { Collapsible } from "bits-ui";
  import { slide } from "svelte/transition";
  import { InfiniteLoader } from "svelte-infinite";

  import { type Group, type Rule } from "../../types";
  import { droppable } from "../actions/dnd";
  import { defaultRule } from "../../utils/defaults";
  import { INTERFACES } from "../../data/interfaces.svelte";
  import {
    Delete,
    Add,
    GroupCollapse,
    MoveUp,
    MoveDown,
    Dots,
    Check,
    Toggle,
  } from "../common/icons";
  import Switch from "../common/Switch.svelte";
  import Tooltip from "../common/Tooltip.svelte";
  import RuleComponent from "../features/Rule.svelte";
  import Button from "../common/Button.svelte";
  import Select from "../common/Select.svelte";
  import DropdownMenu from "../common/DropdownMenu.svelte";

  type Props = {
    group: Group;
    group_index: number;
    total_groups: number;
    showed_limit: number;
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
    groupMoveUp: (group_index: number) => void;
    groupMoveDown: (group_index: number) => void;
    loadMore: (group_index: number) => Promise<void>;
    [key: string]: any;
  };

  let {
    group = $bindable(),
    group_index,
    total_groups = $bindable(),
    showed_limit = $bindable(),
    open = $bindable(),
    deleteGroup,
    addRuleToGroup,
    deleteRuleFromGroup,
    changeRuleIndex,
    groupMoveUp,
    groupMoveDown,
    loadMore,
    ...rest
  }: Props = $props();

  let client_width = $state<number>(Infinity);
  let is_desktop = $derived(client_width > 668);
</script>

<svelte:window bind:innerWidth={client_width} />

<div class="group" data-uuid={group.id}>
  <Collapsible.Root bind:open>
    <div
      class="group-header"
      data-group-index={group_index}
      use:droppable={{
        data: { rule_id: "", rule_index: 0, group_id: group.id, group_index },
        scope: "rule",
      }}
    >
      <div class="group-left">
        <label class="group-color" style="background: {group.color}">
          <input type="color" bind:value={group.color} />
        </label>
        <input type="text" placeholder="group name..." class="group-name" bind:value={group.name} />
      </div>
      <div class="group-actions">
        <Select
          options={INTERFACES.map((item) => ({ value: item, label: item }))}
          bind:selected={group.interface}
        />

        {#if is_desktop}
          <Tooltip value="Enable Group">
            <Switch class="enable-group" bind:checked={group.enable} />
          </Tooltip>
          <Tooltip value="Delete Group">
            <Button small onclick={() => deleteGroup(group_index)}>
              <Delete size={20} />
            </Button>
          </Tooltip>
          <Tooltip value="Add Rule">
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
          <Tooltip value="Move Up">
            <Button small inactive={group_index === 0} onclick={() => groupMoveUp(group_index)}>
              <MoveUp size={20} />
            </Button>
          </Tooltip>
          <Tooltip value="Move Down">
            <Button
              small
              inactive={group_index === total_groups - 1}
              onclick={() => groupMoveDown(group_index)}
            >
              <MoveDown size={20} />
            </Button>
          </Tooltip>
        {:else}
          <DropdownMenu>
            {#snippet trigger()}
              <Dots size={20} />
            {/snippet}
            {#snippet item1()}
              <Button general onclick={() => (group.enable = !group.enable)}>
                <div class="dd-icon"><Toggle size={20} /></div>
                <div class="dd-label">Enable Group</div>
                <div class="dd-check">
                  {#if group.enable}
                    <Check size={16} />
                  {/if}
                </div>
              </Button>
            {/snippet}
            {#snippet item2()}
              <Button general onclick={() => deleteGroup(group_index)}>
                <div class="dd-icon"><Delete size={20} /></div>
                <div class="dd-label">Delete Group</div>
              </Button>
            {/snippet}
            {#snippet item3()}
              <Button
                general
                onclick={() => {
                  addRuleToGroup(group_index, defaultRule(), true);
                  open = true;
                }}
              >
                <div class="dd-icon"><Add size={20} /></div>
                <div class="dd-label">Add Rule</div>
              </Button>
            {/snippet}
            {#snippet item4()}
              <Button general inactive={group_index === 0} onclick={() => groupMoveUp(group_index)}>
                <div class="dd-icon"><MoveUp size={20} /></div>
                <div class="dd-label">Move Up</div>
              </Button>
            {/snippet}
            {#snippet item5()}
              <Button
                general
                inactive={group_index === total_groups - 1}
                onclick={() => groupMoveDown(group_index)}
              >
                <div class="dd-icon"><MoveDown size={20} /></div>
                <div class="dd-label">Move Down</div>
              </Button>
            {/snippet}
          </DropdownMenu>
        {/if}

        <Tooltip value="Collapse Group">
          <Collapsible.Trigger>
            <GroupCollapse />
          </Collapsible.Trigger>
        </Tooltip>
      </div>
    </div>

    <Collapsible.Content>
      <div transition:slide>
        {#if group.rules.length > 0}
          <div class="group-rules-header">
            <div class="group-rules-header-column total">
              #{group.rules.length}
            </div>
            <div class="group-rules-header-column">Name</div>
            <div class="group-rules-header-column">Type</div>
            <div class="group-rules-header-column">Pattern</div>
            <div class="group-rules-header-column">Enabled</div>
            <div></div>
          </div>
        {/if}
        <div class="group-rules">
          <InfiniteLoader triggerLoad={() => loadMore(group_index)} loopDetectionTimeout={10}>
            {#each group.rules.slice(0, showed_limit) as rule, rule_index (rule.id)}
              <RuleComponent
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
            {/each}
          </InfiniteLoader>
        </div>
      </div>
    </Collapsible.Content>
  </Collapsible.Root>
</div>

<style>
  .group {
    & {
      margin-bottom: 1rem;
      background-color: var(--bg-medium);
      border-radius: 0.5rem;
      border: 1px solid var(--bg-light-extra);
    }
    &:last-child {
      margin-bottom: 0;
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
    gap: 0.5rem;
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
      margin-left: 2rem;
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
        border: none;
        display: inline-flex;
        align-items: center;
        justify-content: center;
        padding: 0.2rem;
        border-radius: 0.5rem;
        cursor: pointer;
      }

      &:hover {
        background-color: var(--bg-dark);
        outline: 1px solid var(--bg-light-extra);
        color: var(--text);
      }
    }
    .infinite-intersection-target {
      padding-block: 0 !important;
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
        margin-left: 2.5rem;
      }
      & label {
        height: calc(100% + 1px);
      }
    }

    .group-actions {
      width: calc(100% - 2rem);
      justify-content: end;
      margin-left: 2rem;
    }

    :global(.group-actions > *:nth-child(1)) {
      margin-right: auto;
      width: 150px;
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
