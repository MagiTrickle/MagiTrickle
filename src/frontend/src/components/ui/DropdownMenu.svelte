<script lang="ts">
  import { DropdownMenu } from "bits-ui";
  import type { Snippet } from "svelte";

  type Props = {
    trigger: Snippet;
    [key: string]: Snippet;
  };
  let { trigger, ...rest }: Props = $props();

  const items = $derived(
    Object.entries(rest)
      .filter(([name, f]) => typeof f === "function" && name.startsWith("item"))
      .map(([, f]) => f),
  );
</script>

<DropdownMenu.Root>
  <DropdownMenu.Trigger>{@render trigger()}</DropdownMenu.Trigger>
  <DropdownMenu.Content alignOffset={0} align="end">
    {#each items as item, index}
      <DropdownMenu.Item>
        {@render item()}
      </DropdownMenu.Item>
    {/each}
  </DropdownMenu.Content>
</DropdownMenu.Root>

<style>
  :global {
    [data-dropdown-menu-trigger] {
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

    [data-dropdown-menu-content] {
      padding: 0.2rem;
      background-color: var(--bg-dark-extra);
      border-radius: 0.5rem;
      border: 1px solid var(--bg-light-extra);
      box-shadow: var(--shadow-popover);
      z-index: 100;
      min-width: 160px;
    }

    [data-dropdown-menu-item] {
      & {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 0.1rem;

        border-radius: 0.2rem;
        cursor: default;
      }
    }

    .dd-icon {
      width: 30px;
      color: var(--text-2);
      display: flex;
      align-items: center;
      justify-content: center;
    }

    .dd-label {
      position: relative;
      top: 2px;
      margin-right: auto;
    }

    .dd-check {
      display: flex;
      align-items: center;
      justify-content: center;
    }
  }
</style>
