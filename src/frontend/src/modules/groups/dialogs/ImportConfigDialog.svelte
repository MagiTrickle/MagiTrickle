<script lang="ts">
  import Button from "../../../components/ui/Button.svelte";
  import Checkbox from "../../../components/ui/Checkbox.svelte";
  import GenericDialog from "../../../components/ui/GenericDialog.svelte";
  import Switch from "../../../components/ui/Switch.svelte";
  import { t } from "../../../data/locale.svelte";

  import { Check, Scan } from "../../../components/ui/icons";
  import type { Group } from "../../../types";

  let {
    open = false,
    groups = [],
    fileName = "",
    onclose,
    onimport,
  }: {
    open?: boolean;
    groups?: Group[];
    fileName?: string;
    onclose?: () => void;
    onimport?: (data: { groups: Group[]; replace: boolean }) => void;
  } = $props();

  let triedSubmit = $state(false);
  let selection = $state(new Set<number>());
  let replaceMode = $state(false);

  $effect(() => {
    if (open) {
      selection = new Set(groups.map((_, index) => index));
      triedSubmit = false;
      replaceMode = false;
    }
  });

  let selectedCount = $derived(selection.size);

  function toggleGroup(index: number, checked: boolean) {
    const next = new Set(selection);
    if (checked) {
      next.add(index);
    } else {
      next.delete(index);
    }
    selection = next;
  }

  function selectAll(value: boolean) {
    selection = value ? new Set(groups.map((_, index) => index)) : new Set();
  }

  function submit() {
    const selectedGroups = groups.filter((_, index) => selection.has(index));

    if (!selectedGroups.length) {
      triedSubmit = true;
      return;
    }

    onimport?.({ groups: selectedGroups, replace: replaceMode });
    close();
  }

  function close() {
    onclose?.();
  }
</script>

<GenericDialog
  {open}
  title={t("Import Config")}
  textareaValue=""
  textareaPlaceholder=""
  {triedSubmit}
  maxWidth={420}
  on:close={close}
  on:submit={submit}
>
  <div slot="body" class="import-config">
    <p class="file-name">
      {#if fileName}
        {t("Select groups to import")} - <i>{fileName}</i>
      {:else}
        {t("Select groups to import")}
      {/if}
    </p>
    <div class="controls-row">
      <div class="controls">
        <Button type="button" onclick={() => selectAll(true)}>
          <Check size={16} />
          <span class="with-icon">{t("All")}</span>
        </Button>
        <Button type="button" onclick={() => selectAll(false)}>
          <Scan size={16} />
          <span class="with-icon">{t("Reset")}</span>
        </Button>
      </div>

      <div class="selected-count">
        {selectedCount} / {groups.length}
      </div>
    </div>
    <div class="group-list">
      {#each groups as group, index (index)}
        <label class="group-option">
          <Checkbox
            checked={selection.has(index)}
            on:change={(event) => toggleGroup(index, event.detail.checked)}
          />
          <div class="group-info">
            <span class="group-name">{group.name || `${t("Group")} ${index + 1}`}</span>
            <span class="group-meta">#{group.rules.length}</span>
          </div>
        </label>
      {/each}
    </div>
    {#if triedSubmit && !selectedCount}
      <div class="validation">{t("Select at least one group")}</div>
    {/if}
  </div>
  <div slot="actions" class="dialog-actions">
    <div class="mode-toggle">
      <span class:active={!replaceMode}>{t("Append")}</span>
      <Switch bind:checked={replaceMode} />
      <span class:active={replaceMode}>{t("Replace")}</span>
    </div>
    <div class="buttons">
      <Button type="button" onclick={close}>{t("Cancel")}</Button>
      <Button type="submit" onclick={submit}>{t("Import")}</Button>
    </div>
  </div>
</GenericDialog>

<style>
  .import-config {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    padding-top: 0.25rem;
  }

  .file-name {
    font-size: 0.95rem;
    color: var(--text-2);
    margin: 0;
    line-height: 1.4;
  }

  .controls-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 0.5rem;
    flex-wrap: wrap;
  }

  .controls {
    display: inline-flex;
    gap: 0.5rem;
  }

  .with-icon {
    margin-left: 0.25rem;
  }

  .mode-toggle {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    font-size: 0.85rem;
    color: var(--text-2);
  }

  .mode-toggle span {
    transition: color 0.2s ease;
  }

  .mode-toggle span.active {
    color: var(--text);
    font-weight: 500;
  }

  .group-list {
    max-height: 260px;
    overflow: auto;
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
    margin: 0 -1rem;
    padding: 0.5rem 1rem;
    scrollbar-gutter: stable;
  }

  .group-option {
    display: flex;
    gap: 0.75rem;
    align-items: center;
    padding: 0.75rem 1rem;
    border-radius: 0.55rem;
    border: 1px solid var(--bg-light-extra);
    background-color: var(--bg-light);
    position: relative;
  }

  .group-info {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.75rem;
    width: 100%;
  }

  .group-name {
    font-weight: 600;
    color: var(--text);
    word-break: break-word;
    flex: 1;
  }

  .group-meta {
    font-size: 0.8rem;
    color: var(--text-2);
    white-space: nowrap;
    font-variant-numeric: tabular-nums;
  }

  .validation {
    color: var(--danger);
    font-size: 0.85rem;
  }

  .dialog-actions {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-top: 0.75rem;
  }

  @media (max-width: 500px) {
    .dialog-actions {
      flex-direction: column;
      align-items: stretch;
      gap: 1rem;
    }

    .mode-toggle {
      justify-content: center;
    }

    .buttons {
      display: flex;
      width: 100%;
      justify-content: space-between;
    }
  }

  .buttons {
    display: flex;
    gap: 0.5rem;
  }

  .selected-count {
    font-size: 0.85rem;
    color: var(--text-2);
  }
</style>
