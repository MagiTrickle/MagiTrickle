<script lang="ts">
  import LoaderCircle from "lucide-svelte/icons/loader-circle";
  import { createEventDispatcher, tick } from "svelte";
  import { slide } from "svelte/transition";

  import Button from "../../../components/ui/Button.svelte";
  import GenericDialog from "../../../components/ui/GenericDialog.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import { t } from "../../../data/locale.svelte";

  import { RULE_TYPES, type Rule } from "../../../types";
  import { defaultRule } from "../../../utils/defaults";
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
  let editorHeight = $state(200);
  let editorMinHeight = $state(200);
  let editorMaxHeight = $state(500);

  const MIN_EDITOR_HEIGHT = 200;
  const ABSOLUTE_MIN_EDITOR_HEIGHT = 120;
  const DESKTOP_MAX_EDITOR_HEIGHT = 500;
  const ROWS_LIMIT = 500;

  function getEditorHeightBounds() {
    if (typeof window === "undefined") {
      return { minHeight: MIN_EDITOR_HEIGHT, maxHeight: DESKTOP_MAX_EDITOR_HEIGHT };
    }
    const rootFontSize = parseFloat(getComputedStyle(document.documentElement).fontSize) || 16;
    const isMobile = window.matchMedia("(max-width: 600px)").matches;
    const viewportHeight = window.visualViewport?.height || window.innerHeight;
    const reservedHeight = (isMobile ? 10 : 14) * rootFontSize;
    const viewportLimitedMax = viewportHeight - reservedHeight;
    const maxHeight = Math.min(
      DESKTOP_MAX_EDITOR_HEIGHT,
      Math.max(ABSOLUTE_MIN_EDITOR_HEIGHT, viewportLimitedMax),
    );
    const minHeight = Math.min(MIN_EDITOR_HEIGHT, maxHeight);
    return { minHeight, maxHeight };
  }

  function getTextMetrics() {
    if (!textAreaRef || typeof window === "undefined") {
      return { lineHeight: 22, paddingY: 24 };
    }
    const styles = getComputedStyle(textAreaRef);
    const fontSize = parseFloat(styles.fontSize) || 16;
    let lineHeight = parseFloat(styles.lineHeight);
    if (Number.isNaN(lineHeight)) {
      lineHeight = fontSize * 1.5;
    }
    const paddingY = (parseFloat(styles.paddingTop) || 0) + (parseFloat(styles.paddingBottom) || 0);
    return { lineHeight, paddingY };
  }

  function scheduleEditorResize() {
    if (!textAreaRef) return;
    requestAnimationFrame(() => {
      if (!textAreaRef) return;
      const { minHeight, maxHeight } = getEditorHeightBounds();
      editorMinHeight = minHeight;
      editorMaxHeight = maxHeight;
      const tokens = import_rules_text.split(/[\n,]+/).filter((t) => t.trim().length > 0);
      const lineCount = Math.max(1, tokens.length);
      const { lineHeight, paddingY } = getTextMetrics();
      const baseLines = Math.max(1, Math.floor((minHeight - paddingY) / lineHeight));
      const extraLines = Math.max(0, lineCount - baseLines);
      const targetHeight = minHeight + extraLines * lineHeight;
      editorHeight = Math.max(minHeight, Math.min(targetHeight, maxHeight));
    });
  }

  type ParsedLine = {
    text: string;
    type: string | null;
    isValid: boolean;
  };

  let previewLines = $state<ParsedLine[]>([]);
  // svelte-ignore non_reactive_update
  let allParsedLines: ParsedLine[] = [];
  let stats = $state<Record<string, number>>({});

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

  $effect(() => {
    import_rules_text;
    scheduleEditorResize();
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
    const lines = getParsedData(import_rules_text, selectedRuleType);
    allParsedLines = lines;
    previewLines = lines.slice(0, ROWS_LIMIT);

    const c: Record<string, number> = {};
    lines.forEach((l) => {
      if (l.text.trim() && !l.text.trim().startsWith("#")) {
        const type = l.type || "INVALID";
        c[type] = (c[type] || 0) + 1;
      }
    });
    stats = c;

    isParsing = false;
  }

  function submit() {
    triedSubmit = true;
    if (group_index === null) return;

    const currentParsed = isEditing
      ? getParsedData(import_rules_text, selectedRuleType)
      : allParsedLines;

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
    close();
  }

  function close() {
    import_rules_text = "";
    previewLines = [];
    allParsedLines = [];
    stats = {};
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
</script>

<svelte:window onkeydown={handleKeydown} onresize={scheduleEditorResize} />

<GenericDialog
  {open}
  {triedSubmit}
  title={t("Import Rule List")}
  maxWidth={550}
  on:close={close}
  on:submit={submit}
>
  <div slot="body" class="dialog-body">
    <div
      class="editor-container"
      class:parsing={isParsing}
      style={`--editor-height: ${editorHeight}px; --editor-min-height: ${editorMinHeight}px; --editor-max-height: ${editorMaxHeight}px`}
    >
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
          {#if previewLines.length === 0}
            <div class="empty-state">{t("No rules found")}</div>
          {/if}
          {#each previewLines as line}
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
          {#if allParsedLines.length > ROWS_LIMIT}
            <div class="line-row ellipsis-row">
              <span class="line-text">...</span>
            </div>
          {/if}
        </div>
      {/if}
    </div>

    {#if !isEditing && !isParsing && Object.keys(stats).length > 0}
      <div class="counts" transition:slide={{ duration: 160 }}>
        {#each Object.entries(stats) as [type, count]}
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
    min-height: var(--editor-min-height, 200px);
    height: var(--editor-height, 200px);
    max-height: var(--editor-max-height, 500px);
    display: flex;
    flex-direction: column;
    transition: height 0.2s ease-in-out;
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

  .ellipsis-row {
    text-align: center;
    color: var(--text-2);
    opacity: 0.5;
    font-size: 1.5rem;
    line-height: 1;
    user-select: none;
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
    display: flex;
    align-items: center;
    justify-content: space-between;
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
    width: auto !important;
  }

  .rule-type-select :global(.select-root) {
    flex: 1 1 auto;
    min-width: 0;
  }

  .rule-type-select :global(button) {
    flex: 0 0 auto;
  }

  .rule-type-select :global([data-select-content]) {
    z-index: 20;
  }

  @media (max-width: 600px) {
    :global([data-dialog-content]) {
      max-width: calc(100vw - 2rem) !important;
      max-height: calc(100vh - 2rem) !important;
      padding: 0.75rem !important;
      margin: 0 !important;
      left: 50% !important;
      top: 50% !important;
      transform: translate(-50%, -50%) !important;
    }

    .editor-container {
      --editor-max-height: calc(100vh - 10rem);
    }

    textarea,
    .line-row {
      padding-left: 0.75rem;
      padding-right: 0.75rem;
    }

    .rule-type-select {
      display: flex;
      align-items: center;
      justify-content: space-between;
    }
  }
</style>
