<script lang="ts">
  import type { Snippet } from "svelte";

  type Props = {
    children: Snippet;
    small?: boolean;
    general?: boolean;
    inactive?: boolean;
    onclick?: () => void;
    [key: string]: any;
  };
  let { children, onclick, small, general, inactive, ...rest }: Props = $props();
</script>

<button
  class:main={!small}
  class:inactive
  class:general
  onclick={inactive ? () => ({}) : onclick}
  {...rest}
>
  {@render children()}
</button>

<style>
  @keyframes border-spin {
    from {
      transform: translate(-50%, -50%) rotate(0turn);
    }
    to {
      transform: translate(-50%, -50%) rotate(1turn);
    }
  }

  button {
    & {
      box-sizing: border-box;
      color: var(--text-2);
      background-color: transparent;
      border: 1px solid transparent;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      font: 400 1rem var(--font);
      padding: 0.4rem;
      border-radius: 0.5rem;
      cursor: pointer;
      vertical-align: middle;
    }

    &:hover {
      background-color: var(--bg-dark);
      color: var(--text);
      border: 1px solid var(--bg-light-extra);
    }

    :global(&.fail) {
      color: var(--red);
      box-shadow: 0 0 5px var(--red);
    }

    :global(&.success) {
      color: var(--green);
      box-shadow: 0 0 5px var(--green);
    }

    &.main {
      background-color: var(--bg-light);
      padding: 0.6rem;
      transition: all 0.1s ease-in-out;
      border: 1px solid var(--bg-light-extra);

      &:hover {
        background-color: var(--bg-light-extra);
      }
    }

    &.inactive {
      cursor: default;
      opacity: 0.9;
      color: var(--text-2);
      pointer-events: none;
    }

    &.accent.inactive {
      border: 1px solid var(--bg-light-extra) !important;
      background: var(--bg-light) !important;
      color: var(--text-2);
    }

    &.accent.inactive::before {
      opacity: 0;
      background-color: var(--bg-light-extra);
    }

    &.accent.inactive::after {
      background: var(--bg-light);
    }

    &.accent:not(.inactive)::before {
      animation-play-state: running;
    }

    &.general {
      color: var(--text);
      font-size: 1rem;
      font-family: var(--font);
      background: transparent;
      border: 1px solid transparent;
      justify-content: start;
      width: 100%;
      padding: 0.2rem;
      padding-left: 0.1rem;
    }

    &.accent {
      position: relative;
      z-index: 0;
      padding: 0.6rem;
      border: 1px solid transparent !important;
      background: transparent !important;
      color: color-mix(in srgb, var(--accent), transparent 20%);
      clip-path: inset(0 round 0.5rem);
      transition:
        color 0.3s ease,
        opacity 0.3s ease;
    }

    &.accent::before {
      content: "";
      position: absolute;
      top: 50%;
      left: 50%;
      width: 200%;
      height: 200%;
      z-index: -2;
      background-color: var(--bg-light-extra);
      background-image: conic-gradient(transparent 180deg, var(--accent) 360deg);
      animation: border-spin 3s linear infinite;
      animation-play-state: paused;
      transition:
        background-color 0.3s ease,
        opacity 0.3s ease;
    }

    &.accent::after {
      content: "";
      position: absolute;
      z-index: -1;
      inset: 0;
      background: var(--bg-light);
      border-radius: calc(0.5rem - 1px);
    }

    &.accent:hover {
      color: var(--accent);
    }

    &.accent:hover::after {
      background: var(--bg-light-extra);
    }
  }

  button.inactive {
    & {
      cursor: default;
      opacity: 0.3;
    }
    &:hover {
      background-color: transparent;
      color: var(--text-2);
      border: 1px solid transparent;
    }
  }
</style>
