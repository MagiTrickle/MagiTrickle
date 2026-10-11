import assert from "node:assert/strict";
import { Hono } from "hono";

import { registerMockUpdateRoutes } from "../../dev/update-mock.ts";

const BASE = "/api/v1/system/update";
const INSTALL = { tag: "0.8.2.2", release_id: 123, preview: false };

function fixture() {
  let clock = 1000;
  const app = new Hono();
  registerMockUpdateRoutes(app, () => clock);
  return {
    app,
    advance: (ms: number) => {
      clock += ms;
    },
    install: (body: unknown = INSTALL, headers: Record<string, string> = {}) =>
      app.request(BASE + "/install", {
        method: "POST",
        headers: { "Content-Type": "application/json", ...headers },
        body: JSON.stringify(body),
      }),
  };
}

Deno.test(
  "mock update GET routes expose real response shapes without touching GitHub",
  async () => {
    const { app } = fixture();
    const status = await app.request(BASE + "/status");
    assert.equal(status.status, 200);
    assert.deepEqual(await status.json(), { stage: "idle" });
    assert.equal(status.headers.get("Cache-Control"), "no-store");

    const guest = await app.request(BASE);
    assert.equal(guest.status, 200);
    const info = await guest.json();
    assert.equal(info.installed_version, "0.8.2.1");
    assert.equal(info.installed_revision, 1);
    assert.equal(info.asset_suffix, "entware_aarch64-3.10_kn.ipk");
    assert.equal(info.can_install, true);
    assert.equal(info.reason, "");
    assert.equal(guest.headers.get("Cache-Control"), "no-store");

    const otherUser = await app.request(BASE, {
      headers: { Authorization: "Bearer non-root-user" },
    });
    assert.equal((await otherUser.json()).can_install, true);
  },
);

Deno.test("mock update POST requires same-origin JSON but no separate root token", async () => {
  const { app, install } = fixture();
  assert.equal(
    (
      await app.request(BASE + "/install", {
        method: "POST",
        headers: {},
        body: JSON.stringify(INSTALL),
      })
    ).status,
    400,
  );
  assert.equal(
    (
      await app.request(BASE + "/install", {
        method: "POST",
        headers: { "Content-Type": "application/json", "Sec-Fetch-Site": "cross-site" },
        body: JSON.stringify(INSTALL),
      })
    ).status,
    400,
  );

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
  assert.equal(
    (
      await app.request(BASE + "/install", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: "{bad json",
      })
    ).status,
    400,
  );
  assert.deepEqual(await (await app.request(BASE + "/status")).json(), { stage: "idle" });

  const withoutToken = fixture();
  assert.equal((await withoutToken.install(INSTALL)).status, 202);
  const withAnotherIdentity = fixture();
  assert.equal(
    (await withAnotherIdentity.install(INSTALL, {
      Authorization: "Bearer non-root-user",
    })).status,
    202,
  );
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
  advance(700 * 3);
  assert.equal((await (await app.request(BASE + "/status")).json()).stage, "installing");
  advance(700);
  const finished = await (await app.request(BASE + "/status")).json();
  assert.deepEqual(finished, { stage: "idle" });

  const info = await (await app.request(BASE)).json();
  assert.equal(info.installed_version, "0.8.2.2");
  assert.equal(info.installed_revision, 1);
  assert.equal((await install()).status, 400); // no downgrade/equal re-install
  assert.equal((await install({ ...INSTALL, tag: "0.8.3", preview: true })).status, 202);
});
