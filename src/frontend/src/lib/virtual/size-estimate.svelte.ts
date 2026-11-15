export type VirtualMeasurement = { index: number; size: number };

/** Share the usual row size across panels without making their content fixed
 * height. Individual measurements still belong to each VirtualList. */
export class SizeEstimate {
  #desktop = $state<number>();
  #mobile = $state<number>();

  constructor(private readonly fallback: { desktop: number; mobile: number }) {}

  forWidth(width: number): number {
    return width <= 700
      ? (this.#mobile ?? this.fallback.mobile)
      : (this.#desktop ?? this.fallback.desktop);
  }

  learn(width: number, measurements: readonly VirtualMeasurement[]) {
    // A single wrapped row must not change the estimate for the whole list.
    // ResizeObserver reports the visible batch again after width/font changes,
    // so a new common size can replace the old one without resetting to a guess.
    // Use an actual repeated box size, including its fractional pixels. A
    // tolerance/average can accumulate thousands of pixels over a large list.
    const counts = new Map<number, number>();
    let total = 0;
    let commonSize = 0;
    let commonCount = 0;
    for (const { size } of measurements) {
      if (!Number.isFinite(size) || size <= 0) continue;
      total++;
      const count = (counts.get(size) ?? 0) + 1;
      counts.set(size, count);
      if (count > commonCount) {
        commonSize = size;
        commonCount = count;
      }
    }
    if (commonCount < 2 || commonCount < total * 0.75) return;
    if (commonSize === this.forWidth(width)) return;
    if (width <= 700) this.#mobile = commonSize;
    else this.#desktop = commonSize;
  }
}
