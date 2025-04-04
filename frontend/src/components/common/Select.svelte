<script lang="ts">
  import { Select } from "bits-ui";
  import { Check, SelectOpen } from "./icons";

  type Props = {
    options: {
      value: string;
      label: string;
    }[];
    selected: string;
    onValueChange?: (selected: string) => void;
    [key: string]: any;
  };

  let { options, selected = $bindable(), onValueChange, ...rest }: Props = $props();
  // const selected_label = $derived(
  //   selected ? options.find((option) => option.value === selected)?.label : selected,
  // );
</script>

<div class="container" {...rest}>
  <Select.Root type="single" {onValueChange} items={options} bind:value={selected}>
    <Select.Trigger aria-label="...">
      <div class="selected">
        <div class="selected-value">
          {selected}
        </div>
        <div class="selected-open">
          <SelectOpen size={16} />
        </div>
      </div>
    </Select.Trigger>
    <Select.Content>
      {#each options as option}
        <Select.Item value={option.value} label={option.label}>
          {#snippet children({ selected })}
            <div class="option">
              <div class="option-label">
                {option.label}
              </div>
              <div class="option-check">
                {#if selected}
                  <Check size={16} />
                {/if}
              </div>
            </div>
          {/snippet}
        </Select.Item>
      {/each}
    </Select.Content>
  </Select.Root>
</div>

<style>
  .container {
    display: flex;
    width: 100%;
  }

  :global {
    [data-select-root] {
      width: 100%;
      z-index: 100;
    }

    [data-select-trigger] {
      & {
        display: inline-flex;
        align-items: center;
        justify-content: space-between;

        background: transparent;
        border: none;
        border-radius: 0.5rem;
        padding: 0.2rem 0.3rem 0.2rem 0.3rem;
        font-size: 1rem;
        font-weight: 400;
        font-family: var(--font);
        color: var(--text);
        width: 100%;
        height: fit-content;
      }

      &:hover {
        background-color: var(--bg-dark);
        outline: 1px solid var(--bg-light-extra);
      }

      &:focus {
        outline: none;
        background-color: var(--bg-light-extra);
      }
    }

    [data-select-content] {
      display: flex;
      flex-direction: column;
      gap: 0.1rem;
      padding: 0.2rem;
      background-color: var(--bg-dark-extra);
      border-radius: 0.5rem;
      border: 1px solid var(--bg-light-extra);
      box-shadow: var(--shadow-popover);
      z-index: 100;
    }

    [data-select-item] {
      & {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 0.1rem;
        padding-left: 0.2rem;
        padding-right: 0.2rem;
        border-radius: 0.2rem;
        cursor: default;
      }

      &:hover {
        background-color: var(--bg-light-extra);
      }
    }

    [data-select-value] {
      display: flex;
      align-items: center;
      justify-content: center;
      position: relative;
      top: 0.1rem;
    }
  }

  .option {
    display: inline-flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.2rem;
    padding: 0.1rem;
    width: 100%;
  }

  .option-check {
    color: var(--text-2);
    width: 16px;
    height: 16px;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .selected {
    display: inline-flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.2rem;
    width: 100%;
  }

  .selected-value {
    padding-left: 0.3rem;
  }

  .selected-open {
    color: var(--text-2);
    width: 16px;
    height: 16px;
    display: flex;
    align-items: center;
    justify-content: center;
  }
</style>
