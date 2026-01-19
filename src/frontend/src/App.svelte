<script lang="ts">
  import "./app.css";
  import AppLayout from "./components/layout/AppLayout.svelte";
  import AuthPage from "./components/AuthPage.svelte";
  import { token, authState } from "./data/auth.svelte";
  import { fetchInterfaces } from "./data/interfaces.svelte";
  import { fetcher } from "./utils/fetcher";
  import { onMount } from "svelte";

  onMount(async () => {
    try {
      const res = await fetcher.get<{ auth_enabled: boolean }>("/auth");
      authState.enabled = res.auth_enabled;
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
