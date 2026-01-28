<script lang="ts">
  import { createEventDispatcher } from "svelte";

  import Button from "../../../components/ui/Button.svelte";
  import GenericDialog from "../../../components/ui/GenericDialog.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import { interfaces } from "../../../data/interfaces.svelte";
  import { t } from "../../../data/locale.svelte";

  import { Info, Link, LoaderCircle, Network, Refresh, Type } from "../../../components/ui/icons";
  import type { SubscriptionRule } from "../../../types";
  import { handleIntervalChange, intervals } from "../components/SubscriptionPanel.svelte";
  import { fetcher } from "../../../utils/fetcher";

  type DialogProps = {
    open: boolean;
    existingUrls?: string[];
  };

  let { open, existingUrls = [] }: DialogProps = $props();

  const dispatch = createEventDispatcher();

  let step = $state(1);
  let url = $state("");
  let name = $state("");
  let selectedInterface = $state("");
  let selectedInterval = $state(86400);
  let rules = $state<SubscriptionRule[]>([]);
  let isLoading = $state(false);
  let error = $state<string | null>(null);
  let fetchError = $state(false);

  let typeBreakdown = $derived.by(() => {
    const counts: Record<string, number> = {};
    rules.forEach((r) => {
      counts[r.type] = (counts[r.type] || 0) + 1;
    });
    return Object.entries(counts)
      .filter(([_, count]) => count > 0)
      .map(([type, count]) => `${count} ${t(type)}`)
      .join(", ");
  });

  function reset() {
    step = 1;
    url = "";
    name = "";
    selectedInterface = interfaces.list[0] || "";
    selectedInterval = 86400;
    rules = [];
    isLoading = false;
    error = null;
    fetchError = false;
  }

  function handleClose() {
    reset();
    dispatch("close");
  }

  function validateUrl(val: string) {
    if (!val) return false;
    try {
      new URL(val);
      return true;
    } catch {
      return false;
    }
  }

  let isDuplicate = $derived(existingUrls.includes(url));
  let isValidUrl = $derived(validateUrl(url) && !isDuplicate);

  let displayedError = $derived(
    isDuplicate ? t("Subscription already exists") : error || t("Request failed"),
  );
  let isErrorVisible = $derived(isDuplicate || fetchError);
  async function handleNext() {
    if (!url || !isValidUrl) {
      error = t("Invalid URL");
      fetchError = true;
      setTimeout(() => {
        fetchError = false;
      }, 3000);
      return;
    }

    isLoading = true;
    error = null;
    fetchError = false;
    try {
      const res = await fetcher.get<{ rules: SubscriptionRule[] }>(
        `/subscription/rules?url=${encodeURIComponent(url)}`,
      );
      rules = res.rules;
      step = 2;
      if (!selectedInterface) {
        selectedInterface = interfaces.list[0] || "";
      }
    } catch (e) {
      console.error(e);
      error = t("Failed to fetch rules");
      fetchError = true;
      setTimeout(() => {
        fetchError = false;
      }, 3000);
    } finally {
      isLoading = false;
    }
  }

  function handleAdd() {
    dispatch("add", {
      url,
      name,
      rules,
      interface: selectedInterface,
      interval: selectedInterval,
    });
    handleClose();
  }


  function handleKeydown(e: KeyboardEvent) {
    if (e.key === "Enter") {
      e.preventDefault();
      if (step === 1) {
        if (isValidUrl && !isLoading) handleNext();
      } else {
        if (rules.length > 0) {
          handleAdd();
        }
      }
    }
  }
</script>

<GenericDialog
  {open}
  title={step === 1 ? t("Add Subscription") : t("Confirm Subscription")}
  on:close={handleClose}
  maxWidth={400}
