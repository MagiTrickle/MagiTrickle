<script lang="ts">
  import { createEventDispatcher } from "svelte";
  import GenericDialog from "../common/GenericDialog.svelte";
  import Button from "../common/Button.svelte";
  import Checkbox from "../common/Checkbox.svelte";
  import type { Group } from "../../types";
  import { t } from "../../data/locale.svelte";

  export let open = false;
  export let groups: Group[] = [];
  export let fileName = "";

  const dispatch = createEventDispatcher<{
    close: void;
    import: { groups: Group[] };
  }>();

  let triedSubmit = false;
  let selection = new Set<number>();
  let wasOpen = false;

  $: if (open && !wasOpen) {
    selection = new Set(groups.map((_, index) => index));
    triedSubmit = false;
  }

  $: wasOpen = open;
  $: selectedCount = selection.size;

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

    dispatch("import", { groups: selectedGroups });
    close();
  }

  function close() {
    dispatch("close");
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
        {t("Select groups to import")} — {fileName}
      {:else}
        {t("Select groups to import")}
      {/if}
    </p>
    <div class="controls">
      <Button small type="button" onclick={() => selectAll(true)}>{t("Select all")}</Button>
      <Button small type="button" onclick={() => selectAll(false)}>{t("Deselect all")}</Button>
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
    <div class="selected-count">
      {selectedCount} / {groups.length}
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

  .controls {
    display: inline-flex;
    gap: 0.5rem;
    flex-wrap: wrap;
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

  .buttons {
    display: flex;
    gap: 0.5rem;
  }

  .selected-count {
    font-size: 0.85rem;
    color: var(--text-2);
  }
</style>
