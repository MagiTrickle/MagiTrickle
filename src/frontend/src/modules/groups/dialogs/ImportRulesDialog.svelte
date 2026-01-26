<script lang="ts">
  import LoaderCircle from "lucide-svelte/icons/loader-circle";
  import { createEventDispatcher, tick } from "svelte";

  import Button from "../../../components/ui/Button.svelte";
  import GenericDialog from "../../../components/ui/GenericDialog.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import { t } from "../../../data/locale.svelte";

  import { RULE_TYPES, type Rule } from "../../../types";
  import { defaultRule } from "../../../utils/defaults";
  import { toast } from "../../../utils/events";
  import {
    isValidDomain,
    isValidNamespace,
    isValidRegex,
    isValidSubnet,
    isValidSubnet6,
    isValidWildcard,
    VALIDATOP_MAP,
  } from "../../../utils/rule-validators";

  let { open = $bindable(false), group_index = null } = $props();

  const dispatch = createEventDispatcher();

  let import_rules_text = $state("");
  let triedSubmit = $state(false);
  let isParsing = $state(false);
  let isEditing = $state(true);
  let textAreaRef = $state<HTMLTextAreaElement | null>(null);

  type ParsedLine = {
    text: string;
    type: string | null;
    isValid: boolean;
  };

  let parsedLines = $state<ParsedLine[]>([]);

  const RULE_TYPE_SELECT = [{ value: "auto", label: "Auto" }, ...RULE_TYPES];
  type RuleTypeValue = (typeof RULE_TYPE_SELECT)[number]["value"];
  let selectedRuleType = $state<RuleTypeValue>("auto");

  let isEmpty = $derived(!import_rules_text.trim());

  $effect(() => {
    const type = selectedRuleType;
    if (!isEditing && import_rules_text.trim()) {
      parseRules(true);
    }
  });

  const TYPE_COLORS: Record<string, string> = {
    namespace: "#3182ce",
    wildcard: "#d69e2e",
    regex: "#d53f8c",
    domain: "#805ad5",
    subnet: "#38a169",
    subnet6: "#088484",
    INVALID: "#e53e3e",
  };

  const TYPE_LABELS: Record<string, string> = {
    namespace: "namespace",
    wildcard: "wildcard",
    regex: "regex",
    domain: "domain",
    subnet: "IPv4",
    subnet6: "IPv6",
    INVALID: "INVALID",
  };

  function handleKeydown(e: KeyboardEvent) {
    if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
      e.preventDefault();
      submit();
    }
  }

  function detectRuleType(pattern: string): keyof typeof VALIDATOP_MAP | null {
    const p = pattern.trim();
    if (isValidSubnet6(p)) return "subnet6";
    if (isValidSubnet(p)) return "subnet";
    if (isValidNamespace(p)) return "namespace";
    if (isValidDomain(p)) return "domain";
    if (isValidRegex(p)) return "regex";
    if (isValidWildcard(p)) return "wildcard";
    return null;
  }

  function getParsedData(text: string, ruleType: RuleTypeValue): ParsedLine[] {
    const lines = text.split(/[\n,]+/);
    return lines.map((line) => {
      const trimmed = line.trim();
      if (!trimmed || trimmed.startsWith("#")) {
        return { text: line, type: null, isValid: false };
      }

      let type: string | null = null;
      if (ruleType === "auto") {
        type = detectRuleType(trimmed);
      } else {
        const validator = VALIDATOP_MAP[ruleType];
        if (validator && validator(trimmed)) {
          type = ruleType;
        }
      }

      return {
        text: line,
        type: type || "INVALID",
        isValid: !!type,
      };
    });
  }

  async function parseRules(force = false) {
    if (!force && (!import_rules_text.trim() || isParsing)) return;

    isParsing = true;
    isEditing = false;

    await new Promise((r) => setTimeout(r, 400));
    parsedLines = getParsedData(import_rules_text, selectedRuleType);
    isParsing = false;
  }

  function submit() {
    triedSubmit = true;
    if (group_index === null) return;

    const currentParsed = isEditing
      ? getParsedData(import_rules_text, selectedRuleType)
      : parsedLines;

    const rules: Rule[] = [];
    const seen = new Set<string>();

    currentParsed.forEach((l) => {
      const trimmed = l.text.trim();
      if (l.isValid && l.type && l.type !== "INVALID" && trimmed && !trimmed.startsWith("#")) {
        const key = `${l.type}|${trimmed}`;
        if (!seen.has(key)) {
          seen.add(key);
          rules.push({
            ...defaultRule(),
            rule: trimmed,
            type: l.type as any,
          });
        }
      }
    });

    if (rules.length === 0) return;
    finishImport(rules);
  }

  function finishImport(rules: Rule[]) {
    dispatch("import", { group_index, rules });
    toast.success(t("Imported rules: " + rules.length));
    close();
  }

  function close() {
    import_rules_text = "";
    parsedLines = [];
    triedSubmit = false;
    isEditing = true;
    selectedRuleType = "auto";
    dispatch("close");
  }

  function onPaste() {
    setTimeout(() => parseRules(), 50);
  }

  function onBlur() {
    if (import_rules_text.trim()) {
      parseRules();
    }
  }

  async function switchToEdit() {
    isEditing = true;
    await tick();
    textAreaRef?.focus();
  }

  let counts = $derived.by(() => {
    const c: Record<string, number> = {};
    parsedLines.forEach((l) => {
      if (l.text.trim() && !l.text.trim().startsWith("#")) {
        const type = l.type || "INVALID";
        c[type] = (c[type] || 0) + 1;
      }
    });
    return c;
  });
