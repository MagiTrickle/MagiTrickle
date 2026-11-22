<script module lang="ts">
  export type ContextAction = {
    label: string;
    icon: "record" | "stop";
    pressed: boolean;
    disabled: boolean;
    onclick: () => void;
  };
</script>

<script lang="ts">
  import { getContext, untrack, type Snippet } from "svelte";

  import { t } from "../../data/locale.svelte";
  import Button from "../ui/Button.svelte";
  import Tooltip from "../ui/Tooltip.svelte";

  import { ContextActionIcon, Export, Import, Save } from "../ui/icons";
  import { PAGE_CONTROLS_MOTION, type PageControlsMotion } from "./page-controls-motion";

  type Props = {
    search?: Snippet;
    contextAction?: ContextAction;
    addLabel?: string;
    canSave: boolean;
    hasUnsavedChanges: boolean;
    saveBusy?: boolean;
    canAdd?: boolean;
    controlsClass?: string;
    active?: boolean;
    actionsClass?: string;
    onAdd?: () => void;
    onSave: () => void;
    saveButtonId?: string;
    saveLabel: string;
    saveDescriptionId?: string;
    exportLabel?: string;
    canExport?: boolean;
    importAccept?: string;
    importLabel?: string;
    onExport?: () => void;
    onImport?: (event: Event) => void;
  };

  let {
    search,
    contextAction,
    addLabel,
    actionsClass = "",
    canSave,
    hasUnsavedChanges,
    saveBusy = false,
    canAdd = true,
    controlsClass = "",
    active = true,
    exportLabel,
    canExport = true,
    importAccept = ".mtrickle",
    importLabel,
    onAdd,
    onExport,
    onImport,
    onSave,
    saveButtonId,
    saveLabel,
    saveDescriptionId,
  }: Props = $props();

  const saveBlocked = $derived(hasUnsavedChanges && !canSave && !saveBusy);
  const blockedLabel = $derived(t("Can’t save changes yet"));
  const motion = getContext<PageControlsMotion | undefined>(PAGE_CONTROLS_MOTION);
  const hasAdd = $derived(Boolean(contextAction || (onAdd && addLabel)));
  const targetIconMode = $derived(contextAction?.icon ?? "add");
  let iconMode = $state<"add" | "record" | "stop">("add");
  let showAdd = $state(false);
  // Inactive toolbars must be primed from the last visible toolbar before animating.
  let resetting = $state(true);

  $effect(() => {
    if (!active) {
      resetting = true;
      return;
    }
    const next = hasAdd;
    const previous = motion?.hasAdd ?? next;
    const nextIconMode = targetIconMode;
    const previousIconMode = motion?.iconMode ?? nextIconMode;
    if (motion) {
      motion.hasAdd = next;
      motion.iconMode = nextIconMode;
    }

    // An already visible toolbar animates directly from its current geometry.
    if (!untrack(() => resetting)) {
      showAdd = next;
      iconMode = nextIconMode;
      return;
    }
    showAdd = previous;
    iconMode = previousIconMode;

    // Hidden tab panels keep their own controls mounted. Paint the previous
    // visible row's geometry before animating this row to its new Add state.
    let frame = requestAnimationFrame(() => {
      frame = requestAnimationFrame(() => {
        resetting = false;
        showAdd = next;
        iconMode = nextIconMode;
      });
    });
    return () => cancelAnimationFrame(frame);
  });

  let importInputRef = $state<HTMLInputElement>();
  // Reserve enough mobile search space for the widest state of the actions.
  let actionsCount = $derived(2 + (onImport ? 1 : 0) + (onExport ? 1 : 0));
  let actionsReserve = $derived(`${actionsCount * 48 + Math.max(0, actionsCount - 1) * 8 + 4}px`);
</script>

<div
  class={`page-controls ${controlsClass}${search ? "" : " no-search"}`}
  style:--actions-reserve-size={actionsReserve}
