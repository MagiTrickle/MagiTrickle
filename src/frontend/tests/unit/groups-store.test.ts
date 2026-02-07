import assert from "node:assert";
import { describe, it } from "node:test";

import type { Group, Rule } from "../../src/types";
import {
  cloneGroupsWithNewIds,
  prependGroups,
  prependRules,
  restoreGroupRulesOrder,
  sortGroupRules,
  toConfigPayload,
} from "../../src/modules/groups/groups-data";

const makeRule = (id: string, name = id, pattern = `${id}.example.com`): Rule => ({
  id,
  enable: true,
  name,
  rule: pattern,
  type: "domain",
});

const makeGroup = (id: string, rules: Rule[] = []): Group => ({
  id,
  name: `group-${id}`,
  color: "#ffffff",
  interface: "all",
  enable: true,
  rules,
});

describe("Groups data API", () => {
  it("prepends groups in source order and updates open state", async () => {
    const target: Group[] = [];
    const openState: Record<string, boolean> = {};

    await prependGroups(target, openState, [makeGroup("g1"), makeGroup("g2")]);

    assert.deepStrictEqual(
      target.map((group) => group.id),
      ["g1", "g2"],
    );
    assert.strictEqual(openState.g1, true);
    assert.strictEqual(openState.g2, true);
  });

  it("prepends rules with stable order", async () => {
    const group = makeGroup("g1");
    const rules = [makeRule("r1"), makeRule("r2"), makeRule("r3")];

    await prependRules(group, rules);

    assert.deepStrictEqual(
      group.rules.map((rule) => rule.id),
      ["r1", "r2", "r3"],
    );
  });

  it("sorts and restores rule order", () => {
    const group = makeGroup("g1", [makeRule("r1", "beta"), makeRule("r2", "alpha")]);
    const initialOrder = group.rules.map((rule) => rule.id);

    sortGroupRules(group, "name", "asc");
    assert.strictEqual(group.rules[0].name, "alpha");

    const restored = restoreGroupRulesOrder(group, initialOrder);
    assert.strictEqual(restored, true);
    assert.deepStrictEqual(
      group.rules.map((rule) => rule.id),
      initialOrder,
    );
  });

  it("clones groups with fresh ids", async () => {
    const source = [makeGroup("g1", [makeRule("r1"), makeRule("r2")])];

    const cloned = await cloneGroupsWithNewIds(source);

    assert.strictEqual(cloned.length, 1);
    assert.notStrictEqual(cloned[0].id, "g1");
    assert.notStrictEqual(cloned[0].rules[0].id, "r1");
    assert.notStrictEqual(cloned[0].rules[1].id, "r2");
  });

  it("creates detached config payload", () => {
    const groups = [makeGroup("g1", [makeRule("r1")])];
    const payload = toConfigPayload(groups);

    payload.groups[0].name = "mutated";

    assert.strictEqual(groups[0].name, "group-g1");
  });
});
