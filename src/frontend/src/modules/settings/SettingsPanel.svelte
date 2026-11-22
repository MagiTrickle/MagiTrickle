<script lang="ts">
  import PlatformSettings from "#platform/settings";
  import { Popover, Tabs } from "bits-ui";
  import { getContext, onMount, setContext } from "svelte";

  import PageControls from "../../components/layout/PageControls.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { t } from "../../data/locale.svelte";
  import { hide } from "../../lib/tooltip/tooltip.svelte";
  import { persistedState } from "../../utils/persisted-state.svelte";
  import LogsView from "../logs/LogsView.svelte";
  import DnsQueryLog from "./dns/DnsQueryLog.svelte";
  import GeneralSettingsPanel from "./general/GeneralSettingsPanel.svelte";
  import ProfilesPanel from "./ProfilesPanel.svelte";

  import { TriangleAlert } from "../../components/ui/icons";
  import { toast } from "../../utils/events";
  import { CONFIG_TRANSFER_CONTEXT, type ConfigTransferControls } from "../config-transfer/context";
  import {
    SETTINGS_SECTIONS,
    type RegisterSection,
    type SettingsSection,
  } from "./section-controls";

  let { active = true }: { active?: boolean } = $props();

  const transfer = getContext<ConfigTransferControls>(CONFIG_TRANSFER_CONTEXT);
  let sections = $state.raw<SettingsSection[]>([]);
  let saving = $state(false);
  const lastSelected = persistedState("settings_tab", "profiles");
  let selected = $state(lastSelected.current);
  onMount(() => {
    // Stored tabs may no longer exist, for example after changing the platform build.
    if (!sections.some((section) => section.element.id === selected)) selected = "profiles";
  });
  $effect(() => {
    lastSelected.current = selected;
  });
  const visibleSection = $derived(sections.find((section) => section.element.id === selected));
  const tabLabels = $derived<Record<string, string>>({
    general: t("General"),
    profiles: t("Profiles"),
    keenetic: "Keenetic",
    dns: t("DNS queries"),
    logs: t("Logs"),
  });
  const tabSections = $derived([
    ...sections.filter((section) => section.element.id === "general"),
    ...sections.filter((section) => section.element.id !== "general"),
  ]);
  const visibleControls = $derived(visibleSection?.controls());
  const dirty = $derived(sections.some((section) => section.controls().dirty));
  const invalid = $derived(
    sections.find((section) => section.controls().dirty && !section.controls().canSave)?.controls(),
  );
  const validationSections = $derived(
    sections.filter((section) => {
      const controls = section.controls();
      return controls.dirty && controls.validationMessage;
    }),
  );

  // Section feedback belongs behind an explicit action, never in a second
  // header row (including feedback from a currently hidden settings section).
  const feedbackSections = $derived(
    sections.filter((section) => section.controls().toolbarFeedback),
  );
  let feedbackOpen = $state(false);
  $effect(() => {
    if (!active || !feedbackSections.length) feedbackOpen = false;
  });

  const canSave = $derived(
    !saving && !invalid && (transfer.pending() ? transfer.canSave("settings") : dirty),
  );

  async function saveAll() {
    if (!canSave) return;
    // Confirm runtime changes before writing any section, then apply them last
    // so listener/auth changes cannot interrupt the other settings saves.
    const pending = sections
      .filter((section) => section.controls().dirty)
      .sort(
        (a, b) => Number(Boolean(a.controls().saveLast)) - Number(Boolean(b.controls().saveLast)),
      );
    saving = true;
    let succeeded = 0;
    let failed = 0;
    try {
      for (const section of pending) {
        const prepare = section.controls().prepareSave;
        if (prepare && !(await prepare())) return;
      }
      const savingImport = transfer.pending();
      if (savingImport) {
        await transfer.save("settings");
        if (transfer.pending()) return;
        succeeded++;
      }
      for (const section of pending) {
        // The import transaction includes definitions and their consumers.
        // Do not follow it with an independent write of the old section draft.
        if (savingImport && ["profiles", "keenetic"].includes(section.element.id)) continue;
        if (section.controls().saveLast && failed) continue;
        try {
          await section.controls().onSave();
          succeeded++;
        } catch {
          failed++;
        }
      }
      if (failed && succeeded) toast.warning(t("Some settings could not be saved"));
    } finally {
      saving = false;
    }
  }

  function handleSaveShortcut(event: KeyboardEvent) {
    if (
      !active ||
      event.defaultPrevented ||
      event.isComposing ||
      event.altKey ||
      event.shiftKey ||
      !(event.ctrlKey || event.metaKey) ||
      event.key.toLowerCase() !== "s"
    )
      return;
    if (document.querySelector('[data-dialog-content][data-state="open"]')) return;
    event.preventDefault();
    void saveAll();
  }

  setContext<RegisterSection>(SETTINGS_SECTIONS, (section) => {
    sections = [...sections, section];
    return () => {
      sections = sections.filter((s) => s !== section);
    };
  });
