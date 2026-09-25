<script lang="ts">
  import { DropdownMenu, Popover } from "bits-ui";
  import { backOut, cubicIn } from "svelte/easing";
  import { fly } from "svelte/transition";

  import { interfaces } from "../../data/interfaces.svelte";
  import { t } from "../../data/locale.svelte";
  import Button from "../ui/Button.svelte";
  import Select from "../ui/Select.svelte";

  import {
    Check,
    Copy,
    Delete,
    Network,
    SelectOpen,
    ToggleLeft,
    ToggleRight,
    X,
  } from "../ui/icons";

  let {
    count,
    onclear,
    onapply,
    ondelete,
    oncopy,
    onenable,
    onselectall,
  }: {
    count: number;
    onclear: () => void;
    onapply: (value: string) => void;
    ondelete: () => void | Promise<void>;
    oncopy?: () => void | Promise<void>;
    onenable: (enabled: boolean) => void;
    onselectall: () => void;
  } = $props();
  let selectedInterface = $state("");
  let busy = $state(false);
  let interfaceOpen = $state(false);

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
    <div class="selection-count" role="status"><strong>{count} {t("selected")}</strong></div>
    <Button class="bulk-button" onclick={onselectall}><Check size={18} />{t("Select all")}</Button>
    <Button class="bulk-button" onclick={onclear}><X size={18} />{t("Clear selection")}</Button>
  </fieldset>
  <fieldset class="item-actions" disabled={busy} aria-label={t("Actions for selected items")}>
    <Popover.Root bind:open={interfaceOpen}>
      <Popover.Trigger class="bulk-button" disabled={busy}>
        <Network size={18} />{t("Interface")}<SelectOpen size={16} />
      </Popover.Trigger>
      <Popover.Portal>
        <Popover.Content
          class="bulk-interface-popover"
          role="dialog"
          side="top"
          align="start"
          sideOffset={12}
          collisionPadding={12}
          aria-label={t("Change interface")}
        >
          <h3>{t("Change interface")}</h3>
          <div class="interface-form">
            <Select
              ariaLabel={t("Choose interface")}
              options={[
                { value: "", label: t("Choose interface") },
                ...interfaces.list.map((item) => ({
                  value: item.id,
                  label: item.id,
                  description: item.name,
                })),
              ]}
              bind:selected={selectedInterface}
            />
            <Button
              class="apply"
              disabled={!selectedInterface || busy}
              onclick={() => {
                onapply(selectedInterface);
                interfaceOpen = false;
              }}>{t("Apply")}</Button
            >
          </div>
        </Popover.Content>
      </Popover.Portal>
    </Popover.Root>
    <DropdownMenu.Root>
      <DropdownMenu.Trigger class="bulk-button" disabled={busy}>
        <ToggleRight size={18} />{t("State")}<SelectOpen size={16} />
      </DropdownMenu.Trigger>
      <DropdownMenu.Portal>
        <DropdownMenu.Content
          class="bulk-menu"
          align="start"
          side="top"
          sideOffset={12}
          collisionPadding={12}
        >
          <DropdownMenu.Item onSelect={() => onenable(true)}
            ><ToggleRight size={18} />{t("Enable selected")}</DropdownMenu.Item
          >
          <DropdownMenu.Item onSelect={() => onenable(false)}
            ><ToggleLeft size={18} />{t("Disable selected")}</DropdownMenu.Item
          >
        </DropdownMenu.Content>
      </DropdownMenu.Portal>
    </DropdownMenu.Root>
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
    padding: 0 0.5rem;
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
    color: var(--text-2);
    font: 400 1rem var(--font);
    white-space: nowrap;
    cursor: pointer;
  }
  .bulk-actions :global(.bulk-button:hover),
  .bulk-actions :global(.bulk-button[data-state="open"]) {
    background: var(--bg-light-extra);
    color: var(--text);
  }
  .bulk-actions :global(.bulk-button:focus-visible) {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }
  .bulk-actions :global(.delete:hover) {
    color: var(--red);
    border-color: var(--red);
  }
  .bulk-actions :global(button:disabled),
  :global(.bulk-interface-popover button:disabled) {
    opacity: 0.4;
    cursor: default;
  }
  :global(.bulk-interface-popover),
  :global(.bulk-menu) {
    z-index: 10;
    box-sizing: border-box;
    max-width: calc(100vw - 1.5rem);
    padding: 0.4rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.75rem;
    background: var(--bg-dark-extra);
    color: var(--text);
    box-shadow: var(--shadow-popover);
  }
  :global(.bulk-interface-popover) {
    padding: 1rem;
  }
  :global(.bulk-interface-popover h3) {
    margin: 0 0 0.75rem;
    font-size: 1rem;
    font-weight: 600;
  }
  .interface-form {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 0.75rem;
  }
  .interface-form :global(.select-wrap) {
    max-width: 100%;
  }
  .interface-form :global([data-select-trigger]) {
    max-width: 100%;
    height: 3.25rem;
    box-sizing: border-box;
    padding: 0.5rem 0.7rem;
    background: var(--bg-light);
  }
  .interface-form :global(.apply) {
    background: var(--accent);
    color: var(--bg-dark-extra);
  }
  :global(.bulk-menu [data-dropdown-menu-item]) {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0.6rem 0.75rem;
    border-radius: 0.4rem;
  }
  :global(.bulk-menu [data-dropdown-menu-item]:hover),
  :global(.bulk-menu [data-dropdown-menu-item][data-highlighted]) {
    background: var(--bg-light-extra);
    color: var(--text);
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
