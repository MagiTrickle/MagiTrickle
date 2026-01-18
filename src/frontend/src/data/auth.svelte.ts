import { persistedState } from "../utils/persisted-state.svelte";

export const token = persistedState<string | undefined>("auth", undefined);
