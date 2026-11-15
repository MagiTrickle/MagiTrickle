<script lang="ts">
  import { getContext, onDestroy, onMount, setContext, tick } from "svelte";

  import BulkActions from "../../components/bulk/BulkActions.svelte";
  import SelectionFrame from "../../components/bulk/SelectionFrame.svelte";
  import PageControls from "../../components/layout/PageControls.svelte";
  import Placeholder from "../../components/ui/Placeholder.svelte";
  import VirtualList from "../../components/ui/VirtualList.svelte";
  import { t } from "../../data/locale.svelte";
  import { SizeEstimate, type VirtualMeasurement } from "../../lib/virtual/size-estimate.svelte";
  import GroupPanel from "./components/GroupPanel.svelte";
  import Search from "./components/Search.svelte";
  import ImportRulesDialog from "./dialogs/ImportRulesDialog.svelte";
  import {
    GROUPS_STORE_CONTEXT,
    GroupsStore,
    type GroupDragData,
    type GroupDropSlotData,
  } from "./groups.svelte";

  import { droppable } from "../../lib/dnd";
  import { type Rule } from "../../types";
  import { copyRulePatternsToClipboard } from "../../utils/copy-rule-patterns";
  import { toast } from "../../utils/events";
  import { CONFIG_TRANSFER_CONTEXT, type ConfigTransferControls } from "../config-transfer/context";

  type Props = {
    active?: boolean;
    onRenderComplete?: () => void;
  };

  let { active = true, onRenderComplete }: Props = $props();

  const store = new GroupsStore({
    onRenderComplete: () => onRenderComplete?.(),
    isActive: () => active,
  });
  setContext(GROUPS_STORE_CONTEXT, store);
  const transfer = getContext<ConfigTransferControls>(CONFIG_TRANSFER_CONTEXT);
  const unregisterTransfer = transfer.registerGroups(store);

  let groupList = $state<VirtualList>();
  let viewportWidth = $state(1280);
  const ruleSize = new SizeEstimate({ desktop: 38, mobile: 112 });
  const panelSize = new SizeEstimate({ desktop: 72, mobile: 72 });
  const ruleEstimate = $derived(ruleSize.forWidth(viewportWidth));
  function measureRules(measurements: readonly VirtualMeasurement[]) {
    ruleSize.learn(viewportWidth, measurements);
  }
  function measurePanels(measurements: readonly VirtualMeasurement[]) {
    panelSize.learn(
      viewportWidth,
      measurements.filter(({ index }) => {
        const group = store.data[visibleIndices[index]];
        return group && !store.open_state[group.id];
      }),
    );
  }
  const visibleIndices = $derived(
    store.searchActive
      ? store.visibleGroups.map((entry) => entry.group_index)
      : store.data.map((_, index) => index),
  );
  store.ensureGroupVisible = async (groupId) => {
    await tick();
    const index = visibleIndices.findIndex((original) => store.data[original]?.id === groupId);
    if (index >= 0) await groupList?.scrollToIndex(index, "nearest");
    await tick();
  };

  let selectedIds = $state<string[]>([]);
  let selectedItems = $derived(store.data.filter((item) => selectedIds.includes(item.id)));
  function toggleSelection(id: string) {
    selectedIds = selectedIds.includes(id)
      ? selectedIds.filter((value) => value !== id)
      : [...selectedIds, id];
  }
  function applyToSelected(update: { interface: string; profile?: string } | { enable: boolean }) {
    for (const item of selectedItems) Object.assign(item, update);
    store.markDataRevision();
  }
  async function deleteSelected() {
    if (!confirm(`${t("Delete selected items?")} (${selectedItems.length})`)) return;
    const ids = selectedItems.map((item) => item.id);
    for (const id of ids) {
      const index = store.data.findIndex((item) => item.id === id);
      if (index >= 0) await store.deleteGroup(index, true);
    }
    selectedIds = selectedIds.filter((id) => store.data.some((item) => item.id === id));
  }

  let importRulesModal = $state<{ open: boolean; groupIndex: number | null }>({
    open: false,
    groupIndex: null,
  });

  let isImportingRules = $state(false);
  let pendingToast = $state<string | null>(null);

  function openImportRulesModal(groupIndex: number) {
    importRulesModal = { open: true, groupIndex };
  }

  function closeImportRulesModal() {
    importRulesModal = { open: false, groupIndex: null };
  }

  async function handleImportRules(group_index: number, rules: Rule[], signal: AbortSignal) {
    if (!rules.length) return;

    isImportingRules = true;
    await tick();
    try {
      await store.addRulesToGroup(group_index, rules, { signal });
      pendingToast = t("Imported rules: " + rules.length);
    } catch (error) {
      if (!(error instanceof DOMException && error.name === "AbortError")) {
        console.error("Failed to import rules:", error);
        toast.error(t("Failed to import rules"));
      }
      throw error;
    } finally {
      isImportingRules = false;
    }
  }

  $effect(() => {
    const message = pendingToast;
    if (!message) return;
    if (isImportingRules) return;

    let cancelled = false;
    const fire = () => {
      if (cancelled) return;
      if (pendingToast !== message) return;
      toast.success(message);
      pendingToast = null;
    };

    if (typeof requestAnimationFrame === "function") {
      requestAnimationFrame(fire);
    } else {
      setTimeout(fire, 0);
    }

    return () => {
      cancelled = true;
    };
  });

  onMount(() => {
    void store.mount();
  });

  onDestroy(() => {
    unregisterTransfer();
    store.destroy();
  });
