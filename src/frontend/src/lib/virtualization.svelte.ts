import { getContext, onDestroy, setContext } from "svelte";

type OffsetMetrics = {
  offsets: number[];
  total: number;
};

type VariableListOptions<T> = {
  items: () => T[];
  getKey: (item: T, index: number) => string | null | undefined;
  estimateSize: number | ((item: T, index: number) => number);
  gap?: number;
  overscan?: number;
  getScrollParent?: (node: HTMLElement) => HTMLElement | Window;
};

type FixedListOptions = {
  total: () => number;
  enabled?: () => boolean;
  overscan?: number;
  estimateSize?: number;
  rowSelector?: string;
  scrollTop: () => number;
  scrollContainerTop: () => number;
  viewportHeight: () => number;
};

type ScrollTracker = {
  list: (node: HTMLElement) => { destroy: () => void };
  applyScrollDelta: (delta: number) => void;
  get scrollTop(): number;
  get viewportHeight(): number;
  get listTop(): number;
  get scrollContainerTop(): number;
  get scrollContainer(): HTMLElement | Window;
  get lastScrollAt(): number;
};

type ScrollMetrics = {
  get scrollTop(): number;
  get viewportHeight(): number;
  get scrollContainerTop(): number;
};

type VariableEntry<T> = {
  item: T;
  index: number;
  key: string;
  style: string;
};

const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));
const scrollContextKey = Symbol("virtual-scroll-context");

export function provideScrollContext(metrics: ScrollMetrics) {
  setContext(scrollContextKey, metrics);
  return metrics;
}

export function useScrollContext() {
  return getContext<ScrollMetrics | null>(scrollContextKey) ?? null;
}

let layoutTick = $state(0);
let layoutRaf = 0;
let layoutObserversReady = false;
let layoutResizeObserver: ResizeObserver | null = null;

function bumpLayoutTick() {
  if (layoutRaf) return;
  if (typeof window === "undefined") return;
  layoutRaf = window.requestAnimationFrame(() => {
    layoutRaf = 0;
    layoutTick += 1;
  });
}

function ensureLayoutObservers() {
  if (layoutObserversReady || typeof window === "undefined") return;
  if (!document.body || !document.documentElement) return;
  layoutObserversReady = true;
  window.addEventListener("resize", bumpLayoutTick);
  window.addEventListener("orientationchange", bumpLayoutTick);
  if (typeof ResizeObserver === "undefined") return;
  layoutResizeObserver = new ResizeObserver(() => bumpLayoutTick());
  layoutResizeObserver.observe(document.body);
  layoutResizeObserver.observe(document.documentElement);
}

function getLayoutTick() {
  ensureLayoutObservers();
  return layoutTick;
}

function buildOffsets(
  total: number,
  getSize: (index: number) => number,
  gap = 0,
): OffsetMetrics {
  const offsets: number[] = new Array(total);
  let offset = 0;
  for (let index = 0; index < total; index++) {
    offsets[index] = offset;
    offset += getSize(index) + gap;
  }
  if (offset > 0) {
    offset -= gap;
  }
  return { offsets, total: offset };
}

