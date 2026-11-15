<script module lang="ts">
  type Measurement = { index: number; size: number; estimate: number };
  type Measurements = { width: number; rows: Map<string, Measurement> };

  // The collection, not a mounted panel, owns its measured geometry. Weak keys
  // release deleted/replaced drafts without retaining their rule arrays.
  const measurementsByCollection = new WeakMap<object, Measurements>();
</script>

<script lang="ts">
  import { onMount, tick, untrack, type Snippet } from "svelte";

  import { VirtualLayout } from "../../lib/virtual/layout";
  import { VirtualRowPool } from "../../lib/virtual/pool";
  import {
    requestVirtualFrame,
    subscribeVirtualViewport,
    virtualViewport,
    type VirtualViewport,
  } from "../../lib/virtual/viewport";

  type Props = {
    count: number;
    getKey: (index: number) => string;
    estimateSize?: number | ((index: number) => number);
    children: Snippet<[number]>;
    active?: boolean;
    revision?: unknown;
    recycle?: boolean;
    scrollElement?: HTMLElement;
    measurementKey?: object;
    onmeasure?: (measurements: readonly { index: number; size: number }[]) => void;
  };
  let {
    count,
    getKey,
    estimateSize = 40,
    children,
    active = true,
    revision,
    recycle = false,
    scrollElement,
    measurementKey,
    onmeasure,
  }: Props = $props();
  let host: HTMLDivElement;
  let layout = $state.raw(new VirtualLayout([]));
  let version = $state(0);
  let range = $state({ start: 0, end: 0 });
  let retained = $state<number[]>([]);
  const nodes = new Map<number, HTMLElement>();
  const slots = new Map<number, HTMLElement>();
  const pool = new VirtualRowPool();
  const localMeasurements: Measurements = { width: 0, rows: new Map() };
  let measurements = localMeasurements;
  let measured = measurements.rows;
  let observer: ResizeObserver | undefined;
  let ready = false;
  let lastWidth = 0;
  let localVelocity = 0;
  let localDirection = 0;

  function listViewport(viewport = virtualViewport()): VirtualViewport {
    return scrollElement
      ? {
          ...viewport,
          scrollTop: scrollElement.scrollTop,
          height: scrollElement.clientHeight,
          width: scrollElement.clientWidth,
          velocity: localVelocity,
          direction: localDirection,
        }
      : viewport;
  }
  function viewportTop() {
    return scrollElement ? scrollElement.getBoundingClientRect().top + scrollElement.clientTop : 0;
  }
  function scrollBy(delta: number) {
    if (scrollElement) scrollElement.scrollBy(0, delta);
    else window.scrollBy(0, delta);
  }

  function estimate(index: number) {
    return Math.max(1, typeof estimateSize === "number" ? estimateSize : estimateSize(index));
  }
  // Track every panel's geometry, including offscreen expansion and filtered
  // rule counts. Numeric rule estimates stay O(1) and do not read all rule keys.
  const estimates = $derived.by(() =>
    typeof estimateSize === "number"
      ? Math.max(1, estimateSize)
      : Array.from({ length: count }, (_, index) => estimate(index)),
  );
  function rebuild() {
    if (measurementKey) {
      let cached = measurementsByCollection.get(measurementKey);
      if (!cached) {
        cached = { width: 0, rows: new Map() };
        measurementsByCollection.set(measurementKey, cached);
      }
      measurements = cached;
    } else measurements = localMeasurements;
    const width = typeof window === "undefined" ? 0 : listViewport().width;
    const widthChanged = measurements.width !== width;
    const previous = widthChanged ? new Map() : measurements.rows;
    measured = new Map();
    measurements.rows = measured;
    measurements.width = width;
    layout =
      typeof estimates === "number"
        ? new VirtualLayout(count, estimates)
        : new VirtualLayout(estimates);
    // Only revisit rows that were actually measured, not the whole data set.
    // Invalidate only the row whose key or geometry changed. Opening the first
    // panel must not discard measurements for the rest of the document.
    for (const [key, cached] of previous) {
      if (
        cached.index < count &&
        getKey(cached.index) === key &&
        (typeof estimates === "number" || cached.estimate === estimate(cached.index))
      ) {
        measured.set(key, cached);
        layout.update(cached.index, cached.size);
      } else {
        // An estimate can change while a mounted row's actual box stays the
        // same (for example, calibrating the common panel size). Ask for a new
        // observation rather than waiting for a resize that will never occur.
        const node = nodes.get(cached.index);
        if (node) {
          observer?.unobserve(node);
          observer?.observe(node);
        }
      }
    }
    if (widthChanged) {
      // A max-width container may keep its actual boxes unchanged on a window
      // resize. Refill their invalidated measurements even without a DOM resize.
      for (const node of nodes.values()) {
        observer?.unobserve(node);
        observer?.observe(node);
      }
    }
    lastWidth = width;
    version++;
    if (ready) updateRange(listViewport(), true);
  }
  // An expanded panel can remount at the document end. Reserve its measured
  // rule height in the initial DOM: an empty first layout temporarily clamps
  // Safari's scroll offset before the normal reactive rebuild runs.
  untrack(rebuild);
  $effect(() => {
    count;
    revision;
    getKey;
    measurementKey;
    estimates;
    untrack(rebuild);
  });

  function setRange(start: number, end: number) {
    if (range.start !== start || range.end !== end) range = { start, end };
  }
  function updateRange(viewport: VirtualViewport, force = false) {
    if (!ready || !host || !active || !host.getClientRects().length) {
      setRange(0, 0);
      return;
    }
    const top = host.getBoundingClientRect().top - viewportTop();
    const base = Math.max(350, viewport.height * 0.6);
    // Cover a few delayed frames in Safari's asynchronous scrolling. Keep a
    // small buffer behind the user and pre-render in the direction of travel.
    const lead = Math.min(viewport.height, base + viewport.velocity * 80);
    const before = viewport.direction < 0 ? lead : base;
    const after = viewport.direction > 0 ? lead : base;
    const outside = top > viewport.height + after || top + layout.total < -before;
    if (outside) {
      setRange(0, 0);
    } else {
      const start = layout.indexAt(Math.max(0, -top - before));
      const end = Math.min(count, layout.indexAt(viewport.height - top + after) + 1);
      // Refill in batches rather than remounting rows at every row boundary.
      // The current range must still cover the viewport plus a safety margin.
      const neededStart = layout.indexAt(Math.max(0, -top - Math.min(before / 2, 200)));
      const neededEnd = Math.min(
        count,
        layout.indexAt(viewport.height - top + Math.min(after / 2, 200)) + 1,
      );
      if (force || range.start > neededStart || range.end < neededEnd || !range.end)
        setRange(start, end);
    }
    if (force || viewport.interaction) {
      const next: number[] = [];
      for (const [index, node] of nodes) {
        if (
          node.contains(document.activeElement) ||
          (viewport.dragSource && node.contains(viewport.dragSource)) ||
          node.querySelector('[aria-haspopup][aria-expanded="true"]')
        )
          next.push(index);
      }
      if (next.length !== retained.length || next.some((index, i) => index !== retained[i]))
        retained = next;
    }
  }
  function schedule() {
    requestVirtualFrame();
  }
  $effect(() => {
    const element = scrollElement;
    if (!element) return;
    let previousTop = element.scrollTop;
    let previousTime = performance.now();
    localVelocity = 0;
    localDirection = 0;
    const onScroll = () => {
      const now = performance.now();
      const distance = element.scrollTop - previousTop;
      localVelocity = Math.abs(distance) / Math.max(16, now - previousTime);
      localDirection = Math.sign(distance);
      previousTop = element.scrollTop;
      previousTime = now;
      schedule();
    };
    const resize = new ResizeObserver(schedule);
    resize.observe(element);
    element.addEventListener("scroll", onScroll, { passive: true });
    schedule();
    return () => {
      resize.disconnect();
      element.removeEventListener("scroll", onScroll);
    };
  });
  function canRecycle(slot: number) {
    const node = slots.get(slot);
    return (
      !node ||
      (!node.contains(document.activeElement) &&
        !node.querySelector('[aria-haspopup][aria-expanded="true"], .dragging'))
    );
  }
  let entries = $derived.by(() => {
    void version;
    const indices = new Set(retained.filter((i) => i < count));
    for (let i = range.start; i < Math.min(count, range.end); i++) indices.add(i);
    const all = [...indices].sort((a, b) => a - b);
    const keys = all.map(getKey);
    const assigned = recycle ? pool.assign(keys, canRecycle) : [];
    return all.map((index, position) => ({
      index,
      key: recycle ? assigned[position] : keys[position],
      slot: recycle ? assigned[position] : undefined,
      measurement: `${index}:${keys[position]}`,
      gap: layout.offset(index) - (position ? layout.offset(all[position - 1] + 1) : 0),
    }));
  });
  let tail = $derived.by(() => {
    void version;
    return Math.max(
      0,
      layout.total - (entries.length ? layout.offset(entries[entries.length - 1].index + 1) : 0),
    );
  });

  function measure(node: HTMLElement, identity: string) {
    let index = Number(identity.slice(0, identity.indexOf(":")));
    const slot =
      node.dataset.virtualSlot === undefined ? undefined : Number(node.dataset.virtualSlot);
    nodes.set(index, node);
    if (slot !== undefined) slots.set(slot, node);
    observer?.observe(node);
    return {
      update(next: string) {
        if (nodes.get(index) === node) nodes.delete(index);
        index = Number(next.slice(0, next.indexOf(":")));
        nodes.set(index, node);
        // A recycled row can have the same height under a new logical index.
        // Re-observe it so the new position receives a measurement regardless.
        observer?.unobserve(node);
        observer?.observe(node);
        schedule();
      },
      destroy() {
        observer?.unobserve(node);
        if (nodes.get(index) === node) nodes.delete(index);
        if (slot !== undefined && slots.get(slot) === node) slots.delete(slot);
      },
    };
  }
  const focusable =
    'input:not(:disabled), button:not(:disabled), select:not(:disabled), textarea:not(:disabled), a[href], [tabindex="0"]';
  async function moveFocus(event: KeyboardEvent) {
    if (
      event.key !== "Tab" ||
      event.defaultPrevented ||
      event.altKey ||
      event.ctrlKey ||
      event.metaKey
    )
      return;
    const item = [...nodes].find(([, node]) => node.contains(event.target as Node));
    if (!item) return;
    const [index, node] = item;
    const controls = [...node.querySelectorAll<HTMLElement>(focusable)].filter(
      (el) => el.getClientRects().length,
    );
    const boundary = event.shiftKey ? controls[0] : controls.at(-1);
    if (boundary !== event.target) return;
    const next = index + (event.shiftKey ? -1 : 1);
    if (next < 0 || next >= count || nodes.has(next)) return;
    event.preventDefault();
    await scrollToIndex(next);
    await tick();
    const target = nodes.get(next);
    const candidates = target
      ? [...target.querySelectorAll<HTMLElement>(focusable)].filter(
          (el) => el.getClientRects().length,
        )
      : [];
    (event.shiftKey ? candidates.at(-1) : candidates[0])?.focus();
  }

  onMount(() => {
    ready = true;
    observer = new ResizeObserver((records) => {
      let changed = false;
      const sizes: { index: number; size: number }[] = [];
      for (const record of records) {
        const node = record.target as HTMLElement;
        const index = Number(node.dataset.virtualIndex);
        if (index >= count || nodes.get(index) !== node) continue;
        const size = record.borderBoxSize?.[0]?.blockSize ?? record.contentRect.height;
        if (!size) continue;
        measured.set(getKey(index), { index, size, estimate: estimate(index) });
        sizes.push({ index, size });
        const delta = layout.update(index, size);
        if (!delta) continue;
        changed = true;
      }
      if (sizes.length) onmeasure?.(sizes);
      if (!changed) return;
      version++;
      // Native scroll anchoring preserves the visible row. Writing scrollTop
      // here aborts smooth scrolling and nested lists can compensate twice.
      requestVirtualFrame();
    });
    for (const node of nodes.values()) observer.observe(node);
    const hostObserver = new ResizeObserver(schedule);
    hostObserver.observe(host);
    const unsubscribe = subscribeVirtualViewport((viewport) => {
      const current = listViewport(viewport);
      if (lastWidth !== current.width) {
        rebuild();
      }
      updateRange(current);
    });
    host.addEventListener("keydown", moveFocus);
    updateRange(listViewport(), true);
    return () => {
      host.removeEventListener("keydown", moveFocus);
      ready = false;
      observer?.disconnect();
      hostObserver.disconnect();
      unsubscribe();
    };
  });
  $effect(() => {
    active;
    untrack(schedule);
  });

  export function refresh() {
    rebuild();
    schedule();
  }
  export async function scrollToIndex(
    index: number,
    align: "start" | "center" | "nearest" = "nearest",
  ) {
    if (index < 0 || index >= count) return;
    for (let pass = 0; pass < 4; pass++) {
      await tick();
      const viewport = listViewport();
      const inset = scrollElement ? 0 : 80;
      const top = host.getBoundingClientRect().top - viewportTop() + layout.offset(index);
      const height = layout.sizes[index];
      let delta = align === "center" ? top - (viewport.height - height) / 2 : top - inset;
      if (align === "nearest" && top >= inset && top + height <= viewport.height) delta = 0;
      if (delta) scrollBy(delta);
      updateRange(listViewport(), true);
      await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
    }
  }
