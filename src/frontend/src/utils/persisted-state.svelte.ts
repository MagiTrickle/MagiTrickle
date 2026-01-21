// deno-lint-ignore-file prefer-const
export function persistedState<T>(key: string, defaults: T) {
  const stored = localStorage.getItem(key);
  let value = { value: defaults };
  if (stored) {
    try {
      value = JSON.parse(stored);
    } catch {}
  }

  let state = $state(value);
  $effect.root(() => {
    $effect(() => localStorage.setItem(key, JSON.stringify(state)));
  });

  return {
    get current() {
      return state.value;
    },
    set current(value: T) {
      state.value = value;
    },
    reset() {
      state.value = defaults;
    },
    state,
  };
}
