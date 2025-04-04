<script lang="ts">
  import { onMount, onDestroy } from "svelte";
  import { slide } from "svelte/transition";
  import { CircleCheck, CircleInfo, CircleX } from "../common/icons";

  let text = $state("toast");
  let show = $state(false);
  let type = $state("info");
  let background = $derived.by(() => {
    switch (type) {
      case "info":
        return "var(--grey)";
      case "success":
        return "var(--green-vibrant)";
      case "error":
        return "var(--red)";
      case "warning":
        return "var(--yellow-bright)";
    }
  });

  function onToastEvent(event: CustomEvent) {
    text = event.detail.content;
    type = event.detail.type;
    show = true;
    setTimeout(() => {
      show = false;
    }, 3000);
  }

  onMount(() => {
    window.addEventListener("toast", onToastEvent);
  });

  onDestroy(() => {
    window.removeEventListener("toast", onToastEvent);
  });
</script>

{#if show}
  <div class="container" style:background transition:slide={{ duration: 300, delay: 100 }}>
    <div class="icon">
      {#if type === "success"}
        <CircleCheck size={20} />
      {:else if type === "error"}
        <CircleX size={20} />
      {:else if type === "warning"}
        <CircleInfo size={20} />
      {/if}
    </div>
    <div class="content">
      {text}
    </div>
  </div>
{/if}

<style>
  .container {
    position: fixed;
    top: 50px;
    left: 50%;
    transform: translateX(-50%);
    display: flex;
    justify-content: center;
    align-items: center;
    z-index: 9999;
    min-width: 200px;
    max-width: 400px;
    padding: 15px;
    border-radius: 5px;
    box-shadow: 0 0 25px rgba(0, 0, 0, 0.5);
    background: var(--grey);
  }

  .content {
    color: black;
    font-weight: 700;
    display: flex;
    justify-content: center;
    align-items: center;
  }
  .icon {
    margin-right: px;
    color: black;
    position: relative;
    top: 2px;
    margin-right: 0.3rem;
  }
</style>
