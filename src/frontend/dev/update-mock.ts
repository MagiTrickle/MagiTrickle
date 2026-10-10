import type { Hono } from "hono";

import { compareVersions, parseVersion, type UpdateStatus } from "../src/modules/updates/releases.ts";

const UPDATE_BASE = "/api/v1/system/update";
const MOCK_ASSET_SUFFIX = "entware_aarch64-3.10_kn.ipk";
// The mock deliberately starts below the stable GitHub release so the update
// button is testable without creating a release or installing anything.
const MOCK_INSTALLED_VERSION = "0.8.2.1";
const STAGES = [
  "queued",
  "checking",
  "downloading",
  "verifying",
  "backing_up",
  "installing",
  "restarting",
  "succeeded",
] as const;
const STAGE_DURATION_MS = 700;

type MockJob = {
  job_id: string;
  tag: string;
  target_version: string;
  target_revision: number;
  startedAt: number;
};

/**
 * Development-only, in-memory simulation of the three update endpoints.
 * No GitHub fetch, filesystem modification, process execution or router
 * restart happens here. The browser still checks real GitHub Releases.
 *
 * An injectable clock keeps the staged transitions deterministic in tests.
 */
export function registerMockUpdateRoutes(
  app: Hono,
  rootToken: string,
  now: () => number = Date.now,
) {
  let installedVersion = MOCK_INSTALLED_VERSION;
  let installedRevision = 1;
  let job: MockJob | null = null;

  function isRoot(authHeader: string | undefined): boolean {
    return authHeader === "Bearer " + rootToken;
  }

  function status(): UpdateStatus {
    if (!job) return { stage: "idle" };

    const elapsed = Math.max(0, now() - job.startedAt);
    const index = Math.min(Math.floor(elapsed / STAGE_DURATION_MS), STAGES.length - 1);
    const stage = STAGES[index];
    if (stage === "succeeded") {
      installedVersion = job.target_version;
      installedRevision = job.target_revision;
    }

    return {
      job_id: job.job_id,
      tag: job.tag,
      target_version: job.target_version,
      target_revision: job.target_revision,
      stage,
    };
  }

  app.get(UPDATE_BASE, (c) => {
    const allowed = isRoot(c.req.header("Authorization"));
    c.header("Cache-Control", "no-store");
    return c.json({
      installed_version: installedVersion,
      installed_revision: installedRevision,
      asset_suffix: MOCK_ASSET_SUFFIX,
      can_install: allowed,
      reason: allowed ? "" : "Sign in as root to install updates",
    });
  });

  app.get(UPDATE_BASE + "/status", (c) => {
    c.header("Cache-Control", "no-store");
    return c.json(status());
  });

  app.post(UPDATE_BASE + "/install", async (c) => {
    // Mirrors the production endpoint's separate root check even when the
    // development backend's general auth middleware accepts other tokens.
    if (!isRoot(c.req.header("Authorization"))) {
      return c.json({ error: "Sign in as root to install updates" }, 403);
    }

    const site = c.req.header("Sec-Fetch-Site");
    const type = c.req.header("Content-Type");
    if (site === "cross-site" || !type?.startsWith("application/json")) {
      return c.json({ error: "Expected same-origin JSON request" }, 400);
    }

    const text = await c.req.text();
    if (!text || new TextEncoder().encode(text).length > 1024) {
      return c.json({ error: "Invalid or non-newer release" }, 400);
    }

    let input: unknown;
    try {
      input = JSON.parse(text);
    } catch {
      return c.json({ error: "Invalid or non-newer release" }, 400);
    }
    if (!input || typeof input !== "object" || Array.isArray(input)) {
      return c.json({ error: "Invalid or non-newer release" }, 400);
    }
    const body = input as Record<string, unknown>;
    const parsed = typeof body.tag === "string" ? parseVersion(body.tag) : null;
    const newer = parsed
      ? compareVersions(body.tag as string, installedVersion, 1, installedRevision)
      : null;
    if (
      Object.keys(body).length !== 3 ||
      !parsed ||
      parsed.development ||
      newer === null ||
      newer <= 0 ||
      typeof body.release_id !== "number" ||
      !Number.isSafeInteger(body.release_id) ||
      body.release_id < 1 ||
      typeof body.preview !== "boolean"
    ) {
      return c.json({ error: "Invalid or non-newer release" }, 400);
    }
    if (job && !["succeeded", "failed", "interrupted"].includes(status().stage)) {
      return c.json({ error: "An update is already running" }, 409);
    }

    job = {
      job_id: crypto.randomUUID().replaceAll("-", ""),
      tag: body.tag as string,
      target_version: parsed.base,
      target_revision: parsed.revision,
      startedAt: now(),
    };
    return c.json({ job_id: job.job_id, stage: "queued" }, 202);
  });
}