</script>

<svelte:window onkeydown={handleSaveShortcut} />

{#each sections as section (section.element.id)}
  {@const dialog = section.controls().dialog}
  {#if dialog}{@render dialog()}{/if}
{/each}

<Tabs.Root bind:value={selected} class="settings-page">
  <div class="settings-toolbar">
    <PageControls
      {canSave}
      hasUnsavedChanges={dirty || transfer.pending()}
      saveBusy={saving || (transfer.pending() && transfer.busy())}
      controlsClass="settings-controls"
      {active}
      onSave={() => void saveAll()}
      saveButtonId="save-settings"
      saveLabel={t("Save Changes")}
      saveDescriptionId={validationSections.length
        ? validationSections.map((section) => `settings-validation-${section.element.id}`).join(" ")
        : undefined}
      exportLabel={visibleControls?.exportAction?.label ?? t("Export Config")}
      canExport={!visibleControls?.exportAction?.disabled}
      importLabel={t("Import Config")}
      onExport={visibleControls?.exportAction?.onclick ?? (() => void transfer.exportConfig())}
      onImport={visibleControls?.hideImport
        ? undefined
        : (event) => void transfer.importConfig(event)}
      addLabel={visibleControls?.addLabel}
      canAdd={visibleControls?.canAdd ?? false}
      onAdd={visibleControls?.onAdd}
      contextAction={visibleControls?.contextAction}
    >
      {#snippet search()}
        <div class="settings-navigation">
          <Tabs.List class="settings-tabs" aria-label={t("Settings")}>
            {#each tabSections as section (section.element.id)}
              <Tabs.Trigger value={section.element.id}>{tabLabels[section.element.id]}</Tabs.Trigger
              >
            {/each}
          </Tabs.List>
          <div class="settings-feedback-slot">
            <Popover.Root bind:open={feedbackOpen}>
              <Tooltip value={t("Settings need attention")}>
                <Popover.Trigger
                  class="settings-status"
                  hidden={!feedbackSections.length}
                  aria-label={t("Settings need attention")}
                  onclick={hide}
                >
                  <TriangleAlert size={18} aria-hidden="true" />
                </Popover.Trigger>
              </Tooltip>
              <Popover.Portal>
                <Popover.Content
                  class="settings-feedback"
                  role="dialog"
                  aria-label={t("Settings need attention")}
                  align="end"
                  sideOffset={6}
                  collisionPadding={12}
                  onCloseAutoFocus={(event) => {
                    if (!feedbackSections.length) {
                      event.preventDefault();
                      document
                        .querySelector<HTMLButtonElement>('.settings-tabs [data-state="active"]')
                        ?.focus();
                    }
                  }}
                >
                  {#each feedbackSections as section (section.element.id)}
                    {@const feedback = section.controls().toolbarFeedback}
                    {#if feedback}
                      <div class="settings-feedback-section">
                        <h3>{tabLabels[section.element.id]}</h3>
                        {@render feedback()}
                      </div>
                    {/if}
                  {/each}
                </Popover.Content>
              </Popover.Portal>
            </Popover.Root>
          </div>
        </div>
      {/snippet}
    </PageControls>
    {#each validationSections as section (section.element.id)}
      <span
        id={`settings-validation-${section.element.id}`}
        class="validation-description"
        role="status"
      >
        {tabLabels[section.element.id]}: {section.controls().validationMessage}
      </span>
    {/each}
  </div>
  <Tabs.Content value="profiles" class="settings-content"><ProfilesPanel /></Tabs.Content>
  <Tabs.Content value="keenetic" class="settings-content"><PlatformSettings /></Tabs.Content>
  <Tabs.Content value="dns" class="settings-content"
    ><DnsQueryLog active={active && selected === "dns"} /></Tabs.Content
  >
  <Tabs.Content value="logs" class="settings-content">
    <LogsView active={active && selected === "logs"} />
  </Tabs.Content>
  <Tabs.Content value="general" class="settings-content">
    <GeneralSettingsPanel active={active && selected === "general"} pageSaving={saving} />
  </Tabs.Content>
</Tabs.Root>

<style>
  .settings-navigation {
    display: flex;
    align-items: center;
    gap: 0.35rem;
    min-width: 0;
  }
  .settings-navigation :global(.settings-tabs) {
    min-width: 0;
  }
  .settings-feedback-slot {
    flex: 0 0 2rem;
    width: 2rem;
    height: 2rem;
    display: flex;
    align-items: center;
    justify-content: center;
  }
  :global(.settings-status) {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    padding: 0.35rem;
    border: 0;
    border-radius: 0.35rem;
    color: var(--text-2);
    background: transparent;
    cursor: pointer;
  }
  :global(.settings-status[hidden]) {
    display: none;
  }
  :global(.settings-status:focus-visible) {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }
  :global(.settings-feedback) {
    z-index: 20;
    box-sizing: border-box;
    max-width: min(28rem, calc(100vw - 24px));
    max-height: min(30rem, calc(100vh - 24px));
    overflow-y: auto;
    padding: 0.75rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    background: var(--bg-dark-extra);
    color: var(--text);
    font: 400 0.9rem/1.5 var(--font);
    overflow-wrap: anywhere;
  }
  .settings-feedback-section h3 {
    margin: 0;
    font-size: 0.95rem;
    font-weight: 600;
  }
  :global(.settings-page) {
    min-width: 0;
  }
  .settings-toolbar {
    position: sticky;
    top: 0;
    z-index: 5;
    background: var(--bg-dark);
    border-bottom: 1px solid var(--bg-light-extra);
    padding-bottom: 0.45rem;
    --page-controls-background: var(--bg-dark);
  }
  .settings-toolbar :global(.settings-controls) {
    display: flex;
    flex-wrap: wrap;
    margin-bottom: 0;
    padding: 0.3rem 0;
  }
  .settings-toolbar :global(.page-controls-search) {
    padding-right: 0;
    transition: none;
  }
  .settings-toolbar :global(.page-controls-actions) {
    position: static;
    height: auto;
    flex-shrink: 0;
    margin-left: auto;
  }
  :global(.settings-content[data-state="inactive"]) {
    display: none;
  }
  :global(.settings-content) {
    min-width: 0;
    padding-top: 1rem;
  }
  .validation-description,
  :global(.settings-content > section > .sr-only) {
    position: absolute;
    width: 1px;
    height: 1px;
    padding: 0;
    margin: -1px;
    overflow: hidden;
    clip-path: inset(50%);
    border: 0;
  }
  :global(.settings-tabs) {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
  }
  :global(.settings-tabs [data-tabs-trigger]) {
    border: 0;
    border-bottom: 2px solid transparent;
    border-radius: 0;
    padding: 0.5rem;
    background: transparent;
    color: var(--text-2);
    font: 400 1.1rem / 1.4 var(--font);
    cursor: pointer;
  }
  :global(.settings-tabs [data-tabs-trigger][data-state="active"]) {
    color: var(--blue-light-extra);
    border-bottom-color: var(--blue-light-extra);
  }
  :global(.settings-tabs [data-tabs-trigger]:hover) {
    color: var(--text);
  }
  :global(.settings-tabs [data-tabs-trigger]:focus-visible) {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }
  @media (max-width: 700px) {
    :global(.settings-tabs) {
      gap: var(--space-1);
    }
    :global(.settings-tabs [data-tabs-trigger]) {
      padding-inline: 0.35rem;
      font-size: var(--text-small);
    }
    .settings-toolbar :global(.page-controls-search) {
      width: 100%;
    }
  }
</style>
