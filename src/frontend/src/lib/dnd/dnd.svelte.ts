export type DnDState = {
  is_dragging: boolean;
  source: any;
  target: any;
  source_scope: string;
  valid_droppable: boolean;
};

export const dnd_state = $state<DnDState>({
  is_dragging: false,
  source: {} as any,
  target: {} as any,
  source_scope: "",
  valid_droppable: false,
});
