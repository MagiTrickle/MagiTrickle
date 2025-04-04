// deno-lint-ignore-file prefer-const
export function persistedState<T>(key: string, defaults: T) {
  const stored = localStorage.getItem(key);
  const value = stored ? JSON.parse(stored) : { value: defaults };

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
