import { dnd_state } from "./dnd.svelte.ts";

export type DroppableOptions<T> = {
  data: T;
  scope: string;
};

export function droppable<T>(node: HTMLElement, options: DroppableOptions<T>) {
  node.setAttribute("data-droppable", options.scope);

  let timer: number | null = null;

  function handleDrop(event: DragEvent) {
    event.preventDefault();
    if (dnd_state.source_scope !== options.scope) return;
    dnd_state.target = options.data;
    dnd_state.valid_droppable = true;
    if (timer) {
      clearTimeout(timer);
      timer = null;
    }
    if (event.target instanceof HTMLElement) {
      const el = event.target.closest(`[data-droppable="${options.scope}"]`);
      el?.classList.remove("dragover");
    }
  }

  function handleDragOver(event: DragEvent) {
    if (dnd_state.source_scope !== options.scope || !event.target) return;
    if (event.target instanceof HTMLElement) {
      if (timer) {
        clearTimeout(timer);
        timer = null;
      }
      const el = event.target.closest(`[data-droppable="${options.scope}"]`);
      el?.classList.add("dragover");
    }
    event.preventDefault();
  }

  function handleDragEnter(event: DragEvent) {
    if (dnd_state.source_scope !== options.scope || !event.target) return;
    if (event.target instanceof HTMLElement) {
      if (timer) {
        clearTimeout(timer);
        timer = null;
      }
      const el = event.target.closest(`[data-droppable="${options.scope}"]`);
      el?.classList.add("dragover");
    }
    event.preventDefault();
  }

  function handleDragLeave(event: DragEvent) {
    if (dnd_state.source_scope !== options.scope || !event.target) return;
    if (event.target instanceof HTMLElement) {
      const el = event.target.closest(`[data-droppable="${options.scope}"]`);
      el?.classList.remove("dragover");
      timer = setTimeout(() => node.classList.remove("dragover"), 50);
    }
    event.preventDefault();
  }

  node.addEventListener("drop", handleDrop);
  node.addEventListener("dragover", handleDragOver);
  node.addEventListener("dragenter", handleDragEnter);
  node.addEventListener("dragleave", handleDragLeave);

  return {
    update(new_options: DroppableOptions<T>) {
      options = new_options;
    },
    destroy() {
      node.removeEventListener("drop", handleDrop);
      node.removeEventListener("dragover", handleDragOver);
      node.removeEventListener("dragenter", handleDragEnter);
      node.removeEventListener("dragleave", handleDragLeave);
    },
  };
}
