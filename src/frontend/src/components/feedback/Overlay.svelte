<script lang="ts">
  import { onDestroy, onMount } from "svelte";

  let text = $state("loading...");
  let hide = $state(true);

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

  function onShow() {
    document.documentElement.style.scrollbarGutter = "auto";
    document.documentElement.style.overflow = "hidden";
    hide = false;
  }

  function onHide() {
    hide = true;
    document.documentElement.style.scrollbarGutter = "stable both-edges";
    document.documentElement.style.overflow = "auto";
  }

  function preventDefaultScroll(event: WheelEvent | TouchEvent) {
    event.preventDefault();
  }

  onMount(() => {
    window.addEventListener("overlay", onOverlayEvent);
  });

  onDestroy(() => {
    window.removeEventListener("overlay", onOverlayEvent);
  });
</script>

<div class:hide class="overlay">
  <div class="content">
    {text}
  </div>
</div>

<style>
  .overlay {
    position: fixed;
    top: 0;
    left: 0;
    width: 100vw;
    height: 100vh;
    display: flex;
    justify-content: center;
    align-items: center;
    z-index: 9998;
    background-color: rgba(0, 0, 0, 0.7);
    backdrop-filter: blur(2px);
  }

  .content {
    font-size: 2rem;
    background: linear-gradient(90deg, var(--text) 0 50%, var(--text-2) 0 100%);
    background-size: 200% 100%;
    animation: textAnimation 3s ease-in-out infinite;
    color: transparent;
    background-clip: text;
    -webkit-background-clip: text;
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
    display: none;
  }
</style>
