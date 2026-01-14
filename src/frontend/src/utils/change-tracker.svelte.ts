export class ChangeTracker<T extends object> {
  #state: T;
  #proxy: T;

  #originalObjects = new Map<string, any>();
  #originalArrays = new WeakMap<any, string[]>();
  #proxyCache = new WeakMap<object, any>();

  #version = $state(0);

  #dirtyFields = new Set<string>();
  #dirtyArrays = new Set<any>();

  constructor(initialData: T) {
    const snapshot = structuredClone(initialData);
    this.#state = $state(initialData);

    this.#indexAndLink(this.#state, snapshot);
    this.#proxy = this.#createProxy(this.#state);
  }

  #indexAndLink(stateNode: any, snapshotNode: any) {
    if (!stateNode || !snapshotNode || typeof stateNode !== "object") return;

    if (Array.isArray(stateNode)) {
      const ids = Array.isArray(snapshotNode)
        ? snapshotNode.map((item: any) => item?.id ?? item)
        : [];

      this.#originalArrays.set(stateNode, ids);

      stateNode.forEach((child, i) => {
        if (snapshotNode[i]) this.#indexAndLink(child, snapshotNode[i]);
      });
      return;
    }

    if ("id" in stateNode && stateNode.id) {
      this.#originalObjects.set(stateNode.id, { ...snapshotNode });
    }

    for (const key in stateNode) {
      if (typeof stateNode[key] === "object") {
        this.#indexAndLink(stateNode[key], snapshotNode[key]);
      }
    }
  }

  #createProxy(target: any): any {
    if (typeof target !== "object" || target === null) return target;
    if (this.#proxyCache.has(target)) return this.#proxyCache.get(target);

    const isArray = Array.isArray(target);

    const handler: ProxyHandler<any> = {
      get: (target, prop, receiver) => {
        if (
          isArray &&
          typeof prop === "string" &&
          ["push", "pop", "shift", "unshift", "splice", "sort", "reverse"].includes(prop)
        ) {
          return (...args: any[]) => {
            const method = Reflect.get(target, prop, receiver);
            const result = method.apply(target, args);
            this.#checkArrayStructure(target);
            this.#notify();
            return result;
          };
        }

        const value = Reflect.get(target, prop, receiver);
        if (typeof value === "object" && value !== null) {
          return this.#createProxy(value);
        }
        return value;
      },

      set: (target, prop, value) => {
        const res = Reflect.set(target, prop, value);

        if (isArray) {
          if (prop === "length" || !isNaN(Number(prop))) {
            this.#checkArrayStructure(target);
          }
        } else if (typeof prop === "string") {
          this.#checkObjectField(target, prop);
        }

        this.#notify();
        return res;
      },

      deleteProperty: (target, prop) => {
        const res = Reflect.deleteProperty(target, prop);
        if (isArray) {
          this.#checkArrayStructure(target);
        } else if (typeof prop === "string") {
          this.#checkObjectField(target, prop);
        }
        this.#notify();
        return res;
      },
    };

    const proxy = new Proxy(target, handler);
    this.#proxyCache.set(target, proxy);
    return proxy;
  }

  #notify() {
    this.#version += 1;
  }

  #checkObjectField(target: any, prop: string) {
    const id = target.id;
    if (!id || !this.#originalObjects.has(id)) return;

    const originalObj = this.#originalObjects.get(id);
    const originalValue = originalObj[prop];
    const currentValue = target[prop];
    const key = `${id}:${prop}`;

    const isDifferent = currentValue !== originalValue;

    if (isDifferent) {
      this.#dirtyFields.add(key);
    } else {
      this.#dirtyFields.delete(key);
    }
  }

  #checkArrayStructure(array: any) {
    if (!this.#originalArrays.has(array)) return;

    const originalIds = this.#originalArrays.get(array)!;
    const currentIds = array.map((item: any) => item?.id ?? item);

    let isDirty = false;

    if (originalIds.length !== currentIds.length) {
      isDirty = true;
    } else {
      for (let i = 0; i < originalIds.length; i++) {
        if (originalIds[i] !== currentIds[i]) {
          isDirty = true;
          break;
        }
      }
    }

    if (isDirty) {
      this.#dirtyArrays.add(array);
    } else {
      this.#dirtyArrays.delete(array);
    }
  }

  get data() {
    return this.#proxy;
  }

  get isDirty() {
    const _v = this.#version;
    return this.#dirtyFields.size > 0 || this.#dirtyArrays.size > 0;
  }

  reset(newData: T) {
    const snapshot = structuredClone(newData);

    this.#originalObjects.clear();
    this.#originalArrays = new WeakMap();
    this.#proxyCache = new WeakMap();
    this.#dirtyFields.clear();
    this.#dirtyArrays.clear();

    this.#indexAndLink(this.#state, snapshot);
    this.#notify();
  }
}
