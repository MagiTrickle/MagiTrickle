type ScrollbarState = {
  dragging: boolean;
  dragStartY: number;
  scrollStart: number;
  scrollRatio: number;
  hideTimer: number | null;
  rafId: number | null;
  targetElement: HTMLElement | null;
  boundsTop: number;
  boundsBottom: number;
  offsetTop: number;
  offsetBottom: number;
};

type OverlayScrollbarOptions = {
  targetSelector?: string;
};

const HIDE_DELAY = 900;
const MIN_THUMB_HEIGHT = 28;

const getScrollTop = () => document.documentElement.scrollTop || document.body.scrollTop || 0;

const setScrollTop = (value: number) => {
  document.documentElement.scrollTop = value;
  document.body.scrollTop = value;
};

const setVisible = (track: HTMLDivElement, state: ScrollbarState) => {
  track.classList.add("is-visible");
  if (state.hideTimer) window.clearTimeout(state.hideTimer);

  state.hideTimer = window.setTimeout(() => {
    track.classList.remove("is-visible");
  }, HIDE_DELAY);
};

const updateTargetOffsets = (rect: DOMRect, target: HTMLElement, state: ScrollbarState) => {
  const scrollHeight = document.documentElement.scrollHeight;
  const scrollTop = getScrollTop();

  const offsetTop = Math.max(rect.top + scrollTop, 0);
  const offsetBottom = Math.max(scrollHeight - (offsetTop + target.offsetHeight), 0);

  state.offsetTop = offsetTop;
  state.offsetBottom = offsetBottom;
};

const updateBounds = (track: HTMLDivElement, state: ScrollbarState) => {
  const target = state.targetElement ?? document.documentElement;
  const rect = target.getBoundingClientRect();
  const viewportHeight = window.innerHeight;
  const margin = 8;

  if (rect.height <= 0 || viewportHeight <= 0) {
    track.classList.add("is-hidden");
    return;
  }

  track.classList.remove("is-hidden");

  const top = Math.max(rect.top, margin);
  const bottom = Math.max(viewportHeight - rect.bottom, margin);

  state.boundsTop = top;
  state.boundsBottom = bottom;

  track.style.top = `${state.boundsTop}px`;
  track.style.bottom = `${state.boundsBottom}px`;

  updateTargetOffsets(rect, target as HTMLElement, state);
};

const updateThumb = (track: HTMLDivElement, thumb: HTMLDivElement, state: ScrollbarState) => {
  if (state.rafId) return;

  state.rafId = window.requestAnimationFrame(() => {
    state.rafId = null;

    const scrollHeight = document.documentElement.scrollHeight;
    const clientHeight = document.documentElement.clientHeight;
    const scrollTop = getScrollTop();
    const target = state.targetElement;

    if (target) {
      const estimatedDistance = scrollTop - state.offsetTop;
      if (Math.abs(estimatedDistance) < 120) {
        updateTargetOffsets(target.getBoundingClientRect(), target, state);
      }

      if (scrollTop < state.offsetTop + 40 || scrollTop < 200) {
        updateTargetOffsets(target.getBoundingClientRect(), target, state);
      }
    }

    const { offsetTop, offsetBottom } = state;

    const boundedScrollTop = Math.min(
      Math.max(scrollTop - offsetTop, 0),
      Math.max(scrollHeight - clientHeight - offsetBottom, 0),
    );

    if (scrollHeight <= clientHeight) {
      track.classList.add("is-hidden");
      return;
    }

    track.classList.remove("is-hidden");

    const trackHeight = track.clientHeight;
    const maxThumbHeight = trackHeight - 4;

    const thumbHeight = Math.max(
      Math.min((clientHeight / scrollHeight) * trackHeight, maxThumbHeight),
      MIN_THUMB_HEIGHT,
    );

    const maxThumbTop = trackHeight - thumbHeight;
    const scrollable = scrollHeight - clientHeight - offsetBottom - offsetTop;
    const thumbTop = scrollable > 0 ? (boundedScrollTop / scrollable) * maxThumbTop : 0;

    thumb.style.height = `${thumbHeight}px`;
    thumb.style.transform = `translateY(${thumbTop}px)`;
  });
};

