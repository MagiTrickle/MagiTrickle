import { Hono } from "hono";
import { cors } from "hono/cors";
import { serveStatic } from "hono/deno";
import { logger } from "hono/logger";
import { streamSSE } from "hono/streaming";

import { cleanProfiles, profileError } from "../src/modules/settings/profiles-data.ts";
import type { RuleChange } from "../src/modules/subscriptions/subscription-payload.ts";
import type { Group, Interfaces, Profile, Subscription } from "../src/types.ts";
import {
  DEFAULT_GROUP_PRIORITY,
  DEFAULT_SUBSCRIPTION_PRIORITY,
  isValidPriority,
  withPriorityDefault,
} from "../src/utils/priority.ts";

const API_BASE = "/api/v1";

const INTERFACES: Interfaces = {
  interfaces: [
    { id: "nwg0" },
    { id: "longinterf" },
    { id: "eth1" },
    { id: "wg0", name: "WireGuard Interface" },
  ],
};

const DATA = JSON.parse(Deno.readTextFileSync("./dev/groups.json"));
DATA.groups = DATA.groups.map((group: Group) => withPriorityDefault(group, DEFAULT_GROUP_PRIORITY));
const SUBSCRIPTIONS: Subscription[] = [
  {
    id: "a1b2c3d4",
    name: "Bad Bad Services",
    interface: "blackhole",
    enable: true,
    priority: DEFAULT_SUBSCRIPTION_PRIORITY,
    url: "https://services.should.be.blocked.com",
    lastUpdate: Math.floor(Date.now() / 1000),
    interval: 86400,
    rules: [
      { enable: true, id: "11223344", rule: "google.com", type: "domain" },
      { enable: true, id: "55667788", rule: "facebook.com", type: "domain" },
    ],
  },
];

function randomLogLine() {
  function randomIndex(array: any[]) {
    return array[Math.round(Math.random() * (array.length - 1))];
  }
  function randomIP(): string {
    return `${Math.round(Math.random() * 255)}.${Math.round(Math.random() * 255)}.${Math.round(
      Math.random() * 255,
    )}.${Math.round(Math.random() * 255)}`;
  }

  const level = randomIndex(["trace", "debug", "info", "warn", "error", "fatal", "panic"]);

  return {
    time: new Date().toISOString(),
    level: level,
    error: ["error", "fatal", "panic"].includes(level) ? "random error" : undefined,
    message: ["error", "fatal", "panic"].includes(level)
      ? "error message"
      : `group: ${randomIndex(DATA.groups).name}, ip: ${randomIP()} > int: ${randomIndex(
          INTERFACES.interfaces.map((item) => item.id),
        )}`,
  };
}

let sse_id = 0;

const PORT = 6969;
const STATIC_TOKEN = "magitrickle_mock_token_2026";
const AUTH_ENABLED = true;

const app = new Hono();

app.use(logger());
app.use(cors());

// Auth Middleware
app.use(`${API_BASE}/*`, async (c, next) => {
  if (c.req.path === `${API_BASE}/auth` || !AUTH_ENABLED) {
    await next();
    return;
  }

  const authHeader = c.req.header("Authorization");
  if (authHeader !== `Bearer ${STATIC_TOKEN}` && authHeader !== `Bearer disabled`) {
    return c.json({ error: "Unauthorized" }, 401);
  }

  await next();
});

app.get(`${API_BASE}/auth`, async (c) => {
  return c.json({ enabled: AUTH_ENABLED }, 200);
});

app.post(`${API_BASE}/auth`, async (c) => {
  const body = await c.req.json();
  if (body.login === "root" && body.password === "keenetic") {
    return c.json({ token: STATIC_TOKEN });
  }
  return c.json({ error: "Invalid credentials" }, 403);
});

