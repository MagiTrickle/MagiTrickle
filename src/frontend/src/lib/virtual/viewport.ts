export type VirtualViewport = {
  scrollTop: number;
  height: number;
  width: number;
  velocity: number;
  direction: number;
  interaction: boolean;
  dragSource: HTMLElement | undefined;
};

type Subscriber = (viewport: VirtualViewport) => void;
const subscribers = new Set<Subscriber>();
const reads = new Set<() => void>();
let frame = 0;
let interactionPending = false;
let previousTop = 0;
let previousTime = 0;
let dragSource: HTMLElement | undefined;
let listening = false;

export function virtualViewport(): VirtualViewport {
  return {
    scrollTop: window.scrollY,
    height: window.innerHeight,
    width: window.innerWidth,
    velocity: 0,
    direction: 0,
    interaction: true,
    dragSource,
  };
}

/** All nested lists read the viewport in one frame, before Svelte commits their
 * ranges. Interaction changes refresh pins; ordinary scrolls reuse them. */
export function requestVirtualFrame(interaction = false) {
  interactionPending ||= interaction;
  if (frame || (!subscribers.size && !reads.size)) return;
  frame = requestAnimationFrame((now) => {
    frame = 0;
    const pendingReads = [...reads];
    reads.clear();
    for (const read of pendingReads) read();
    const next = virtualViewport();
    const distance = next.scrollTop - previousTop;
    next.velocity = Math.abs(distance) / Math.max(16, now - previousTime);
    next.direction = Math.sign(distance);
    next.interaction = interactionPending;
    interactionPending = false;
    previousTop = next.scrollTop;
    previousTime = now;
    for (const subscriber of subscribers) subscriber(next);
  });
}

/** Batch overlay geometry with the lists, before their DOM range commits. */
export function requestViewportRead(read: () => void) {
  reads.add(read);
  requestVirtualFrame();
}

const onScroll = () => requestVirtualFrame();
const onInteraction = (event: Event) => {
  const target = event.composedPath().find((node): node is Element => node instanceof Element);
  // A modal editor cannot change pins in the document lists behind it. Reading
  // their viewport on every keystroke needlessly flushes a large editor layout.
  // Local virtual lists inside dialogs still need their focus/menu pins.
  if (target?.closest("[data-dialog-content]") && !target.closest("[data-virtual-list]")) return;
  requestVirtualFrame(true);
};
const onDragStart = (event: DragEvent) => {
  dragSource = event.target instanceof HTMLElement ? event.target : undefined;
  requestVirtualFrame(true);
};
const onDragEnd = () => {
  dragSource = undefined;
  requestVirtualFrame(true);
};

export function subscribeVirtualViewport(subscriber: Subscriber): () => void {
  subscribers.add(subscriber);
  if (!listening) {
    listening = true;
    previousTop = window.scrollY;
    previousTime = performance.now();
    window.addEventListener("scroll", onScroll, { passive: true });
    window.addEventListener("resize", onScroll);
    document.addEventListener("focusin", onInteraction);
    document.addEventListener("focusout", onInteraction);
    document.addEventListener("click", onInteraction);
    document.addEventListener("keydown", onInteraction);
    document.addEventListener("dragstart", onDragStart, true);
    document.addEventListener("dragend", onDragEnd, true);
  }
  return () => {
    subscribers.delete(subscriber);
    if (subscribers.size || !listening) return;
    listening = false;
    if (!reads.size) {
      cancelAnimationFrame(frame);
      frame = 0;
    }
    interactionPending = false;
    dragSource = undefined;
    window.removeEventListener("scroll", onScroll);
    window.removeEventListener("resize", onScroll);
    document.removeEventListener("focusin", onInteraction);
    document.removeEventListener("focusout", onInteraction);
    document.removeEventListener("click", onInteraction);
    document.removeEventListener("keydown", onInteraction);
    document.removeEventListener("dragstart", onDragStart, true);
    document.removeEventListener("dragend", onDragEnd, true);
  };
}
