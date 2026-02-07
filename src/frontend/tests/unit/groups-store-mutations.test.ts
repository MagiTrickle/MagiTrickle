import assert from "node:assert";
import { describe, it } from "node:test";

import { installSvelteRunesMocks } from "../mocks/setup-svelte-runes";

installSvelteRunesMocks();

type Rule = {
  id: string;
  enable: boolean;
  name: string;
  rule: string;
  type: string;
};

type Group = {
  id: string;
  name: string;
  color: string;
  interface: string;
  enable: boolean;
  rules: Rule[];
};

const createMemoryStorage = () => {
  const store = new Map<string, string>();

  return {
    getItem(key: string) {
      return store.has(key) ? store.get(key)! : null;
    },
    setItem(key: string, value: string) {
      store.set(key, String(value));
    },
    removeItem(key: string) {
      store.delete(key);
    },
    clear() {
      store.clear();
    },
    key(index: number) {
      return Array.from(store.keys())[index] ?? null;
    },
    get length() {
      return store.size;
    },
  };
};

const patchGlobal = (key: string, value: unknown) => {
  const original = Object.getOwnPropertyDescriptor(globalThis, key);
  Object.defineProperty(globalThis, key, {
    value,
    configurable: true,
    writable: true,
  });

  return () => {
    if (original) {
      Object.defineProperty(globalThis, key, original);
      return;
    }
    delete (globalThis as any)[key];
  };
};

const restoreLocalStorage = patchGlobal("localStorage", createMemoryStorage());
type ProcessLike = { env?: unknown };
const processObj = (globalThis as unknown as { process?: ProcessLike }).process;
const originalProcessEnvDescriptor = processObj
  ? Object.getOwnPropertyDescriptor(processObj, "env")
  : undefined;
if (processObj) {
  Object.defineProperty(processObj, "env", {
    value: { NODE_ENV: "test" },
    configurable: true,
    writable: true,
  });
}
const { GroupsStore } = await import("../../src/modules/groups/groups.svelte");
if (processObj) {
  if (originalProcessEnvDescriptor) {
    Object.defineProperty(processObj, "env", originalProcessEnvDescriptor);
  } else {
    delete processObj.env;
  }
}
restoreLocalStorage();

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

const createStore = (groups: Group[] = []) => {
  const store = new GroupsStore();
  store.data.splice(0, store.data.length, ...structuredClone(groups));
  store.dataRevision = 0;
  store.valid_rules = true;
  store.open_state = {};
  for (const group of store.data) {
    store.open_state[group.id] = false;
  }
  return store;
};

const groupIds = (store: InstanceType<typeof GroupsStore>) => store.data.map((group) => group.id);
const ruleIds = (store: InstanceType<typeof GroupsStore>, groupIndex: number) =>
  store.data[groupIndex]?.rules.map((rule) => rule.id) ?? [];

const withPatchedGlobal = async <T>(key: string, value: unknown, action: () => T | Promise<T>) => {
  const restore = patchGlobal(key, value);
  try {
    return await action();
  } finally {
    restore();
  }
};

