<script lang="ts">
  import { Switch } from "bits-ui";

  type Props = {
    checked: boolean;
    [key: string]: any;
  };

  let { checked = $bindable(false), ...rest }: Props = $props();
</script>

<Switch.Root bind:checked {...rest}>
  <Switch.Thumb />
</Switch.Root>

<style>
  :global {
    [data-switch-root] {
      & {
        height: 18px;
        min-height: 18px;
        width: 32px;
        flex-shrink: 0;
        cursor: pointer;
        align-items: center;
        border-radius: 9999px;
        padding-left: 2px;
        padding-right: 2px;
        transition: background-color 0.2s ease;
        border: none;
      }

      &:focus-visible {
        outline: none;
      }

      &:disabled {
        cursor: not-allowed;
        opacity: 0.5;
      }

      &:hover {
        outline: 1px solid var(--bg-light-extra);
      }

      &[data-state="checked"] {
        background-color: var(--accent);
      }

      &[data-state="unchecked"] {
        background-color: var(--bg-light-extra);
      }
    }

    [data-switch-thumb] {
      & {
        pointer-events: none;
        display: block;
        width: 14px;
        height: 14px;
        flex-shrink: 0;
        border-radius: 9999px;
        background-color: var(--text);
        transition: transform 0.2s ease;
      }

      &[data-state="checked"] {
        transform: translateX(14px);
        background-color: var(--bg-medium);
      }

      &[data-state="unchecked"] {
        transform: translateX(0);
      }
    }
  }
</style>
