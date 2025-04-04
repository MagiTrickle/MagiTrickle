import en from "../locales/en.json";
import ru from "../locales/ru.json";
import { persistedState } from "../utils/persisted-state.svelte.ts";

export const locales: Record<string, Record<string, string>> = { en, ru };

export const locale = persistedState<string>("locale", "en");
const translation = $derived(locales[locale.state]);

export function t(key: string) {
  return translation[key] ?? key;
}
