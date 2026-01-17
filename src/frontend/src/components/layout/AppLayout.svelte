<script lang="ts">
  import { Tabs } from "bits-ui";
  import { t } from "../../data/locale.svelte";
  import { X, Menu, LayoutList } from "../ui/icons";
  import GroupsView from "../../modules/groups/GroupsView.svelte";
  // import LogsPanel from "../../modules/logs/LogsPanel.svelte";
  // import SettingsPanel from "../../modules/settings/SettingsPanel.svelte";
  import Overlay from "../feedback/Overlay.svelte";
  import SnowField from "../feedback/SnowField.svelte";
  import Toast from "../feedback/Toast.svelte";
  import ScrollToTop from "../feedback/ScrollToTop.svelte";
  import HeaderSettings from "./HeaderSettings.svelte";

  let active_tab = $state("groups");
  let menuCheckbox: HTMLInputElement | undefined = $state();

  let innerWidth = $state(0);
  let resizing = $state(false);
  let resizeTimer: number;

  $effect(() => {
    if (innerWidth) {
      resizing = true;
      clearTimeout(resizeTimer);
      resizeTimer = window.setTimeout(() => {
        resizing = false;
      }, 200);
    }
  });

  const closeMenu = () => {
    if (menuCheckbox) menuCheckbox.checked = false;
  };
</script>

<svelte:window bind:innerWidth />

<Toast />
<Overlay />
<ScrollToTop />
{#if [11, 0, 1].includes(new Date().getMonth())}
  <SnowField />
{/if}

<main>
  <Tabs.Root bind:value={active_tab}>
    <nav>
      <div class="nav-left">
        <input type="checkbox" id="mobile-menu-toggle" bind:this={menuCheckbox} />

        <label for="mobile-menu-toggle" class="mobile-dropdown-btn">
          <div class="icon-open"><Menu size={28} /></div>
          <div class="icon-close"><X size={28} /></div>
        </label>

        <div class="tabs-container" class:resizing>
          <Tabs.List>
            <Tabs.Trigger value="groups" onclick={closeMenu}>
              <span class="tab-icon"><LayoutList size={24} /></span>
              {t("Groups")}
            </Tabs.Trigger>

            <!-- <Tabs.Trigger value="settings" onclick={closeMenu}>Settings</Tabs.Trigger>
            <Tabs.Trigger value="logs" onclick={closeMenu}>Logs</Tabs.Trigger> -->
          </Tabs.List>
        </div>
      </div>

      <div class="header-settings">
        <HeaderSettings />
      </div>
    </nav>

    <article>
      <Tabs.Content value="groups">
        <GroupsView />
      </Tabs.Content>
      <!-- <Tabs.Content value="settings">...</Tabs.Content> -->
    </article>
  </Tabs.Root>
</main>

<style>
  main {
    display: flex;
    flex-direction: column;
    align-items: center;
    margin-bottom: 2rem;
    padding: 0.3rem;
  }

  :global([data-tabs-root]) {
    width: 100%;
    max-width: 1000px;
    display: flex;
    flex-direction: column;
  }

  article {
    width: 100%;
  }

  nav {
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    width: 100%;
    max-width: 1000px;
    position: relative;
    z-index: 10;
  }

  .nav-left {
    display: flex;
    align-items: center;
  }

  #mobile-menu-toggle {
    display: none;
  }

  .mobile-dropdown-btn {
    display: none;
  }

  :global([data-tabs-list]) {
    display: flex;
    flex-direction: row;
    gap: 1rem;
    background: transparent;
  }

  :global([data-tabs-trigger]) {
    padding: 0.5rem 0.5rem;
    border: none;
    border-bottom: 2px solid transparent;
    font-size: 1.5rem;
    font-family: var(--font);
    background-color: transparent;
    color: var(--text-2);
    display: flex;
    align-items: center;
    gap: 0.5rem;
    cursor: pointer;
    white-space: nowrap;
    transition:
      color 0.2s,
      border-color 0.2s;
  }

  .tab-icon {
    display: flex; /* Делает размер контейнера равным размеру SVG */
    transform: translateY(-2px);
  }

  :global([data-tabs-trigger][data-state="active"]) {
    color: var(--blue-light-extra);
    border-color: var(--blue-light-extra);
  }

  :global([data-tabs-trigger]:hover) {
    color: var(--text);
  }

  :global([data-tabs-content]) {
    padding-top: 1rem;
  }

  .header-settings {
    overflow: hidden;
  }

  @media (max-width: 700px) {
    .mobile-dropdown-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 0.5rem;
      color: var(--text);
      cursor: pointer;
      user-select: none;
    }

    .icon-close,
    .icon-open {
      display: flex;
    }

    .icon-close {
      display: none;
    }

    .icon-open {
      display: flex;
    }

    #mobile-menu-toggle:checked + .mobile-dropdown-btn .icon-open {
      display: none;
    }

    #mobile-menu-toggle:checked + .mobile-dropdown-btn .icon-close {
      display: flex;
      color: var(--text);
    }

    .tabs-container {
      position: absolute;
      top: 100%;
      left: 0;
      right: 0;
      height: auto;
      background: var(--bg-dark);
      border-top: 1px solid var(--border-light);
      border-bottom: 1px solid var(--border-light);
      padding: 1rem 0.7rem 1rem 0.7rem;
      display: flex;
      flex-direction: column;
      opacity: 0;
      visibility: hidden;
      transform: translateY(-10px);
      transition:
        opacity 0.2s ease,
        transform 0.2s ease,
        visibility 0.2s;
      z-index: 99;
    }

    #mobile-menu-toggle:checked ~ .tabs-container {
      opacity: 1;
      visibility: visible;
      transform: translateY(0);
    }

    .tabs-container.resizing {
      transition: none !important;
    }

    :global([data-tabs-list]) {
      flex-direction: column;
      gap: 1.2rem;
      align-items: flex-start;
    }

    :global([data-tabs-trigger]) {
      font-size: 1.5rem;
      font-weight: 500;
      border-bottom: none;
      padding: 0;
      width: 100%;
      justify-content: flex-start;
      color: var(--text-2);
      display: flex;
      align-items: center;
      line-height: 1;
      gap: 0.8rem;
    }

    :global([data-tabs-trigger]:hover) {
      color: var(--text);
    }

    :global([data-tabs-trigger][data-state="active"]) {
      color: var(--blue-light-extra);
      background-color: transparent;
    }
  }
</style>
