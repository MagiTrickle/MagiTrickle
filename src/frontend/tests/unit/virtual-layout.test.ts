import { strict as assert } from "node:assert";

import { VirtualLayout } from "../../src/lib/virtual/layout";

Deno.test("virtual layout resolves measured offsets and exact boundaries", () => {
  const layout = new VirtualLayout([30, 50, 70]);
  assert.equal(layout.total, 150);
  assert.equal(layout.indexAt(0), 0);
  assert.equal(layout.indexAt(29), 0);
  assert.equal(layout.indexAt(30), 1);
  assert.equal(layout.indexAt(80), 2);
  assert.equal(layout.indexAt(999), 2);
  assert.equal(layout.update(1, 100), 50);
  assert.equal(layout.offset(2), 130);
  assert.equal(layout.total, 200);
  assert.equal(layout.indexAt(129), 1);
  assert.equal(layout.indexAt(130), 2);
  assert.equal(new VirtualLayout([]).total, 0);
});

Deno.test("virtual layout preserves prefix sums after measurements at 100k scale", () => {
  const sizes = Array.from({ length: 100000 }, () => 40);
  const layout = new VirtualLayout(sizes);
  for (let i = 0; i < sizes.length; i += 937) {
    sizes[i] = 60 + (i % 77);
    layout.update(i, sizes[i]);
  }
  let sum = 0;
  for (let i = 0; i < sizes.length; i++) {
    if (i % 911 === 0) {
      assert.equal(layout.offset(i), sum);
      assert.equal(layout.indexAt(sum), i);
      assert.equal(layout.indexAt(sum + sizes[i] - 0.01), i);
    }
    sum += sizes[i];
  }
  assert.equal(layout.total, sum);
});

Deno.test("uniform layout matches explicit sizes before and after sparse measurements", () => {
  const explicit = new VirtualLayout(Array.from({ length: 100000 }, () => 35));
  const uniform = new VirtualLayout(100000, 35);
  for (const index of [0, 1, 109, 50000, 99999, 100000]) {
    assert.equal(uniform.offset(index), explicit.offset(index));
    assert.equal(uniform.indexAt(index * 35 + 7), explicit.indexAt(index * 35 + 7));
  }
  for (const index of [0, 7, 50000, 99999]) {
    explicit.update(index, 71);
    uniform.update(index, 71);
  }
  assert.equal(uniform.total, explicit.total);
  for (const offset of [0, 35, 71, 1000, 1750000, 3499999])
    assert.equal(uniform.indexAt(offset), explicit.indexAt(offset));
});

Deno.test("fractional row measurements keep the same height when the mounted range changes", () => {
  const size = 37.96875;
  const layout = new VirtualLayout(100000, 38);
  for (let index = 0; index < 100000; index++) {
    assert.equal(layout.update(index, size), -0.03125);
  }
  assert.equal(layout.total, 100000 * size);

  for (const [start, end] of [
    [0, 50],
    [50000, 50080],
    [99965, 100000],
  ]) {
    const head = layout.offset(start);
    const mounted = (end - start) * size;
    const tail = layout.total - layout.offset(end);
    assert.equal(head + mounted + tail, layout.total);
  }
  assert.equal(layout.update(99999, size), 0);
});