function findStartIndex(offsets: number[], target: number) {
  let low = 0;
  let high = offsets.length - 1;
  while (low <= high) {
    const mid = (low + high) >> 1;
    if (offsets[mid] <= target) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  return Math.max(0, low - 1);
}

function computeWindowFromOffsets(
  offsets: number[],
  viewportTop: number,
  viewportHeight: number,
  overscan: number,
) {
  const total = offsets.length;
  if (!total) {
    return { start: 0, end: 0 };
  }
  const startIndex = findStartIndex(offsets, viewportTop);
  const endIndex = findStartIndex(offsets, viewportTop + viewportHeight);
  const start = clamp(startIndex - overscan, 0, total);
  const end = clamp(endIndex + overscan + 1, start, total);
  return { start, end };
}

function computeWindowForFixedItems(
  itemSize: number,
  total: number,
  viewportTop: number,
  viewportHeight: number,
  overscan: number,
) {
  if (!total || itemSize <= 0) {
    return { start: 0, end: 0 };
  }
  const viewBottom = viewportTop + viewportHeight;
  const start = clamp(Math.floor(viewportTop / itemSize) - overscan, 0, total);
  const end = clamp(Math.ceil(viewBottom / itemSize) + overscan, start, total);
  return { start, end };
}

function getScrollDeltaForResize(
  itemTop: number,
  itemHeight: number,
  nextHeight: number,
  viewportTop: number,
) {
  const delta = nextHeight - itemHeight;
  if (!delta) return 0;
  const itemBottom = itemTop + itemHeight;
  if (itemBottom <= viewportTop) {
    return delta;
  }
  return 0;
}

function isScrollable(element: HTMLElement) {
  const style = getComputedStyle(element);
  const overflowY = style.overflowY;
  const overflow = style.overflow;
  const canScroll =
    /(auto|scroll|overlay)/.test(overflowY) || /(auto|scroll|overlay)/.test(overflow);
  return canScroll && element.scrollHeight > element.clientHeight;
}

function findScrollParent(node: HTMLElement | null): HTMLElement | Window {
  if (!node || typeof window === "undefined") return window;
  let current = node.parentElement;
  while (current) {
    if (
      current !== document.body &&
      current !== document.documentElement &&
      isScrollable(current)
    ) {
      return current;
    }
    current = current.parentElement;
  }
  return window;
}

function createScrollTracker(getScrollParent?: (node: HTMLElement) => HTMLElement | Window): ScrollTracker {
  let listEl = $state<HTMLElement | null>(null);
  let scrollTop = $state(0);
  let viewportHeight = $state(0);
  let listTop = $state(0);
  let scrollContainerTop = $state(0);
  let scrollContainer: HTMLElement | Window = window;
  let scrollListenersAttached = false;
  let scrollRaf = 0;
  let lastScrollAt = 0;
  let listResizeObserver: ResizeObserver | null = null;
  let containerResizeObserver: ResizeObserver | null = null;
  const scrollListenerOptions: AddEventListenerOptions = { passive: true };

  function updateScrollPosition() {
    const isWindow = scrollContainer === window;
    const nextScrollTop = isWindow
      ? window.pageYOffset || document.documentElement.scrollTop || document.body.scrollTop || 0
      : (scrollContainer as HTMLElement).scrollTop;
    const nextViewportHeight = isWindow
      ? window.innerHeight
      : (scrollContainer as HTMLElement).clientHeight;
    scrollTop = nextScrollTop;
    viewportHeight = nextViewportHeight;
    lastScrollAt = Date.now();
  }

  function updateLayoutMetrics() {
    if (!listEl) return;
    updateScrollPosition();
    const isWindow = scrollContainer === window;
    if (isWindow) {
      scrollContainerTop = 0;
      const rect = listEl.getBoundingClientRect();
      listTop = rect.top + scrollTop;
      return;
    }

    const containerRect = (scrollContainer as HTMLElement).getBoundingClientRect();
    scrollContainerTop = containerRect.top;
    const rect = listEl.getBoundingClientRect();
    listTop = rect.top - containerRect.top + scrollTop;
  }

  function scheduleScrollUpdate() {
    if (scrollRaf) return;
    if (typeof window === "undefined") return;
    scrollRaf = window.requestAnimationFrame(() => {
      scrollRaf = 0;
      updateScrollPosition();
    });
  }

  function removeScrollListeners(target: HTMLElement | Window) {
    target.removeEventListener("scroll", scheduleScrollUpdate, scrollListenerOptions);
    if (target === window) {
      window.removeEventListener("resize", scheduleScrollUpdate);
    }
  }

  function addScrollListeners(target: HTMLElement | Window) {
    target.addEventListener("scroll", scheduleScrollUpdate, scrollListenerOptions);
    if (target === window) {
      window.addEventListener("resize", scheduleScrollUpdate);
    }
  }

  function applyScrollDelta(delta: number) {
    if (!delta) return;
    if (scrollContainer === window) {
      window.scrollTo({ top: scrollTop + delta });
    } else {
      (scrollContainer as HTMLElement).scrollTop += delta;
    }
    scrollTop = scrollTop + delta;
    scheduleScrollUpdate();
  }

  function observeListSize() {
    if (!listEl || typeof ResizeObserver === "undefined") return;
    listResizeObserver?.disconnect();
    listResizeObserver = new ResizeObserver(() => {
      updateLayoutMetrics();
    });
    listResizeObserver.observe(listEl);
  }

  function observeContainerSize() {
    if (scrollContainer === window || typeof ResizeObserver === "undefined") {
      containerResizeObserver?.disconnect();
      containerResizeObserver = null;
      return;
    }
    const container = scrollContainer as HTMLElement;
    containerResizeObserver?.disconnect();
    containerResizeObserver = new ResizeObserver(() => {
      updateLayoutMetrics();
    });
    containerResizeObserver.observe(container);
  }

  $effect(() => {
    if (!listEl) return;
    if (typeof window === "undefined") return;
    const nextContainer = getScrollParent ? getScrollParent(listEl) : findScrollParent(listEl);
    if (!scrollListenersAttached || scrollContainer !== nextContainer) {
      if (scrollListenersAttached) {
        removeScrollListeners(scrollContainer);
      }
      scrollContainer = nextContainer;
      addScrollListeners(scrollContainer);
      scrollListenersAttached = true;
      observeContainerSize();
    }
    updateLayoutMetrics();
    scheduleScrollUpdate();
  });

  $effect(() => {
    getLayoutTick();
    if (!listEl) return;
    updateLayoutMetrics();
  });

  onDestroy(() => {
    if (typeof window !== "undefined") {
      if (scrollListenersAttached) {
        removeScrollListeners(scrollContainer);
        scrollListenersAttached = false;
      }
      if (scrollRaf) {
        window.cancelAnimationFrame(scrollRaf);
        scrollRaf = 0;
      }
    }
    listResizeObserver?.disconnect();
    listResizeObserver = null;
    containerResizeObserver?.disconnect();
    containerResizeObserver = null;
  });

  return {
    list(node: HTMLElement) {
      listEl = node;
      if (typeof window !== "undefined") {
        updateLayoutMetrics();
      }
      observeListSize();
      scheduleScrollUpdate();
      return {
        destroy() {
          if (listEl === node) {
            listEl = null;
            if (scrollListenersAttached) {
              removeScrollListeners(scrollContainer);
              scrollListenersAttached = false;
            }
          }
          listResizeObserver?.disconnect();
          listResizeObserver = null;
        },
      };
    },
    applyScrollDelta,
    get scrollTop() {
      return scrollTop;
    },
    get viewportHeight() {
      return viewportHeight;
    },
    get listTop() {
      return listTop;
    },
    get scrollContainerTop() {
      return scrollContainerTop;
    },
    get scrollContainer() {
      return scrollContainer;
    },
    get lastScrollAt() {
      return lastScrollAt;
    },
  };
}

export function createVariableVirtualList<T>(options: VariableListOptions<T>) {
  const overscan = options.overscan ?? 0;
  const gap = options.gap ?? 0;

  const estimateSizeOption = options.estimateSize;
  const estimateSize: (item: T, index: number) => number =
    typeof estimateSizeOption === "function"
      ? estimateSizeOption
      : () => estimateSizeOption;

  const scroll = createScrollTracker(options.getScrollParent);

  const sizes = new Map<string, number>();
  let sizeRevision = $state(0);
  let sizeRaf = 0;
  let sizeFlushScheduled = false;
  const pendingSizes = new Map<string, number>();
  const IMMEDIATE_SIZE_DELTA = 120;
  let offsets = $state<number[]>([]);
  let total = $state(0);
  let start = $state(0);
  let end = $state(0);
  let indexByKey = new Map<string, number>();

  function setItemSize(key: string, nextSize: number) {
    if (!nextSize) return;
    const prevSize = Number(pendingSizes.get(key) ?? sizes.get(key) ?? 0);
    if (prevSize === nextSize) return;
    const delta = Math.abs(nextSize - prevSize);
    if (delta >= IMMEDIATE_SIZE_DELTA) {
      pendingSizes.delete(key);
      if (!shouldSkipScrollAdjust()) {
        maybeAdjustScrollForResize(key, prevSize, nextSize);
      }
      sizes.set(key, nextSize);
      sizeRevision = sizeRevision + 1;
      return;
    }
    pendingSizes.set(key, nextSize);
    scheduleSizeFlush();
  }

  function measure(node: HTMLElement, params: { key?: string }) {
    if (typeof ResizeObserver === "undefined") return;
    let key = params.key;
    const update = () => {
      if (!key) return;
      const height = Math.ceil(node.getBoundingClientRect().height);
      if (height > 0) {
        setItemSize(key, height);
      }
    };
    const observer = new ResizeObserver(update);
    observer.observe(node);
    update();
    return {
      update(next: { key?: string }) {
        key = next.key;
        update();
      },
      destroy() {
        observer.disconnect();
      },
    };
  }

  function maybeAdjustScrollForResize(key: string, prevSize: number, nextSize: number) {
    if (!prevSize || !offsets.length) return;
    if (scroll.scrollContainer === window) return;
    const index = indexByKey.get(key);
    if (index === undefined) return;
    const itemTop = offsets[index] ?? 0;
    const viewTop = Math.max(0, scroll.scrollTop - scroll.listTop);
    const delta = getScrollDeltaForResize(itemTop, prevSize, nextSize, viewTop);
    scroll.applyScrollDelta(delta);
  }

  function shouldSkipScrollAdjust() {
    const sinceScroll = Date.now() - scroll.lastScrollAt;
    return sinceScroll < 120;
  }

  function flushSizes() {
    if (!pendingSizes.size) return;
    let changed = false;
    for (const [key, nextSize] of pendingSizes.entries()) {
      pendingSizes.delete(key);
      const prevSize = Number(sizes.get(key) ?? 0);
      if (prevSize === nextSize) continue;
      if (!shouldSkipScrollAdjust()) {
        maybeAdjustScrollForResize(key, prevSize, nextSize);
      }
      sizes.set(key, nextSize);
      changed = true;
    }
    if (changed) {
      sizeRevision = sizeRevision + 1;
    }
  }

  function scheduleSizeFlush() {
    if (sizeFlushScheduled) return;
    sizeFlushScheduled = true;
    if (typeof window === "undefined") {
      sizeFlushScheduled = false;
      flushSizes();
      return;
    }
    if (typeof queueMicrotask === "function") {
      queueMicrotask(() => {
        sizeFlushScheduled = false;
        flushSizes();
      });
      return;
    }
    sizeRaf = window.requestAnimationFrame(() => {
      sizeRaf = 0;
      sizeFlushScheduled = false;
      flushSizes();
    });
  }

  $effect(() => {
    const items = options.items();
    const nextIndexByKey = new Map<string, number>();
    for (let index = 0; index < items.length; index++) {
      const key = options.getKey(items[index], index);
      if (key) {
        nextIndexByKey.set(key, index);
      }
    }
    for (const key of sizes.keys()) {
      if (!nextIndexByKey.has(key)) {
        sizes.delete(key);
      }
    }
    indexByKey = nextIndexByKey;
  });

  $effect(() => {
    const items = options.items();
    sizeRevision;
    const metrics = buildOffsets(
      items.length,
      (index) => {
        const item = items[index];
        const key = options.getKey(item, index);
        if (key && sizes.has(key)) {
          return sizes.get(key) ?? 0;
        }
        return estimateSize(item, index);
      },
      gap,
    );
    offsets = metrics.offsets;
    total = metrics.total;
  });

  $effect(() => {
    const items = options.items();
    const viewTop = Math.max(0, scroll.scrollTop - scroll.listTop);
    const viewportHeight =
      scroll.viewportHeight || (typeof window !== "undefined" ? window.innerHeight : 0);
    const { start: nextStart, end: nextEnd } = computeWindowFromOffsets(
      offsets,
      viewTop,
      viewportHeight,
      overscan,
    );
    start = nextStart;
    end = Math.min(nextEnd, items.length);
  });

  let entries = $state<VariableEntry<T>[]>([]);

  $effect(() => {
    const items = options.items();
    const count = Math.max(0, end - start);
    const result: VariableEntry<T>[] = new Array(count);
    let out = 0;
    for (let index = start; index < end; index++) {
      const item = items[index];
      if (item === undefined) continue;
      const key = options.getKey(item, index) ?? `idx-${index}`;
      const top = offsets[index] ?? 0;
      result[out] = { item, index, key, style: `top: ${top}px` };
      out += 1;
    }
    if (out !== result.length) {
      result.length = out;
    }
    entries = result;
  });

  onDestroy(() => {
    if (typeof window !== "undefined" && sizeRaf) {
      window.cancelAnimationFrame(sizeRaf);
      sizeRaf = 0;
    }
    sizeFlushScheduled = false;
  });

  return {
    list: scroll.list,
    item: measure,
    get entries() {
      return entries;
    },
    get totalHeight() {
      return total;
    },
    get scrollTop() {
      return scroll.scrollTop;
    },
    get viewportHeight() {
      return scroll.viewportHeight;
    },
    get scrollContainerTop() {
      return scroll.scrollContainerTop;
    },
  };
}

type GroupListOptions<T> = Omit<VariableListOptions<T>, "estimateSize" | "gap" | "overscan"> & {
  estimateSize?: VariableListOptions<T>["estimateSize"];
  gap?: number;
  overscan?: number;
};

export function createGroupsVirtualList<T>(options: GroupListOptions<T>) {
  const list = createVariableVirtualList({
    ...options,
    estimateSize: options.estimateSize ?? 140,
    gap: options.gap ?? 16,
    overscan: options.overscan ?? 3,
  });
  provideScrollContext(list);
  return list;
}

export function createFixedVirtualList(options: FixedListOptions) {
  const overscan = options.overscan ?? 0;
  const enabled = options.enabled ?? (() => true);
  const estimateSize = options.estimateSize ?? 1;

  let container = $state<HTMLElement | null>(null);
  let start = $state(0);
  let end = $state(0);
  let rowHeight = $state(estimateSize);
  let measureRaf = 0;
  let updateRaf = 0;
  let resizeObserver: ResizeObserver | null = null;

  function updateWindow() {
    if (!enabled() || !container) {
      start = 0;
      end = 0;
      return;
    }
    const total = options.total();
    if (!total) {
      start = 0;
      end = 0;
      return;
    }
    const rect = container.getBoundingClientRect();
    const scrollTop = options.scrollTop();
    const listTop = rect.top - options.scrollContainerTop() + scrollTop;
    const viewTop = scrollTop - listTop;
    const viewportHeight =
      options.viewportHeight() ||
      (typeof window !== "undefined" ? window.innerHeight : 0);
    const { start: nextStart, end: nextEnd } = computeWindowForFixedItems(
      rowHeight,
      total,
      viewTop,
      viewportHeight,
      overscan,
    );
    start = nextStart;
    end = nextEnd;
  }

  function measureRow() {
    if (!container || !options.rowSelector) return;
    const row = container.querySelector<HTMLElement>(options.rowSelector);
    if (!row) return;
    const height = Math.ceil(row.getBoundingClientRect().height);
    if (height > 0 && height !== rowHeight) {
      rowHeight = height;
    }
  }

  function scheduleMeasure() {
    if (measureRaf) return;
    if (typeof window === "undefined") {
      measureRow();
      return;
    }
    measureRaf = window.requestAnimationFrame(() => {
      measureRaf = 0;
      measureRow();
    });
  }

  function scheduleUpdate() {
    if (updateRaf) return;
    if (typeof window === "undefined") {
      updateWindow();
      return;
    }
    updateRaf = window.requestAnimationFrame(() => {
      updateRaf = 0;
      updateWindow();
    });
  }

  $effect(() => {
    getLayoutTick();
    enabled();
    options.total();
    options.scrollTop();
    options.scrollContainerTop();
    options.viewportHeight();
    rowHeight;
    container;
    updateWindow();
  });

  $effect(() => {
    if (!container || typeof ResizeObserver === "undefined") return;
    resizeObserver?.disconnect();
    resizeObserver = new ResizeObserver(() => {
      scheduleUpdate();
      scheduleMeasure();
    });
    resizeObserver.observe(container);
    return () => {
      resizeObserver?.disconnect();
      resizeObserver = null;
    };
  });

  onDestroy(() => {
    if (typeof window !== "undefined") {
      if (measureRaf) {
        window.cancelAnimationFrame(measureRaf);
        measureRaf = 0;
      }
      if (updateRaf) {
        window.cancelAnimationFrame(updateRaf);
        updateRaf = 0;
      }
    }
    resizeObserver?.disconnect();
    resizeObserver = null;
  });

  return {
    list(node: HTMLElement) {
      container = node;
      updateWindow();
      measureRow();
      scheduleUpdate();
      scheduleMeasure();
      return {
        destroy() {
          if (container === node) {
            container = null;
          }
        },
      };
    },
    get start() {
      return start;
    },
    get end() {
      return end;
    },
    get topSpacer() {
      return Math.max(0, start * rowHeight);
    },
    get bottomSpacer() {
      return Math.max(0, (options.total() - end) * rowHeight);
    },
  };
}
