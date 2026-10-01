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
  rules: [rule],
};
const sub = { ...group, url: "https://example.com/list", interval: 86400, lastUpdate: 1700000000 };
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
