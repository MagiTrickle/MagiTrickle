/** GitHub is contacted by the browser, never by the routing daemon's timers. */
export const RELEASE_API = "https://api.github.com/repos/dan0102dan/mt-c/releases";
export const RELEASE_WEB = "https://github.com/dan0102dan/mt-c/releases";
export const CACHE_TTL = 60 * 60 * 1000;

export type BuildInfo = {
  installed_version: string;
  installed_revision: number;
  asset_suffix: string;
  can_install: boolean;
  reason: string;
};
export type Asset = {
  name: string;
  browser_download_url: string;
  state: string;
  size: number;
  digest: string | null;
};
export type Release = {
  id: number;
  tag_name: string;
  name: string;
  body: string;
  published_at: string;
  draft: boolean;
  prerelease: boolean;
  assets: Asset[];
};
export const STAGES = [
  "idle",
  "queued",
  "checking",
  "downloading",
  "verifying",
  "backing_up",
  "installing",
  "restarting",
  "succeeded",
  "failed",
  "interrupted",
] as const;
export type UpdateStatus = {
  stage: (typeof STAGES)[number];
  job_id?: string;
  tag?: string;
  target_version?: string;
  target_revision?: number;
  error?: string;
  backup_path?: string;
};

function record(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
function integer(value: unknown): value is number {
  return typeof value === "number" && Number.isSafeInteger(value) && value > 0;
}
function text(value: unknown, limit = 16384): value is string {
  return typeof value === "string" && value.length <= limit;
}
export function parseBuildInfo(value: unknown): BuildInfo {
  if (
    !record(value) ||
    !text(value.installed_version, 128) ||
    !integer(value.installed_revision) ||
    !text(value.asset_suffix, 192) ||
    typeof value.can_install !== "boolean" ||
    !text(value.reason, 256)
  ) {
    throw new Error("Invalid update response");
  }
  return {
    installed_version: value.installed_version,
    installed_revision: value.installed_revision,
    asset_suffix: value.asset_suffix,
    can_install: value.can_install,
    reason: value.reason,
  };
}
export function parseStatus(value: unknown): UpdateStatus {
  if (!record(value) || !STAGES.some((stage) => stage === value.stage)) {
    throw new Error("Invalid update response");
  }
  const stage = STAGES.find((item) => item === value.stage)!;
  if (stage !== "idle" && (!text(value.job_id, 32) || !/^[a-f0-9]{32}$/.test(value.job_id))) {
    throw new Error("Invalid update response");
  }
  return {
    stage,
    job_id: text(value.job_id, 32) ? value.job_id : undefined,
    tag: text(value.tag, 80) ? value.tag : undefined,
    target_version: text(value.target_version, 128) ? value.target_version : undefined,
    target_revision: integer(value.target_revision) ? value.target_revision : undefined,
    error: text(value.error, 512) ? value.error : undefined,
    backup_path: text(value.backup_path, 512) ? value.backup_path : undefined,
  };
}
export function isActive(status: UpdateStatus): boolean {
  return !["idle", "succeeded", "failed", "interrupted"].includes(status.stage);
}

export function parseVersion(input: string, revision = 1) {
  const match =
    /^(v?\d{1,9}\.\d{1,9}(?:\.\d{1,9}){0,2})(?:(-rev)(\d{1,9})|(~git\d{14}\.[a-fA-F0-9]{7,40}|_pre\d{14}))?$/.exec(
      input,
    );
  if (!match || !integer(revision)) return null;
  const parts = match[1].replace(/^v/, "").split(".").map(Number);
  while (parts.length < 4) parts.push(0);
  const rev = match[3] ? Number(match[3]) : revision;
  if (!integer(rev)) return null;
  return { parts, revision: rev, development: !!match[4], base: match[1] };
}
export function compareVersions(a: string, b: string, aRevision = 1, bRevision = 1): number | null {
  const left = parseVersion(a, aRevision);
  const right = parseVersion(b, bRevision);
  if (!left || !right) return null;
  for (let i = 0; i < 4; i++) {
    if (left.parts[i] !== right.parts[i]) return Math.sign(left.parts[i] - right.parts[i]);
  }
  if (left.development !== right.development) return left.development ? -1 : 1;
  return Math.sign(left.revision - right.revision);
}

export function parseRelease(value: unknown): Release {
  if (
    !record(value) ||
    !integer(value.id) ||
    !text(value.tag_name, 80) ||
    typeof value.draft !== "boolean" ||
    typeof value.prerelease !== "boolean" ||
    !text(value.published_at, 20) ||
    !/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/.test(value.published_at) ||
    !Number.isFinite(Date.parse(value.published_at)) ||
    !Array.isArray(value.assets)
  ) {
    throw new Error("Invalid release metadata");
  }
  const assets: Asset[] = [];
  for (const item of value.assets) {
    if (
      !record(item) ||
      !text(item.name, 256) ||
      !text(item.browser_download_url, 1024) ||
      !text(item.state, 32) ||
      !integer(item.size)
    )
      continue;
    assets.push({
      name: item.name,
      browser_download_url: item.browser_download_url,
      state: item.state,
      size: item.size,
      digest: text(item.digest, 128) ? item.digest : null,
    });
  }
  return {
    id: value.id,
    tag_name: value.tag_name,
    name: text(value.name, 256) ? value.name : value.tag_name,
    body: text(value.body, 65536) ? value.body : "",
    published_at: value.published_at,
    draft: value.draft,
    prerelease: value.prerelease,
    assets,
  };
}
function parseReleases(value: unknown): Release[] {
  if (!Array.isArray(value)) throw new Error("Invalid release metadata");
  return value
    .filter((item) => record(item) && item.draft === false && item.published_at)
    .map(parseRelease);
}
export function pickRelease(value: unknown, preview: boolean): Release | null {
  return selectRelease(parseReleases(value), preview);
}
function selectRelease(releases: Release[], preview: boolean): Release | null {
  return releases.reduce<Release | null>(
    (latest, item) =>
      (!preview && item.prerelease) || (latest && item.published_at <= latest.published_at)
        ? latest
        : item,
    null,
  );
}
export function candidate(release: Release | null, info: BuildInfo | null) {
  if (!info || !release) return { newer: false, asset: null, reason: "" };
  const target = parseVersion(release.tag_name);
  const comparison = compareVersions(
    release.tag_name,
    info.installed_version,
    1,
    info.installed_revision,
  );
  if (!target || target.development || comparison === null) {
    return { newer: false, asset: null, reason: "Unknown installed or release version" };
  }
  if (comparison <= 0) return { newer: false, asset: null, reason: "" };
  if (!info.asset_suffix) return { newer: true, asset: null, reason: "Unsupported build target" };
  const names = [`mt-c_${target.base}-${target.revision}_${info.asset_suffix}`];
  if (info.asset_suffix.endsWith(".apk"))
    names.push(`mt-c_${target.base}-r${target.revision}_${info.asset_suffix}`);
  const matches = release.assets.filter((item) => names.includes(item.name));
  if (matches.length !== 1)
    return {
      newer: true,
      asset: null,
      reason: matches.length ? "Ambiguous package assets" : "No compatible package",
    };
  const asset = matches[0];
  if (
    asset.state !== "uploaded" ||
    asset.size > 64 * 1024 * 1024 ||
    !/^sha256:[a-f0-9]{64}$/.test(asset.digest ?? "") ||
    asset.browser_download_url !== `${RELEASE_WEB}/download/${release.tag_name}/${asset.name}`
  ) {
    return { newer: true, asset: null, reason: "Package verification metadata missing" };
  }
  return { newer: true, asset, reason: info.can_install ? "" : info.reason };
}

export type ReleaseCache = { release: Release | null; checkedAt: number; etag: string };
type ReleaseListCache = { releases: Release[]; checkedAt: number; etag: string };
type StorageLike = Pick<Storage, "getItem" | "setItem">;
const RELEASE_CACHE_KEY = "mt-c.releases.v2";

/** One shared list for both channels. A router JWT is NEVER sent to GitHub.
 * Memory keeps channel switching local even when browser storage is blocked. */
export class ReleaseClient {
  private inFlight: Promise<ReleaseListCache> | null = null;
  private blockedUntil = 0;
  private snapshot: ReleaseListCache | null = null;
  constructor(
    private storage: StorageLike | null,
    private request: typeof fetch = (...args) => fetch(...args),
  ) {}

  private readCache(): ReleaseListCache | null {
    if (this.snapshot) return this.snapshot;
    try {
      const value: unknown = JSON.parse(this.storage?.getItem(RELEASE_CACHE_KEY) ?? "null");
      if (
        !record(value) ||
        typeof value.checkedAt !== "number" ||
        !Number.isFinite(value.checkedAt) ||
        !text(value.etag, 256)
      )
        return null;
      this.snapshot = {
        checkedAt: value.checkedAt,
        etag: value.etag,
        releases: parseReleases(value.releases),
      };
      return this.snapshot;
    } catch {
      return null;
    }
  }

  cached(preview: boolean): ReleaseCache | null {
    const cached = this.readCache();
    return cached
      ? {
          release: selectRelease(cached.releases, preview),
          checkedAt: cached.checkedAt,
          etag: cached.etag,
        }
      : null;
  }

  async latest(preview: boolean, force: boolean, signal?: AbortSignal): Promise<ReleaseCache> {
    const cached = this.readCache();
    const age = cached ? Date.now() - cached.checkedAt : Infinity;
    let result: ReleaseListCache;
    if (this.inFlight) result = await this.inFlight;
    else if (!force && cached && age >= 0 && age < CACHE_TTL) result = cached;
    else {
      if (Date.now() < this.blockedUntil) throw new Error("GitHub rate limit; try again later");
      const operation = this.load(cached, signal).finally(() => {
        this.inFlight = null;
      });
      this.inFlight = operation;
      result = await operation;
    }
    return {
      release: selectRelease(result.releases, preview),
      checkedAt: result.checkedAt,
      etag: result.etag,
    };
  }

  private async load(
    cached: ReleaseListCache | null,
    signal?: AbortSignal,
  ): Promise<ReleaseListCache> {
    const controller = new AbortController();
    const abort = () => controller.abort();
    signal?.addEventListener("abort", abort, { once: true });
    if (signal?.aborted) controller.abort();
    const timeout = setTimeout(abort, 15000);
    try {
      const headers: Record<string, string> = { Accept: "application/vnd.github+json" };
      if (cached?.etag) headers["If-None-Match"] = cached.etag;
      const response = await this.request(`${RELEASE_API}?per_page=100`, {
        credentials: "omit",
        headers,
        signal: controller.signal,
        cache: "no-cache",
      });
      if (response.status === 403 || response.status === 429) {
        const reset = Number(response.headers.get("X-RateLimit-Reset")) * 1000;
        const retry = Number(response.headers.get("Retry-After")) * 1000;
        this.blockedUntil = Math.max(
          Date.now() + 60000,
          Number.isFinite(reset) ? reset : 0,
          Date.now() + (Number.isFinite(retry) ? retry : 0),
        );
        throw new Error("GitHub rate limit; try again later");
      }
      let releases: Release[];
      if (response.status === 304 && cached) releases = cached.releases;
      else if (response.status === 404) releases = [];
      else {
        if (!response.ok) throw new Error("Cannot check GitHub releases");
        const raw = await response.text();
        if (raw.length > 4 * 1024 * 1024) throw new Error("Invalid release metadata");
        releases = parseReleases(JSON.parse(raw));
      }
      const result = {
        releases,
        checkedAt: Date.now(),
        etag: response.status === 404 ? "" : (response.headers.get("ETag") ?? cached?.etag ?? ""),
      };
      this.snapshot = result;
      try {
        this.storage?.setItem(RELEASE_CACHE_KEY, JSON.stringify(result));
      } catch {
        /* optional cache */
      }
      return result;
    } finally {
      clearTimeout(timeout);
      signal?.removeEventListener("abort", abort);
    }
  }
}
