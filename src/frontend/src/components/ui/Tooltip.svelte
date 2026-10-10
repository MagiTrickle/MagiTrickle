<script lang="ts">
  import { onDestroy, type Snippet } from "svelte";

  import { hide, hideFor, show } from "../../lib/tooltip/tooltip.svelte";

  let { value, children }: { value: string; children: Snippet } = $props();
  let triggerEl: HTMLElement;

  onDestroy(() => hideFor(triggerEl));
</script>

<div
  role="tooltip"
  data-value={value}
  bind:this={triggerEl}
  style:display="inline-flex"
  onpointerenter={() => show(triggerEl, value)}
  onpointerleave={hide}
>
  {@render children()}
</div>
