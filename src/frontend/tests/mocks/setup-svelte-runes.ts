export function installSvelteRunesMocks() {
  const g = globalThis as any;
  if (g.__svelteRunesMocksInstalled) return;

  const state = ((value: unknown) => value) as any;
  state.snapshot = (value: unknown) => {
    try {
      return structuredClone(value);
    } catch {
      return JSON.parse(JSON.stringify(value));
    }
  };

  const derived = ((value: unknown) => (typeof value === "function" ? (value as any)() : value)) as any;
  derived.by = (fn: () => unknown) => fn();

  const effect = (() => {}) as any;
  effect.root = (fn: () => void | (() => void)) => {
    const cleanup = fn();
    return typeof cleanup === "function" ? cleanup : () => {};
  };

  g.$state = state;
  g.$derived = derived;
  g.$effect = effect;
  g.__svelteRunesMocksInstalled = true;
}
