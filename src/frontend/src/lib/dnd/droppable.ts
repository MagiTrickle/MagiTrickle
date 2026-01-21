import { dnd_state } from "./dnd.svelte";

type Effect = "none" | "copy" | "link" | "move";

export type DroppableOptions<T> = {
  data: T;
  scope: string;
  canDrop?: (source: any, target: T) => boolean;
  dropEffect?: Effect;
  onDrop?: (source: any, target: T) => void;
  debug?: boolean;
};

// одна активная подсветка на scope
let CURRENT: { node: HTMLElement | null; scope: string | null } = { node: null, scope: null };

export function droppable<T>(node: HTMLElement, options: DroppableOptions<T>) {
  node.setAttribute("data-droppable", options.scope);
  const dbg = (...a: any[]) => options?.debug && console.debug("[dnd:droppable]", ...a);

  let enterCount = 0;

  const active = () => dnd_state.is_dragging && dnd_state.source_scope === options.scope;

  function validateAndDecorate(e: DragEvent) {
    if (!active()) return false;

    const valid = options?.canDrop ? !!options.canDrop(dnd_state.source, options.data) : true;

    dnd_state.target = options.data;
    dnd_state.valid_droppable = valid;

    if (e.dataTransfer) {
      e.dataTransfer.dropEffect = valid ? (options?.dropEffect ?? "move") : "none";
    }

    // статус для стилизации
    (node as HTMLElement).dataset.drop = valid ? "allowed" : "denied";

    if (valid) {
      // обеспечить одну подсветку
      if (CURRENT.node && CURRENT.node !== node && CURRENT.scope === options.scope) {
        CURRENT.node.classList.remove("dragover");
        (CURRENT.node as HTMLElement).dataset.drop = "";
      }
      CURRENT = { node, scope: options.scope };
      node.classList.add("dragover");
    } else if (CURRENT.node === node) {
      node.classList.remove("dragover");
      (node as HTMLElement).dataset.drop = "";
      CURRENT = { node: null, scope: null };
    }

    return valid;
  }

  function onDragEnter(e: DragEvent) {
    if (!active()) return;
    enterCount++;
    validateAndDecorate(e);
    e.preventDefault(); // Safari любит preventDefault на enter
  }

  function onDragOver(e: DragEvent) {
    const valid = validateAndDecorate(e);
    if (valid) e.preventDefault();
  }

  function onDragLeave(_e: DragEvent) {
    if (!active()) return;
    enterCount = Math.max(0, enterCount - 1);
    if (enterCount === 0 && CURRENT.node === node) {
      node.classList.remove("dragover");
      (node as HTMLElement).dataset.drop = "";
      CURRENT = { node: null, scope: null };
    }
  }

  function onDrop(e: DragEvent) {
    const valid = validateAndDecorate(e);
    e.preventDefault();
    enterCount = 0;
    if (valid && options?.onDrop) {
      // fire custom drop handler immediately so consumers can react before dragend
      try {
        options.onDrop(dnd_state.source, options.data);
      } finally {
        dnd_state.valid_droppable = false;
      }
    }
    if (CURRENT.node === node) {
      node.classList.remove("dragover");
      (node as HTMLElement).dataset.drop = "";
      CURRENT = { node: null, scope: null };
    }
  }

  node.addEventListener("dragenter", onDragEnter);
  node.addEventListener("dragover", onDragOver);
  node.addEventListener("dragleave", onDragLeave);
  node.addEventListener("drop", onDrop);

  return {
    update(new_options: DroppableOptions<T>) {
      options = new_options;
      node.setAttribute("data-droppable", options.scope);
    },
    destroy() {
      node.removeEventListener("dragenter", onDragEnter);
      node.removeEventListener("dragover", onDragOver);
      node.removeEventListener("dragleave", onDragLeave);
      node.removeEventListener("drop", onDrop);
      if (CURRENT.node === node) CURRENT = { node: null, scope: null };
      node.classList.remove("dragover");
      (node as HTMLElement).dataset.drop = "";
    },
  };
}
