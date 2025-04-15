<script lang="ts">
  import { Tabs } from "bits-ui";
  import Groups from "./blocks/Groups.svelte";
  import HeaderSettings from "./blocks/HeaderSettings.svelte";
  import Logs from "./blocks/Logs.svelte";
  import Overlay from "./blocks/Overlay.svelte";
  import Toast from "./blocks/Toast.svelte";
  // import SettingsPage from "./blocks/Settings.svelte";

  let active_tab = $state("groups");
</script>

<!-- TODO: add locales -->
<!-- TODO: add white/dark themes -->

<Toast />
<Overlay />

<main>
  <Tabs.Root bind:value={active_tab}>
    <nav>
      <Tabs.List>
        <Tabs.Trigger value="groups">Groups</Tabs.Trigger>
        <!-- <Tabs.Trigger value="settings">Settings</Tabs.Trigger> -->
        <Tabs.Trigger value="logs">Logs</Tabs.Trigger>
      </Tabs.List>
      <div class="header-settings">
        <HeaderSettings />
      </div>
    </nav>
    <article>
      <Tabs.Content value="groups">
        <Groups />
      </Tabs.Content>
      <!-- <Tabs.Content value="settings">
        <SettingsPage />
      </Tabs.Content> -->
      <Tabs.Content value="logs">
        <Logs />
      </Tabs.Content>
    </article>
  </Tabs.Root>
</main>

<style>
  main {
    display: flex;
    flex-direction: column;
    align-items: center;
    margin-bottom: 2rem;
  }

  nav {
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
  }

  :global {
    [data-tabs-root] {
      max-width: 1000px;
      width: 100%;
    }

    [data-tabs-list] {
      display: flex;
      flex-direction: row;
      gap: 1rem;
    }

    [data-tabs-trigger] {
      & {
        padding: 0.5rem 1rem;
        border: none;
        border-bottom: 2px solid transparent;
        font-size: 1.5rem;
        font-family: var(--font);
        background-color: transparent;
        color: var(--text);
      }

      &[data-state="active"] {
        color: var(--accent);
        border-color: var(--accent);
      }

      &[data-state="inactive"]:hover {
        border-color: var(--text);
      }
    }

    [data-tabs-content] {
      padding-top: 1rem;
    }
  }

  @media (max-width: 700px) {
    :global {
      [data-tabs-root] {
        width: 100%;
      }
      [data-tabs-trigger] {
        font-size: 1.2rem;
        padding: 0.5rem 0.5rem;
      }
      [data-tabs-list] {
        gap: 0.5rem;
      }
    }
  }
</style>
