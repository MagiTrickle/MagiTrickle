import { strict as assert } from "node:assert";

import { buildGroupUpdate, snapshotGroupRules } from "../../src/modules/groups/group-payload";
import type { Group } from "../../src/types";

const large = (): Group => ({
  id: "aabbccdd",
  name: "group",
  color: "#ffffff",
  interface: "eth0",
  enable: true,
  priority: 300,
  rules: Array.from({ length: 50000 }, (_, i) => ({
    id: (i + 1).toString(16).padStart(8, "0"),
    name: "",
    rule: `item${i}.example.com`,
    type: "namespace",
    enable: true,
  })),
});
Deno.test("50k group metadata and one-rule edits do not resend rule arrays", () => {
  const group = large();
  const baseline = snapshotGroupRules([group]).get(group.id)!;
  group.name = "renamed";
  let payload = buildGroupUpdate(group, baseline);
  assert(!("rules" in payload));
  assert(JSON.stringify(payload).length < 512);
  group.rules[49999].name = "edited";
  payload = buildGroupUpdate(group, baseline);
  assert("ruleChanges" in payload);
  assert.equal(payload.ruleChanges.length, 1);
  assert.equal(payload.ruleChanges[0].previous.name, "");
  assert(JSON.stringify(payload).length < 1024);
});
Deno.test("new/imported/reordered groups preserve full desired rule order", () => {
  const group = large();
  const baseline = snapshotGroupRules([group]).get(group.id)!;
  assert("rules" in buildGroupUpdate(group, undefined));
  group.rules.reverse();
  const payload = buildGroupUpdate(group, baseline);
  assert("rules" in payload);
  assert.equal(payload.rules[0].id, baseline[49999].id);
});

Deno.test("group priority updates preserve profiles and allow switching to a direct route", () => {
  const group = large();
  const baseline = snapshotGroupRules([group]).get(group.id)!;
  group.priority = 900;
  group.profile = "shared";
  const metadata = buildGroupUpdate(group, baseline);
  assert.equal(metadata.priority, 900);
  assert.equal(metadata.profile, "shared");
  assert(!("rules" in metadata));
  assert.equal(buildGroupUpdate(group, undefined).priority, 900);
  group.profile = undefined;
  group.interface = "eth1";
  const direct = JSON.parse(JSON.stringify(buildGroupUpdate(group, baseline)));
  assert.equal(direct.priority, 900);
  assert.equal(direct.interface, "eth1");
  assert(!("profile" in direct));
  assert.deepEqual(direct.ruleChanges, []);
});
