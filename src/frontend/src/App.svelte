<script lang="ts">
  import "./app.css";
  import AppLayout from "./components/layout/AppLayout.svelte";
  import AuthPage from "./components/AuthPage.svelte";
  import { token } from "./data/auth.svelte";
  import { fetchInterfaces } from "./data/interfaces.svelte";

  $effect(() => {
    if (token.current) {
      fetchInterfaces();
    }
  });
</script>

{#if token.current}
  <AppLayout />
{:else}
  <AuthPage />
{/if}
