<script lang="ts">
  import { Tabs } from "bits-ui";

  import { t } from "../../data/locale.svelte";
  import GroupsView from "../../modules/groups/GroupsView.svelte";
  // import LogsPanel from "../../modules/logs/LogsPanel.svelte";
  // import SettingsPanel from "../../modules/settings/SettingsPanel.svelte";
  import Overlay from "../feedback/Overlay.svelte";
  import ScrollToTop from "../feedback/ScrollToTop.svelte";
  import SnowField from "../feedback/SnowField.svelte";
  import Toast from "../feedback/Toast.svelte";
  import HeaderSettings from "./HeaderSettings.svelte";

  import { LayoutList, Menu } from "../ui/icons";

  let active_tab = $state("groups");
  let isMenuOpen = $state(false);
  let isRenderComplete = $state(false);

  const toggleMenu = () => (isMenuOpen = !isMenuOpen);
  const closeMenu = () => (isMenuOpen = false);
</script>

<Toast />
<Overlay />
<ScrollToTop />
{#if [11, 0, 1].includes(new Date().getMonth())}
  <SnowField visible={isRenderComplete} />
{/if}

<main>
  <Tabs.Root bind:value={active_tab}>
    <nav>
      <div class="nav-left">
        <button
          type="button"
          class="mobile-dropdown-btn"
          aria-label={isMenuOpen ? "Close menu" : "Open menu"}
          aria-expanded={isMenuOpen}
          onclick={toggleMenu}
        >
          <span class="menu-morph" class:open={isMenuOpen} aria-hidden="true">
            <Menu size={24} />
          </span>
        </button>

        <!-- Tabs container -->
        <div class="tabs-panel" class:open={isMenuOpen}>
          <Tabs.List>
            <Tabs.Trigger value="groups" onclick={closeMenu}>
              <span class="tab-icon"><LayoutList size={24} /></span>
              {t("Groups")}
            </Tabs.Trigger>

            <!--
            <Tabs.Trigger value="settings" onclick={closeMenu}>Settings</Tabs.Trigger>
            <Tabs.Trigger value="logs" onclick={closeMenu}>Logs</Tabs.Trigger>
            -->
          </Tabs.List>
        </div>
      </div>

      <div class="header-settings">
        <HeaderSettings />
      </div>
    </nav>

    <article>
      <Tabs.Content value="groups">
        <GroupsView onRenderComplete={() => (isRenderComplete = true)} />
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

  .header-settings {
    overflow: hidden;
  }

  .mobile-dropdown-btn {
    display: none;
  }

  .tabs-panel {
    display: block;
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
    display: flex;
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

  @media (max-width: 700px) {
    .mobile-dropdown-btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      padding: 0.5rem;
      border: 0;
      background: transparent;
      color: var(--text);
      cursor: pointer;
      user-select: none;
      -webkit-tap-highlight-color: transparent;
    }

    .tabs-panel {
      position: absolute;
      top: 100%;
      left: 0;
      right: 0;
      background: var(--bg-dark);
      border-top: 1px solid var(--border-light);
      border-bottom: 1px solid var(--border-light);
      padding: 1rem 0.7rem;
      display: flex;
      flex-direction: column;
      z-index: 99;
      opacity: 0;
      visibility: hidden;
      pointer-events: none;
      transition: none;
    }

    .tabs-panel.open {
      opacity: 1;
      visibility: visible;
      pointer-events: auto;
      transform: translateY(0);
      transition:
        opacity 160ms ease,
        transform 160ms ease,
        visibility 0s linear 0s;
    }

    @supports selector(:has(*)) {
      @starting-style {
        .tabs-panel.open {
          transform: translateY(-10px);
        }
      }
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
      gap: 0.8rem;
      line-height: 1;
    }
  }

  .menu-morph {
    --icon: 24px;
    --dur-move: 170ms;
    --dur-fade: 110ms;
    --ease: cubic-bezier(0.2, 0.8, 0.2, 1);
    --offset: calc(var(--icon) * 7 / 24);
    --xscale: 1.08;

    width: var(--icon);
    height: var(--icon);
    display: inline-block;
  }

  .menu-morph :global(svg) {
    width: var(--icon);
    height: var(--icon);
    display: block;
  }

  .menu-morph :global(path) {
    vector-effect: non-scaling-stroke;
    transform-box: fill-box;
    transform-origin: center;
    will-change: transform, opacity;

    transition-property: transform, opacity;
    transition-duration: var(--dur-move), var(--dur-fade);
    transition-timing-function: var(--ease), ease-out;
  }

  .menu-morph:not(.open) :global(path:nth-child(2)) {
    transition-delay: 60ms, 0ms;
  }

  .menu-morph.open :global(path:nth-child(1)) {
    transform: translateY(var(--offset)) rotate(45deg) scale(var(--xscale));
  }

  .menu-morph.open :global(path:nth-child(3)) {
    transform: translateY(calc(var(--offset) * -1)) rotate(-45deg) scale(var(--xscale));
  }

  .menu-morph.open :global(path:nth-child(2)) {
    opacity: 0;
    transform: scaleX(0);
    transition-delay: 0ms, 0ms;
  }

  @media (prefers-reduced-motion: reduce) {
    .menu-morph :global(path),
    .tabs-panel.open {
      transition: none !important;
    }
  }
</style>