</script>

<svelte:window onkeydown={handleKeydown} />

<GenericDialog
  {open}
  {triedSubmit}
  title={t("Import Rule List")}
  maxWidth={550}
  on:close={close}
  on:submit={submit}
>
  <div slot="body" class="dialog-body">
    <div class="editor-container" class:parsing={isParsing} class:empty={isEmpty}>
      {#if isEditing || isParsing}
        <textarea
          bind:this={textAreaRef}
          bind:value={import_rules_text}
          placeholder={t("Insert a list of IPs or domains, one per line")}
          class:invalid={triedSubmit && isEmpty}
          disabled={isParsing}
          onpaste={onPaste}
          onblur={onBlur}
        ></textarea>
        {#if isParsing}
          <div class="parsing-overlay">
            <span class="spin"><LoaderCircle size={30} /></span>
          </div>
        {/if}
      {:else}
        <div
          class="results-view"
          onclick={switchToEdit}
          onkeydown={(e) => e.key === "Enter" && switchToEdit()}
          role="button"
          tabindex="0"
        >
          {#if parsedLines.length === 0}
            <div class="empty-state">{t("No rules found")}</div>
          {/if}
          {#each parsedLines as line}
            <div class="line-row" class:invalid={!line.isValid && line.text.trim().length > 0}>
              <span class="line-text">{line.text}</span>
              {#if line.text.trim() && !line.text.trim().startsWith("#")}
                <span
                  class="badge"
                  style="--badge-color: {TYPE_COLORS[line.type || 'INVALID'] || 'var(--text-2)'}"
                >
                  {TYPE_LABELS[line.type || "INVALID"] || line.type}
                </span>
              {/if}
            </div>
          {/each}
        </div>
      {/if}
    </div>

    {#if !isEditing && !isParsing && Object.keys(counts).length > 0}
      <div class="counts">
        {#each Object.entries(counts) as [type, count]}
          <span
            class="badge count-badge"
            style="--badge-color: {TYPE_COLORS[type] || 'var(--text-2)'}"
          >
            {TYPE_LABELS[type] || type} <span class="count-val">{count}</span>
          </span>
        {/each}
      </div>
    {/if}
  </div>

  <div slot="actions" class="rule-type-select">
    <Select options={RULE_TYPE_SELECT} bind:selected={selectedRuleType} />
    <Button type="submit" onclick={submit} style="color: var(--text); font-size: 1rem;"
      >{t("Import")}</Button
    >
  </div>
</GenericDialog>

<style>
  .dialog-body {
    width: 100%;
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }

  .editor-container {
    position: relative;
    width: 100%;
    height: 40vh;
    min-height: 200px;
    max-height: 400px;
    display: flex;
    flex-direction: column;
    transition: height 0.2s ease-in-out;
  }

  .editor-container.empty {
    height: 20vh;
    min-height: 100px;
    max-height: 200px;
  }

  textarea,
  .results-view {
    width: 100%;
    height: 100%;
    box-sizing: border-box;
    font: inherit;
    font-size: 0.95rem;
    line-height: 1.5;
    border-radius: 0.5rem;
    border: 1.5px solid var(--bg-light-extra);
    background: var(--bg-light);
    color: var(--text);
  }

  textarea {
    resize: none;
    padding: 0.75rem 1rem;
    white-space: pre;
    transition:
      border 0.15s,
      box-shadow 0.15s;
  }

  textarea:focus {
    outline: none;
    border-color: var(--accent);
    box-shadow: 0 0 0 2px var(--accent-light, #aaf2ff33);
  }

  textarea.invalid {
    border-color: var(--danger) !important;
  }

  .results-view {
    overflow-y: auto;
    padding: 0.75rem 0;
    cursor: text;
    display: flex;
    flex-direction: column;
  }

  .line-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 1rem;
    min-height: 1.5em;
    line-height: 1.5;
    gap: 0.75rem;
  }

  .line-row:hover {
    background: var(--bg-light-extra);
  }

  .line-text {
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    flex: 1;
  }

  .line-row.invalid .line-text {
    color: var(--text-2);
    text-decoration: line-through;
    opacity: 0.7;
  }

  .parsing-overlay {
    position: absolute;
    inset: 0;
    background: rgba(0, 0, 0, 0.8);
    backdrop-filter: blur(3px);
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 0.5rem;
    color: var(--text);
    font-weight: 600;
    animation: pulse 1.5s infinite;
  }

  .spin {
    color: var(--accent);
    display: inline-flex;
    transform-origin: 50% 50%;
    animation: spin 1s linear infinite;
  }

  @keyframes spin {
    to {
      transform: rotate(360deg);
    }
  }

  @keyframes placeholder-fade {
    to {
      opacity: 1;
    }
  }

  @keyframes pulse {
    0%,
    100% {
      opacity: 0.6;
    }
    50% {
      opacity: 1;
    }
  }

  .badge {
    font-size: 0.7rem;
    padding: 0.1rem 0.3rem;
    border-radius: 0.3rem;
    background: var(--badge-color);
    color: #fff;
    font-weight: 600;
    flex-shrink: 0;
    text-transform: uppercase;
  }

  .counts {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem;
    padding: 0 0.5rem;
  }

  .count-badge {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
  }

  .count-val {
    background: rgba(0, 0, 0, 0.25);
    padding: 0.05rem 0.3rem;
    border-radius: 0.2rem;
    font-weight: 700;
    line-height: 1.1;
  }

  .rule-type-select {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0.5rem;
    width: 100%;
  }

  .rule-type-select :global(.select-root),
  .rule-type-select :global(button) {
    height: 2.5rem;
    font-size: 0.95rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    background-color: var(--bg-light);
    width: 100% !important;
  }

  @media (max-width: 600px) {
    :global([data-dialog-content]) {
      max-width: calc(100vw - 2rem) !important;
      max-height: calc(100vh - 2rem) !important;
      padding: 0.75rem !important;
      margin: 1rem !important;
    }

    .editor-container {
      height: calc(100vh - 15rem);
      max-height: none;
    }

    .editor-container.empty {
      height: calc(50vh - 7.5rem);
      max-height: none;
    }

    textarea,
    .line-row {
      padding-left: 0.75rem;
      padding-right: 0.75rem;
    }

    .rule-type-select {
      grid-template-columns: 1fr;
    }
  }
</style>
