import { strict as assert } from "node:assert";

import { installSvelteRunesMocks } from "../mocks/setup-svelte-runes";

// These checks cover size selection only. Browser tests cover real observers,
// reactive rendering and scroll anchoring.
installSvelteRunesMocks();
const { SizeEstimate } = await import("../../src/lib/virtual/size-estimate.svelte");

const rows = (...sizes: number[]) => sizes.map((size, index) => ({ index, size }));

Deno.test(
  "a shared estimate learns the common row size without absorbing a wrapped outlier",
  () => {
    const estimate = new SizeEstimate({ desktop: 35, mobile: 112 });
    estimate.learn(1280, rows(38, 38, 76, 38));
    assert.equal(estimate.forWidth(1280), 38);

    estimate.learn(1280, rows(76));
    assert.equal(estimate.forWidth(1280), 38);

    estimate.learn(1280, rows(76, 38, 76, 38));
    assert.equal(estimate.forWidth(1280), 38);
  },
);

Deno.test("desktop and mobile estimates survive switching widths independently", () => {
  const estimate = new SizeEstimate({ desktop: 35, mobile: 112 });
  estimate.learn(1280, rows(38, 38, 38));
  assert.equal(estimate.forWidth(390), 112);

  estimate.learn(390, rows(96, 96, 96));
  assert.equal(estimate.forWidth(700), 96);
  assert.equal(estimate.forWidth(701), 38);
  assert.equal(estimate.forWidth(1440), 38);
  assert.equal(estimate.forWidth(390), 96);
});

Deno.test("a new homogeneous resize or font batch replaces the retained estimate", () => {
  const estimate = new SizeEstimate({ desktop: 35, mobile: 112 });
  estimate.learn(1280, rows(38, 38, 38));
  assert.equal(estimate.forWidth(1024), 38);

  estimate.learn(1024, rows(42, 42, 42));
  assert.equal(estimate.forWidth(1024), 42);

  estimate.learn(1024, rows(44, 44, 44));
  assert.equal(estimate.forWidth(1024), 44);
});

Deno.test("empty and invalid measurements cannot replace a valid estimate", () => {
  const estimate = new SizeEstimate({ desktop: 35, mobile: 112 });
  estimate.learn(1280, []);
  estimate.learn(1280, rows(0, -1, NaN, Infinity));
  assert.equal(estimate.forWidth(1280), 35);

  estimate.learn(1280, rows(38, 0, 38, NaN, 38, Infinity));
  assert.equal(estimate.forWidth(1280), 38);
});

Deno.test("a repeated fractional box size replaces even a very close estimate exactly", () => {
  const estimate = new SizeEstimate({ desktop: 38, mobile: 112 });
  estimate.learn(1280, rows(37.96875, 37.96875, 37.96875, 75.9375));
  assert.equal(estimate.forWidth(1280), 37.96875);

  // Different fractional boxes are individual measurements, not evidence for
  // a new common size that should replace every offscreen row's estimate.
  estimate.learn(1280, rows(37.984375, 38, 38.015625));
  assert.equal(estimate.forWidth(1280), 37.96875);
});