describe("GroupsStore mutations and validation", () => {
  it("checkRulesValidityState toggles valid_rules based on invalid inputs", async () => {
    const store = createStore();

    await withPatchedGlobal("document", { querySelector: () => ({}) }, () => {
      store.checkRulesValidityState();
      assert.strictEqual(store.valid_rules, false);
    });

    await withPatchedGlobal("document", { querySelector: () => null }, () => {
      store.checkRulesValidityState();
      assert.strictEqual(store.valid_rules, true);
    });
  });

  it("markDataRevision increments revision counter", () => {
    const store = createStore();
    store.markDataRevision();
    store.markDataRevision();
    assert.strictEqual(store.dataRevision, 2);
  });

  it("addGroup prepends group, adds default rule and opens it", async () => {
    const store = createStore([makeGroup("g1", [makeRule("r1")])]);

    await withPatchedGlobal("document", { querySelector: () => null }, async () => {
      await store.addGroup();
    });

    assert.strictEqual(store.data.length, 2);
    assert.strictEqual(store.data[0].rules.length, 1);
    assert.strictEqual(store.open_state[store.data[0].id], true);
    assert.deepStrictEqual(groupIds(store).slice(1), ["g1"]);
    assert.strictEqual(store.dataRevision, 2);
  });

  it("deleteGroup respects confirm and removes open_state entry", async () => {
    const store = createStore([makeGroup("g1"), makeGroup("g2")]);
    store.open_state.g1 = true;
    store.open_state.g2 = true;

    await withPatchedGlobal(
      "confirm",
      () => false,
      () => {
        store.deleteGroup(0);
        assert.deepStrictEqual(groupIds(store), ["g1", "g2"]);
        assert.strictEqual(store.dataRevision, 0);
      },
    );

    await withPatchedGlobal(
      "confirm",
      () => true,
      () => {
        store.deleteGroup(0);
        assert.deepStrictEqual(groupIds(store), ["g2"]);
        assert.strictEqual("g1" in store.open_state, false);
        assert.strictEqual(store.dataRevision, 1);
      },
    );
  });

  it("changeGroupIndex supports before/after and clamps target indexes", () => {
    const store = createStore([makeGroup("a"), makeGroup("b"), makeGroup("c")]);

    store.changeGroupIndex(0, 2, "before");
    assert.deepStrictEqual(groupIds(store), ["b", "a", "c"]);

    store.changeGroupIndex(0, 99, "after");
    assert.deepStrictEqual(groupIds(store), ["a", "c", "b"]);

    store.changeGroupIndex(0, 0, "before");
    assert.deepStrictEqual(groupIds(store), ["a", "c", "b"]);
    assert.strictEqual(store.dataRevision, 2);
  });

  it("handleGroupSlotDrop applies group reorder semantics", () => {
    const store = createStore([makeGroup("a"), makeGroup("b"), makeGroup("c")]);

    store.handleGroupSlotDrop(
      { group_id: "a", group_index: 0, name: "group-a", color: "#111111", count: 0 },
      { group_index: 2, insert: "before" },
    );

    assert.deepStrictEqual(groupIds(store), ["b", "a", "c"]);
    assert.strictEqual(store.dataRevision, 1);
  });

  it("addRuleToGroup prepends and marks rules invalid for empty fields", async () => {
    const store = createStore([makeGroup("g1", [makeRule("r1")])]);

    await store.addRuleToGroup(0, makeRule("r2", "", ""), false);
    assert.deepStrictEqual(ruleIds(store, 0), ["r2", "r1"]);
    assert.strictEqual(store.valid_rules, false);
    assert.strictEqual(store.dataRevision, 1);

    await store.addRuleToGroup(99, makeRule("r3"), false);
    assert.deepStrictEqual(ruleIds(store, 0), ["r2", "r1"]);
    assert.strictEqual(store.dataRevision, 1);
  });

  it("deleteRuleFromGroup removes by index and keeps out-of-range behavior", () => {
    const store = createStore([makeGroup("g1", [makeRule("r1"), makeRule("r2"), makeRule("r3")])]);

    store.deleteRuleFromGroup(0, 1);
    assert.deepStrictEqual(ruleIds(store, 0), ["r1", "r3"]);
    assert.strictEqual(store.dataRevision, 1);

    store.deleteRuleFromGroup(0, 99);
    assert.deepStrictEqual(ruleIds(store, 0), ["r1", "r3"]);
    assert.strictEqual(store.dataRevision, 2);
  });

  it("changeRuleIndex reorders rules inside and across groups", () => {
    const store = createStore([
      makeGroup("g1", [makeRule("a"), makeRule("b"), makeRule("c")]),
      makeGroup("g2", [makeRule("d")]),
    ]);

    store.changeRuleIndex(0, 0, 0, 2, "c", "before");
    assert.deepStrictEqual(ruleIds(store, 0), ["b", "a", "c"]);

    store.changeRuleIndex(0, 0, 1, 0, "d", "after");
    assert.deepStrictEqual(ruleIds(store, 0), ["a", "c"]);
    assert.deepStrictEqual(ruleIds(store, 1), ["d", "b"]);
    assert.strictEqual(store.dataRevision, 2);
  });

  it("sortGroupRules and restoreGroupRulesOrder update order with return flags", () => {
    const store = createStore([makeGroup("g1", [makeRule("r1", "beta"), makeRule("r2", "alpha")])]);
    const initial = ruleIds(store, 0);

    const sorted = store.sortGroupRules(0, "name", "asc");
    assert.strictEqual(sorted, true);
    assert.deepStrictEqual(ruleIds(store, 0), ["r2", "r1"]);

    const restored = store.restoreGroupRulesOrder(0, initial);
    assert.strictEqual(restored, true);
    assert.deepStrictEqual(ruleIds(store, 0), initial);

    const failed = store.restoreGroupRulesOrder(0, ["missing"]);
    assert.strictEqual(failed, false);
  });

  it("addGroups prepends in source order and overwriteGroups resets state", async () => {
    const store = createStore([makeGroup("base")]);

    await store.addGroups([]);
    assert.deepStrictEqual(groupIds(store), ["base"]);
    assert.strictEqual(store.dataRevision, 0);

    await store.addGroups([makeGroup("g1"), makeGroup("g2")]);
    assert.deepStrictEqual(groupIds(store), ["g1", "g2", "base"]);
    assert.strictEqual(store.open_state.g1, true);
    assert.strictEqual(store.open_state.g2, true);

    store.renderGroupsLimit = 5;
    store.finishedGroupsCount = 9;
    await store.overwriteGroups([makeGroup("new1")]);
    assert.deepStrictEqual(groupIds(store), ["new1"]);
    assert.strictEqual("g1" in store.open_state, false);
    assert.strictEqual(store.open_state.new1, true);
    assert.strictEqual(store.renderGroupsLimit, 1);
    assert.strictEqual(store.finishedGroupsCount, 0);
  });

  it("addRulesToGroup prepends rules and no-ops for empty or invalid target", async () => {
    const store = createStore([makeGroup("g1", [makeRule("r1")])]);

    await store.addRulesToGroup(0, []);
    assert.deepStrictEqual(ruleIds(store, 0), ["r1"]);
    assert.strictEqual(store.dataRevision, 0);

    await store.addRulesToGroup(0, [makeRule("r2"), makeRule("r3")]);
    assert.deepStrictEqual(ruleIds(store, 0), ["r2", "r3", "r1"]);
    assert.strictEqual(store.dataRevision, 1);

    await store.addRulesToGroup(99, [makeRule("r4")]);
    assert.deepStrictEqual(ruleIds(store, 0), ["r2", "r3", "r1"]);
    assert.strictEqual(store.dataRevision, 1);
  });

  it("toConfigPayload returns detached snapshot of current groups", () => {
    const store = createStore([makeGroup("g1", [makeRule("r1")])]);

    const payload = store.toConfigPayload();
    payload.groups[0].name = "mutated";
    payload.groups[0].rules[0].name = "mutated-rule";

    assert.strictEqual(store.data[0].name, "group-g1");
    assert.strictEqual(store.data[0].rules[0].name, "r1");
  });
});
