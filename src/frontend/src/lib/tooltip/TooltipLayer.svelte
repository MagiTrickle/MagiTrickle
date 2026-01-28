<script lang="ts">
  import { tooltip } from "./tooltip.svelte";
</script>

{#if tooltip.visible}
  <div
    id="global-tooltip"
    class="tooltip-content"
    style:left="{tooltip.x}px"
    style:top="{tooltip.y}px"
    style:opacity={tooltip.opacity}
  >
    {tooltip.text}
  </div>
{/if}

<style>
  .tooltip-content {
    position: fixed;
    z-index: 15;
    pointer-events: none;
    background-color: var(--bg-dark);
    color: var(--text);
    border: 1px solid var(--bg-light-extra);
    padding: 0.2rem 0.5rem;
    border-radius: 0.5rem;
    font-size: smaller;
    white-space: nowrap;
    transition: opacity 0.1s ease-out;
  }
</style>
