<script lang="ts">
  import { onMount } from "svelte";

  import Button from "../../../components/ui/Button.svelte";
  import Placeholder from "../../../components/ui/Placeholder.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import Switch from "../../../components/ui/Switch.svelte";
  import { locales, t } from "../../../data/locale.svelte";
  import {
    fetchPolicySettings,
    policy,
    policyDirty,
    refreshPolicies,
    savePolicySettings,
    selectedPolicyId,
  } from "./state.svelte";

  import { settingsSection } from "../section-controls";
  import ru from "./ru.json";

  // This module and its translations are never imported by generic builds.
  Object.assign(locales.ru, ru);
  const section = settingsSection();
  const dirty = $derived(policyDirty());
  const selectedId = $derived(selectedPolicyId());
  const selected = $derived(policy.policies.find((p) => p.id === selectedId));
  const valid = $derived(
    !policy.draft.enabled ||
      Boolean(selected?.available && policy.listLoaded && !policy.listFailed),
  );
  const options = $derived([
    ...(policy.policies.some((p) => p.name === "mt") ? [] : [{ value: "", label: "mt" }]),
    ...policy.policies.map((p) => ({ value: p.id, label: p.name || p.id })),
  ]);

  onMount(() => {
    void fetchPolicySettings();
    if (!policy.listLoaded && !policy.listFailed) void refreshPolicies();
    const timer = setInterval(() => {
      if (
        policy.loaded &&
        !policy.saving &&
        (policy.runtime.state !== "disabled" || policy.draft.enabled)
      )
        void fetchPolicySettings(true);
    }, 3000);
    return () => clearInterval(timer);
  });
  async function save() {
    await savePolicySettings();
  }
</script>

<section
  class="policy-settings"
  aria-labelledby="keenetic-policy-title"
  aria-busy={policy.saving}
  use:section={() => ({
    title: t("Keenetic policy"),
    dirty,
    canSave: dirty && policy.loaded && !policy.saving && valid,
    onSave: save,
    saveLabel: policy.saving ? t("Saving...") : t("Save Changes"),
  })}
>
  <h2 id="keenetic-policy-title">{t("Keenetic policy")}</h2>

  {#if !policy.loaded}
    <Placeholder variant={policy.failed ? "error" : "loading"} minHeight="auto">
      {policy.failed ? t("Failed to load policy settings") : t("Loading...")}
      {#snippet actions()}
        {#if policy.failed}<Button
            disabled={policy.loading}
            onclick={() => void fetchPolicySettings()}>{t("Retry")}</Button
          >{/if}
      {/snippet}
    </Placeholder>
  {:else}
    <fieldset class="policy-layout" class:enabled={policy.draft.enabled} disabled={policy.saving}>
      <div class="policy-switches">
        <div class="switch-row">
          <Switch
            id="keenetic-policy-enabled"
            bind:checked={policy.draft.enabled}
            aria-label={t("Use policy based filter")}
          />
          <label for="keenetic-policy-enabled">{t("Use policy based filter")}</label>
        </div>
        {#if policy.draft.enabled}
          <div class="switch-row mode-switch">
            <Switch
              id="keenetic-policy-mode"
              bind:checked={
                () => policy.draft.mode === "exclude",
                (checked) => (policy.draft.mode = checked ? "exclude" : "include")
              }
              aria-label={t("Processing mode")}
            />
            <label for="keenetic-policy-mode">
              {policy.draft.mode === "include"
                ? t("Include: MT processes only devices in the selected policy")
                : t("Exclude: MT processes all devices except those in the selected policy")}
            </label>
          </div>
        {/if}
      </div>
      {#if policy.draft.enabled}
        <div class="policy-fields">
          <div class="policy-choice">
            <span id="keenetic-policy-label">{t("Router policy")}</span>
            <div class="picker-row">
              <Select
                {options}
                compact
                bind:selected={() => selectedId, (id) => (policy.draft.policy_id = id)}
                ariaLabel={t("Router policy")}
                disabled={policy.listing || policy.saving}
                missingLabel={policy.draft.policy_id
                  ? `${policy.draft.policy_id} · ${t("Policy unavailable")}`
                  : "mt"}
              />
              {#if !policy.listing && !valid}
                <p class="validation" role="status">
                  {policy.listFailed
                    ? t("Could not load policies. Reload the page to retry.")
                    : !selectedId
                      ? t('Create the "mt" policy in Keenetic, then reload this page.')
                      : t("Select an available policy to save.")}
                </p>
              {/if}
            </div>
          </div>
        </div>
      {/if}
    </fieldset>
    {#if policy.persistencePending}
      <p class="notice" role="alert">
        {t("Changes are applied but not saved to disk. Retry saving.")}
      </p>
    {/if}
  {/if}
</section>

<style>
  h2 {
    margin: 0 0 1rem;
    color: var(--text-2);
    font: 600 1.3rem / 1.4 var(--font);
  }
  fieldset {
    min-width: 0;
    border: 0;
    padding: 0;
    margin: 0;
  }
  .switch-row {
    display: flex;
    align-items: center;
    gap: 0.65rem;
    padding: 0.5rem 0;
    line-height: 1.5;
  }
  .policy-layout.enabled {
    display: grid;
    grid-template-columns: minmax(0, 3fr) minmax(0, 2fr);
    gap: 1.5rem;
    align-items: stretch;
  }
  .policy-switches {
    min-width: 0;
  }
  .policy-fields {
    min-width: 0;
    border-left: 1px solid var(--bg-light-extra);
    padding: 0.5rem 0 0.5rem 1.5rem;
  }
  .policy-choice {
    display: grid;
    gap: 0.5rem;
  }
  .policy-choice > span {
    line-height: 1.5;
    color: var(--text-2);
  }
  .picker-row {
    min-width: 0;
  }
  .picker-row :global(.select-wrap),
  .picker-row :global([data-select-trigger]) {
    max-width: 100%;
    min-width: 0;
  }
  .picker-row :global([data-select-trigger]) {
    padding: 0.3rem 0.5rem;
    background: var(--bg-light);
    border: 1px solid var(--bg-light-extra);
  }
  .picker-row :global([data-select-trigger]:hover),
  .picker-row :global([data-select-trigger]:focus-visible) {
    background: var(--bg-light-extra);
  }
  .mode-switch {
    align-items: flex-start;
    padding: 0.4rem 0;
  }
  .mode-switch :global([data-switch-root]) {
    margin-top: 0.2rem;
  }
  .switch-row label {
    cursor: pointer;
  }
  .validation {
    margin: 0.5rem 0 0;
    color: var(--text-2);
    font-size: 0.8rem;
    font-style: italic;
    line-height: 1.5;
  }
  .notice {
    color: var(--text);
    line-height: 1.5;
    border-left: 2px solid var(--accent);
    padding-left: 0.75rem;
    margin: 0.5rem 0;
  }
  .policy-settings :global(button:focus-visible) {
    outline: 2px solid var(--accent);
    outline-offset: 3px;
  }
  @media (max-width: 700px) {
    .policy-layout.enabled {
      grid-template-columns: minmax(0, 1fr);
      gap: 1rem;
    }
    .policy-fields {
      border-left: 0;
      padding: 0;
    }
  }
</style>
