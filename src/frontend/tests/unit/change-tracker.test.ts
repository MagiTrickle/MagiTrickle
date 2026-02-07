import assert from "node:assert";
import { describe, it } from "node:test";

import { installSvelteRunesMocks } from "../mocks/setup-svelte-runes";

installSvelteRunesMocks();

const { ChangeTracker } = await import("../../src/utils/change-tracker.svelte");

type Item = {
  id: string;
  name: string;
  tags: string[];
};

describe("ChangeTracker", () => {
  it("tracks object field changes and revert", () => {
    const tracker = new ChangeTracker<Item[]>([{ id: "a", name: "alpha", tags: [] }]);

    assert.strictEqual(tracker.isDirty, false);

    tracker.data[0].name = "beta";
    assert.strictEqual(tracker.isDirty, true);

    tracker.data[0].name = "alpha";
    assert.strictEqual(tracker.isDirty, false);
  });

  it("tracks array structure changes", () => {
    const tracker = new ChangeTracker<Item[]>([
      { id: "a", name: "alpha", tags: [] },
      { id: "b", name: "beta", tags: [] },
    ]);

    tracker.data.splice(0, 1);
    assert.strictEqual(tracker.isDirty, true);

    tracker.data.splice(0, 0, { id: "a", name: "alpha", tags: [] });
    assert.strictEqual(tracker.isDirty, false);

    tracker.data.reverse();
    assert.strictEqual(tracker.isDirty, true);
  });

  it("reset clears dirty state", () => {
    const tracker = new ChangeTracker<Item[]>([{ id: "a", name: "alpha", tags: [] }]);

    tracker.data[0].name = "beta";
    assert.strictEqual(tracker.isDirty, true);

    const snapshot = (globalThis as any).$state.snapshot(tracker.data);
    tracker.reset(snapshot);
    assert.strictEqual(tracker.isDirty, false);
  });
});
