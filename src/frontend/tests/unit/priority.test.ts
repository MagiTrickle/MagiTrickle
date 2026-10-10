import assert from "node:assert/strict";
import { safeParse } from "valibot";

import {
  DEFAULT_GROUP_PRIORITY,
  DEFAULT_SUBSCRIPTION_PRIORITY,
  isValidPriority,
  PrioritySchema,
  withPriorityDefault,
} from "../../src/utils/priority";

Deno.test("priority accepts integer boundaries and rejects invalid values consistently", () => {
  for (const value of [1, 100, 300, 999]) {
    assert.equal(isValidPriority(value), true);
    assert.equal(safeParse(PrioritySchema, value).success, true);
  }
  for (const value of [
    undefined,
    null,
    false,
    "300",
    "",
    -1,
    0,
    1000,
    1001,
    1.5,
    NaN,
    Infinity,
    -Infinity,
  ]) {
    assert.equal(isValidPriority(value), false);
    assert.equal(safeParse(PrioritySchema, value).success, false);
  }
});

Deno.test("only omitted priorities receive the group or subscription default", () => {
  const legacy: { id: string; priority?: number } = { id: "aabbccdd" };
  assert.equal(withPriorityDefault(legacy, DEFAULT_GROUP_PRIORITY).priority, 300);
  assert.equal(withPriorityDefault(legacy, DEFAULT_SUBSCRIPTION_PRIORITY).priority, 100);
  assert.equal(legacy.priority, undefined);
  for (const priority of [1, 1000, 0, 1001, 1.5]) {
    assert.equal(
      withPriorityDefault({ ...legacy, priority }, DEFAULT_GROUP_PRIORITY).priority,
      priority,
    );
  }
});
