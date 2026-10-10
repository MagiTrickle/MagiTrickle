<script lang="ts">
  import { Popover } from "bits-ui";
  import { onMount } from "svelte";

  import Button from "../../components/ui/Button.svelte";
  import Switch from "../../components/ui/Switch.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { t } from "../../data/locale.svelte";
  import {
    checkUpdates,
    installUpdate,
    mountUpdates,
    setUpdateChannel,
    updates,
  } from "../../data/updates.svelte";

  import { candidate, isActive, RELEASE_WEB } from "./releases";

  onMount(mountUpdates);
  const version = import.meta.env.VITE_PKG_VERSION || "0.0.0";
  const commit = version.match(/(?:\(|~git\d{14}\.)([a-f0-9]{7,40})\)?$/i)?.[1] ?? "";
  const isDev =
    import.meta.env.VITE_PKG_VERSION_IS_DEV?.toLowerCase() === "true" || version === "0.0.0";
  const isMobile =
    /Android|iPhone|iPad|iPod/i.test(navigator.userAgent) ||
    (navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1);
  const reloadInstruction = isMobile
    ? "Refresh the page in your browser."
    : /Mac/i.test(navigator.platform)
      ? "Press ⌘R to reload the page."
      : "Press F5 to reload the page.";
  const choice = $derived(candidate(updates.release, updates.info));
  const busy = $derived(isActive(updates.status) || updates.submitting || updates.reconnecting);
  const completed = $derived(
    updates.installedThisSession && !busy && !updates.error && !choice.newer,
  );
  const upToDate = $derived(
    completed ||
      (!!updates.info &&
        updates.checkedAt > 0 &&
        !updates.checking &&
        !updates.error &&
        !choice.reason &&
        !choice.newer &&
        !busy),
  );
  const canUpdate = $derived(
    !busy && !completed && !updates.checking && !updates.error && !!choice.asset && !choice.reason,
  );
  const canRetry = $derived(!busy && !updates.checking && !!updates.error);
  const buildHint = $derived(
    choice.newer
      ? t("New version available")
      : isDev
        ? t("dev-build")
        : `${t("build")}: ${version}`,
  );
  const hint = $derived(commit ? `${buildHint} · ${commit}` : buildHint);
  const statusText = $derived(
    {
      idle: "Ready to update",
      queued: "Preparing update",
      checking: "Verifying GitHub release",
      downloading: "Downloading package",
      verifying: "Verifying package checksum",
      backing_up: "Backing up settings",
      installing: "Installing package",
      restarting: "Waiting for service restart",
      succeeded: "Update installed",
      failed: "Update failed",
      interrupted: "Update interrupted",
    }[updates.status.stage],
  );
</script>