>
  <div slot="body" class="subscription-dialog-content">
    {#if step === 1}
      <div class="field">
        <label for="sub-url">{t("URL")}</label>
        <div class="subscription-input-wrapper">
          <span class="icon"><Link size={18} /></span>
          <!-- svelte-ignore a11y_autofocus -->
          <input
            id="sub-url"
            type="text"
            bind:value={url}
            placeholder="https://example.com/list.txt"
            class:invalid={url !== "" && !isValidUrl}
            onkeydown={handleKeydown}
            oninput={() => {
              error = null;
              fetchError = false;
            }}
            autofocus
          />
        </div>
      </div>
    {:else}
      <div class="subscription-preview">
        <div class="rules-count-row">
          <span class="total">{t("Found rules")}: {rules.length}</span>
          {#if typeBreakdown}
            <span class="breakdown">({typeBreakdown})</span>
          {/if}
        </div>

        <div class="field">
          <label for="sub-name">{t("Name")}</label>
          <div class="subscription-input-wrapper">
            <span class="icon"><Type size={18} /></span>
            <!-- svelte-ignore a11y_autofocus -->
            <input
              id="sub-name"
              type="text"
              bind:value={name}
              placeholder={t("Subscription Name")}
              onkeydown={handleKeydown}
              autofocus
            />
          </div>
        </div>

        <div class="field-row">
          <div class="field">
            <label for="sub-interval">{t("Update every")}</label>
            <div class="subscription-input-wrapper">
              <span class="icon"><Refresh size={18} /></span>
              <Select
                id="sub-interval"
              options={intervals.map((item) => ({
                value: String(item.value),
                label: t(item.labelKey),
              }))}
              selected={String(selectedInterval)}
              onValueChange={(value) =>
                handleIntervalChange(value, (next) => {
                  selectedInterval = next;
                })}
              class="interval-select"
            />
            </div>
          </div>

          <div class="field">
            <label for="sub-interface">{t("Interface")}</label>
            <div class="subscription-input-wrapper">
              <span class="icon"><Network size={18} /></span>
              <Select
                id="sub-interface"
                options={interfaces.list.map((i) => ({ value: i, label: i }))}
                bind:selected={selectedInterface}
                class="interface-select"
              />
            </div>
          </div>
        </div>
      </div>
    {/if}
  </div>

  <div slot="actions" class="subscription-dialog-actions">
    <div class="helper-text" class:visible={isErrorVisible}>
      <Info size={16} /><span> {displayedError}</span>
    </div>
    <div class="button-container">
      {#if step === 1}
        <Button
          class={fetchError ? "fail" : ""}
          onclick={handleNext}
          disabled={!isValidUrl || isLoading}
          style="width: 100%"
        >
          <div class="button-content">
            {#if isLoading}
              <LoaderCircle class="spin" size={18} />
            {/if}
            <span>{t("Next")}</span>
          </div>
        </Button>
      {:else}
        <Button
          onclick={handleAdd}
          disabled={rules.length === 0}
          style="width: 100%"
        >
          >{t("Add")}</Button
        >
      {/if}
    </div>
  </div>
</GenericDialog>

<style>
  .subscription-dialog-content {
    display: flex;
    flex-direction: column;
    gap: 1.5rem;
    padding: 1rem 0;
  }

  .field {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }

  .field-row {
    display: flex;
    gap: 1rem;
    align-items: flex-start;
  }

  .field-row .field {
    flex: 1 1 0;
  }

  @media (max-width: 520px) {
    .field-row {
      flex-direction: column;
    }
  }

  label {
    color: var(--text-2);
    font-size: 0.9rem;
  }

  .subscription-input-wrapper {
    position: relative;
    display: flex;
    align-items: center;
  }

  .icon {
    position: absolute;
    left: 0.75rem;
    color: var(--text-2);
    display: flex;
    align-items: center;
    pointer-events: none;
    z-index: 1;
  }

  input,
  :global(.interface-select [data-select-trigger]),
  :global(.interval-select [data-select-trigger]) {
    background-color: var(--bg-dark-extra) !important;
    border: 1px solid var(--bg-light-extra) !important;
    color: var(--text) !important;
    padding: 0.75rem !important;
    padding-left: 2.5rem !important;
    padding-right: 2.5rem !important;
    border-radius: 0.5rem !important;
    font-size: 1rem !important;
    font-family: var(--font) !important;
    outline: none !important;
    transition: border-color 0.2s !important;
    width: 100% !important;
    box-sizing: border-box !important;
  }

  :global(.interface-select [data-select-trigger]),
  :global(.interval-select [data-select-trigger]) {
    display: flex;
    align-items: center;
    justify-content: flex-start;
    height: auto !important;
    min-height: 2.85rem;
  }

  input:focus,
  :global(.interface-select [data-select-trigger]:focus) {
    border-color: var(--accent) !important;
  }

  input.invalid {
    border-color: var(--red) !important;
  }

  /* Hack for autofill color */
  input:-webkit-autofill,
  input:-webkit-autofill:hover,
  input:-webkit-autofill:focus,
  input:-webkit-autofill:active {
    -webkit-box-shadow: 0 0 0 30px var(--bg-dark-extra) inset !important;
    -webkit-text-fill-color: var(--text) !important;
  }

  .subscription-dialog-actions {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 1rem;
    width: 100%;
    margin-top: 1rem;
  }

  .helper-text {
    flex: 1;
    color: var(--red);
    font-size: 0.8rem;
    font-style: italic;
    display: flex;
    align-items: center;
    gap: 0.35rem;
    opacity: 0;
    transition: opacity 0.3s ease-in-out;
    pointer-events: none;
  }

  .helper-text.visible {
    opacity: 1;
  }

  .button-container {
    width: 33.33%;
  }

  .button-content {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 0.5rem;
  }

  .subscription-preview {
    display: flex;
    flex-direction: column;
    gap: 1.2rem;
    color: var(--text);
  }

  .rules-count-row {
    display: flex;
    align-items: baseline;
    gap: 0.5rem;
    margin-bottom: -0.2rem;
  }

  .total {
    font-weight: 600;
  }

  .breakdown {
    font-size: 0.8rem;
    color: var(--text-2);
    font-style: italic;
  }

  :global(.spin) {
    animation: spin 1s linear infinite;
  }

  @keyframes spin {
    from {
      transform: rotate(0deg);
    }
    to {
      transform: rotate(360deg);
    }
  }
</style>
