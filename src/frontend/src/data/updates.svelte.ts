import {
  candidate,
  isActive,
  parseBuildInfo,
  parseStatus,
  parseVersion,
  ReleaseClient,
  type BuildInfo,
  type Release,
  type UpdateStatus,
} from "../modules/updates/releases";
import { fetcher } from "../utils/fetcher";
import { HttpError } from "../utils/http-error";

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

type PendingInstall = { tag: string; version: string; revision: number };
const PENDING_KEY = "mt-c.update-pending.v1";
const UNCONFIRMED = "Update could not be confirmed; check the package manager from CLI";
let pending: PendingInstall | null = null;
let owner: AbortController | null = null;
let timer: ReturnType<typeof setTimeout> | undefined;
let mounts = 0;
let client: ReleaseClient | null = null;
let reconnectAttempts = 0;
let polling: AbortSignal | null = null;
/* A status GET from before the new install request must not replace the
 * accepted job or clear its pending version confirmation. */
let operationEpoch = 0;
let infoRequest: { signal: AbortSignal; promise: Promise<void> } | null = null;

function storage(): Storage | null {
  try {
    return window.localStorage;
  } catch {
    return null;
  }
}

function rememberPending(value: PendingInstall | null) {
  pending = value;
  try {
    if (value) window.sessionStorage.setItem(PENDING_KEY, JSON.stringify(value));
    else window.sessionStorage.removeItem(PENDING_KEY);
  } catch {
    /* Optional per-tab intent, never an installation log. */
  }
}

function restorePending() {
  pending = null;
  try {
    const value = JSON.parse(window.sessionStorage.getItem(PENDING_KEY) ?? "null");
    if (
      value &&
      typeof value.tag === "string" &&
      typeof value.version === "string" &&
      Number.isSafeInteger(value.revision) &&
      value.revision > 0 &&
      parseVersion(value.tag) &&
      parseVersion(value.version, value.revision)
    )
      pending = { tag: value.tag, version: value.version, revision: value.revision };
  } catch {
    /* A blocked or stale session must not break the header. */
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
  updates.checking = true;
  updates.error = "";
  try {
    // Explicit Retry must refresh the installed router version as well.
    // Normal popover opens and channel switches continue using the cache.
    if (force || !updates.info) await refreshBuildInfo(signal);
    if (signal.aborted) return;
    const result = await client!.latest(updates.preview, force, signal);
    if (signal.aborted) return;
    updates.release = result.release;
    updates.checkedAt = result.checkedAt;
  } catch (error) {
    if (!signal.aborted)
      updates.error = error instanceof Error ? error.message : "Cannot check GitHub releases";
  } finally {
    if (!signal.aborted) updates.checking = false;
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
    /* Optional. */
  }
}

function stopWaiting() {
  rememberPending(null);
  updates.reconnecting = false;
  updates.status = { stage: "idle" };
  updates.error = UNCONFIRMED;
}

async function pollStatus() {
  if (!owner || owner.signal.aborted || polling === owner.signal || updates.submitting) return;
  const signal = owner.signal;
  const epoch = operationEpoch;
  polling = signal;
  try {
    const status = parseStatus(
      await fetcher.get<unknown>("/system/update/status", {
        silent: true,
        signal,
        timeoutMs: 7000,
      }),
    );
    // Discard a GET started before a new POST, even if it arrives last.
    if (signal.aborted || epoch !== operationEpoch) return;
    const wasActive = isActive(updates.status);
    updates.status = status;
    updates.reconnecting = false;
    if (isActive(status)) {
      // An existing preparation may be observed by another tab. A restarted
      // daemon can only report that the system installer holds the lock.
      if (!pending && status.tag && status.target_version && status.target_revision) {
        rememberPending({
          tag: status.tag,
          version: status.target_version,
          revision: status.target_revision,
        });
      }
      reconnectAttempts = 0;
    } else if (status.stage === "failed" || status.stage === "interrupted") {
      rememberPending(null);
      updates.info = null;
      await refreshBuildInfo(signal);
      if (!signal.aborted) void checkUpdates();
    } else if (pending) {
      // The daemon deliberately does not persist job outcomes. A free lock or
      // reachable HTTP alone is not success: read the newly running version.
      updates.reconnecting = true;
      const expected = pending;
      await refreshBuildInfo(signal);
      if (signal.aborted) return;
      if (
        updates.info?.installed_version === expected.version &&
        updates.info?.installed_revision === expected.revision
      ) {
        rememberPending(null);
        updates.installedThisSession = true;
        updates.reconnecting = false;
        updates.error = "";
        reconnectAttempts = 0;
      } else {
        stopWaiting();
      }
    } else if (wasActive) {
      updates.info = null;
      await refreshBuildInfo(signal);
      if (!signal.aborted) void checkUpdates();
    }
  } catch (error) {
    if (!signal.aborted) {
      if (pending || isActive(updates.status) || updates.submitting || updates.reconnecting) {
        updates.reconnecting = true;
        reconnectAttempts++;
        if (pending) updates.error = "Installed version could not be confirmed";
        if (reconnectAttempts >= 30) stopWaiting();
      } else {
        updates.error = error instanceof Error ? error.message : "Invalid update response";
      }
    }
  } finally {
    if (polling === signal) polling = null;
    if (
      !signal.aborted &&
      (pending || isActive(updates.status) || updates.reconnecting || updates.submitting)
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
    pending ||
    isActive(updates.status)
  )
    return;
  const signal = owner.signal;
  const release = updates.release;
  const version = parseVersion(release.tag_name)!;
  operationEpoch++; // Invalidate earlier status GETs before submitting.
  updates.submitting = true;
  updates.installedThisSession = false;
  updates.error = "";
  rememberPending({ tag: release.tag_name, version: version.base, revision: version.revision });
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
    if (!signal.aborted) updates.status = result;
  } catch (error) {
    if (!signal.aborted) {
      // Never repeat a possibly accepted POST. Definite rejections discard intent;
      // an uncertain transport result is resolved by status + installed version.
      if (error instanceof HttpError && error.status < 500) rememberPending(null);
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

export function mountUpdates() {
  if (++mounts === 1) {
    owner = new AbortController();
    operationEpoch++; // A previous mount's responses are no longer relevant.
    const signal = owner.signal;
    client = new ReleaseClient(storage());
    updates.info = null;
    updates.status = { stage: "idle" };
    updates.error = "";
    updates.checking = false;
    updates.submitting = false;
    updates.reconnecting = false;
    updates.installedThisSession = false;
    reconnectAttempts = 0;
    restorePending();
    updates.reconnecting = pending !== null;
    try {
      updates.preview = storage()?.getItem("mt-c.update-preview") === "true";
    } catch {
      /* Default stable. */
    }
    const cached = client.cached(updates.preview);
    updates.release = cached?.release ?? null;
    updates.checkedAt = cached?.checkedAt ?? 0;
    void (async () => {
      await pollStatus();
      if (signal.aborted) return;
      try {
        if (!updates.info) await refreshBuildInfo(signal);
      } catch (error) {
        if (!signal.aborted)
          updates.error = error instanceof Error ? error.message : "Invalid update response";
        return;
      }
      if (!signal.aborted && !pending && !updates.error) void checkUpdates();
    })();
  }
  return () => {
    if (--mounts === 0) {
      owner?.abort();
      owner = null;
      if (timer) clearTimeout(timer);
      updates.dialogOpen = false;
    }
  };
}
