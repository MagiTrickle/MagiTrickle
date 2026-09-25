<script lang="ts">
  import type { Snippet } from "svelte";

  import { t } from "../../data/locale.svelte";

  import { Check } from "../ui/icons";

  let {
    selected,
    name,
    ontoggle,
    children,
  }: {
    selected: boolean;
    name: string;
    ontoggle: () => void;
    children: Snippet;
  } = $props();
</script>

<div class="selection-frame" class:selected>
  {@render children()}
  <button
    class="selection-tail"
    type="button"
    aria-label={`${t("Select item")}: ${name}`}
    aria-pressed={selected}
    title={t(selected ? "Deselect item" : "Select item")}
    onclick={ontoggle}
    ><span class="tail-surface" aria-hidden="true"><Check size={16} strokeWidth={2.5} /></span
    ></button
  >
</div>

<style>
  .selection-frame {
    position: relative;
    border-radius: 0.5rem;
  }
  .selected > :global(.group),
  .selected > :global(.subscription-panel) {
    border-color: var(--accent);
  }
  .selection-tail {
    position: absolute;
    top: 0;
    left: 0;
    z-index: 2;
    box-sizing: border-box;
    width: calc(2rem + 2px);
    height: calc(2rem + 2px);
    padding: 0;
    border: 0;
    border-radius: 0.5rem 0 0 0;
    clip-path: polygon(0 0, 100% 0, 0 100%);
    background: transparent;
    cursor: pointer;
  }
  .tail-surface {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: flex-start;
    justify-content: flex-start;
    padding: 4px;
    border-radius: inherit;
    background: var(--bg-light-extra);
    pointer-events: none;
    visibility: hidden;
    color: color-mix(in srgb, var(--text) 35%, var(--bg-light));
    cursor: pointer;
    opacity: 0;
    transition:
      opacity 120ms ease,
      visibility 0s linear 120ms;
  }
  .tail-surface::before {
    content: "";
    position: absolute;
    inset: 1px;
    border-radius: calc(0.5rem - 1px) 0 0 0;
    clip-path: polygon(0 0, calc(100% - 1px) 0, 0 calc(100% - 1px));
    background: var(--bg-light);
    pointer-events: none;
  }
  .tail-surface :global(svg) {
    position: relative;
  }
  .selected .tail-surface::before,
  .selection-tail:hover .tail-surface::before,
  .selection-tail:focus-visible .tail-surface::before {
    inset: 1px 0 0 1px;
    clip-path: polygon(0 0, 100% 0, 0 100%);
    background: var(--accent);
  }
  .selection-frame:hover .tail-surface,
  .selection-tail:focus-visible .tail-surface,
  .selected .tail-surface {
    visibility: visible;
    transition-delay: 0s;
    opacity: 1;
  }
  .selected .tail-surface,
  .selection-tail:hover .tail-surface,
  .selection-tail:focus-visible .tail-surface {
    color: var(--bg-dark-extra);
  }
  .selected .tail-surface {
    background: var(--accent);
  }
  .selection-tail:focus-visible {
    outline: none;
    filter: drop-shadow(0 0 3px var(--accent));
  }
  @media (hover: none) {
    .tail-surface {
      visibility: visible;
      opacity: 1;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .tail-surface {
      transition: none;
    }
  }
</style>
