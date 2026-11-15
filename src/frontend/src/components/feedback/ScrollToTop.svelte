<script lang="ts">
  import { onMount } from "svelte";
  import { fly } from "svelte/transition";

  import { t } from "../../data/locale.svelte";

  import { initDocumentScrollShortcuts, scrollToDocumentBoundary } from "../../utils/scroll-motion";
  import { ToTop } from "../ui/icons";

  let scrollY = $state(0);
  let innerWidth = $state(0);
  const visible = $derived(scrollY > 0 && innerWidth > 668);

  function backToTop() {
    scrollToDocumentBoundary("start");
  }

  onMount(initDocumentScrollShortcuts);
</script>

<svelte:window bind:scrollY bind:innerWidth />

{#if visible}
  <button type="button" aria-label={t("Back to top")} transition:fly onclick={backToTop}>
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
    z-index: 1;
    cursor: pointer;
    box-shadow: 0 0 10px 2px var(--bg-dark-extra);
  }

  button:hover {
    background: var(--bg-light-extra);
  }
</style>
