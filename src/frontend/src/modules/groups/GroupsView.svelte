<script lang="ts">
  import { onDestroy, onMount, setContext } from "svelte";

  import Button from "../../components/ui/Button.svelte";
  import Placeholder from "../../components/ui/Placeholder.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { t } from "../../data/locale.svelte";
  import { smoothReflow } from "../../lib/smooth-reflow.svelte";
  import GroupPanel from "./components/GroupPanel.svelte";
  import Search from "./components/Search.svelte";
  import ImportConfigDialog from "./dialogs/ImportConfigDialog.svelte";
  import ImportRulesDialog from "./dialogs/ImportRulesDialog.svelte";

  import { Add, Export, Import, Save } from "../../components/ui/icons";
  import { droppable } from "../../lib/dnd";
  import {
    GROUPS_STORE_CONTEXT,
    GroupsStore,
    type GroupDragData,
    type GroupDropSlotData,
  } from "./groups.svelte";

  type Props = {
    onRenderComplete?: () => void;
  };

  let { onRenderComplete }: Props = $props();

  const store = new GroupsStore({ onRenderComplete });
  setContext(GROUPS_STORE_CONTEXT, store);

  onMount(() => {
    void store.mount();
  });

  onDestroy(() => {
    store.destroy();
  });
</script>

<div class="groups-page" use:smoothReflow>
  <div class="group-controls" data-reflow-skip>
    <Search />

    <div class="group-controls-actions">
      <Tooltip value={t("Save Changes")}>
        <Button
          onclick={() => store.saveChanges()}
          id="save-changes"
          class="accent"
          inactive={!store.canSave}
        >
          <Save size={22} />
        </Button>
      </Tooltip>
      <Tooltip value={t("Import Config")}>
        <input type="file" id="import-config" hidden accept=".mtrickle" onchange={store.importConfig} />
        <Button onclick={() => document.getElementById("import-config")!.click()}>
          <Import size={22} />
        </Button>
      </Tooltip>
      <Tooltip value={t("Export Config")}>
        <Button onclick={() => store.exportConfig()}>
          <Export size={22} />
        </Button>
      </Tooltip>
      <Tooltip value={t("Add Group")}>
        <Button onclick={() => store.addGroup()}><Add size={22} /></Button>
      </Tooltip>
    </div>
  </div>

  {#if store.fetchError}
    <Placeholder variant="error" minHeight="auto" subtitle={t("Check connection or try again")}>
      {t("Failed to load groups")}
    </Placeholder>
  {:else if !store.isAllRendered}
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
    class:visible={store.isAllRendered}
    style={store.isAllRendered ? "" : "display: none;"}
    oninput={store.markDataRevision}
    onchange={store.markDataRevision}
  >
    {#each store.data.slice(0, store.renderGroupsLimit) as group, group_index (group.id)}
      {@const isVisible = !store.searchActive || store.visibilityMap.has(group_index)}

      <div class="group-wrapper" style={isVisible ? "" : "display: none"}>
        {#if group_index === store.firstVisibleGroupIndex}
          <div
            class="group-drop-slot group-drop-slot--top"
            aria-hidden="true"
            use:droppable={{
              data: { group_index, insert: "before" } as GroupDropSlotData,
              scope: "group",
              canDrop: (source: GroupDragData, target: GroupDropSlotData) =>
                source.group_index !== target.group_index,
              dropEffect: "move",
              onDrop: store.handleGroupSlotDrop,
            }}
          ></div>
        {/if}

        <GroupPanel {group_index} />

        <div
          class="group-drop-slot group-drop-slot--bottom"
          aria-hidden="true"
          use:droppable={{
            data: { group_index, insert: "after" } as GroupDropSlotData,
            scope: "group",
            canDrop: () => true,
            dropEffect: "move",
            onDrop: store.handleGroupSlotDrop,
          }}
        ></div>
      </div>
    {/each}
  </div>
</div>

<ImportRulesDialog
  open={store.importRulesModal.open}
  group_index={store.importRulesModal.groupIndex}
  on:close={store.closeImportRulesModal}
  on:import={(e) => store.handleImportRules(e.detail)}
/>

<ImportConfigDialog
  open={store.importConfigModal.open}
  groups={store.importedGroups}
  fileName={store.importConfigModal.fileName}
  onclose={store.resetImportConfigModal}
  onimport={store.handleImportConfig}
/>

<style>
  .group-list {
    min-height: 1px;
    opacity: 0;
    transition: opacity 0.4s ease-in-out;
  }

  .group-list.visible {
    opacity: 1;
  }

  .group-wrapper {
    position: relative;
    margin: 1rem 0;
    animation: group-appear 0.15s ease-out forwards;
  }

  .group-wrapper:has(:global([data-select-trigger][data-state="open"])) {
    z-index: 1;
  }

  @keyframes group-appear {
    from {
      opacity: 0;
    }
    to {
      opacity: 1;
    }
  }

  .group-wrapper:first-of-type {
    margin-top: 1rem;
  }

  .group-wrapper:last-of-type {
    margin-bottom: 1rem;
  }

  .group-drop-slot {
    position: absolute;
    left: 0;
    right: 0;
    height: 1rem;
    pointer-events: none;
    background: color-mix(in oklab, var(--accent) 28%, transparent);
    box-shadow: inset 0 0 0 2px color-mix(in oklab, var(--accent) 54%, transparent);
    opacity: 0;
    transition: opacity 0.12s ease;
  }

  .group-drop-slot--top {
    top: -1rem;
  }

  .group-drop-slot--bottom {
    bottom: -1rem;
  }

  :global(html[data-dnd-scope="group"]) .group-drop-slot {
    pointer-events: auto;
  }

  :global(html[data-dnd-scope="group"]) .group-drop-slot:global(.dragover) {
    opacity: 1;
  }

  .group-controls {
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

  @media (max-width: 570px) {
    .group-controls {
      flex-wrap: wrap;
    }
    .group-controls-actions {
      justify-content: flex-start;
    }
  }

  @media (max-width: 700px) {
    .group-controls {
      margin-bottom: 1rem;
    }
  }

  .group-controls-actions {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }
</style>
