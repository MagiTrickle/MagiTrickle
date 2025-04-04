<script lang="ts">
  import { fly } from "svelte/transition";
  import { ToTop } from "../common/icons";

  let visible = $state(false);
</script>

<svelte:window onscroll={() => (visible = window.pageYOffset > 300)} />

{#if visible}
  <button transition:fly onclick={() => window.scrollTo({ top: 0, behavior: "smooth" })}>
    <ToTop size={20} />
  </button>
{/if}

<style>
  button {
    display: flex;
    align-items: center;
    justify-content: center;
    position: fixed;
    bottom: 20px;
    right: 20px;
    background: var(--bg-light);
    border: 1px solid var(--bg-light-extra);
    border-radius: 50%;
    color: var(--text-2);
    padding: 15px;
    z-index: 2;
    cursor: pointer;
    box-shadow: 0 0 10px 2px var(--bg-dark-extra);
  }

  button:hover {
    background: var(--bg-light-extra);
  }
</style>
