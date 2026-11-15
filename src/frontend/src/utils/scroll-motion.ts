let motion: { finish: () => void; cancel: () => void } | undefined;

/** Let the browser animate each leg; retain an explicit edge request while
 * virtual panels supply their final geometry. Never retarget mid-animation. */
export function scrollToDocumentBoundary(edge: "start" | "end") {
  motion?.finish();
  const destination = () =>
    edge === "start" ? 0 : Math.max(0, document.documentElement.scrollHeight - window.innerHeight);
  const smooth = !window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  const behavior = smooth ? "smooth" : "instant";

  const controller = new AbortController();
  let frame = 0;
  let idleTimer: ReturnType<typeof setTimeout> | undefined;
  let timeout: ReturnType<typeof setTimeout> | undefined;
  const current = {
    finish() {
      controller.abort();
      cancelAnimationFrame(frame);
      clearTimeout(idleTimer);
      clearTimeout(timeout);
      if (motion === current) motion = undefined;
    },
    cancel() {
      if (motion !== current) return;
      current.finish();
      window.scrollTo({ top: window.scrollY, behavior: "instant" });
    },
  };
  motion = current;
  function settle() {
    cancelAnimationFrame(frame);
    clearTimeout(idleTimer);
    // ResizeObserver and Svelte can still commit the destination panel after
    // scrollend. Read its geometry after those frames, not from the observer.
    frame = requestAnimationFrame(() => {
      frame = requestAnimationFrame(() => {
        if (motion !== current) return;
        const top = destination();
        if (Math.abs(window.scrollY - top) < 1) current.finish();
        else {
          window.scrollTo({ top, behavior });
          if (!smooth) settle();
        }
      });
    });
  }
  const options = { passive: true, signal: controller.signal };
  // Cancel without preventing the browser's default action. Chromium may
  // clear the first wheel delta when activating the JS-set scroll position:
  // https://chromium.googlesource.com/chromium/src/+/main/cc/animation/keyframe_effect.h
  // Do not replay deltas; cancellation removes our input listeners.
  const beforeScroll = { passive: false, signal: controller.signal };
  window.addEventListener("wheel", current.cancel, beforeScroll);
  window.addEventListener("touchstart", current.cancel, beforeScroll);
  window.addEventListener("pointerdown", current.cancel, options);
  window.addEventListener(
    "keydown",
    (event) => {
      if (
        [
          "ArrowUp",
          "ArrowDown",
          "PageUp",
          "PageDown",
          "Home",
          "End",
          " ",
          "Tab",
          "Escape",
        ].includes(event.key)
      )
        current.cancel();
    },
    { signal: controller.signal },
  );
  document.addEventListener(
    "scrollend",
    (event) => {
      if (event.target === document) settle();
    },
    options,
  );
  if (!("onscrollend" in document)) {
    window.addEventListener(
      "scroll",
      () => {
        clearTimeout(idleTimer);
        idleTimer = setTimeout(settle, 150);
      },
      options,
    );
  }
  // Bound the lifetime even if the browser never dispatches scrollend.
  timeout = setTimeout(current.finish, 5000);
  window.scrollTo({ top: destination(), behavior });
  if (!smooth || Math.abs(window.scrollY - destination()) < 1) settle();
}

/** Home/End and ordinary navigation retain their native browser behavior. */
export function initDocumentScrollShortcuts(): () => void {
  function onKey(event: KeyboardEvent) {
    if (
      event.defaultPrevented ||
      event.isComposing ||
      event.altKey ||
      event.shiftKey ||
      !(event.ctrlKey || event.metaKey) ||
      !["ArrowUp", "ArrowDown"].includes(event.key)
    )
      return;

    const target = event
      .composedPath()
      .find((node): node is HTMLElement => node instanceof HTMLElement);
    if (
      target?.closest(
        'input, textarea, select, [contenteditable]:not([contenteditable="false"]), [role="textbox"], [role="combobox"], [role="listbox"], [role="menu"], [role="dialog"], [data-popover-content]',
      ) ||
      document.querySelector(
        '[data-dialog-content][data-state="open"], [data-alert-dialog-content][data-state="open"], [data-popover-content][data-state="open"], [data-dropdown-menu-content][data-state="open"], [data-select-content][data-state="open"]',
      ) ||
      window.getSelection()?.isCollapsed === false
    )
      return;

    event.preventDefault();
    scrollToDocumentBoundary(event.key === "ArrowUp" ? "start" : "end");
  }

  window.addEventListener("keydown", onKey);
  return () => {
    window.removeEventListener("keydown", onKey);
    motion?.cancel();
  };
}
