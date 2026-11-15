/** Prefix sums with logarithmic updates and viewport lookup. DOM measurements
 * replace estimates; no full-list work is needed during scrolling. */
export class VirtualLayout {
  readonly sizes: Float64Array;
  private readonly tree: Float64Array;
  constructor(sizes: readonly number[] | number, estimate?: number | ((index: number) => number)) {
    const count = typeof sizes === "number" ? sizes : sizes.length;
    this.sizes = new Float64Array(count);
    this.tree = new Float64Array(count + 1);
    if (typeof sizes === "number" && typeof estimate === "number") {
      this.sizes.fill(estimate);
      for (let i = 1; i < this.tree.length; i++) this.tree[i] = estimate * (i & -i);
      return;
    }
    for (let i = 1; i < this.tree.length; i++) {
      const size =
        typeof sizes !== "number"
          ? sizes[i - 1]
          : typeof estimate === "function"
            ? estimate(i - 1)
            : (estimate ?? 40);
      this.sizes[i - 1] = size;
      this.tree[i] += size;
      const parent = i + (i & -i);
      if (parent < this.tree.length) this.tree[parent] += this.tree[i];
    }
  }
  offset(index: number): number {
    let sum = 0;
    for (let i = Math.min(index, this.sizes.length); i > 0; i -= i & -i) sum += this.tree[i];
    return sum;
  }
  get total(): number {
    return this.offset(this.sizes.length);
  }
  update(index: number, size: number): number {
    const delta = size - this.sizes[index];
    // Browser layout uses fractional CSS pixels. Dropping a small difference
    // per row makes the document height depend on how many rows are mounted.
    if (!Number.isFinite(delta) || delta === 0) return 0;
    this.sizes[index] = size;
    for (let i = index + 1; i < this.tree.length; i += i & -i) this.tree[i] += delta;
    return delta;
  }
  indexAt(offset: number): number {
    let index = 0;
    let sum = 0;
    let bit = 1;
    while (bit * 2 < this.tree.length) bit *= 2;
    for (; bit > 0; bit >>= 1) {
      const next = index + bit;
      if (next < this.tree.length && sum + this.tree[next] <= offset) {
        sum += this.tree[next];
        index = next;
      }
    }
    return Math.min(index, Math.max(0, this.sizes.length - 1));
  }
}
