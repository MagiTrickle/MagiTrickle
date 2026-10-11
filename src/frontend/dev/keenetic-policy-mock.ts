import type { Hono } from "hono";

import type {
  Policy,
  PolicySettings,
  Status,
} from "../src/modules/settings/keenetic/state.svelte.ts";

const BASE = "/api/v1/keenetic";
const POLICIES: Policy[] = [
  { id: "Policy0", name: "Default", available: true },
  { id: "Policy1", name: "VPN", available: true },
  { id: "Policy2", name: "Guest", available: true },
  { id: "Policy3", name: "Offline VPN", available: false },
];

// In-memory dev simulation: no RCI calls or routing changes. Restart resets settings.
export function registerMockPolicyRoutes(app: Hono) {
  let settings: PolicySettings = { enabled: false, policy_id: "", mode: "include" };
  function status(): Status {
    return { settings, runtime: { state: settings.enabled ? "ready" : "disabled" } };
  }

  app.get(`${BASE}/policies`, (c) => c.json({ policies: POLICIES }));
  app.get(`${BASE}/policy`, (c) => c.json(status()));
  app.put(`${BASE}/policy`, async (c) => {
    let input: unknown;
    try {
      input = await c.req.json();
    } catch {
      return c.json({ error: "Invalid JSON" }, 400);
    }
    if (!input || typeof input !== "object" || Array.isArray(input))
      return c.json({ error: "Invalid policy settings" }, 400);
    const body = input as Record<string, unknown>;
    if (
      Object.keys(body).length !== 3 ||
      typeof body.enabled !== "boolean" ||
      typeof body.policy_id !== "string" ||
      new TextEncoder().encode(body.policy_id).length >= 128 ||
      /[\x00-\x1f\x7f]/.test(body.policy_id) ||
      (body.mode !== "include" && body.mode !== "exclude") ||
      (body.enabled && !body.policy_id)
    ) {
      return c.json(
        { error: "Expected enabled (boolean), policy_id (string) and mode (include or exclude)" },
        400,
      );
    }
    // Like the router, disabling never requires the selected policy to exist.
    if (body.enabled) {
      const selected = POLICIES.find((policy) => policy.id === body.policy_id);
      if (!selected?.available)
        return c.json(
          {
            code: selected ? "POLICY_UNAVAILABLE" : "POLICY_NOT_FOUND",
            error: "The selected Keenetic policy is missing or has no usable mark",
          },
          409,
        );
    }
    settings = { enabled: body.enabled, policy_id: body.policy_id, mode: body.mode };
    return c.json(status());
  });
}
