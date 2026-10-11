import assert from "node:assert/strict";
import { Hono } from "hono";

import { registerMockPolicyRoutes } from "../../dev/keenetic-policy-mock.ts";

const BASE = "/api/v1/keenetic";
function fixture() {
  const app = new Hono();
  registerMockPolicyRoutes(app);
  return {
    app,
    save: (body: unknown) =>
      app.request(`${BASE}/policy`, {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      }),
  };
}

Deno.test(
  "mock policies can be selected, saved in both modes, read back and disabled",
  async () => {
    const { app, save } = fixture();
    assert.deepEqual(await (await app.request(`${BASE}/policy`)).json(), {
      settings: { enabled: false, policy_id: "", mode: "include" },
      runtime: { state: "disabled" },
    });
    const { policies } = await (await app.request(`${BASE}/policies`)).json();
    assert.ok(policies.some((p: { available: boolean }) => !p.available));
    const policy_id = policies.find((p: { available: boolean }) => p.available).id;
    for (const mode of ["include", "exclude"]) {
      const settings = { enabled: true, policy_id, mode };
      const response = await save(settings);
      assert.equal(response.status, 200);
      const expected = { settings, runtime: { state: "ready" } };
      assert.deepEqual(await response.json(), expected);
      assert.deepEqual(await (await app.request(`${BASE}/policy`)).json(), expected);
    }
    const disabled = { enabled: false, policy_id: "deleted-policy", mode: "exclude" };
    assert.deepEqual(await (await save(disabled)).json(), {
      settings: disabled,
      runtime: { state: "disabled" },
    });
  },
);

Deno.test(
  "mock rejects invalid or unavailable settings without overwriting saved state",
  async () => {
    const { app, save } = fixture();
    const settings = { enabled: true, policy_id: "Policy1", mode: "include" };
    await save(settings);
    for (const body of [
      null,
      [],
      {},
      { ...settings, enabled: "true" },
      { ...settings, policy_id: "" },
      { ...settings, policy_id: "я".repeat(64) },
      { ...settings, policy_id: "bad\nID" },
      { ...settings, mode: "invalid" },
      { ...settings, extra: true },
    ])
      assert.equal((await save(body)).status, 400);
    const malformed = await app.request(`${BASE}/policy`, { method: "PUT", body: "{" });
    assert.equal(malformed.status, 400);
    for (const [policy_id, code] of [
      ["Policy3", "POLICY_UNAVAILABLE"],
      ["missing", "POLICY_NOT_FOUND"],
    ]) {
      const response = await save({ ...settings, policy_id });
      assert.equal(response.status, 409);
      assert.equal((await response.json()).code, code);
    }
    assert.deepEqual(await (await app.request(`${BASE}/policy`)).json(), {
      settings,
      runtime: { state: "ready" },
    });
  },
);
