<script lang="ts">
  import { type Snippet } from "svelte";

  import { hide, show } from "../../lib/tooltip/tooltip.svelte";

  let { value, children }: { value: string; children: Snippet } = $props();
  let triggerEl: HTMLElement;
</script>

<div
  data-value={value}
  bind:this={triggerEl}
  style:display="inline-flex"
  onpointerenter={() => show(triggerEl, value)}
  onpointerleave={hide}
>
  {@render children()}
</div>
