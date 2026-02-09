<script lang="ts">
  import { onDestroy, onMount, setContext, tick } from "svelte";

  import Button from "../../components/ui/Button.svelte";
  import Placeholder from "../../components/ui/Placeholder.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { t } from "../../data/locale.svelte";
  import GroupPanel from "./components/GroupPanel.svelte";
  import Search from "./components/Search.svelte";
  import ImportConfigDialog from "./dialogs/ImportConfigDialog.svelte";
  import ImportRulesDialog from "./dialogs/ImportRulesDialog.svelte";

  import { Add, Export, Import, Save } from "../../components/ui/icons";
  import { droppable } from "../../lib/dnd";
  import { toast } from "../../utils/events";
  import { parseConfig, type Group, type Rule } from "../../types";
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

  const store = new GroupsStore({ onRenderComplete: () => onRenderComplete?.() });
  setContext(GROUPS_STORE_CONTEXT, store);

  let importRulesModal = $state<{ open: boolean; groupIndex: number | null }>({
    open: false,
    groupIndex: null,
  });

  let importConfigModal = $state<{ open: boolean; fileName: string }>({
    open: false,
    fileName: "",
  });

  let importedGroups = $state<Group[]>([]);
  let isImportingConfig = $state(false);
  let isImportingRules = $state(false);
  let pendingToast = $state<string | null>(null);

  function resetImportConfigModal() {
    importConfigModal = { open: false, fileName: "" };
    importedGroups = [];
  }

  function openImportRulesModal(groupIndex: number) {
    importRulesModal = { open: true, groupIndex };
  }

  function closeImportRulesModal() {
    importRulesModal = { open: false, groupIndex: null };
  }

  function exportConfig() {
    const payload = store.toConfigPayload();
    if (!payload.groups.length) {
      toast.warning(t("Empty config exported"));
    }
    const blob = new Blob([JSON.stringify(payload)], { type: "application/json" });
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = "config.mtrickle";
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
  }

  function importConfig() {
    const input = document.getElementById("import-config") as HTMLInputElement;
    const file = input?.files?.[0];
    if (!file) {
      alert(t("Please select a CONFIG file to load."));
      return;
    }

    const reader = new FileReader();
    reader.onload = (event) => {
      try {
        const { groups } = parseConfig(event.target?.result as string);
        if (!groups?.length) {
          toast.error(t("Invalid config file"));
          return;
        }

        importedGroups = groups;
        importConfigModal = {
          open: true,
          fileName: file.name,
        };
      } catch (error) {
        console.error("Error parsing CONFIG:", error);
        toast.error(t("Invalid config file"));
      }
    };
    reader.onerror = (event) => {
      console.error("Error reading file:", event.target?.error);
      toast.error(t("Invalid config file"));
    };

    reader.readAsText(file);
    input.value = "";
  }

  async function handleImportRules(event: CustomEvent<{ group_index: number; rules: Rule[] }>) {
    const { group_index, rules } = event.detail;
    if (!rules.length) return;

    isImportingRules = true;
    await tick();
    try {
      await store.addRulesToGroup(group_index, rules);
      pendingToast = t("Imported rules: " + rules.length);
    } catch (error) {
      console.error("Failed to import rules:", error);
      toast.error(t("Failed to import rules"));
    } finally {
      isImportingRules = false;
    }
  }

  async function handleImportConfig(payload: { groups: Group[]; replace: boolean }) {
    if (!payload.groups.length) return;

    isImportingConfig = true;
    await tick();
    try {
      const cloned = await store.cloneGroupsWithNewIds(payload.groups);
      if (payload.replace) {
        await store.overwriteGroups(cloned);
      } else {
        await store.addGroups(cloned);
      }
      pendingToast = `${t("Config imported")}: ${cloned.length}`;
    } catch (error) {
      console.error("Failed to import config:", error);
      toast.error(t("Failed to import config"));
    } finally {
      isImportingConfig = false;
      resetImportConfigModal();
    }
  }

  $effect(() => {
    const message = pendingToast;
    if (!message) return;
    if (isImportingConfig || isImportingRules) return;
    if (!store.isAllRendered) return;

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
    store.destroy();
  });
</script>

<div class="groups-page">
  <div class="group-controls">
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
        <input type="file" id="import-config" hidden accept=".mtrickle" onchange={importConfig} />
        <Button onclick={() => document.getElementById("import-config")!.click()}>
          <Import size={22} />
        </Button>
      </Tooltip>
      <Tooltip value={t("Export Config")}>
        <Button onclick={exportConfig}>
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
  {:else if isImportingConfig || isImportingRules || !store.isAllRendered}
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
    class:visible={store.isAllRendered && !isImportingConfig && !isImportingRules}
    style={store.isAllRendered && !isImportingConfig && !isImportingRules ? "" : "display: none;"}
    oninput={store.markDataRevision}
    onchange={store.markDataRevision}
  >
    {#each store.data.slice(0, store.renderGroupsLimit) as group, group_index (group.id)}
      {@const isVisible = !store.searchActive || store.visibilityMap.has(group_index)}

      <div class="group-wrapper" class:is-hidden={!isVisible}>
        <div class="group-wrapper-inner">
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

          <GroupPanel {group_index} on:importRules={() => openImportRulesModal(group_index)} />

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
      </div>
    {/each}
  </div>
</div>

<ImportRulesDialog
  open={importRulesModal.open}
  group_index={importRulesModal.groupIndex}
  on:close={closeImportRulesModal}
  on:import={handleImportRules}
/>

<ImportConfigDialog
  open={importConfigModal.open}
  groups={importedGroups}
  fileName={importConfigModal.fileName}
  onclose={resetImportConfigModal}
  onimport={handleImportConfig}
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
    display: grid;
    grid-template-rows: 1fr;
    opacity: 1;
    transition: none;
  }

  .group-wrapper-inner {
    min-height: 0;
    overflow: hidden;
  }

  .group-wrapper.is-hidden {
    grid-template-rows: 0fr;
    opacity: 0;
    margin-top: 0;
    margin-bottom: 0;
    pointer-events: none;
  }

  .group-wrapper:has(:global([data-select-trigger][data-state="open"])),
  .group-wrapper:has(:global([data-dropdown-menu-trigger][data-state="open"])) {
    z-index: 1;
  }

  @keyframes search-list-open {
    from {
      opacity: 0.85;
      transform: translateY(8px);
    }

    to {
      opacity: 1;
      transform: translateY(0);
    }
  }

  .groups-page:has(:global(.group-controls .search-container.active)) .group-list.visible {
    animation: none;
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
    .group-wrapper {
      will-change: grid-template-rows, opacity, margin;
      transition:
        grid-template-rows 0.34s cubic-bezier(0.22, 0.9, 0.28, 1),
        opacity 0.28s ease-out,
        margin 0.34s cubic-bezier(0.22, 0.9, 0.28, 1);
    }

    .groups-page:has(:global(.group-controls .search-container.active)) .group-list.visible {
      animation: search-list-open 0.32s cubic-bezier(0.22, 0.9, 0.28, 1);
    }

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
