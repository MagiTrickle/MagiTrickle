import { dnd_state } from "./dnd.svelte.ts";

export type DraggableOptions<S, T> = {
  data: S;
  scope: string;
  onDrop?: (source: S, target: T) => void;
};

export function draggable<S, T>(node: HTMLElement, options: DraggableOptions<S, T>) {
  node.draggable = true;

  function handleDragStart() {
    dnd_state.is_dragging = true;
    dnd_state.source = options.data;
    dnd_state.source_scope = options.scope;
  }

  function handleDragEnd(event: DragEvent) {
    if (dnd_state.valid_droppable && options.onDrop) {
      options.onDrop(dnd_state.source, dnd_state.target);
    }
    dnd_state.is_dragging = false;
    dnd_state.source = null;
    dnd_state.target = null;
    dnd_state.valid_droppable = false;
    dnd_state.source_scope = "";
    event.preventDefault();
  }

  node.addEventListener("dragstart", handleDragStart);
  node.addEventListener("dragend", handleDragEnd);

  return {
    update(new_options: DraggableOptions<S, T>) {
      options = new_options;
      node.draggable = true;
    },
    destroy() {
      node.removeEventListener("dragstart", handleDragStart);
      node.removeEventListener("dragend", handleDragEnd);
    },
  };
}
