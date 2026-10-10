import {
  candidate,
  isActive,
  parseBuildInfo,
  parseStatus,
  ReleaseClient,
  type BuildInfo,
  type Release,
  type UpdateStatus,
} from "../modules/updates/releases";
import { fetcher } from "../utils/fetcher";

export const updates = $state({
  info: null as BuildInfo | null,
  release: null as Release | null,
  status: { stage: "idle" } as UpdateStatus,
  preview: false,
  checking: false,
  submitting: false,
  reconnecting: false,
  checkedAt: 0,
  error: "",
  dialogOpen: false,
  installedThisSession: false,
});

let owner: AbortController | null = null;
let timer: ReturnType<typeof setTimeout> | undefined;
let generation = 0;
let mounts = 0;
let client: ReleaseClient | null = null;
let reconnectAttempts = 0;
let awaitingTag: string | null = null;
let observedJob: string | null = null;
let pendingConfirmation: UpdateStatus | null = null;
let polling: AbortSignal | null = null;
let infoRequest: { signal: AbortSignal; promise: Promise<void> } | null = null;

function storage(): Storage | null {
  try {
    return window.localStorage;
  } catch {
    return null;
  }
}

async function refreshBuildInfo(signal: AbortSignal) {
  if (infoRequest?.signal === signal) return infoRequest.promise;
  const promise = (async () => {
    const info = parseBuildInfo(
      await fetcher.get<unknown>("/system/update", { silent: true, signal, timeoutMs: 7000 }),
    );
    if (!signal.aborted) updates.info = info;
  })();
  infoRequest = { signal, promise };
  try {
    await promise;
  } finally {
    if (infoRequest?.promise === promise) infoRequest = null;
  }
}

export async function checkUpdates(force = false) {
  if (
    !owner ||
    updates.checking ||
    updates.submitting ||
    updates.reconnecting ||
    isActive(updates.status)
  )
    return;
  const signal = owner.signal;
  const revision = ++generation;
  updates.checking = true;
  updates.error = "";
  try {
    // Reopening the popup reuses build info and the one-hour release cache.
    // A failed initial build-info request can still be retried without reload.
    if (!updates.info) await refreshBuildInfo(signal);
    if (signal.aborted || revision !== generation) return;
    const result = await client!.latest(updates.preview, force, signal);
    if (signal.aborted || revision !== generation) return;
    updates.release = result.release;
    updates.checkedAt = result.checkedAt;
  } catch (error) {
    if (!signal.aborted && revision === generation) {
      updates.error = error instanceof Error ? error.message : "Cannot check GitHub releases";
    }
  } finally {
    if (!signal.aborted && revision === generation) updates.checking = false;
  }
}

export function setUpdateChannel(preview: boolean) {
  if (updates.submitting || updates.checking || updates.reconnecting || isActive(updates.status))
    return;
  updates.preview = preview;
  const cached = client?.cached(preview);
  updates.release = cached?.release ?? null;
  updates.checkedAt = cached?.checkedAt ?? 0;
  try {
    storage()?.setItem("mt-c.update-preview", String(preview));
  } catch {
    /* optional preference */
  }
}

async function confirmInstallation(signal: AbortSignal) {
  const expected = pendingConfirmation;
  if (!expected) return;
  await refreshBuildInfo(signal);
  if (signal.aborted) return;
  if (
    updates.info?.installed_version !== expected.target_version ||
    updates.info?.installed_revision !== expected.target_revision
  ) {
    throw new Error("Installed version could not be confirmed");
  }
  pendingConfirmation = null;
  updates.reconnecting = false;
  reconnectAttempts = 0;
  updates.error = "";
  // Historical jobs must not restore this hint after a page reload.
  updates.installedThisSession = true;
}

