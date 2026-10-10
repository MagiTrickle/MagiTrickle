import assert from "node:assert/strict";

import { Hono } from "hono";

import { registerMockUpdateRoutes } from "../../dev/update-mock.ts";

const BASE = "/api/v1/system/update";
const ROOT = "development-root-token";
const AUTH = { Authorization: "Bearer " + ROOT };
const INSTALL = { tag: "0.8.2.2", release_id: 123, preview: false };

function fixture() {
  let clock = 1000;
  const app = new Hono();
  registerMockUpdateRoutes(app, ROOT, () => clock);
  return {
    app,
    advance: (ms: number) => { clock += ms; },
    install: (body: unknown = INSTALL, headers: Record<string, string> = AUTH) =>
      app.request(BASE + "/install", {
        method: "POST",
        headers: { "Content-Type": "application/json", ...headers },
        body: JSON.stringify(body),
      }),
  };
}

Deno.test("mock update GET routes expose real response shapes without touching GitHub", async () => {
  const { app } = fixture();
  const status = await app.request(BASE + "/status");
  assert.equal(status.status, 200);
  assert.deepEqual(await status.json(), { stage: "idle" });
  assert.equal(status.headers.get("Cache-Control"), "no-store");

  const unauthorized = await app.request(BASE);
  assert.equal(unauthorized.status, 200);
  const guestInfo = await unauthorized.json();
  assert.equal(guestInfo.installed_version, "0.8.2.1");
  assert.equal(guestInfo.installed_revision, 1);
  assert.equal(guestInfo.asset_suffix, "entware_aarch64-3.10_kn.ipk");
  assert.equal(guestInfo.can_install, false);
  assert.equal(guestInfo.reason, "Sign in as root to install updates");
  assert.equal(unauthorized.headers.get("Cache-Control"), "no-store");

  const root = await app.request(BASE, { headers: AUTH });
  assert.equal(root.status, 200);
  const info = await root.json();
  assert.equal(info.can_install, true);
  assert.equal(info.reason, "");
});

Deno.test("mock update POST requires a root token and well-formed same-origin JSON", async () => {
  const { app, install } = fixture();
  assert.equal((await install(INSTALL, { Authorization: "Bearer disabled" })).status, 403);
  assert.equal((await install(INSTALL, { Authorization: "Bearer wrong" })).status, 403);
  assert.equal((await app.request(BASE + "/install", {
    method: "POST", headers: AUTH,
    body: JSON.stringify(INSTALL),
  })).status, 400);
  assert.equal((await app.request(BASE + "/install", {
    method: "POST",
    headers: { ...AUTH, "Content-Type": "application/json", "Sec-Fetch-Site": "cross-site" },
    body: JSON.stringify(INSTALL),
  })).status, 400);

  for (const bad of [
    { ...INSTALL, tag: "0.8.2.1" },
    { ...INSTALL, tag: "0.8.2.1~git20261010120000.abcdef0" },
    { ...INSTALL, tag: "0.8.2.2/evil" },
    { ...INSTALL, release_id: 0 },
    { ...INSTALL, release_id: 1.5 },
    { ...INSTALL, release_id: "123" },
    { ...INSTALL, preview: "false" },
    { ...INSTALL, download_url: "https://example.org/package.ipk" },
  ]) {
    assert.equal((await install(bad)).status, 400);
  }
  assert.equal((await app.request(BASE + "/install", {
    method: "POST",
    headers: { ...AUTH, "Content-Type": "application/json" },
    body: "{bad json",
  })).status, 400);
  assert.deepEqual(await (await app.request(BASE + "/status")).json(), { stage: "idle" });
});

Deno.test("mock update simulates one job and later returns the new installed version", async () => {
  const { app, advance, install } = fixture();
  const accepted = await install();
  assert.equal(accepted.status, 202);
  const started = await accepted.json();
  assert.match(started.job_id, /^[0-9a-f]{32}$/);
  assert.equal(started.stage, "queued");

  assert.equal((await install({ ...INSTALL, tag: "0.8.3" })).status, 409);
  const queued = await (await app.request(BASE + "/status")).json();
  assert.equal(queued.stage, "queued");
  assert.equal(queued.tag, INSTALL.tag);
  assert.equal(queued.job_id, started.job_id);
  assert.equal(queued.target_version, INSTALL.tag);
  assert.equal(queued.target_revision, 1);

  advance(700);
  assert.equal((await (await app.request(BASE + "/status")).json()).stage, "checking");
  advance(700 * 5);
  assert.equal((await (await app.request(BASE + "/status")).json()).stage, "restarting");
  advance(700);
  const finished = await (await app.request(BASE + "/status")).json();
  assert.equal(finished.stage, "succeeded");
  assert.equal(finished.job_id, started.job_id);

  const info = await (await app.request(BASE, { headers: AUTH })).json();
  assert.equal(info.installed_version, "0.8.2.2");
  assert.equal(info.installed_revision, 1);
  assert.equal((await install()).status, 400); // no downgrade/equal re-install
  assert.equal((await install({ ...INSTALL, tag: "0.8.3", preview: true })).status, 202);
});