</script>

<div class="virtual-list" bind:this={host} data-virtual-list role="list">
  {#each entries as entry (entry.key)}
    {#if entry.gap > 0}<div
        class="virtual-spacer"
        style:height={`${Math.floor(entry.gap)}px`}
        style:padding-bottom={`${entry.gap % 1}px`}
        aria-hidden="true"
      ></div>{/if}
    <div
      class="virtual-item"
      role="listitem"
      aria-posinset={entry.index + 1}
      aria-setsize={count}
      data-virtual-index={entry.index}
      data-virtual-slot={entry.slot}
      use:measure={entry.measurement}
    >
      {@render children(entry.index)}
    </div>
  {/each}
  {#if tail > 0}<div
      class="virtual-spacer"
      style:height={`${Math.floor(tail)}px`}
      style:padding-bottom={`${tail % 1}px`}
      aria-hidden="true"
    ></div>{/if}
</div>

<style>
  .virtual-list {
    min-width: 0;
  }
  .virtual-item {
    display: flow-root;
    min-width: 0;
  }
  .virtual-spacer {
    /* Huge CSS lengths lose fractional pixels to float precision. Keep the
       fraction in a small padding length, added by the browser's box layout. */
    box-sizing: content-box;
    pointer-events: none;
    overflow-anchor: none;
  }
</style>
