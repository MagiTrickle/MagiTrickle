<script lang="ts">
  import type { Snippet } from "svelte";

  import Button from "../ui/Button.svelte";
  import Tooltip from "../ui/Tooltip.svelte";

  import { Add, Export, Import, Save } from "../ui/icons";

  type Props = {
    search: Snippet;
    addLabel: string;
    canSave: boolean;
    controlsClass?: string;
    actionsClass?: string;
    onAdd: () => void;
    onSave: () => void;
    saveButtonId?: string;
    saveLabel: string;
    exportLabel?: string;
    importAccept?: string;
    importLabel?: string;
    onExport?: () => void;
    onImport?: (event: Event) => void;
  };

  let {
    search,
    addLabel,
    actionsClass = "",
    canSave,
    controlsClass = "",
    exportLabel,
    importAccept = ".mtrickle",
    importLabel,
    onAdd,
    onExport,
    onImport,
    onSave,
    saveButtonId,
    saveLabel,
  }: Props = $props();

  let importInputRef = $state<HTMLInputElement>();
  let actionsCount = $derived(2 + (onImport ? 1 : 0) + (onExport ? 1 : 0));
  let actionsReserve = $derived(`${actionsCount * 48 + Math.max(0, actionsCount - 1) * 8 + 4}px`);
</script>

<div class={`page-controls ${controlsClass}`} style:--actions-reserve-size={actionsReserve}>
  <div class="page-controls-search">
    {@render search()}
  </div>

  <div class={`page-controls-actions ${actionsClass}`}>
    <Tooltip value={saveLabel}>
      <Button onclick={onSave} id={saveButtonId} class="accent" inactive={!canSave}>
        <Save size={22} />
      </Button>
    </Tooltip>

    {#if onImport && importLabel}
      <Tooltip value={importLabel}>
        <input
          bind:this={importInputRef}
          type="file"
          hidden
          accept={importAccept}
          onchange={onImport}
        />
        <Button onclick={() => importInputRef?.click()}>
          <Import size={22} />
        </Button>
      </Tooltip>
    {/if}

    {#if onExport && exportLabel}
      <Tooltip value={exportLabel}>
        <Button onclick={onExport}>
          <Export size={22} />
        </Button>
      </Tooltip>
    {/if}

    <Tooltip value={addLabel}>
      <Button onclick={onAdd}><Add size={22} /></Button>
    </Tooltip>
  </div>
</div>

<style>
  .page-controls {
    display: flex;
    align-items: center;
    justify-content: space-between;
    flex-wrap: nowrap;
    gap: 0.75rem;
    padding: 0.3rem 0rem;
    margin-bottom: 1rem;
    position: sticky;
    top: 0;
    z-index: 5;
    background: color-mix(in oklab, var(--bg-dark) 92%, var(--bg-dark-extra) 8%);
  }

  .page-controls-search {
    min-width: 0;
  }

  .page-controls-actions {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }

  @media (max-width: 570px) {
    .page-controls {
      display: block;
      padding: 0.3rem 0;
      padding-bottom: 0;
      transition: padding-bottom 220ms cubic-bezier(0.2, 0, 0.2, 1);
      --row-h: 48px;
      --gap: 10px;
      --actions-top: 0px;
      --actions-reserve: var(--actions-reserve-size);
    }

    .page-controls-search {
      padding-right: var(--actions-reserve);
      transition: padding-right 220ms cubic-bezier(0.2, 0, 0.2, 1);
    }

    .page-controls-actions {
      position: absolute;
      right: 0;
      top: var(--actions-top);
      height: var(--row-h);
      transition: top 220ms cubic-bezier(0.2, 0, 0.2, 1);
    }

    .page-controls:has(:global(.search-container:focus-within)),
    .page-controls:has(:global(.search-input:not(:placeholder-shown))) {
      padding-bottom: calc(var(--row-h) + var(--gap));
      --actions-top: calc(var(--row-h) + var(--gap));
      --actions-reserve: 0px;
    }

    .page-controls:has(:global(.search-container:focus-within)) :global(.search-container),
    .page-controls:has(:global(.search-input:not(:placeholder-shown))) :global(.search-container) {
      width: 100%;
    }

    .page-controls:has(:global(.search-container:focus-within))
      :global(.search-container .input-wrapper),
    .page-controls:has(:global(.search-input:not(:placeholder-shown)))
      :global(.search-container .input-wrapper) {
      width: 100%;
    }
  }

  @media (max-width: 700px) {
    .page-controls {
      margin-bottom: 1rem;
    }
  }
</style>
