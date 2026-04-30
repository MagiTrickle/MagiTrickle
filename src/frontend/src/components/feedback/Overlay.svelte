<script lang="ts">
  import { onMount, tick } from "svelte";

  let text = $state("loading...");
  let hide = $state(true);
  let overlayEl = $state<HTMLDivElement | null>(null);
  let previousFocus = $state<HTMLElement | null>(null);

  function onOverlayEvent(event: CustomEvent) {
    switch (event.detail.type) {
      case "show":
        text = event.detail.content;
        onShow();
        break;
      case "hide":
        onHide();
        break;
    }
  }

  async function onShow() {
    previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    hide = false;
    await tick();
    overlayEl?.focus();
  }

  function onHide() {
    hide = true;
    previousFocus?.focus?.();
    previousFocus = null;
  }

  function preventDefaultScroll(event: WheelEvent | TouchEvent) {
    event.preventDefault();
  }

  function handleKeydown(event: KeyboardEvent) {
    if (
      event.key === " " ||
      event.key === "PageUp" ||
      event.key === "PageDown" ||
      event.key === "ArrowUp" ||
      event.key === "ArrowDown" ||
      event.key === "Home" ||
      event.key === "End"
    ) {
      event.preventDefault();
    }
  }

  onMount(() => {
    window.addEventListener("overlay", onOverlayEvent);

    const currentOverlay = overlayEl;
    if (currentOverlay) {
      currentOverlay.addEventListener("wheel", preventDefaultScroll, { passive: false });
      currentOverlay.addEventListener("touchmove", preventDefaultScroll, { passive: false });
      currentOverlay.addEventListener("keydown", handleKeydown);
    }

    return () => {
      window.removeEventListener("overlay", onOverlayEvent);
      if (currentOverlay) {
        currentOverlay.removeEventListener("wheel", preventDefaultScroll);
        currentOverlay.removeEventListener("touchmove", preventDefaultScroll);
        currentOverlay.removeEventListener("keydown", handleKeydown);
      }
    };
  });
</script>

<div
  bind:this={overlayEl}
  class:hide
  class="overlay"
  tabindex="-1"
  role="status"
  aria-live="polite"
  aria-hidden={hide}
  aria-busy={!hide}
>
  <div class="content">
    {text}
  </div>
</div>

<style>
  .overlay {
    position: fixed;
    inset: 0;
    display: flex;
    justify-content: center;
    align-items: center;
    z-index: 15;
    background-color: rgba(0, 0, 0, 0.7);
    backdrop-filter: blur(2px);
    opacity: 1;
    transition: opacity 120ms ease;
  }

  .content {
    font-size: 2rem;
    background: linear-gradient(90deg, var(--text) 0 50%, var(--text-2) 0 100%);
    background-size: 200% 100%;
    animation: textAnimation 3s ease-in-out infinite;
    color: transparent;
    background-clip: text;
    -webkit-background-clip: text;
    transform: translateY(0) scale(1);
    opacity: 1;
    transition:
      opacity 120ms ease,
      transform 120ms ease;
  }

  @keyframes textAnimation {
    0% {
      background-position: 0% 0%;
    }
    50% {
      background-position: 100% 100%;
    }
    100% {
      background-position: 0% 0%;
    }
  }

  .hide {
    opacity: 0;
    visibility: hidden;
    pointer-events: none;
  }

  .hide .content {
    opacity: 0;
    transform: translateY(8px) scale(0.985);
  }
</style>
