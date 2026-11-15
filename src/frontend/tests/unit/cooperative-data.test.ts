import assert from "node:assert/strict";

import { cloneCooperatively, CooperativeBudget } from "../../src/utils/cooperative-data";

async function clone<T>(value: T): Promise<T> {
  const budget = new CooperativeBudget(0);
  try {
    return await cloneCooperatively(value, budget);
  } finally {
    budget.close();
  }
}

Deno.test(
  "cooperative clone preserves cycles and shared nested values without sharing input",
  async () => {
    const shared = { text: "source", nested: [{ enabled: true }] };
    const root = { first: shared, second: shared, nested: [[shared]] };
    const source = { ...root, self: root };
    source.self = source;
    Object.freeze(source);
    const actual = await clone(source);
    assert.deepEqual(actual, source);
    assert.equal(actual.self, actual);
    assert.equal(actual.first, actual.second);
    assert.equal(actual.nested[0][0], actual.first);
    assert.notEqual(actual.first, shared);
    actual.first.nested[0].enabled = false;
    assert.equal(shared.nested[0].enabled, true);
  },
);

Deno.test("cooperative clone preserves null prototypes and special own properties", async () => {
  for (const nullPrototype of [false, true]) {
    const source: { __proto__: { marker: string }; constructor: { marker: string } } = JSON.parse(
      '{"__proto__":{"marker":"own value"},"constructor":{"marker":"own constructor"}}',
    );
    if (nullPrototype) Object.setPrototypeOf(source, null);
    Object.defineProperty(source, "hidden", { value: "not enumerable" });
    Object.freeze(source);
    const actual = await clone(source);
    assert.deepEqual(actual, source);
    assert.equal(Object.getPrototypeOf(actual), nullPrototype ? null : Object.prototype);
    assert.equal(Object.hasOwn(actual, "__proto__"), true);
    assert.equal(Object.hasOwn(actual, "constructor"), true);
    assert.equal(Object.hasOwn(actual, "hidden"), false);
    assert.equal(Object.hasOwn(Object.prototype, "marker"), false);
    assert.deepEqual(Object.getOwnPropertyDescriptor(actual, "__proto__"), {
      value: { marker: "own value" },
      enumerable: true,
      writable: true,
      configurable: true,
    });
    actual.__proto__.marker = "changed";
    actual.constructor.marker = "changed constructor";
    assert.equal(source.__proto__.marker, "own value");
    assert.equal(source.constructor.marker, "own constructor");
  }
});

Deno.test("concurrent cooperative clones keep their nested field values independent", async () => {
  const inputs = Array.from({ length: 4 }, (_, job) => ({
    job,
    entries: Array.from({ length: 1500 }, (_, index) => ({
      id: `${job}-${index}`,
      object: { nested: { value: `${job}:${index}` } },
      array: [{ job, index }, [job, index], null, undefined],
      last: index % 2 === 0,
    })),
    last: `end-${job}`,
  }));
  const results = await Promise.all(inputs.map((input) => clone(input)));
  for (let index = 0; index < inputs.length; index++) {
    assert.deepEqual(results[index], inputs[index]);
    assert.notEqual(results[index].entries[1499].object, inputs[index].entries[1499].object);
  }
});

Deno.test(
  "cooperative clone rejects cancellation between tasks without changing input",
  async () => {
    const input = Array.from({ length: 10000 }, (_, index) => ({
      id: String(index),
      nested: [index],
    }));
    const controller = new AbortController();
    const budget = new CooperativeBudget(0, controller.signal);
    let timerRan = false;
    const timer = setTimeout(() => {
      timerRan = true;
      controller.abort();
    }, 0);
    try {
      await assert.rejects(cloneCooperatively(input, budget), { name: "AbortError" });
      assert.equal(timerRan, true);
      assert.deepEqual(input[0], { id: "0", nested: [0] });
      assert.deepEqual(input[9999], { id: "9999", nested: [9999] });
    } finally {
      clearTimeout(timer);
      budget.close();
    }
    const cancelledBudget = new CooperativeBudget(0, controller.signal);
    try {
      await assert.rejects(cloneCooperatively(input, cancelledBudget), { name: "AbortError" });
    } finally {
      cancelledBudget.close();
    }
  },
);