let PROFILES: Profile[] = [];
function profileResponse() {
  return {
    profiles: PROFILES.map((profile) => ({
      ...profile,
      usage: {
        groups: DATA.groups.filter((g: Group) => g.profile === profile.id).length,
        subscriptions: SUBSCRIPTIONS.filter((s) => s.profile === profile.id).length,
      },
    })),
  };
}
function applyProfile<T extends { interface: string; profile?: string }>(item: T): T {
  const profile = PROFILES.find((p) => p.id === item.profile);
  return profile ? { ...item, interface: profile.interfaces[0] } : item;
}
app.get(`${API_BASE}/profiles`, (c) => c.json(profileResponse()));
app.put(`${API_BASE}/profiles`, async (c) => {
  const body = await c.req.json();
  if (!Array.isArray(body.profiles)) return c.json({ error: "profiles must be an array" }, 400);
  let next: Profile[];
  try {
    next = cleanProfiles(body.profiles);
  } catch {
    return c.json({ error: "Invalid profiles" }, 400);
  }
  const error = profileError(next);
  if (error) return c.json({ error }, 400);
  const ids = new Set(next.map((p) => p.id));
  if ([...DATA.groups, ...SUBSCRIPTIONS].some((item) => item.profile && !ids.has(item.profile))) {
    return c.json({ error: "Reassign groups and subscriptions before deleting this profile" }, 409);
  }
  PROFILES = next;
  DATA.groups = DATA.groups.map(applyProfile);
  for (let i = 0; i < SUBSCRIPTIONS.length; i++) SUBSCRIPTIONS[i] = applyProfile(SUBSCRIPTIONS[i]);
  return c.json(profileResponse());
});

app.get(`${API_BASE}/groups`, (c) => c.json(DATA));
app.put(`${API_BASE}/groups`, async (c) => {
  const body = await c.req.json();
  const replacement: Group[] = [];
  for (const incoming of body.groups) {
    if (incoming.priority !== undefined && !isValidPriority(incoming.priority))
      return c.json({ error: "priority must be an integer from 1 to 1000" }, 400);
    const previous: Group | undefined = DATA.groups.find(
      (group: Group) => group.id === incoming.id,
    );
    if (incoming.ruleChanges && !previous)
      return c.json({ error: "group changed; reload before saving" }, 409);
    const rules = structuredClone(incoming.rules ?? previous?.rules ?? []);
    const byId = new Map<string, Group["rules"][number]>(
      rules.map((rule: Group["rules"][number]) => [rule.id, rule]),
    );
    for (const change of incoming.ruleChanges ?? []) {
      const rule = byId.get(change.id);
      if (
        !rule ||
        ["name", "type", "rule", "enable"].some(
          (key) => rule[key as keyof typeof rule] !== change.previous?.[key],
        )
      )
        return c.json({ error: "group changed; reload before saving" }, 409);
      Object.assign(rule, {
        name: change.name,
        type: change.type,
        rule: change.rule,
        enable: change.enable,
      });
    }
    const { ruleChanges: _, ...metadata } = incoming;
    if (metadata.profile && !PROFILES.some((p) => p.id === metadata.profile)) {
      return c.json({ error: "Missing routing profile" }, 400);
    }
    replacement.push(
      withPriorityDefault(
        applyProfile({ ...previous, ...metadata, profile: metadata.profile, rules }),
        DEFAULT_GROUP_PRIORITY,
      ),
    );
  }
  DATA.groups = replacement;
  return c.json({ groups: DATA.groups });
});

app.get(`${API_BASE}/subscriptions`, (c) => c.json({ subscriptions: SUBSCRIPTIONS }));

function mockRules() {
  return Array.from({ length: 50 }, (_, i) => ({
    enable: true,
    id: (i + 1).toString(16).padStart(8, "0"),
    rule: `mock.rule.${i}.com`,
    type: "namespace",
  }));
}

