import { dnd_state } from "./dnd.svelte";

export type DragEffects = {
  effectAllowed?: DataTransfer["effectAllowed"];
  dropEffect?: DataTransfer["dropEffect"];
};

export type DraggableOptions<S, T> = {
  data: S;
  scope: string;
  onDrop?: (source: S, target: T) => void;
  handle?: string;
  effects?: DragEffects;
  dragImage?: HTMLElement | ((node: HTMLElement, data: S) => HTMLElement | null);
  debug?: boolean;
};

let idCounter = 0;

function isEventFromHandle(
  target: EventTarget | null,
  root: HTMLElement,
  selector: string,
): boolean {
  if (!selector) return true;
  if (!(target instanceof Element)) return false;

  if (target.matches?.(selector)) return true;

  const viaClosest = typeof target.closest === "function" ? target.closest(selector) : null;
  if (viaClosest && root.contains(viaClosest)) return true;

  const handleEl = root.querySelector(selector);
  return !!(handleEl && handleEl.contains(target));
}

export function draggable<S, T>(node: HTMLElement, options: DraggableOptions<S, T>) {
  const id = ++idCounter;
  const log = (...a: any[]) => options?.debug && console.debug(`[dnd:draggable#${id}]`, ...a);

  node.draggable = true;

  let startedFromHandle = false;
  let cleanupDragImg: (() => void) | null = null;

  function isHandleVisible(root: HTMLElement, selector: string) {
    const el = root.querySelector(selector) as HTMLElement | null;
    if (!el) return false;
    const s = getComputedStyle(el);
    if (s.display === "none" || s.visibility === "hidden" || s.pointerEvents === "none")
      return false;
    const r = el.getBoundingClientRect();
    return r.width > 0 && r.height > 0;
  }

  let blockersOn = false;
  let savedUserSelect = "";
  let savedWebkitUserSelect = "";
  let pointerUpReset: number | null = null;
  let activeDrag = false;

  function preventSelect(e: Event) {
    e.preventDefault();
  }

  function installBlockers() {
    if (blockersOn) return;
    blockersOn = true;
    window.addEventListener("selectstart", preventSelect, true);
    const style = document.documentElement.style as any;
    savedUserSelect = style.userSelect ?? "";
    savedWebkitUserSelect = style.webkitUserSelect ?? "";
    style.userSelect = "none";
    style.webkitUserSelect = "none";
  }

  function removeBlockers() {
    if (!blockersOn) return;
    blockersOn = false;
    window.removeEventListener("selectstart", preventSelect, true);
    const style = document.documentElement.style as any;
    style.userSelect = savedUserSelect;
    style.webkitUserSelect = savedWebkitUserSelect;
  }

  function onPointerDown(e: PointerEvent) {
    const needHandle = !!options?.handle && isHandleVisible(node, options.handle!);

    if (needHandle) {
      startedFromHandle = isEventFromHandle(e.target, node, options.handle!);
      node.draggable = startedFromHandle;
    } else {
      startedFromHandle = true;
      node.draggable = true;
    }

    if (startedFromHandle) {
      document.documentElement.classList.add("dnd-possible");
      installBlockers(); // важно: до возможного selectstart
    }
  }

  function onPointerUp() {
    if (pointerUpReset != null) window.clearTimeout(pointerUpReset);
    // Safari может завершить pointer sequence до события dragstart.
    pointerUpReset = window.setTimeout(() => {
      if (dnd_state.is_dragging) return;
      startedFromHandle = false;
      node.draggable = true;
      document.documentElement.classList.remove("dnd-possible");
      removeBlockers();
      pointerUpReset = null;
    }, 0);
  }

  function makeTransparentDragImage() {
    const el = document.createElement("div");
    el.style.width = "1px";
    el.style.height = "1px";
    el.style.opacity = "0";
    el.style.position = "fixed";
    el.style.top = "-10px";
    el.style.pointerEvents = "none";
    document.body.appendChild(el);
    return { el, cleanup: () => el.remove() };
  }

  function setDT(ev: DragEvent) {
    if (!ev.dataTransfer) return;

    ev.dataTransfer.effectAllowed = options?.effects?.effectAllowed ?? "move";
    ev.dataTransfer.dropEffect = options?.effects?.dropEffect ?? "move";

    try {
      ev.dataTransfer.setData("text/plain", "drag");
      ev.dataTransfer.setData("application/x-dnd-scope", options.scope);
      ev.dataTransfer.setData("application/json", JSON.stringify(options.data));
    } catch {
      /* noop */
    }

    if (options?.dragImage) {
      const img =
        typeof options.dragImage === "function"
          ? options.dragImage(node, options.data)
          : options.dragImage;

      if (img) {
        if (!document.body.contains(img)) document.body.appendChild(img);
        cleanupDragImg = () => img.remove();
        ev.dataTransfer.setDragImage(img, 12, 12);
        return;
      }
    }

    const { el, cleanup } = makeTransparentDragImage();
    cleanupDragImg = cleanup;
    ev.dataTransfer.setDragImage(el, 0, 0);
  }

  function handleDragStart(ev: DragEvent) {
    if (ev.target !== node) return;
    log("dragstart", { target: ev.target, handle: options?.handle, startedFromHandle });

    const needHandle = !!options?.handle && isHandleVisible(node, options.handle!);
    if (needHandle && !startedFromHandle) {
      // Safari может пропустить pointerdown, повторно проверяем ручку на dragstart.
      startedFromHandle = isEventFromHandle(ev.target, node, options.handle!);
    }
    if (needHandle && !startedFromHandle) {
      ev.preventDefault();
      return;
    }

    dnd_state.is_dragging = true;
    dnd_state.source = options.data;
    dnd_state.source_scope = options.scope;
    dnd_state.valid_droppable = false;

    activeDrag = true;

    document.documentElement.setAttribute("data-dnd-scope", options.scope);

    node.classList.add("dragging");
    node.setAttribute("aria-grabbed", "true");

    document.documentElement.classList.remove("dnd-possible");
    document.documentElement.classList.add("dnd-dragging");

    installBlockers();

    setDT(ev);
  }

  function cleanupOverClasses() {
    document.querySelectorAll<HTMLElement>("[data-droppable].dragover").forEach((el) => {
      el.classList.remove("dragover");
      (el as HTMLElement).dataset.drop = "";
    });
  }

  function handleDragEnd(_ev: DragEvent) {
    if (!activeDrag) return;
    activeDrag = false;

    if (dnd_state.valid_droppable && options?.onDrop) {
      options.onDrop(dnd_state.source as S, dnd_state.target as T);
    }

    dnd_state.is_dragging = false;
    dnd_state.source = null;
    dnd_state.target = null;
    dnd_state.valid_droppable = false;
    dnd_state.source_scope = "";

    node.classList.remove("dragging");
    node.removeAttribute("aria-grabbed");

    document.documentElement.classList.remove("dnd-dragging", "dnd-possible");
    document.documentElement.removeAttribute("data-dnd-scope");
    removeBlockers();
    cleanupOverClasses();

    if (cleanupDragImg) {
      cleanupDragImg();
      cleanupDragImg = null;
    }

    startedFromHandle = false;
    node.draggable = true;
  }

  node.addEventListener("pointerdown", onPointerDown, true);
  window.addEventListener("pointerup", onPointerUp, true);
  node.addEventListener("dragstart", handleDragStart, true);
  node.addEventListener("dragend", handleDragEnd);

  window.addEventListener("drop", handleDragEnd as any);

  return {
    update(new_options: DraggableOptions<S, T>) {
      options = new_options;
      node.draggable = true;
    },
    destroy() {
      node.removeEventListener("pointerdown", onPointerDown, true);
      window.removeEventListener("pointerup", onPointerUp, true);
      node.removeEventListener("dragstart", handleDragStart, true);
      node.removeEventListener("dragend", handleDragEnd);
      window.removeEventListener("drop", handleDragEnd as any);
      if (pointerUpReset != null) window.clearTimeout(pointerUpReset);
      activeDrag = false;
      removeBlockers();
      document.documentElement.classList.remove("dnd-dragging", "dnd-possible");
    },
  };
}