>
  {#if search}
    <div class="page-controls-search">
      {@render search()}
    </div>
  {/if}

  <div class={`page-controls-actions ${actionsClass}`}>
    <Tooltip value={saveBlocked ? blockedLabel : saveLabel}>
      <Button
        onclick={onSave}
        id={saveButtonId}
        aria-label={saveLabel}
        aria-description={saveBlocked && !saveDescriptionId ? blockedLabel : undefined}
        aria-describedby={saveDescriptionId}
        aria-busy={saveBusy}
        data-save-blocked={saveBlocked || undefined}
        class="accent"
        inactive={!canSave && !saveBusy}
        disabled={!canSave || saveBusy}
      >
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
        <Button onclick={() => importInputRef?.click()} aria-label={importLabel}>
          <Import size={22} />
        </Button>
      </Tooltip>
    {/if}

    {#if onExport && exportLabel}
      <Tooltip value={exportLabel}>
        <Button onclick={onExport} aria-label={exportLabel} disabled={!canExport}>
          <Export size={22} />
        </Button>
      </Tooltip>
    {/if}

    <div
      class="context-add"
      class:resetting
      class:present={showAdd}
      class:recording={contextAction?.pressed}
      inert={!hasAdd}
      aria-hidden={!hasAdd}
    >
      <div class="context-add-motion">
        <Tooltip value={contextAction?.label ?? addLabel ?? ""}>
          <Button
            type="button"
            onclick={contextAction?.onclick ?? onAdd}
            aria-label={contextAction?.label ?? addLabel}
            aria-pressed={contextAction?.pressed}
            disabled={contextAction ? contextAction.disabled : !onAdd || !canAdd}
          >
            <ContextActionIcon mode={iconMode} />
          </Button>
        </Tooltip>
      </div>
    </div>
  </div>
</div>

<style>
  .page-controls {
    --row-h: 48px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    min-height: var(--row-h);
    flex-wrap: nowrap;
    gap: 0.75rem;
    padding: 0.3rem 0rem;
    margin-bottom: 1rem;
    position: sticky;
    top: 0;
    z-index: 5;
    background: var(
      --page-controls-background,
      color-mix(in oklab, var(--bg-dark) 92%, var(--bg-dark-extra) 8%)
    );
  }

  .page-controls.no-search {
    justify-content: flex-end;
  }

  .page-controls-search {
    min-width: 0;
  }

  .page-controls-actions {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }

  .page-controls-actions :global(button[data-save-blocked]) {
    opacity: 1;
  }

  .page-controls-actions :global(button.accent[data-save-blocked]::before) {
    --accent: var(--yellow);
    opacity: 1;
    animation-play-state: running;
  }

  .context-add {
    width: 0;
    margin-left: -0.5rem;
    overflow: clip;
    transition:
      width 220ms cubic-bezier(0.2, 0, 0.2, 1),
      margin-left 220ms cubic-bezier(0.2, 0, 0.2, 1);
  }
  .context-add.present {
    width: calc(22px + 1.2rem + 2px);
    margin-left: 0;
  }
  .context-add-motion {
    opacity: 0;
    transform: translateX(16px);
    visibility: hidden;
    transition:
      opacity 160ms ease,
      transform 220ms cubic-bezier(0.2, 0, 0.2, 1),
      visibility 0s 220ms;
  }
  .context-add.present .context-add-motion {
    opacity: 1;
    transform: translateX(0);
    visibility: visible;
    transition-delay: 0s;
  }
  .context-add.resetting,
  .context-add.resetting .context-add-motion {
    transition: none;
  }
  .context-add.resetting :global(svg *) {
    transition: none;
    animation: none;
  }
  .context-add :global(button) {
    flex-shrink: 0;
  }
  .context-add.recording :global(button) {
    color: var(--red);
  }
  @media (prefers-reduced-motion: reduce) {
    .context-add {
      transition: none;
      transform: none;
    }
    .context-add-motion {
      transition: none;
      transform: none;
    }
    .page-controls,
    .page-controls-search,
    .page-controls-actions {
      transition: none !important;
    }
  }

  @media (max-width: 570px) {
    .page-controls {
      display: block;
      padding: 0.3rem 0;
      transition: padding-bottom 220ms cubic-bezier(0.2, 0, 0.2, 1);
      --row-h: calc(22px + 1.2rem + 2px);
      --gap: 10px;
      --actions-top: 0.3rem;
      --actions-reserve: var(--actions-reserve-size);
    }

    .page-controls.no-search {
      min-height: var(--row-h);
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
      padding-bottom: calc(var(--row-h) + var(--gap) + 0.3rem);
      --actions-top: calc(var(--row-h) + var(--gap) + 0.3rem);
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
