<script lang="ts">
  import type { Snippet } from "svelte";

  import { CircleInfo, LoaderCircle, SearchSlash, TriangleAlert } from "./icons";

  type Variant = "neutral" | "empty" | "loading" | "error";

  type Props = {
    children?: Snippet;
    loading?: boolean;
    variant?: Variant;
    compact?: boolean;
    minHeight?: string;
    subtitle?: string;
    actions?: Snippet;
    [key: string]: unknown;
  };

  let {
    children,
    loading = false,
    variant = "neutral",
    compact = false,
    minHeight = "300px",
    subtitle,
    actions,
    ...rest
  }: Props = $props();

  const resolvedVariant = $derived(loading ? "loading" : variant);

  const role = $derived(resolvedVariant === "error" ? "alert" : "status");
  const ariaLive = $derived(resolvedVariant === "error" ? "assertive" : "polite");
</script>

<div class="placeholder" style={`min-height: ${minHeight};`} {...rest}>
  <div
    class="card"
    data-variant={resolvedVariant}
    data-compact={compact ? "true" : "false"}
    data-no-subtitle={subtitle ? "false" : "true"}
    {role}
    aria-live={ariaLive}
  >
    <div class="icon" aria-hidden="true">
      {#if resolvedVariant === "loading"}
        <span class="spin">
          <LoaderCircle size={30} />
        </span>
      {:else if resolvedVariant === "error"}
        <TriangleAlert size={30} />
      {:else if resolvedVariant === "empty"}
        <SearchSlash size={30} />
      {:else}
        <CircleInfo size={30} />
      {/if}
    </div>

    <div class="content">
      <div class="title">
        {@render children?.()}
      </div>

      {#if subtitle}
        <div class="subtitle">{subtitle}</div>
      {/if}

      {#if actions}
        <div class="actions">
          {@render actions?.()}
        </div>
      {/if}
    </div>
  </div>
</div>

<style>
  .placeholder {
    width: 100%;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .card {
    width: min(320px, calc(100vw - 0.3rem));
    display: flex;
    align-items: flex-start;
    gap: 12px;

    padding: 14px 14px;
    border-radius: 12px;

    background: var(--bg-medium);
    border: 1px solid var(--bg-light-extra);

    opacity: 0.3;
    animation: placeholder-fade 0.3s ease forwards;
  }

  .card[data-no-subtitle="true"] .content {
    justify-content: center;
  }

  .card[data-compact="true"] {
    padding: 10px 12px;
    border-radius: 10px;
    gap: 10px;
  }

  .icon {
    width: 40px;
    height: 40px;
    border-radius: 12px;

    display: grid;
    place-items: center;

    background: transparent;
    color: var(--accent);
    flex: 0 0 auto;
  }

  .card[data-variant="error"] .icon {
    color: var(--red);
  }

  .card[data-variant="empty"] .icon {
    color: var(--text-2);
  }

  .card[data-variant="loading"] .icon {
    color: var(--blue-light-extra);
  }

  .content {
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 4px;
    align-self: center;
  }

  .title {
    color: var(--text);
    font-size: 0.98rem;
    font-weight: 400;
    line-height: 1.25;
  }

  .subtitle {
    color: var(--text-2);
    font-size: 0.9rem;
    line-height: 1.25;
  }

  .actions {
    margin-top: 8px;
    display: flex;
    gap: 8px;
    flex-wrap: wrap;
  }

  .spin {
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
</style>
