export class ChangeTracker<T extends object> {
  private state: T;
  private proxy: T;

  private originalObjects = new Map<string, any>();
  private originalArrays = new WeakMap<object, string[]>();
  private proxyCache = new WeakMap<object, any>();

  private dirtyObjectProps = new Map<string, Set<string>>();
  private dirtyArrays = new Set<object>();

  private version = $state(0);

  constructor(initialData: T) {
    this.state = $state(initialData);
    this.init(structuredClone(initialData));
    this.proxy = this.createProxy(this.state);
  }

  private init(snapshot: any) {
    this.originalObjects.clear();
    this.originalArrays = new WeakMap();
    this.proxyCache = new WeakMap();
    this.dirtyObjectProps.clear();
    this.dirtyArrays.clear();
    this.indexAndLink(this.state, snapshot);
  }

  private indexAndLink(stateNode: any, snapshotNode: any) {
    if (!stateNode || typeof stateNode !== "object") return;

    if (Array.isArray(stateNode)) {
      const ids = Array.isArray(snapshotNode)
        ? snapshotNode.map((item: any) => item?.id ?? item)
        : [];
      this.originalArrays.set(stateNode, ids);

      for (let i = 0; i < stateNode.length; i++) {
        if (snapshotNode?.[i]) this.indexAndLink(stateNode[i], snapshotNode[i]);
      }
      return;
    }

    if ("id" in stateNode && stateNode.id) {
      this.originalObjects.set(stateNode.id, { ...snapshotNode });
    }

    for (const key in stateNode) {
      if (typeof stateNode[key] === "object") {
        this.indexAndLink(stateNode[key], snapshotNode?.[key]);
      }
    }
  }

  private createProxy(target: any): any {
    if (typeof target !== "object" || target === null) return target;
    if (this.proxyCache.has(target)) return this.proxyCache.get(target);

    const isArray = Array.isArray(target);

    const handler: ProxyHandler<any> = {
      get: (tgt, prop, receiver) => {
        if (
          isArray &&
          typeof prop === "string" &&
          ["push", "pop", "shift", "unshift", "splice", "sort", "reverse"].includes(prop)
        ) {
          return (...args: any[]) => {
            const res = Reflect.get(tgt, prop, receiver).apply(tgt, args);
            this.checkArrayStructure(tgt);
            this.notify();
            return res;
          };
        }
        const val = Reflect.get(tgt, prop, receiver);
        return typeof val === "object" && val !== null ? this.createProxy(val) : val;
      },
      set: (tgt, prop, val) => {
        const res = Reflect.set(tgt, prop, val);
        if (isArray) {
          if (prop === "length" || !isNaN(Number(prop))) this.checkArrayStructure(tgt);
        } else if (typeof prop === "string") {
          this.checkObjectField(tgt, prop);
        }
        this.notify();
        return res;
      },
      deleteProperty: (tgt, prop) => {
        const res = Reflect.deleteProperty(tgt, prop);
        if (isArray) this.checkArrayStructure(tgt);
        else if (typeof prop === "string") this.checkObjectField(tgt, prop);
        this.notify();
        return res;
      },
    };

    const proxy = new Proxy(target, handler);
    this.proxyCache.set(target, proxy);
    return proxy;
  }

  private checkObjectField(target: any, prop: string) {
    const id = target.id;
    if (!id || !this.originalObjects.has(id)) return;

    const originalVal = this.originalObjects.get(id)[prop];
    const currentVal = target[prop];

    let props = this.dirtyObjectProps.get(id);
    if (currentVal !== originalVal) {
      if (!props) {
        props = new Set();
        this.dirtyObjectProps.set(id, props);
      }
      props.add(prop);
    } else if (props) {
      props.delete(prop);
      if (props.size === 0) this.dirtyObjectProps.delete(id);
    }
  }

  private checkArrayStructure(array: any) {
    if (!this.originalArrays.has(array)) return;
    const originals = this.originalArrays.get(array)!;
    const currentIds = array.map((i: any) => i?.id ?? i);

    let dirty = originals.length !== currentIds.length;
    if (!dirty) {
      for (let i = 0; i < originals.length; i++) {
        if (originals[i] !== currentIds[i]) {
          dirty = true;
          break;
        }
      }
    }

    if (dirty) this.dirtyArrays.add(array);
    else this.dirtyArrays.delete(array);
  }

  private notify() {
    this.version += 1;
  }

  private traverse(node: any, map: Map<string, any>) {
    if (Array.isArray(node)) {
      for (const child of node) this.traverse(child, map);
    } else if (node && typeof node === "object") {
      if (node.id) map.set(node.id, node);
      for (const key in node) {
        const child = node[key];
        if (typeof child === "object") this.traverse(child, map);
      }
    }
  }

  get data() {
    return this.proxy;
  }

  get isDirty() {
    const _ = this.version;
    return this.dirtyObjectProps.size > 0 || this.dirtyArrays.size > 0;
  }

  get changes() {
    if (!this.isDirty) {
      return { added: [], deleted: [], mutated: [] };
    }

    const _ = this.version;
    const currentMap = new Map<string, any>();
    this.traverse(this.state, currentMap);

    const added: any[] = [];
    const deleted: any[] = [];
    const mutated: any[] = [];

    for (const [id, node] of currentMap) {
      if (!this.originalObjects.has(id)) {
        added.push(node);
      } else if (this.dirtyObjectProps.has(id)) {
        mutated.push(node);
      }
    }

    for (const [id, original] of this.originalObjects) {
      if (!currentMap.has(id)) {
        deleted.push(original);
      }
    }

    return $state.snapshot({ added, deleted, mutated });
  }

  reset(newData: T) {
    const snapshot = structuredClone(newData);

    if (Array.isArray(this.state) && Array.isArray(newData)) {
      this.state.length = 0;
      this.state.push(...newData);
    } else {
      const s = this.state as any;
      Object.keys(s).forEach((k) => delete s[k]);
      Object.assign(s, newData);
    }

    this.init(snapshot);
    this.notify();
  }
}
