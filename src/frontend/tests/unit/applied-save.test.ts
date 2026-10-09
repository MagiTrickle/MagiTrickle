import assert from "node:assert/strict";

import { appliedGroups, appliedSubscriptions } from "../../src/utils/applied-save.ts";
import { HttpError } from "../../src/utils/http-error.ts";

const rule = { id: "11223344", name: "", rule: "example.com", type: "domain", enable: false };
const group = {
  id: "aabbccdd",
  name: "Group",
  color: "#ffffff",
  interface: "all",
  enable: true,
  priority: 300,
  rules: [rule],
};
const sub = {
  ...group,
  priority: 100,
  url: "https://example.com/list",
  interval: 86400,
  lastUpdate: 1700000000,
};
const failure = (extra: object = {}, status = 500) =>
  new HttpError(
    status,
    JSON.stringify({
      code: "PERSISTENCE_FAILED",
      applied: true,
      groups: [group],
      subscriptions: [sub],
      ...extra,
    }),
  );

Deno.test(
  "explicit applied persistence failure exposes canonical group and subscription state",
  () => {
    assert.deepEqual(appliedGroups(failure()), [group]);
    assert.deepEqual(appliedSubscriptions(failure()), [sub]);
    assert.deepEqual(appliedGroups(failure({ groups: [] })), []);
    assert.deepEqual(appliedSubscriptions(failure({ subscriptions: [] })), []);
  },
);

Deno.test("generic errors, transport errors and rejected edits never advance a baseline", () => {
  for (const error of [
    new Error("network"),
    new HttpError(500, "disk"),
    new HttpError(500, "{broken"),
    failure({}, 409),
    failure({}, 502),
    failure({ applied: false }),
    failure({ code: "APPLY_FAILED" }),
    new HttpError(500, JSON.stringify({ error: "changes are active only in memory" })),
  ]) {
    assert.equal(appliedGroups(error), undefined);
    assert.equal(appliedSubscriptions(error), undefined);
  }
});

Deno.test("malformed/duplicate rule identities are not repaired into an invented baseline", () => {
  for (const rules of [
    null,
    [{}],
    [{ ...rule, id: "client-only" }],
    [rule, rule],
    [{ ...rule, enable: "false" }],
  ]) {
    assert.equal(appliedGroups(failure({ groups: [{ ...group, rules }] })), undefined);
    assert.equal(appliedSubscriptions(failure({ subscriptions: [{ ...sub, rules }] })), undefined);
  }
  assert.equal(appliedGroups(failure({ groups: [group, group] })), undefined);
  assert.equal(
    appliedSubscriptions(failure({ subscriptions: [{ ...sub, lastUpdate: null }] })),
    undefined,
  );
});

Deno.test("applied state preserves priorities and only defaults omitted legacy values", () => {
  const applied = failure({
    groups: [{ ...group, priority: 1000, profile: "shared" }],
    subscriptions: [{ ...sub, priority: 1, profile: "shared" }],
  });
  assert.equal(appliedGroups(applied)?.[0].priority, 1000);
  assert.equal(appliedSubscriptions(applied)?.[0].priority, 1);
  assert.equal(appliedGroups(applied)?.[0].profile, "shared");
  assert.equal(appliedSubscriptions(applied)?.[0].profile, "shared");
  const legacy = failure({
    groups: [{ ...group, priority: undefined }],
    subscriptions: [{ ...sub, priority: undefined }],
  });
  assert.equal(appliedGroups(legacy)?.[0].priority, 300);
  assert.equal(appliedSubscriptions(legacy)?.[0].priority, 100);
  for (const priority of [null, 0, -1, 1001, 1.5, "300"]) {
    const invalid = failure({
      groups: [{ ...group, priority }],
      subscriptions: [{ ...sub, priority }],
    });
    assert.equal(appliedGroups(invalid), undefined);
    assert.equal(appliedSubscriptions(invalid), undefined);
  }
});
