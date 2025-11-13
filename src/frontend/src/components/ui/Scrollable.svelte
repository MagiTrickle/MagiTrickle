<script lang="ts">
  import { ScrollArea } from "bits-ui";
  import type { Snippet } from "svelte";

  type Props = {
    children: Snippet;
    [key: string]: any;
  };
  let { children, ...rest }: Props = $props();
</script>

<ScrollArea.Root {...rest}>
  <ScrollArea.Viewport>
    {@render children()}
  </ScrollArea.Viewport>
  <ScrollArea.Scrollbar orientation="vertical">
    <ScrollArea.Thumb />
  </ScrollArea.Scrollbar>
  <ScrollArea.Corner />
</ScrollArea.Root>

<style>
  :global {
    [data-scroll-area-root] {
      position: relative;
      height: calc(100vh - 140px);
      border-radius: 0.5rem;
    }

    [data-scroll-area-viewport] {
      height: 100%;
      width: 100%;
      position: relative;
    }

    [data-scroll-area-scrollbar-x],
    [data-scroll-area-scrollbar-y] {
      user-select: none;
      touch-action: none;
      display: flex;
      height: calc(100% - 1.4rem);
      background-color: transparent;
      width: 0.4rem;
      margin: 0.2rem;
      padding: 0.2rem;
    }

    [data-scroll-area-thumb-x],
    [data-scroll-area-thumb-y] {
      & {
        flex: 1;
        position: relative;
        opacity: 0.6;
        background-color: var(--bg-light-extra);
        border-radius: 0.5rem;
        width: 0.4rem;
        padding: 0.3rem;
        transition: all 0.3 ease-in-out;
      }

      &:hover {
        opacity: 1;
      }
    }
  }
</style>