</script>

<svelte:window bind:innerWidth={viewportWidth} />

<div class="groups-page">
  <PageControls
    {active}
    actionsClass="group-controls-actions"
    controlsClass="group-controls"
    addLabel={t("Add Group")}
    canSave={transfer.pending() ? transfer.canSave("groups") : store.canSave}
    exportLabel={t("Export Config")}
    importLabel={t("Import Config")}
    onAdd={() => store.addGroup()}
    onExport={() => void transfer.exportConfig()}
    onImport={(event) => void transfer.importConfig(event)}
    onSave={() => store.saveChanges()}
    saveButtonId="save-changes"
    saveLabel={t("Save Changes")}
  >
    {#snippet search()}
      <Search />
    {/snippet}
  </PageControls>

  {#if store.fetchError}
    <Placeholder variant="error" minHeight="auto" subtitle={t("Check connection or try again")}>
      {t("Failed to load groups")}
    </Placeholder>
  {:else if !store.dataLoaded}
    <Placeholder variant="loading" minHeight="auto">
      {t("Loading groups...")}
    </Placeholder>
  {:else if store.noVisibleGroups}
    <Placeholder variant="empty" minHeight="auto">
      {t("No matches found")}
    </Placeholder>
  {:else if store.isEmptyData}
    <Placeholder variant="empty" minHeight="auto" subtitle={t("Create a new group to get started")}>
      {t("No groups yet")}
    </Placeholder>
  {/if}

  <div
    class="group-list"
    class:visible={store.dataLoaded}
    oninput={store.markDataRevision}
    onchange={store.markDataRevision}
  >
    <VirtualList
      bind:this={groupList}
      count={visibleIndices.length}
      getKey={(index) => store.data[visibleIndices[index]].id}
      estimateSize={(index) => {
        const groupIndex = visibleIndices[index];
        const group = store.data[groupIndex];
        const count = store.searchActive
          ? (store.visibilityMap.get(groupIndex)?.length ?? group.rules.length)
          : group.rules.length;
        return (
          panelSize.forWidth(viewportWidth) +
          (store.open_state[group.id] && count ? 28 + count * ruleEstimate : 0)
        );
      }}
      {active}
      revision={visibleIndices}
      onmeasure={measurePanels}
    >
      {#snippet children(index)}
        {@const group_index = visibleIndices[index]}
        {@const group = store.data[group_index]}
        <div class="group-wrapper" class:last-group={index === visibleIndices.length - 1}>
          {#if group_index === store.firstVisibleGroupIndex}
            <div
              class="group-drop-slot group-drop-slot--top"
              aria-hidden="true"
              use:droppable={{
                data: { group_index, insert: "before" } as GroupDropSlotData,
                scope: "group",
                canDrop: (source: GroupDragData, target: GroupDropSlotData) =>
                  store.canDropGroup(source, target),
                dropEffect: "move",
                onDrop: store.handleGroupSlotDrop,
              }}
            ></div>
          {/if}

          <SelectionFrame
            selected={selectedIds.includes(group.id)}
            name={group.name}
            ontoggle={() => toggleSelection(group.id)}
          >
            <GroupPanel
              {group_index}
              {active}
              {ruleEstimate}
              onmeasureRules={measureRules}
              selectionActive={selectedItems.length > 0}
              selected={selectedIds.includes(group.id)}
              ontoggleSelection={() => toggleSelection(group.id)}
              on:importRules={() => openImportRulesModal(group_index)}
            />
          </SelectionFrame>

          <div
            class="group-drop-slot group-drop-slot--bottom"
            aria-hidden="true"
            use:droppable={{
              data: { group_index, insert: "after" } as GroupDropSlotData,
              scope: "group",
              canDrop: store.canDropGroup,
              dropEffect: "move",
              onDrop: store.handleGroupSlotDrop,
            }}
          ></div>
        </div>
      {/snippet}
    </VirtualList>
  </div>
</div>

<ImportRulesDialog
  open={importRulesModal.open}
  group_index={importRulesModal.groupIndex}
  on:close={closeImportRulesModal}
  onImportRules={handleImportRules}
/>

{#if selectedItems.length}
  <BulkActions
    count={selectedItems.length}
    totalCount={store.data.length}
    currentInterface={selectedItems.every(
      (item) =>
        item.interface === selectedItems[0]?.interface &&
        item.profile === selectedItems[0]?.profile,
    )
      ? selectedItems[0]?.interface
      : undefined}
    onclear={() => (selectedIds = [])}
    onapply={(value) => applyToSelected(value)}
    currentProfile={selectedItems.every((item) => item.profile === selectedItems[0]?.profile)
      ? selectedItems[0]?.profile
      : undefined}
    currentEnabled={selectedItems.every((item) => item.enable === selectedItems[0]?.enable)
      ? selectedItems[0]?.enable
      : undefined}
    onenable={(enable) => applyToSelected({ enable })}
    ondelete={deleteSelected}
    oncopy={() => copyRulePatternsToClipboard(selectedItems.flatMap((item) => item.rules))}
    onselectall={() =>
      (selectedIds = store.data
        .filter((_, index) => !store.searchActive || store.visibilityMap.has(index))
        .map((item) => item.id))}
  />
{/if}

<style>
  .group-list {
    min-height: 1px;
    opacity: 0;
  }

  .group-list.visible {
    opacity: 1;
  }

  .group-wrapper {
    position: relative;
    padding-bottom: 1rem;
  }

  .group-drop-slot {
    position: absolute;
    left: 0;
    right: 0;
    height: 1rem;
    pointer-events: none;
    opacity: 0;
  }

  /* Fill the gap, including the space around the rounded card corners. */
  .group-drop-slot::before {
    content: "";
    position: absolute;
    inset: -0.5rem 0;
    pointer-events: none;
    background: color-mix(in oklab, var(--accent) 35%, transparent);
    mask:
      radial-gradient(circle at 100% 0, transparent 0.5rem, black calc(0.5rem + 0.5px)) top left /
        0.5rem 0.5rem,
      radial-gradient(circle at 0 0, transparent 0.5rem, black calc(0.5rem + 0.5px)) top right /
        0.5rem 0.5rem,
      radial-gradient(circle at 100% 100%, transparent 0.5rem, black calc(0.5rem + 0.5px)) bottom
        left / 0.5rem 0.5rem,
      radial-gradient(circle at 0 100%, transparent 0.5rem, black calc(0.5rem + 0.5px)) bottom
        right / 0.5rem 0.5rem,
      linear-gradient(black, black) center / 100% 1rem;
    mask-repeat: no-repeat;
  }

  .group-drop-slot--top::before {
    mask:
      radial-gradient(circle at 100% 100%, transparent 0.5rem, black calc(0.5rem + 0.5px)) bottom
        left / 0.5rem 0.5rem no-repeat,
      radial-gradient(circle at 0 100%, transparent 0.5rem, black calc(0.5rem + 0.5px)) bottom
        right / 0.5rem 0.5rem no-repeat,
      linear-gradient(black, black) center / 100% 1rem no-repeat;
  }

  .group-wrapper.last-group .group-drop-slot--bottom::before {
    mask:
      radial-gradient(circle at 100% 0, transparent 0.5rem, black calc(0.5rem + 0.5px)) top left /
        0.5rem 0.5rem no-repeat,
      radial-gradient(circle at 0 0, transparent 0.5rem, black calc(0.5rem + 0.5px)) top right /
        0.5rem 0.5rem no-repeat,
      linear-gradient(black, black) center / 100% 1rem no-repeat;
  }

  .group-list :global([data-droppable="rule"]) {
    position: relative;
  }

  .group-list :global([data-droppable="rule"].dragover)::after {
    content: "";
    position: absolute;
    inset-inline: 0.25rem;
    bottom: -1.5px;
    height: 3px;
    border-radius: 999px;
    background: var(--accent);
    z-index: 1;
    pointer-events: none;
  }

  .group-list :global([data-droppable="rule"][data-drop-edge="before"].dragover)::after {
    top: -1.5px;
    bottom: auto;
  }

  .group-drop-slot--top {
    top: -1rem;
  }

  .group-drop-slot--bottom {
    bottom: 0;
  }

  :global(html[data-dnd-scope="group"]) .group-drop-slot {
    pointer-events: auto;
  }

  :global(html[data-dnd-scope="group"]) .group-drop-slot:global(.dragover) {
    opacity: 1;
  }
</style>