app.put(`${API_BASE}/subscriptions`, async (c) => {
  const body = await c.req.json();
  const replacement: Subscription[] = [];
  for (const incoming of body.subscriptions) {
    if (incoming.priority !== undefined && !isValidPriority(incoming.priority))
      return c.json({ error: "priority must be an integer from 1 to 1000" }, 400);
    const previous = SUBSCRIPTIONS.find((s) => s.id === incoming.id);
    if (incoming.ruleChanges && !previous)
      return c.json({ error: "subscription changed; reload before saving" }, 409);
    const rules = structuredClone(incoming.rules ?? previous?.rules ?? []);
    const byId = new Map<string, Subscription["rules"][number]>(
      rules.map((r: Subscription["rules"][number]) => [r.id, r]),
    );
    for (const change of (incoming.ruleChanges ?? []) as RuleChange[]) {
      const rule = byId.get(change.id);
      if (
        !rule ||
        rule.rule !== change.rule ||
        rule.type !== change.previousType ||
        rule.enable !== change.previousEnable
      ) {
        return c.json({ error: "subscription rules changed; reload before saving" }, 409);
      }
      rule.type = change.type;
      rule.enable = change.enable;
    }
    const { ruleChanges: _, ...metadata } = incoming;
    if (metadata.profile && !PROFILES.some((p) => p.id === metadata.profile)) {
      return c.json({ error: "Missing routing profile" }, 400);
    }
    replacement.push(
      withPriorityDefault(
        applyProfile({ ...previous, ...metadata, profile: metadata.profile, rules }),
        DEFAULT_SUBSCRIPTION_PRIORITY,
      ),
    );
  }
  SUBSCRIPTIONS.splice(0, SUBSCRIPTIONS.length, ...replacement);
  return c.json({ status: "ok" });
});

app.post(`${API_BASE}/subscriptions`, async (c) => {
  const body = applyProfile(await c.req.json());
  if (body.profile && !PROFILES.some((p) => p.id === body.profile))
    return c.json({ error: "Missing routing profile" }, 400);
  if (body.priority !== undefined && !isValidPriority(body.priority))
    return c.json({ error: "priority must be an integer from 1 to 1000" }, 400);
  body.priority ??= DEFAULT_SUBSCRIPTION_PRIORITY;
  if (c.req.query("fetch") === "true") {
    const subscription = {
      ...body,
      id: crypto.randomUUID().replaceAll("-", "").slice(0, 8),
      rules: mockRules(),
      lastUpdate: Math.floor(Date.now() / 1000),
    };
    SUBSCRIPTIONS.unshift(subscription);
    return c.json({ subscription });
  }
  SUBSCRIPTIONS.unshift(body);
  return c.json({ status: "ok" });
});

app.get(`${API_BASE}/subscriptions/rules`, (c) => {
  const rules = mockRules();
  if (c.req.query("summary") === "true") {
    return c.json({ count: rules.length, types: { namespace: rules.length } });
  }
  return c.json({ rules });
});

app.post(`${API_BASE}/subscriptions/:id/sync`, async (c) => {
  const id = c.req.param("id");
  const body = await c.req.json();
  console.debug("updated subscription, syncing rules", id, body);
  const index = SUBSCRIPTIONS.findIndex((s) => s.id === id);

  if (index !== -1) {
    const count = Math.floor(Math.random() * 70) + 5;
    const rules = Array.from({ length: count }).map(() => ({
      enable: true,
      id: Math.random().toString(16).substring(2, 10),
      rule: `mock.rule.${Math.random().toString(36).substring(7)}.com`,
      type: Math.random() < 0.5 ? "namespace" : "domain",
    }));

    const updatedSub = {
      ...SUBSCRIPTIONS[index],
      ...body,
      rules: rules,
      lastUpdate: Math.floor(Date.now() / 1000),
    };

    SUBSCRIPTIONS[index] = updatedSub;
    return c.json({ rules: updatedSub.rules, lastUpdate: updatedSub.lastUpdate });
  }
  return c.json({ error: "Subscription not found" }, 404);
});

app.delete(`${API_BASE}/subscriptions/:id`, async (c) => {
  const id = c.req.param("id");
  console.debug("deleting subscription", id);
  const index = SUBSCRIPTIONS.findIndex((s) => s.id === id);
  if (index !== -1) {
    SUBSCRIPTIONS.splice(index, 1);
    return c.json({ status: "ok" });
  }
  return c.json({ error: "Subscription not found" }, 404);
});

app.get(`${API_BASE}/system/interfaces`, (c) => c.json(INTERFACES));
app.get(`${API_BASE}/logs`, async (c) => {
  return streamSSE(c, async (stream) => {
    while (true) {
      await stream.writeSSE({
        data: JSON.stringify(randomLogLine()),
        id: String(sse_id++),
      });
      await stream.sleep(Math.round(Math.random() * 1000));
    }
  });
});

app.get("*", serveStatic({ root: "./dist" }));

Deno.serve(
  { port: PORT, onListen: () => console.log(`running mock server on port ${PORT}...`) },
  app.fetch,
);
