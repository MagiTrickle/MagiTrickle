import { strict as assert } from "node:assert";

import {
  buildSubscriptionUpdate,
  snapshotRules,
} from "../../src/modules/subscriptions/subscription-payload";
import type { Subscription } from "../../src/types";

const large = (): Subscription => ({
  id: "aabbccdd",
  name: "large",
  url: "https://example.com/list",
  interface: "eth0",
  enable: true,
  priority: 100,
  interval: 86400,
  lastUpdate: 1700000000,
  rules: Array.from({ length: 50000 }, (_, i) => ({
    id: (i + 1).toString(16).padStart(8, "0"),
    rule: `10.${i >> 8}.${i & 255}.0/24`,
    type: "subnet",
    enable: true,
  })),
});

Deno.test("50k unchanged rules produce a small metadata-only update", () => {
  const sub = large();
  const payload = buildSubscriptionUpdate(sub, snapshotRules(sub.rules));
  assert(!("rules" in payload));
  assert(!("lastUpdate" in payload));
  assert.equal(payload.ruleChanges.length, 0);
  assert(JSON.stringify(payload).length < 512);
});

Deno.test("one edit carries only canonical ID and optimistic baseline", () => {
  const sub = large();
  const baseline = snapshotRules(sub.rules);
  sub.rules[49999].enable = false;
  const payload = buildSubscriptionUpdate(sub, baseline);
  assert.equal(payload.ruleChanges.length, 1);
  assert.deepEqual(payload.ruleChanges[0], {
    id: sub.rules[49999].id,
    rule: sub.rules[49999].rule,
    previousType: "subnet",
    previousEnable: true,
    type: "subnet",
    enable: false,
  });
  assert(JSON.stringify(payload).length < 1024);
  assert.equal(baseline[49999].enable, true);
});

Deno.test("missing baseline, changed text and duplicate IDs fail closed", () => {
  const sub = large();
  const baseline = snapshotRules(sub.rules);
  assert.throws(() => buildSubscriptionUpdate(sub, undefined), /reload/);
  sub.rules[0].rule = "changed.example";
  assert.throws(() => buildSubscriptionUpdate(sub, baseline), /reload/);
  sub.rules[0].rule = baseline[0].rule;
  sub.rules[1] = { ...sub.rules[0] };
  assert.throws(() => buildSubscriptionUpdate(sub, baseline), /reload/);
});

Deno.test(
  "subscription priority updates preserve profiles and allow switching to a direct route",
  () => {
    const sub = large();
    const baseline = snapshotRules(sub.rules);
    sub.priority = 600;
    sub.profile = "shared";
    const payload = buildSubscriptionUpdate(sub, baseline);
    assert.equal(payload.priority, 600);
    assert.equal(payload.profile, "shared");
    assert.equal(payload.ruleChanges.length, 0);
    assert(!("rules" in payload));
    sub.profile = undefined;
    sub.interface = "eth1";
    const direct = buildSubscriptionUpdate(sub, baseline);
    assert.equal(direct.priority, 600);
    assert.equal(direct.interface, "eth1");
    assert(!("profile" in direct));
    assert.deepEqual(direct.ruleChanges, []);
  },
);
