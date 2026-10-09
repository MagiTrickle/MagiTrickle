<script lang="ts">
  import { backOut, cubicIn } from "svelte/easing";
  import { fly } from "svelte/transition";

  import { interfaces } from "../../data/interfaces.svelte";
  import { t } from "../../data/locale.svelte";
  import { profiles } from "../../data/profiles.svelte";
  import Button from "../ui/Button.svelte";
  import RouteSelect from "../ui/RouteSelect.svelte";
  import Select from "../ui/Select.svelte";

  import type { RouteChoice } from "../../modules/settings/profiles-data";
  import { Check, Copy, Delete, SelectOpen, ToggleLeft, ToggleRight, X } from "../ui/icons";

  let {
    count,
    totalCount,
    currentInterface,
    currentProfile,
    currentEnabled,
    onclear,
    onapply,
    ondelete,
    oncopy,
    onenable,
    onselectall,
  }: {
    count: number;
    totalCount: number;
    currentInterface?: string;
    currentProfile?: string;
    currentEnabled?: boolean;
    onclear: () => void;
    onapply: (value: RouteChoice) => void;
    ondelete: () => void | Promise<void>;
    oncopy?: () => void | Promise<void>;
    onenable: (enabled: boolean) => void;
    onselectall: () => void;
  } = $props();
  let busy = $state(false);

  function panelMotion(node: HTMLElement, { entering }: { entering: boolean }) {
    const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    const bottom = parseFloat(getComputedStyle(node).bottom) || 0;
    return fly(node, {
      y: reducedMotion ? 0 : node.offsetHeight + bottom + 24,
      duration: reducedMotion ? 0 : entering ? 420 : 200,
      easing: entering ? backOut : cubicIn,
      opacity: 0,
    });
  }

  async function run(action: () => void | Promise<void>) {
    if (busy) return;
    busy = true;
    try {
      await action();
    } finally {
      busy = false;
    }
  }
</script>

<svelte:window
  onkeydown={(event) => {
    if (
      event.key === "Escape" &&
      !event.defaultPrevented &&
      !busy &&
      !document.querySelector(
        '[data-select-content], [data-dropdown-menu-content], [data-popover-content], [role="dialog"]',
      )
    )
      onclear();
  }}
/>

<div
  class="bulk-actions"
  role="region"
  aria-label={t("Bulk actions")}
  aria-busy={busy}
  in:panelMotion|global={{ entering: true }}
  out:panelMotion|global={{ entering: false }}
>
  <fieldset class="selection-controls" disabled={busy} aria-label={t("Selection")}>
    <div class="selection-count" role="status">
      <strong>
        <span class="count-placeholder" aria-hidden="true"
          >{"0".repeat(String(totalCount).length)} {t("selected")}</span
        >
        <span>{count} {t("selected")}</span>
      </strong>
    </div>
    <Button class="bulk-button" onclick={onselectall}><Check size={18} />{t("Select all")}</Button>
    <Button class="bulk-button" onclick={onclear}><X size={18} />{t("Clear selection")}</Button>
  </fieldset>
  <fieldset class="item-actions" disabled={busy} aria-label={t("Actions for selected items")}>
    <RouteSelect
      ariaLabel={t("Interface")}
      placeholder={t("Interface")}
      triggerClass="bulk-button"
      interface={currentInterface ?? ""}
      profile={currentProfile}
      disabled={busy || (!interfaces.list.length && !profiles.list.length)}
      onChange={onapply}
    />
    <Select
      ariaLabel={t("State")}
      triggerClass="bulk-button"
      selected={currentEnabled === undefined ? "" : String(currentEnabled)}
      disabled={busy}
      options={[
        { value: "true", label: t("Enabled") },
        { value: "false", label: t("Disabled") },
      ]}
      onValueChange={(value) => onenable(value === "true")}
    >
      {#snippet trigger()}
        {#if currentEnabled === false}<ToggleLeft size={18} />{:else}<ToggleRight size={18} />{/if}
        {currentEnabled === undefined ? t("State") : t(currentEnabled ? "Enabled" : "Disabled")}
        <SelectOpen size={16} />
      {/snippet}
    </Select>
    {#if oncopy}
      <Button class="bulk-button" aria-label={t("Copy to Clipboard")} onclick={() => run(oncopy!)}
        ><Copy size={18} />{t("Copy")}</Button
      >
    {/if}
    <Button
      class="bulk-button delete"
      aria-label={t("Delete selected")}
      onclick={() => run(ondelete)}><Delete size={18} />{t("Delete")}</Button
    >
  </fieldset>
</div>

<style>
  .bulk-actions {
    position: fixed;
    bottom: max(1.25rem, env(safe-area-inset-bottom));
    left: 50%;
    transform: translateX(-50%);
    z-index: 5;
    display: flex;
    align-items: center;
    gap: 1rem;
    width: max-content;
    max-width: calc(100vw - 2rem);
    box-sizing: border-box;
    padding: 1rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: 1.25rem;
    background: var(--bg-dark-extra);
    box-shadow: 0 12px 40px #0005;
    color: var(--text);
  }
  .selection-count {
    white-space: nowrap;
    padding-right: 0.5rem;
  }
  .selection-count strong {
    display: grid;
    font-variant-numeric: tabular-nums;
  }
  .selection-count strong > span {
    grid-area: 1 / 1;
  }
  .count-placeholder {
    visibility: hidden;
  }
  fieldset {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    border: 0;
    padding: 0;
    margin: 0;
    min-width: 0;
  }
  .item-actions {
    border-left: 1px solid var(--bg-light-extra);
    padding-left: 1rem;
  }
  .bulk-actions :global(.bulk-button) {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 0.45rem;
    height: 2.75rem;
    padding: 0 0.7rem;
    box-sizing: border-box;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    background: var(--bg-light);
    color: var(--text);
    font: 400 1rem var(--font);
    white-space: nowrap;
    cursor: pointer;
    transition:
      background-color 0.1s ease-in-out,
      color 0.1s ease-in-out,
      border-color 0.1s ease-in-out;
  }
  .bulk-actions :global(.bulk-button:hover:not(:disabled)),
  .bulk-actions :global(.bulk-button[data-state="open"]:not(:disabled)) {
    background: var(--bg-light-extra);
    color: var(--text);
  }
  .bulk-actions :global(.bulk-button:focus-visible) {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }
  .bulk-actions :global(.delete:hover:not(:disabled)) {
    color: var(--red);
    border-color: var(--red);
  }
  .bulk-actions :global(button:disabled) {
    opacity: 0.4;
    cursor: default;
  }
  @media (max-width: 1100px) {
    .bulk-actions {
      flex-direction: column;
      align-items: stretch;
      gap: 0.75rem;
    }
    .item-actions {
      border-left: 0;
      border-top: 1px solid var(--bg-light-extra);
      padding-left: 0;
      padding-top: 0.75rem;
    }
    .selection-count {
      margin-right: auto;
    }
  }
  @media (max-width: 600px) {
    .bulk-actions {
      width: calc(100vw - 1rem);
      max-width: none;
      padding: 0.75rem;
      bottom: max(0.5rem, env(safe-area-inset-bottom));
    }
    .selection-controls {
      flex-wrap: wrap;
    }
    .selection-count {
      width: 100%;
    }
    .item-actions {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }
    .bulk-actions :global(.bulk-button) {
      white-space: normal;
      height: auto;
      min-height: 2.75rem;
    }
  }
</style>
