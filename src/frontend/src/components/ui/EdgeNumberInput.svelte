<script lang="ts">
  import { Popover } from "bits-ui";

  import { t } from "../../data/locale.svelte";

  import { isValidPriority, MAX_PRIORITY, MIN_PRIORITY } from "../../utils/priority";

  type Props = {
    label: string;
    title: string;
    value: number;
  };

  let { label, title, value = $bindable() }: Props = $props();
  let draft = $state("");
  let trigger = $state<HTMLButtonElement | null>(null);
  let content = $state<HTMLDivElement | null>(null);
  const titleId = $props.id();

  $effect(() => {
    draft = Number.isFinite(value) ? String(value) : "";
  });

  function restoreDraft() {
    draft = Number.isFinite(value) ? String(value) : "";
  }

  function handleInput(event: Event & { currentTarget: HTMLInputElement }) {
    const input = event.currentTarget;
    // A temporarily empty field lets users replace the number. It never
    // replaces the saved value and is restored when the editor loses focus.
    if (input.value === "" && !input.validity.badInput) {
      draft = "";
      return;
    }
    if (input.validity.valid && isValidPriority(input.valueAsNumber)) {
      value = input.valueAsNumber;
      draft = input.value;
      return;
    }
    restoreDraft();
    input.value = draft;
  }

  function stepValue(delta: number) {
    const next = value + delta;
    if (isValidPriority(next)) value = next;
  }
</script>

<div class="edge-number">
  <Popover.Root
    onOpenChange={(open) => {
      if (!open) restoreDraft();
    }}
  >
    <Popover.Trigger bind:ref={trigger} class="priority-trigger" aria-label={label} title={label}>
      <span class="priority-value">{Number.isFinite(value) ? value : "—"}</span>
    </Popover.Trigger>
    <Popover.Portal>
      <Popover.Content
        bind:ref={content}
        class="priority-popover"
        onOpenAutoFocus={(event) => {
          event.preventDefault();
          content
            ?.querySelector<HTMLElement>("button:not(:disabled), input")
            ?.focus({ preventScroll: true });
        }}
        onCloseAutoFocus={(event) => {
          event.preventDefault();
          trigger?.focus({ preventScroll: true });
        }}
        side="left"
        sideOffset={8}
        align="center"
        collisionPadding={8}
        preventScroll={true}
        aria-labelledby={titleId}
      >
        <h3 id={titleId}>{title}</h3>
        <div class="priority-editor">
          <button
            type="button"
            aria-label={t("Decrease priority")}
            disabled={!isValidPriority(value) || value <= MIN_PRIORITY}
            onclick={() => stepValue(-1)}>−</button
          >
          <input
            type="number"
            min={MIN_PRIORITY}
            max={MAX_PRIORITY}
            step="1"
            aria-label={label}
            value={draft}
            oninput={handleInput}
            onblur={restoreDraft}
            onkeydown={(event) => {
              if (event.key === "Enter") {
                event.preventDefault();
                event.currentTarget.blur();
              }
            }}
          />
          <button
            type="button"
            aria-label={t("Increase priority")}
            disabled={!isValidPriority(value) || value >= MAX_PRIORITY}
            onclick={() => stepValue(1)}>+</button
          >
        </div>
      </Popover.Content>
    </Popover.Portal>
  </Popover.Root>
</div>

<style>
  .edge-number {
    position: absolute;
    top: 0;
    right: 0;
    bottom: 0;
    width: 1.5rem;
    border-left: 1px solid var(--bg-light-extra);
    border-radius: 0 0.5rem 0.5rem 0;
    background-color: var(--bg-medium);
  }

  :global(.priority-trigger) {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 100%;
    height: 100%;
    padding: 0;
    border: 0;
    border-radius: inherit;
    background: transparent;
    color: var(--text);
    cursor: pointer;
  }

  .priority-value {
    transform: rotate(-90deg);
    font-family: var(--font);
    font-size: 1rem;
    font-style: italic;
    font-weight: 600;
  }

  :global(.priority-trigger:hover),
  :global(.priority-trigger[data-state="open"]) {
    background-color: var(--bg-dark);
  }

  :global(.priority-trigger:focus-visible) {
    outline: 1px solid var(--accent);
    outline-offset: -1px;
  }

  :global(.priority-popover) {
    z-index: 20;
    padding: 0.65rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    background-color: var(--bg-dark-extra);
    color: var(--text);
    box-shadow: var(--shadow-popover);
  }

  h3 {
    margin: 0 0 0.5rem;
    color: var(--text-2);
    font-size: 0.85rem;
    font-weight: 600;
    text-align: center;
  }

  .priority-editor {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 0.4rem;
  }

  .priority-editor button,
  input {
    box-sizing: border-box;
    height: 2rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.3rem;
    background-color: var(--bg-light);
    color: var(--text);
    font-family: var(--font);
    font-size: 1rem;
    text-align: center;
  }

  .priority-editor button {
    width: 2rem;
    padding: 0;
    cursor: pointer;
  }

  .priority-editor button:hover {
    background-color: var(--bg-medium);
  }

  .priority-editor button:disabled {
    opacity: 0.4;
    cursor: default;
  }

  input {
    width: 4.5rem;
    padding: 0 0.25rem;
    appearance: textfield;
  }

  input::-webkit-inner-spin-button,
  input::-webkit-outer-spin-button {
    margin: 0;
    -webkit-appearance: none;
  }

  .priority-editor button:focus-visible,
  input:focus-visible {
    outline: 1px solid var(--accent);
    outline-offset: 1px;
  }
</style>
