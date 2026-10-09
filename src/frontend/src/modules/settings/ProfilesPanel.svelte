<script lang="ts">
  import { onMount } from "svelte";

  import PageControls from "../../components/layout/PageControls.svelte";
  import Button from "../../components/ui/Button.svelte";
  import Placeholder from "../../components/ui/Placeholder.svelte";
  import { t } from "../../data/locale.svelte";
  import { fetchProfiles, profiles, profilesDirty, saveProfiles } from "../../data/profiles.svelte";
  import ProfilePanel from "./ProfilePanel.svelte";

  import { toast } from "../../utils/events";
  import { newProfileId, profileError } from "./profiles-data";

  const dirty = $derived(profilesDirty());
  const validation = $derived(profileError(profiles.draft));
  const busy = $derived(profiles.saving || profiles.loading);
  const unavailable = $derived(busy || !profiles.loaded || profiles.failed);
  onMount(() => {
    void fetchProfiles();
  });

  function addProfile() {
    profiles.draft.push({
      id: newProfileId(),
      name: "",
      interfaces: [],
      on_unavailable: "blackhole",
    });
  }

  async function save() {
    try {
      await saveProfiles();
      toast.success(t("Profiles saved"));
    } catch {
      // Request failures already use the shared fetcher notification.
      if (profiles.persistencePending)
        toast.error(t("Changes are applied but not saved to disk. Retry saving."));
    }
  }
</script>

<section class="profiles-settings" aria-label={t("Routing profiles")} aria-busy={busy}>
  <PageControls
    addLabel={t("Add profile")}
    canAdd={!unavailable}
    canSave={dirty && !unavailable && !validation}
    onAdd={addProfile}
    onSave={save}
    saveButtonId="save-profiles"
    saveLabel={profiles.saving
      ? t("Saving...")
      : profiles.persistencePending
        ? t("Changes are applied but not saved to disk. Retry saving.")
        : dirty && validation
          ? t(validation)
          : t("Save Changes")}
  >
    {#snippet search()}
      <h2 id="interface-priority-profiles">{t("Interface priority profiles")}</h2>
    {/snippet}
  </PageControls>

  {#if profiles.failed}
    <Placeholder variant="error" minHeight="auto">
      {t("Failed to load profiles")}
      {#snippet actions()}
        <Button onclick={() => void fetchProfiles()} disabled={busy}>{t("Retry")}</Button>
      {/snippet}
    </Placeholder>
  {:else if !profiles.loaded && profiles.loading}
    <Placeholder variant="loading" minHeight="auto">{t("Loading...")}</Placeholder>
  {:else if profiles.loaded && !profiles.draft.length}
    <Placeholder
      variant="empty"
      minHeight="auto"
      subtitle={t("Direct interface selection continues to work as before.")}
    >
      {t("No profiles yet")}
    </Placeholder>
  {/if}

  <fieldset disabled={unavailable}>
    {#each profiles.draft as profile (profile.id)}
      <ProfilePanel
        {profile}
        disabled={unavailable}
        usage={profiles.list.find((item) => item.id === profile.id)?.usage}
        ondelete={() => {
          if (confirm(`${t("Delete profile?")} ${profile.name}`))
            profiles.draft = profiles.draft.filter((item) => item.id !== profile.id);
        }}
      />
    {/each}
  </fieldset>
</section>

<style>
  h2 {
    margin: 0;
    color: var(--text-2);
    font: 600 1.3rem var(--font);
  }

  fieldset {
    min-width: 0;
    border: 0;
    padding: 0;
    margin: 0;
  }

  @media (max-width: 570px) {
    h2 {
      display: flex;
      align-items: center;
      min-height: var(--row-h);
    }
  }
</style>
