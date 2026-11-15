<script lang="ts">
  import LoaderCircle from "@lucide/svelte/icons/loader-circle";
  import { createEventDispatcher, flushSync, onDestroy, tick, untrack } from "svelte";
  import { slide } from "svelte/transition";

  import Button from "../../../components/ui/Button.svelte";
  import GenericDialog from "../../../components/ui/GenericDialog.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import VirtualList from "../../../components/ui/VirtualList.svelte";
  import { t } from "../../../data/locale.svelte";

  import { Dots } from "../../../components/ui/icons";
  import { RULE_TYPES, type Rule } from "../../../types";
  import { toast } from "../../../utils/events";
  import {
    boundedRuleListLines,
    type ParsedRuleLine,
    type RuleListType,
  } from "../rule-list-parser";
  import { RuleListParserClient } from "../rule-list-parser-client";

  type Props = {
    open?: boolean;
    group_index?: number | null;
    onImportRules?: (groupIndex: number, rules: Rule[], signal: AbortSignal) => Promise<void>;
  };
  let { open = $bindable(false), group_index = null, onImportRules }: Props = $props();
  const dispatch = createEventDispatcher();

  let import_rules_text = $state("");
  let triedSubmit = $state(false);
  let isParsing = $state(false);
  let isApplying = $state(false);
  let isEditing = $state(true);
  let editorFocused = $state(false);
  let bulkPastePending = $state(false);
  let textAreaRef = $state<HTMLTextAreaElement | null>(null);
  let clearEditorPending = $state(false);
  let editorHeight = $state(200);
  let editorMinHeight = $state(200);
  let editorMaxHeight = $state(500);
  let previewLines = $state.raw<ParsedRuleLine[]>([]);
  let totalLines = $state(0);
  let stats = $state<Record<string, number>>({});
  let selectedRuleType = $state<RuleListType>("auto");
  let previewScroller = $state<HTMLDivElement>();
  let previewLineHeight = $state(22.8);

  const MIN_EDITOR_HEIGHT = 200;
  const ABSOLUTE_MIN_EDITOR_HEIGHT = 120;
  const DESKTOP_MAX_EDITOR_HEIGHT = 500;
  const RULE_TYPE_SELECT = [{ value: "auto", label: "Auto" }, ...RULE_TYPES];
  const isEmpty = $derived(!import_rules_text.trim());
  const isBulkText = $derived(bulkPastePending || import_rules_text.length >= 65536);

  let resizeFrame = 0;
  let pasteTimer: ReturnType<typeof setTimeout> | undefined;
  const parser = new RuleListParserClient(
    () => new Worker(new URL("../rule-list-parser.worker.ts", import.meta.url), { type: "module" }),
  );
  let version = 0;
  let parsedVersion = -1;
  let submitRequested = false;
  let applyController: AbortController | undefined;

  function invalidateParser() {
    version++;
    parsedVersion = -1;
    parser.dispose();
    applyController?.abort();
    applyController = undefined;
    isApplying = false;
    isParsing = false;
  }

  function getEditorHeightBounds() {
    const rootFontSize = parseFloat(getComputedStyle(document.documentElement).fontSize) || 16;
    const isMobile = window.matchMedia("(max-width: 600px)").matches;
    const viewportHeight = window.visualViewport?.height || window.innerHeight;
    const reservedHeight = (isMobile ? 10 : 14) * rootFontSize;
    const maxHeight = Math.min(
      DESKTOP_MAX_EDITOR_HEIGHT,
      Math.max(ABSOLUTE_MIN_EDITOR_HEIGHT, viewportHeight - reservedHeight),
    );
    return { minHeight: Math.min(MIN_EDITOR_HEIGHT, maxHeight), maxHeight };
  }

  function getTextMetrics() {
    if (!textAreaRef) return { lineHeight: 22.8, paddingY: 24 };
    const styles = getComputedStyle(textAreaRef);
    const fontSize = parseFloat(styles.fontSize) || 16;
    return {
      lineHeight: parseFloat(styles.lineHeight) || fontSize * 1.5,
      paddingY: (parseFloat(styles.paddingTop) || 0) + (parseFloat(styles.paddingBottom) || 0),
    };
  }

  function scheduleEditorResize(force = false) {
    if (!open || clearEditorPending || !textAreaRef || resizeFrame) return;
    // Once bulk input fills the box, typing changes no dimensions. Reading
    // computed styles here would force native layout of the entire text.
    if (!force && isBulkText && editorHeight === editorMaxHeight) return;
    resizeFrame = requestAnimationFrame(() => {
      resizeFrame = 0;
      if (!open || clearEditorPending || !textAreaRef) return;
      if (!force && isBulkText && editorHeight === editorMaxHeight) return;
      const { minHeight, maxHeight } = getEditorHeightBounds();
      editorMinHeight = minHeight;
      editorMaxHeight = maxHeight;
      const { lineHeight, paddingY } = getTextMetrics();
      previewLineHeight = lineHeight;
      const baseLines = Math.max(1, Math.floor((minHeight - paddingY) / lineHeight));
      const saturation = baseLines + Math.ceil((maxHeight - minHeight) / lineHeight);
      const lineCount = isBulkText
        ? saturation
        : boundedRuleListLines(import_rules_text, saturation);
      editorHeight = Math.max(
        minHeight,
        Math.min(minHeight + Math.max(0, lineCount - baseLines) * lineHeight, maxHeight),
      );
    });
  }

  $effect(() => {
    selectedRuleType;
    untrack(() => {
      const previewVisible = open && !isEditing;
      invalidateParser();
      if (previewVisible) void parseRules();
    });
  });
  $effect(() => {
    import_rules_text;
    scheduleEditorResize();
  });
  $effect(() => {
    if (!open)
      untrack(() => {
        invalidateParser();
        clearEditorPending = true;
        cancelAnimationFrame(resizeFrame);
        resizeFrame = 0;
        clearTimeout(pasteTimer);
      });
  });
  $effect(() => {
    // Bits UI retains the dialog during its outro. Wait for the actual editor
    // teardown so clearing a large value cannot rebuild its native text layout.
    if (clearEditorPending && !textAreaRef) {
      import_rules_text = "";
      clearEditorPending = false;
    }
  });
  onDestroy(() => {
    applyController?.abort();
    parser.dispose();
    cancelAnimationFrame(resizeFrame);
    clearTimeout(pasteTimer);
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

  function handleKeydown(event: KeyboardEvent) {
    if (open && (event.metaKey || event.ctrlKey) && event.key === "Enter") {
      event.preventDefault();
      submit();
    }
  }

  async function parseRules() {
    if (!open || clearEditorPending || !import_rules_text.trim() || isParsing) return;
    const requestedVersion = version;
    isParsing = true;
    isEditing = false;
    let applying = false;
    try {
      if (parsedVersion !== requestedVersion) {
        const response = await parser.request({
          type: "parse",
          version: requestedVersion,
          text: import_rules_text,
          ruleType: selectedRuleType,
        });
        if (!open || requestedVersion !== version || response.type !== "preview") return;
        previewLines = response.preview.lines;
        totalLines = response.preview.totalLines;
        stats = response.preview.stats;
        parsedVersion = requestedVersion;
      }
      if (submitRequested) {
        const response = await parser.request({ type: "apply", version: requestedVersion });
        if (!open || requestedVersion !== version || response.type !== "rules") return;
        if (response.rules.length && group_index !== null) {
          applying = true;
          isApplying = true;
          const controller = new AbortController();
          applyController = controller;
          if (onImportRules) await onImportRules(group_index, response.rules, controller.signal);
          else dispatch("import", { group_index, rules: response.rules });
          if (!open || requestedVersion !== version || controller.signal.aborted) return;
          applyController = undefined;
          close();
        }
      }
    } catch (error) {
      if (requestedVersion !== version || (error instanceof Error && error.name === "AbortError"))
        return;
      if (!applying) {
        parsedVersion = -1;
        console.error("Rule list parsing failed", error);
        toast.error(t("Failed to import rules"));
        isEditing = true;
      }
      // The owner reports apply errors; preserve the parsed input for retry.
    } finally {
      if (requestedVersion === version) {
        isParsing = false;
        isApplying = false;
        applyController = undefined;
        submitRequested = false;
        if (open && !isEditing && document.activeElement === textAreaRef) {
          await tick();
          // Automatic paste parsing may finish while the user is still in the
          // editor. Respect a newer focus move before exposing the preview.
          if (requestedVersion === version && !isEditing && document.activeElement === textAreaRef)
            previewScroller?.focus({ preventScroll: true });
        }
      }
    }
  }

  function submit() {
    triedSubmit = true;
    if (clearEditorPending || isApplying || group_index === null || !import_rules_text.trim())
      return;
    submitRequested = true;
    if (!isParsing) void parseRules();
  }

  function close() {
    invalidateParser();
    clearEditorPending = true;
    clearTimeout(pasteTimer);
    submitRequested = false;
    previewLines = [];
    totalLines = 0;
    stats = {};
    triedSubmit = false;
    editorFocused = false;
    bulkPastePending = false;
    isEditing = true;
    selectedRuleType = "auto";
    dispatch("close");
  }

  function handleInput(event: Event) {
    if (!(event instanceof InputEvent) || event.inputType !== "insertFromPaste")
      clearTimeout(pasteTimer);
    invalidateParser();
    isEditing = true;
    bulkPastePending = false;
    submitRequested = false;
  }

  function onPaste(event: ClipboardEvent) {
    if ((event.clipboardData?.getData("text/plain").length ?? 0) >= 65536) {
      // Apply the final bulk dimensions before native paste lays out its text.
      // Native insertion and undo remain owned by the browser.
      const { minHeight, maxHeight } = getEditorHeightBounds();
      cancelAnimationFrame(resizeFrame);
      resizeFrame = 0;
      flushSync(() => {
        bulkPastePending = true;
        editorMinHeight = minHeight;
        editorMaxHeight = maxHeight;
        editorHeight = maxHeight;
      });
    }
    clearTimeout(pasteTimer);
    pasteTimer = setTimeout(() => void parseRules(), 50);
  }

  function onBlur() {
    editorFocused = false;
    if (!isEditing || isParsing || isApplying || !import_rules_text.trim()) return;
    if (parsedVersion === version) {
      isEditing = false;
      return;
    }
    void parseRules();
  }

  async function switchToEdit() {
    isEditing = true;
    await tick();
    textAreaRef?.focus();
  }
</script>

<svelte:window onkeydown={handleKeydown} onresize={() => scheduleEditorResize(true)} />

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
      class:bulk={isBulkText}
      style={`--editor-height: ${editorHeight}px; --editor-min-height: ${editorMinHeight}px; --editor-max-height: ${editorMaxHeight}px`}
    >
      <!-- Preview retains this native buffer. A rapid reopen removes the closing
           editor first, then mounts a fresh empty one after bind:this is cleared. -->
      {#if !open || !clearEditorPending}
        <textarea
          bind:this={textAreaRef}
          bind:value={import_rules_text}
          aria-hidden={!isEditing && !isParsing && !editorFocused}
          tabindex={isEditing ? 0 : -1}
          spellcheck={false}
          autocapitalize="off"
          {...{ autocorrect: "off" }}
          autocomplete="off"
          writingsuggestions="false"
          wrap="off"
          oninput={handleInput}
          placeholder={t("Insert a list of IPs or domains, one per line")}
          class:invalid={triedSubmit && isEmpty}
          onfocus={() => (editorFocused = true)}
          onpaste={onPaste}
          onblur={onBlur}
        ></textarea>
      {/if}
      <div class="editor-focus-ring" aria-hidden="true"></div>
      {#if isParsing}
        <div class="parsing-overlay">
          <span class="spin"><LoaderCircle size={30} /></span>
        </div>
      {:else if !isEditing}
        <div
          class="results-view"
          class:has-more={totalLines > previewLines.length}
          bind:this={previewScroller}
          onclick={switchToEdit}
          onkeydown={(e) => e.key === "Enter" && switchToEdit()}
          role="button"
          tabindex="0"
        >
          {#if previewLines.length === 0}
            <div class="empty-state">{t("No rules found")}</div>
          {/if}
          <VirtualList
            count={previewLines.length}
            getKey={(index) => String(index)}
            estimateSize={previewLineHeight}
            scrollElement={previewScroller}
            revision={previewLines}
            active={open && !isEditing && !isParsing}
          >
            {#snippet children(index)}
              {@const line = previewLines[index]}
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
            {/snippet}
          </VirtualList>
          {#if totalLines > previewLines.length}
            <div class="line-row ellipsis-row">
              <Dots size={24} />
            </div>
          {/if}
        </div>
      {/if}
    </div>

    {#snippet counts()}
      <div class="counts">
        {#each Object.entries(stats) as [type, count]}
          <span
            class="badge count-badge"
            style="--badge-color: {TYPE_COLORS[type] || 'var(--text-2)'}"
          >
            {TYPE_LABELS[type] || type} <span class="count-val">{count}</span>
          </span>
        {/each}
      </div>
    {/snippet}
    {#if !isEditing && !isParsing && Object.keys(stats).length > 0}
      {#if isBulkText}
        {@render counts()}
      {:else}
        <div transition:slide={{ duration: 160 }}>{@render counts()}</div>
      {/if}
    {/if}
  </div>

  <div slot="actions" class="rule-type-select">
    <Select options={RULE_TYPE_SELECT} bind:selected={selectedRuleType} disabled={isApplying} />
    <Button
      type="submit"
      onclick={submit}
      disabled={isApplying}
      style="color: var(--text); font-size: 1rem;">{t("Import")}</Button
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

  .editor-container.bulk {
    transition: none;
  }

  textarea {
    position: absolute;
    inset: 0;
    resize: none;
    padding: 0.75rem 1rem;
    white-space: pre;
    outline: none;
  }

  .editor-focus-ring {
    position: absolute;
    inset: 0;
    border: 1.5px solid transparent;
    border-radius: 0.5rem;
    pointer-events: none;
    transition:
      border 0.15s,
      box-shadow 0.15s;
  }

  textarea:focus + .editor-focus-ring {
    border-color: var(--accent);
    box-shadow: 0 0 0 2px var(--accent-light, #aaf2ff33);
  }

  textarea.invalid + .editor-focus-ring {
    border-color: var(--red);
  }

  textarea.invalid {
    border-color: var(--red) !important;
  }

  .results-view {
    position: absolute;
    inset: 0;
    overflow-y: auto;
    padding: 0.75rem 0;
    cursor: text;
    display: flex;
    flex-direction: column;
  }

  .results-view :global(.virtual-list) {
    flex-shrink: 0;
  }

  .results-view.has-more {
    padding-bottom: 0;
    border-bottom: 0;
  }

  .line-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 1rem;
    height: 1.5em;
    min-height: 1.5em;
    flex-shrink: 0;
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
    box-sizing: border-box;
    border-top: 1px solid var(--bg-light-extra);
    border-bottom: 1px solid transparent;
    justify-content: center;
    color: var(--text-2);
    cursor: pointer;
    user-select: none;
    transition:
      background-color 0.15s,
      color 0.15s;
  }

  .ellipsis-row:hover,
  .results-view:focus-visible .ellipsis-row {
    background: var(--bg-light-extra);
    color: var(--text);
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
