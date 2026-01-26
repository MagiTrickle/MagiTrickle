<script lang="ts">
  import { Dialog } from "bits-ui";
  import { createEventDispatcher } from "svelte";
  import { scale } from "svelte/transition";

  import { Add } from "./icons";

  export let open = false;
  export let title = "";
  export let textareaValue = "";
  export let textareaPlaceholder = "";
  export let maxWidth = 300;

  let textArea: HTMLTextAreaElement;

  export let triedSubmit = false;

  const dispatch = createEventDispatcher();

  function handleOpenChange(v: boolean) {
    if (!v) dispatch("close");
  }
</script>

<Dialog.Root {open} onOpenChange={handleOpenChange}>
  <Dialog.Portal>
    <Dialog.Overlay />
    <Dialog.Content
      class="dialog"
      data-state={open ? "open" : "closed"}
      escapeKeydownBehavior="close"
      forceMount
      onOpenAutoFocus={(e) => {
        e.preventDefault();
        // textArea?.focus();
      }}
      style={`--generic-dialog-max-width: ${maxWidth}px`}
    >
      {#snippet child({ props, open })}
        {#if open}
          <div
            {...props}
            class="modal"
            in:scale={{ duration: 120, start: 0.96 }}
            out:scale={{ duration: 55 }}
          >
            <Dialog.Title class="title">{title}</Dialog.Title>
            <Dialog.Close class="close">
              <Add size={22} style="transform:rotate(45deg)" />
            </Dialog.Close>
            <form on:submit|preventDefault={() => dispatch("submit")}>
              <div class="body">
                <slot name="body">
                  <textarea
                    bind:this={textArea}
                    bind:value={textareaValue}
                    placeholder={textareaPlaceholder}
                    class:invalid={triedSubmit && !textareaValue.trim()}
                    on:input={(e) =>
                      dispatch("textareaInput", (e.target as HTMLTextAreaElement).value)}
                  ></textarea>
                </slot>
              </div>

              <div class="actions">
                <slot name="actions" />
              </div>
            </form>
          </div>
        {/if}
      {/snippet}
    </Dialog.Content>
  </Dialog.Portal>
</Dialog.Root>

<style>
  textarea {
    width: 100%;
    min-height: 120px;
    height: 120px;
    max-height: 50vh;
    margin-top: 1rem;
    resize: vertical;
    font: inherit;
    padding: 0.75rem 1rem;
    border-radius: 0.5rem;
    border: 1.5px solid var(--bg-light-extra);
    background: var(--bg-light);
    color: var(--text);
    box-sizing: border-box;
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

  .actions {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
    margin-top: 0.5rem;
  }

  .body {
    width: 100%;
    margin-top: 1rem;
  }

  :global([data-dialog-overlay]) {
    position: fixed;
    top: 0;
    left: 0;
    width: 100vw;
    height: 100vh;
    display: flex;
    justify-content: center;
    align-items: center;
    background-color: rgba(0, 0, 0, 0.7);
    backdrop-filter: blur(2px);
  }

  :global([data-dialog-content]) {
    position: fixed;
    left: 50%;
    top: 50%;
    transform: translate(-50%, -50%);
    width: 100%;
    max-width: min(var(--generic-dialog-max-width, 300px), calc(100vw - 5rem));
    background-color: var(--bg-dark);
    border-radius: 0.5rem;
    border: 1px solid var(--bg-light-extra);
    padding: 1rem;
  }

  :global([data-dialog-title]) {
    font-size: 1.3rem;
    font-weight: 600;
    font-family: var(--font);
    text-align: left;
    border-bottom: 1px solid var(--bg-light-extra);
    padding-bottom: 0.5rem;
    padding-right: 32px;
  }

  :global([data-dialog-description]) {
    font-size: 0.9rem;
    color: var(--text-2);
    margin-top: 0.5rem;
  }

  :global(.footer) {
    display: flex;
    justify-content: end;
    align-items: center;
    gap: 0.5rem;
    margin-top: 1rem;
  }

  .modal-close {
    background: none;
    border: none;
    padding: 0;
    cursor: pointer;
    border-radius: 50%;
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.13);
    transition:
      box-shadow 0.15s,
      opacity 0.15s;
    width: 32px;
    height: 32px;
    display: flex;
    align-items: center;
    justify-content: center;
    color: var(--text-2);
    opacity: 0.55;
    position: static;
  }

  :global(button[data-dialog-close].close) {
    position: absolute;
    right: 0.5rem;
    top: 0.5rem;
    color: var(--text-2);
    display: inline-flex;
    align-items: center;
    justify-content: center;
    padding: 0.4rem;
    border-radius: 0.5rem;
    border: 1px solid transparent;
    background-color: transparent;
    cursor: pointer;
  }
  :global(button[data-dialog-close].close:hover) {
    box-shadow: 0 4px 16px rgba(0, 0, 0, 0.18);
    background: var(--bg-light-extra);
    opacity: 1;
  }

  :global(button[data-dialog-close].submit) {
    font-size: 1rem;
    font-weight: 400;
    font-family: var(--font);
    color: var(--text);
    display: inline-flex;
    align-items: center;
    justify-content: center;
    padding: 0.4rem;
    border-radius: 0.5rem;
    border: 1px solid var(--bg-light-extra);
    background-color: var(--bg-light);
    cursor: pointer;
  }
  :global(button[data-dialog-close].submit:hover) {
    background-color: var(--bg-light-extra);
    color: var(--text);
    border: 1px solid var(--bg-light-extra);
  }
</style>
