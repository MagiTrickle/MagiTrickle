import { getContext } from "svelte";

export type SectionControls = {
  title: string;
  canSave: boolean;
  dirty: boolean;
  saveLabel: string;
  onSave: () => Promise<void>;
  addLabel?: string;
  canAdd?: boolean;
  onAdd?: () => void;
};
export type SettingsSection = {
  element: HTMLElement;
  controls: () => SectionControls;
};
export const SETTINGS_SECTIONS = Symbol("settings-sections");
export type RegisterSection = (section: SettingsSection) => () => void;

// Each section owns its draft and API write. The page coordinates one Save
// across all dirty sections; generic builds never import Keenetic code.
export function settingsSection() {
  const register = getContext<RegisterSection>(SETTINGS_SECTIONS);
  return (element: HTMLElement, controls: () => SectionControls) => ({
    destroy: register({ element, controls }),
  });
}