<div class="version">
  <Popover.Root
    bind:open={updates.dialogOpen}
    onOpenChange={(open) => {
      if (open) void checkUpdates();
    }}
  >
    <Tooltip value={hint}>
      <Popover.Trigger
        class="version-trigger"
        aria-label={`${t("Software update")}: ${version}. ${hint}`}
      >
        <span class="version-text">{version}</span>
        {#if choice.newer || isDev}
          <span class="version-dot" class:available={choice.newer} aria-hidden="true"></span>
        {/if}
      </Popover.Trigger>
    </Tooltip>
    <Popover.Portal>
      <Popover.Content
        class="update-popover"
        role="dialog"
        side="bottom"
        align="end"
        sideOffset={8}
        collisionPadding={8}
        aria-label={t("Software update")}
      >
        <div class="update-action" class:ready={canUpdate || canRetry} class:busy aria-busy={busy}>
          <Button
            type="button"
            class="accent"
            inactive={!canUpdate && !canRetry && !busy}
            disabled={!canUpdate && !canRetry}
            onclick={() => void (canRetry ? checkUpdates(true) : installUpdate())}
            >{busy
              ? t("Updating…")
              : canRetry
                ? t("Check for updates")
                : upToDate
                  ? t("Up to date")
                  : t("Update")}</Button
          >
        </div>
        <div role="status" aria-live="polite">
          {#if completed}
            <p>{t(reloadInstruction)}</p>
          {:else if busy}
            <span class="sr-only"
              >{updates.reconnecting
                ? t("Reconnecting to router; update status is not yet known")
                : t(statusText)}</span
            >
          {:else if ["failed", "interrupted"].includes(updates.status.stage) && !updates.status.error}
            <p>{t(statusText)}</p>
          {/if}
          {#if updates.error}<p>{t(updates.error)}</p>{/if}
          {#if !busy && !completed && choice.reason}<p>{t(choice.reason)}</p>{/if}
        </div>
        {#if updates.status.error}<p role="alert">{t(updates.status.error)}</p>{/if}
        <div class="update-footer">
          {#if updates.release}
            <a
              class="release-notes"
              href={`${RELEASE_WEB}/tag/${encodeURIComponent(updates.release.tag_name)}`}
              target="_blank"
              rel="noopener noreferrer">{updates.release.tag_name} · {t("Release notes")}</a
            >
          {/if}
          <label class="dev-builds">
            <Switch
              checked={updates.preview}
              onCheckedChange={setUpdateChannel}
              disabled={busy || updates.checking}
              aria-label={t("dev")}
            />
            <span>{t("dev")}</span>
          </label>
        </div>
      </Popover.Content>
    </Popover.Portal>
  </Popover.Root>
</div>

<style>
  .version {
    min-width: 0;
  }
  .version :global([role="tooltip"]) {
    max-width: 100%;
  }
  :global(.version-trigger) {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    min-width: 0;
    padding: 0.35rem 0;
    border: 0;
    background: transparent;
    color: var(--text-2);
    font: inherit;
    cursor: pointer;
  }
  :global(.version-trigger:focus-visible) {
    outline: 2px solid var(--blue-light-extra);
    outline-offset: 3px;
    border-radius: 0.25rem;
  }
  .version-text {
    font-size: smaller;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .version-dot {
    order: -1;
    width: 6px;
    height: 6px;
    flex: 0 0 auto;
    border-radius: 50%;
    background: var(--orange);
  }
  .version-dot.available {
    background: var(--blue-light-extra);
  }
  :global(.update-popover) {
    z-index: 50;
    width: max-content;
    max-width: calc(100vw - 16px);
    box-sizing: border-box;
    max-height: var(--bits-popover-content-available-height);
    overflow: auto;
    padding: 0.65rem;
    background: var(--bg-light);
    border: 1px solid var(--border-light);
    border-radius: 0.5rem;
    box-shadow: 0 4px 16px #0003;
    color: var(--text);
    font: 0.9rem var(--font);
    overflow-wrap: anywhere;
  }
  .update-footer {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
    margin-top: 0.6rem;
    flex-wrap: wrap;
  }
  .dev-builds {
    margin-left: auto;
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
    font-size: 0.8rem;
    color: var(--text-2);
    white-space: nowrap;
    cursor: pointer;
  }
  .update-footer :global([data-switch-root]:focus-visible) {
    outline: 2px solid var(--orange);
    outline-offset: 3px;
  }
  .dev-builds :global([data-switch-root][data-state="checked"]) {
    background-color: var(--orange);
  }
  .release-notes {
    min-width: 0;
    text-align: left;
    font-size: 0.8rem;
    color: var(--blue-light-extra);
  }
  .update-action :global(button) {
    width: 100%;
  }
  .update-action :global(button.accent) {
    overflow: hidden;
    transition:
      border-color 0.3s ease,
      color 0.3s ease,
      opacity 0.3s ease;
  }
  .update-action.ready :global(button.accent) {
    border-color: color-mix(in srgb, var(--accent), transparent 35%) !important;
    color: var(--accent);
  }
  .update-action :global(button.accent::after) {
    transition: background-color 0.3s ease;
  }
  .update-action.ready :global(button.accent::after) {
    background: color-mix(in srgb, var(--accent) 8%, var(--bg-light));
  }
  .update-action.ready :global(button.accent:hover::after) {
    background: color-mix(in srgb, var(--accent) 14%, var(--bg-light));
  }
  .update-action :global(button.accent::before) {
    opacity: 0;
    animation-play-state: paused;
  }
  .update-action.busy :global(button.accent::before) {
    opacity: 1;
    animation-play-state: running;
  }
  .update-action.busy :global(button) {
    cursor: wait;
  }
  p {
    margin: 0.6rem 0 0;
    color: var(--text-2);
  }
  .sr-only {
    position: absolute;
    width: 1px;
    height: 1px;
    padding: 0;
    margin: -1px;
    overflow: hidden;
    clip-path: inset(50%);
    white-space: nowrap;
  }
  @media (prefers-reduced-motion: reduce) {
    .update-action :global(button.accent),
    .update-action :global(button.accent::before),
    .update-action :global(button.accent::after) {
      transition: none;
    }
    .update-action.busy :global(button.accent::before) {
      animation: none;
    }
  }
</style>
