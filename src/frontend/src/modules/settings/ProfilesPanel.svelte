<script lang="ts">
  import { onMount } from "svelte";

  import Button from "../../components/ui/Button.svelte";
  import Placeholder from "../../components/ui/Placeholder.svelte";
  import { t } from "../../data/locale.svelte";
  import { fetchProfiles, profiles, profilesDirty, saveProfiles } from "../../data/profiles.svelte";
  import ProfilePanel from "./ProfilePanel.svelte";

  import { toast } from "../../utils/events";
  import { newProfileId, profileError } from "./profiles-data";
  import { settingsSection } from "./section-controls";

  const section = settingsSection();
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
    } catch (error) {
      if (profiles.persistencePending)
        toast.error(t("Changes are applied but not saved to disk. Retry saving."));
      throw error;
    }
  }
</script>

<section
  class="profiles-settings"
  aria-label={t("Interface priority profiles")}
  aria-busy={busy}
  use:section={() => ({
    title: t("Interface priority profiles"),
    addLabel: t("Add profile"),
    canAdd: !unavailable,
    dirty,
    canSave: dirty && !unavailable && !validation,
    onAdd: addProfile,
    onSave: save,
    saveLabel: profiles.saving
      ? t("Saving...")
      : profiles.persistencePending
        ? t("Changes are applied but not saved to disk. Retry saving.")
        : dirty && validation
          ? t(validation)
          : t("Save Changes"),
  })}
>
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
  fieldset {
    min-width: 0;
    border: 0;
    padding: 0;
    margin: 0;
  }
</style>
