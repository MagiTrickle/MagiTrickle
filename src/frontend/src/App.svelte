<script lang="ts">
  import "./app.css";

  import { onMount } from "svelte";

  import AuthPage from "./components/AuthPage.svelte";
  import AppLayout from "./components/layout/AppLayout.svelte";
  import { authState, token } from "./data/auth.svelte";
  import { fetchInterfaces } from "./data/interfaces.svelte";
  import TooltipLayer from "./lib/tooltip/TooltipLayer.svelte";

  import { fetcher } from "./utils/fetcher";

  onMount(async () => {
    try {
      const { enabled } = await fetcher.get<{ enabled: boolean }>("/auth");
      authState.enabled = enabled;
    } catch (e) {
      console.error("Failed to check auth status", e);
    } finally {
      authState.checked = true;
    }
  });

  $effect(() => {
    if (token.current || (authState.checked && !authState.enabled)) {
      fetchInterfaces();
    }
  });
</script>

{#if !authState.checked}
  <!-- loading... -->
{:else if authState.enabled && !token.current}
  <AuthPage />
{:else}
  <AppLayout />
{/if}

<TooltipLayer />
