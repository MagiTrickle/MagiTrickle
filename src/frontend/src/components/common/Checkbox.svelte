<script lang="ts">
  import { createEventDispatcher } from "svelte";

  export let checked = false;
  export let disabled = false;
  export let name: string | undefined = undefined;
  export let value: string | undefined = undefined;
  export let ariaLabel: string | undefined = undefined;
  export let ariaLabelledby: string | undefined = undefined;
  export let tabindex: number | undefined = undefined;

  const dispatch = createEventDispatcher<{
    change: { checked: boolean };
  }>();

  function handleInput(event: Event) {
    const next = (event.currentTarget as HTMLInputElement).checked;
    checked = next;
    dispatch("change", { checked: next });
  }
</script>

<div
  class="checkbox"
  data-checked={checked ? "true" : undefined}
  data-disabled={disabled ? "true" : undefined}
>
  <input
    type="checkbox"
    bind:checked
    {disabled}
    {name}
    {value}
    aria-label={ariaLabel}
    aria-labelledby={ariaLabelledby}
    {tabindex}
    on:input={handleInput}
  />
  <span class="box">
    <svg viewBox="0 0 16 16" aria-hidden="true">
      <path d="M3.2 8.2 6.4 11.4 12.8 5" />
    </svg>
  </span>
</div>

<style>
  .checkbox {
    position: relative;
    width: 20px;
    height: 20px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
    cursor: pointer;
  }

  .checkbox[data-disabled] {
    cursor: not-allowed;
    opacity: 0.5;
  }

  input {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    opacity: 0;
    margin: 0;
    cursor: inherit;
  }

  .box {
    width: 100%;
    height: 100%;
    border-radius: 0.45rem;
    border: 1.5px solid var(--bg-light-extra);
    background: var(--bg-dark);
    display: flex;
    align-items: center;
    justify-content: center;
    transition:
      border-color 0.12s ease,
      background-color 0.12s ease,
      box-shadow 0.18s ease;
    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.35);
    position: relative;
  }

  .checkbox:not([data-disabled]):hover .box {
    border-color: var(--accent);
  }

  input:focus-visible + .box {
    outline: 2px solid var(--accent);
    outline-offset: 3px;
  }

  .box::before {
    content: "";
    position: absolute;
    inset: 0;
    border-radius: inherit;
    background: radial-gradient(circle, rgba(66, 189, 249, 0.45) 0%, rgba(66, 189, 249, 0) 70%);
    opacity: 0;
    transform: scale(0.45);
    transition:
      opacity 0.25s ease,
      transform 0.25s cubic-bezier(0.19, 1, 0.22, 1);
  }

  svg {
    width: 11px;
    height: 11px;
    stroke: var(--bg-dark);
    stroke-width: 2.4;
    stroke-linecap: round;
    stroke-linejoin: round;
    fill: none;
    opacity: 0;
    transform: scale(0.6);
    transition:
      opacity 0.18s ease,
      transform 0.2s cubic-bezier(0.19, 1, 0.22, 1);
  }

  .checkbox[data-checked] .box {
    background: var(--accent);
    border-color: var(--accent);
    box-shadow: 0 0 0 2px rgba(66, 189, 249, 0.28);
  }

  .checkbox[data-checked] svg {
    opacity: 1;
    transform: scale(1);
  }

  .checkbox[data-checked] .box::before {
    opacity: 1;
    transform: scale(1.2);
  }
</style>