async function pollStatus() {
  if (!owner || owner.signal.aborted || polling === owner.signal) return;
  const signal = owner.signal;
  polling = signal;
  try {
    if (pendingConfirmation) {
      // Retry only the idempotent version read, never the install POST.
      await confirmInstallation(signal);
      return;
    }
    const status = parseStatus(
      await fetcher.get<unknown>("/system/update/status", {
        silent: true,
        signal,
        timeoutMs: 7000,
      }),
    );
    if (signal.aborted) return;
    if (awaitingTag && status.tag !== awaitingTag) {
      updates.error = "Update start was not confirmed; no automatic retry was made";
      updates.reconnecting = false;
      awaitingTag = null;
      return;
    }
    const wasActive = isActive(updates.status);
    if (awaitingTag) updates.error = "";
    awaitingTag = null;
    if (isActive(status)) observedJob = status.job_id ?? null;
    updates.status = status;
    updates.reconnecting = false;
    reconnectAttempts = 0;
    if (status.stage === "succeeded" && observedJob === status.job_id) {
      pendingConfirmation = status;
      updates.reconnecting = true;
      await confirmInstallation(signal);
    } else if (wasActive && !isActive(status)) {
      // A page reopened during an update must regain a usable candidate even
      // when the worker fails. The package may have changed before failure.
      updates.info = null;
      await refreshBuildInfo(signal);
      if (!signal.aborted) void checkUpdates();
    }
  } catch (error) {
    if (!signal.aborted) {
      if (
        pendingConfirmation ||
        isActive(updates.status) ||
        updates.submitting ||
        updates.reconnecting
      ) {
        updates.reconnecting = true;
        reconnectAttempts++;
        if (pendingConfirmation) updates.error = "Installed version could not be confirmed";
      } else {
        updates.error = error instanceof Error ? error.message : "Invalid update response";
      }
    }
  } finally {
    if (polling === signal) polling = null;
    if (
      !signal.aborted &&
      (pendingConfirmation ||
        isActive(updates.status) ||
        updates.reconnecting ||
        updates.submitting)
    ) {
      if (timer) clearTimeout(timer);
      timer = setTimeout(() => void pollStatus(), Math.min(10000, 2000 + reconnectAttempts * 1000));
    }
  }
}

export async function installUpdate() {
  const selected = candidate(updates.release, updates.info);
  if (
    !owner ||
    !updates.release ||
    !selected.asset ||
    selected.reason ||
    updates.submitting ||
    updates.checking ||
    updates.reconnecting ||
    pendingConfirmation ||
    isActive(updates.status)
  )
    return;
  const signal = owner.signal;
  const release = updates.release;
  updates.submitting = true;
  updates.installedThisSession = false;
  awaitingTag = release.tag_name;
  updates.error = "";
  try {
    const result = parseStatus(
      await fetcher<unknown>("/system/update/install", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          tag: release.tag_name,
          release_id: release.id,
          preview: updates.preview,
        }),
        silent: true,
        signal,
        timeoutMs: 10000,
      }),
    );
    if (signal.aborted) return;
    updates.status = result;
    observedJob = result.job_id ?? null;
  } catch {
    if (!signal.aborted) {
      // The response can be lost after acceptance. Never blindly retry the POST.
      updates.error = "Could not confirm update start; checking router status";
      updates.reconnecting = true;
    }
  } finally {
    if (!signal.aborted) {
      updates.submitting = false;
      if (timer) clearTimeout(timer);
      void pollStatus();
    }
  }
}

/** One owner in the persistent header. Closing the popup never cancels the
 * router's job; unmounting only cleans up browser requests and timers. */
export function mountUpdates() {
  if (++mounts === 1) {
    owner = new AbortController();
    const signal = owner.signal;
    client = new ReleaseClient(storage());
    updates.info = null;
    updates.status = { stage: "idle" };
    updates.error = "";
    updates.checking = false;
    updates.submitting = false;
    updates.reconnecting = false;
    updates.installedThisSession = false;
    awaitingTag = null;
    observedJob = null;
    pendingConfirmation = null;
    reconnectAttempts = 0;
    try {
      updates.preview = storage()?.getItem("mt-c.update-preview") === "true";
    } catch {
      /* default stable */
    }
    const cached = client.cached(updates.preview);
    updates.release = cached?.release ?? null;
    updates.checkedAt = cached?.checkedAt ?? 0;
    void (async () => {
      await pollStatus();
      if (signal.aborted) return;
      // Unlike discovery, this read must not be skipped during an active job.
      // A simultaneous popup open may already have loaded it.
      try {
        if (!updates.info) await refreshBuildInfo(signal);
      } catch (error) {
        if (!signal.aborted) {
          updates.error = error instanceof Error ? error.message : "Invalid update response";
        }
        return;
      }
      if (!signal.aborted) void checkUpdates();
    })();
  }
  return () => {
    if (--mounts === 0) {
      owner?.abort();
      owner = null;
      generation++;
      if (timer) clearTimeout(timer);
      updates.dialogOpen = false;
    }
  };
}