export const initOverlayScrollbar = (options: OverlayScrollbarOptions = {}) => {
  document.documentElement.classList.add("has-overlay-scrollbar");
  document.body.classList.add("has-overlay-scrollbar");

  const track = document.createElement("div");
  track.className = "overlay-scrollbar";

  const thumb = document.createElement("div");
  thumb.className = "overlay-scrollbar__thumb";

  track.appendChild(thumb);
  document.body.appendChild(track);

  const state: ScrollbarState = {
    dragging: false,
    dragStartY: 0,
    scrollStart: 0,
    scrollRatio: 1,
    hideTimer: null,
    rafId: null,
    targetElement: null,
    boundsTop: 0,
    boundsBottom: 0,
    offsetTop: 0,
    offsetBottom: 0,
  };

  const resolveTarget = () => {
    if (!options.targetSelector) return;

    const target = document.querySelector<HTMLElement>(options.targetSelector);
    if (target && target !== state.targetElement) {
      state.targetElement = target;
      updateBounds(track, state);
    }
  };

  const update = () => {
    resolveTarget();
    updateThumb(track, thumb, state);
  };

  resolveTarget();
  updateBounds(track, state);
  update();

  window.addEventListener(
    "scroll",
    () => {
      setVisible(track, state);
      updateThumb(track, thumb, state);
    },
    { passive: true },
  );

  window.addEventListener("resize", () => {
    updateBounds(track, state);
    update();
  });

  track.addEventListener("mouseenter", () => setVisible(track, state));

  const resizeObserver = new ResizeObserver(() => {
    updateBounds(track, state);
    update();
  });
  resizeObserver.observe(document.documentElement);

  if (options.targetSelector) {
    const mutationObserver = new MutationObserver(() => {
      resolveTarget();
      updateBounds(track, state);
      update();
    });
    mutationObserver.observe(document.body, { childList: true, subtree: true });
  }

  thumb.addEventListener("pointerdown", (event) => {
    state.dragging = true;
    state.dragStartY = event.clientY;
    state.scrollStart = getScrollTop();

    const scrollHeight = document.documentElement.scrollHeight;
    const clientHeight = document.documentElement.clientHeight;

    const { offsetTop, offsetBottom } = state;

    const trackHeight = track.clientHeight;
    const thumbHeight = thumb.offsetHeight;

    const maxThumbTop = trackHeight - thumbHeight;
    const scrollable = scrollHeight - clientHeight - offsetBottom - offsetTop;

    state.scrollRatio = maxThumbTop > 0 && scrollable > 0 ? scrollable / maxThumbTop : 1;

    state.scrollStart = Math.min(Math.max(state.scrollStart - offsetTop, 0), Math.max(scrollable, 0));

    thumb.setPointerCapture(event.pointerId);
    setVisible(track, state);
  });

  thumb.addEventListener("pointermove", (event) => {
    if (!state.dragging) return;

    const delta = event.clientY - state.dragStartY;
    const { offsetTop } = state;
    setScrollTop(offsetTop + state.scrollStart + delta * state.scrollRatio);
  });

  const stopDragging = (event: PointerEvent) => {
    if (!state.dragging) return;

    state.dragging = false;
    try {
      thumb.releasePointerCapture(event.pointerId);
    } catch {
      // no-op
    }
  };

  thumb.addEventListener("pointerup", stopDragging);
  thumb.addEventListener("pointercancel", stopDragging);

  track.addEventListener("pointerdown", (event) => {
    if (event.target !== track) return;

    const trackRect = track.getBoundingClientRect();
    const offset = event.clientY - trackRect.top;

    const scrollHeight = document.documentElement.scrollHeight;
    const clientHeight = document.documentElement.clientHeight;

    const { offsetTop, offsetBottom } = state;

    const trackHeight = track.clientHeight;
    const thumbHeight = thumb.offsetHeight;

    const maxThumbTop = trackHeight - thumbHeight;
    const scrollable = scrollHeight - clientHeight - offsetBottom - offsetTop;

    if (maxThumbTop <= 0) return;

    const targetThumbTop = Math.min(Math.max(offset - thumbHeight / 2, 0), maxThumbTop);
    setScrollTop(offsetTop + (targetThumbTop / maxThumbTop) * scrollable);

    setVisible(track, state);
  });
};
