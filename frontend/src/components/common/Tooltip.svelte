<script lang="ts">
  import type { Snippet } from "svelte";

  type Props = {
    value: string;
    children: Snippet;
  };

  let { value, children }: Props = $props();
</script>

<div data-tooltip={value}>
  {@render children()}
</div>

<style>
  [data-tooltip] {
    & {
      position: relative;
      display: flex;
      align-items: center;
      justify-content: center;
    }

    &::after {
      position: absolute;
      bottom: calc(100% + 5px);
      left: 50%;
      transform: translateX(-50%);
      content: attr(data-tooltip);
      border: 1px solid var(--bg-light-extra);
      border-radius: 0.5rem;
      background-color: var(--bg-dark);
      padding: 0.2rem 0.5rem 0.1rem 0.5rem;
      font-size: smaller;
      color: var(--text);
      white-space: nowrap;
      visibility: hidden;
      opacity: 0;
      transition: opacity 0.2s;
    }

    @media (hover: hover) and (pointer: fine) {
      &:hover::after {
        visibility: visible;
        opacity: 1;
      }
    }

    @media (max-width: 700px) {
      &:after {
        display: none;
      }
    }
  }
</style>
