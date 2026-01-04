<script lang="ts">
  import { createEventDispatcher } from "svelte";
  import GenericDialog from "../../../components/ui/GenericDialog.svelte";
  import Button from "../../../components/ui/Button.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import { RULE_TYPES } from "../../../types";
  import { defaultRule } from "../../../utils/defaults";
  import {
    isValidSubnet,
    isValidSubnet6,
    isValidNamespace,
    isValidDomain,
    isValidWildcard,
    isValidRegex,
    VALIDATOP_MAP,
  } from "../../../utils/rule-validators";
  import type { Rule } from "../../../types";
  import { toast } from "../../../utils/events";
  import { t } from "../../../data/locale.svelte";

  export let open = false;
  export let group_index: number | null = null;

  const dispatch = createEventDispatcher();
  let import_rules_text = "";
  let triedSubmit = false;

  const RULE_TYPE_SELECT = [{ value: "auto", label: "Auto" }, ...RULE_TYPES];
  type RuleTypeValue = (typeof RULE_TYPE_SELECT)[number]["value"];
  let selectedRuleType: RuleTypeValue = "auto";

  function handleKeydown(e: KeyboardEvent) {
    if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
      e.preventDefault();
      submit();
    }
  }

  function detectRuleType(pattern: string): keyof typeof VALIDATOP_MAP {
    const p = pattern.trim();

    if (isValidSubnet6(p)) return "subnet6";
    if (isValidSubnet(p)) return "subnet";
    if (p.split('.').length >= 3 && isValidDomain(p)) return "domain";
    if (isValidNamespace(p)) return "namespace";
    if (isValidRegex(p)) return "regex";
    if (isValidWildcard(p)) return "wildcard";

    return "domain";
  }

  function submit() {
    triedSubmit = true;

    if (!import_rules_text.trim() || group_index === null) return;

    const seen = new Set<string>();
    const lines = import_rules_text
      .split(/[\n,]+/)
      .map((l) => l.trim())
      .filter((l) => l && !l.startsWith("#"))
      .filter((l) => {
        const type = selectedRuleType === "auto" ? detectRuleType(l) : selectedRuleType;
        const key = `${type}|${l}`;
        if (seen.has(key)) return false;
        seen.add(key);
        return true;
      });

    if (!lines.length) return;

    const rules: Rule[] = lines.map((line) => ({
      ...defaultRule(),
      rule: line,
      type: selectedRuleType === "auto" ? detectRuleType(line) : selectedRuleType,
    }));

    dispatch("import", { group_index, rules });
    toast.success(t("Imported rules: " + rules.length));
    close();
  }

  function close() {
    import_rules_text = "";
    triedSubmit = false;
    selectedRuleType = "auto";
    dispatch("close");
  }
</script>

<svelte:window on:keydown={handleKeydown} />
<GenericDialog
  {open}
  {triedSubmit}
  title={t("Import Rule List")}
  textareaValue={import_rules_text}
  textareaPlaceholder={t("Insert a list of IPs or domains, one per line")}
  on:close={close}
  on:textareaInput={(e) => (import_rules_text = e.detail)}
  on:submit={submit}
>
  <div slot="actions" class="rule-type-select">
    <Select options={RULE_TYPE_SELECT} bind:selected={selectedRuleType} />
    <Button type="submit" on:click={submit}>{t("Import")}</Button>
  </div>
</GenericDialog>

<style>
  .rule-type-select :global(.select-wrap),
  .rule-type-select :global([data-select-root]),
  .rule-type-select :global([data-select-trigger]) {
    display: block !important;
    width: 100% !important;
    box-sizing: border-box;
  }
  .rule-type-select :global([data-select-trigger] .selected) {
    display: flex;
    align-items: center;
    justify-content: space-between;
    width: 100% !important;
  }
  .rule-type-select :global([data-select-content]) {
    min-width: 100% !important;
    width: auto;
  }
  .rule-type-select {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0.5rem;
    width: 100%;
  }
  .rule-type-select :global(.select-root),
  .rule-type-select :global([data-select-trigger]) {
    width: 100% !important;
    justify-content: space-between;
  }
  .rule-type-select :global(.select-root),
  .rule-type-select :global(button) {
    height: 2.5rem;
    font-size: 0.85rem;
    color: var(--text);
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    background-color: var(--bg-light);
  }
  .rule-type-select :global(button) {
    padding: 0 1rem;
  }
  .rule-type-select :global([data-select-trigger]:hover),
  .rule-type-select :global(button:hover) {
    background-color: var(--bg-light-extra);
  }
  @media (max-width: 315px) {
    .rule-type-select {
      grid-template-columns: 1fr;
    }
  }
</style>
