<script lang="ts">
  import { onMount } from "svelte";
  import type { Snippet } from "svelte";

  type Props = {
    value: string;
    children: Snippet;
  };
  let { value, children }: Props = $props();

  let wrapperEl: HTMLElement;
  let tooltipEl: HTMLElement;
  const PAD = 6;

  function reposition() {
    tooltipEl.style.left = "50%";
    tooltipEl.style.transform = "translateX(-50%)";
    const rect = tooltipEl.getBoundingClientRect();
    const vw = window.innerWidth;
    let shift = 0;
    if (rect.left < PAD) shift = PAD - rect.left;
    else if (rect.right > vw - PAD) shift = -(rect.right - (vw - PAD));
    tooltipEl.style.transform = `translateX(calc(-50% + ${shift}px))`;

    const maxWidth = vw - PAD * 2;
    if (rect.width > maxWidth) {
      tooltipEl.style.maxWidth = `${maxWidth}px`;
      tooltipEl.style.overflow = "hidden";
      tooltipEl.style.textOverflow = "ellipsis";
    }
  }

  onMount(() => {
    wrapperEl.addEventListener("pointerenter", reposition);
    window.addEventListener("resize", reposition);

    const observer = new ResizeObserver(reposition);
    observer.observe(tooltipEl);

    return () => {
      observer.disconnect();
      window.removeEventListener("resize", reposition);
      wrapperEl.removeEventListener("pointerenter", reposition);
    };
  });
</script>

<div bind:this={wrapperEl} class="tooltip-wrapper">
  {@render children()}
  <span bind:this={tooltipEl} class="tooltip">{value}</span>
</div>

<style>
  .tooltip-wrapper {
    position: relative;
    display: flex;
    align-items: center;
    justify-content: center;
    overflow: visible;
  }

  .tooltip {
    position: absolute;
    bottom: calc(100% + 5px);
    left: 50%;
    transform: translateX(-50%);
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    background-color: var(--bg-dark);
    padding: 0.2rem 0.5rem 0.1rem 0.5rem;
    font-size: smaller;
    color: var(--text);
    white-space: nowrap;
    visibility: hidden;
    opacity: 0;
    pointer-events: none;

    transition:
      opacity 0.2s ease-out,
      visibility 0s linear 0.2s;
    z-index: 9999;
  }

  .tooltip-wrapper:hover .tooltip {
    opacity: 1;
    visibility: visible;
    transition:
      opacity 0.2s ease-out,
      visibility 0s;
  }

  @media (max-width: 700px) {
    .tooltip {
      display: none;
    }
  }
</style>
