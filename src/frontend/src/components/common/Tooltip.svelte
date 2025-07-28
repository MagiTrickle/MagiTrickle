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
  let observer: ResizeObserver;
  const PAD = 16;

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

  function show() {
    tooltipEl.style.visibility = "visible";
    reposition();
    tooltipEl.style.opacity = "1";
  }

  function hide() {
    tooltipEl.style.opacity = "0";
    const onEnd = () => {
      tooltipEl.style.visibility = "hidden";
      tooltipEl.removeEventListener("transitionend", onEnd);
    };
    tooltipEl.addEventListener("transitionend", onEnd, { once: true });
  }

  onMount(() => {
    wrapperEl.addEventListener("mouseenter", show);
    wrapperEl.addEventListener("mouseleave", hide);
    window.addEventListener("resize", reposition);

    observer = new ResizeObserver(reposition);
    observer.observe(tooltipEl);
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
    z-index: 9999;
    transition: opacity 0.2s ease-out;
  }

  @media (max-width: 700px) {
    .tooltip {
      display: none;
    }
  }
</style>
