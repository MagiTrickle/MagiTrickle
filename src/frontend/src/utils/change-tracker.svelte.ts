import { createSubscriber } from "svelte/reactivity";

import { cloneCooperatively, CooperativeBudget } from "./cooperative-data";

type IdObject = { id: string } & Record<string, unknown>;

function isIdObject(value: unknown): value is IdObject {
  return (
    value !== null &&
    typeof value === "object" &&
    "id" in value &&
    typeof value.id === "string" &&
    value.id.length > 0
  );
}

function sameValue(left: unknown, right: unknown): boolean {
  if (left === right) return true;
  if (!left || !right || typeof left !== "object" || typeof right !== "object") return false;
  if (Array.isArray(left) || Array.isArray(right)) {
    return (
      Array.isArray(left) &&
      Array.isArray(right) &&
      left.length === right.length &&
      left.every((value, index) => sameValue(value, right[index]))
    );
  }
  const a = left as Record<string, unknown>;
  const b = right as Record<string, unknown>;
  const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
  return [...keys].every((key) => sameValue(a[key], b[key]));
}

const OBJECT_KEYS = Symbol("tracked object keys");
const ARRAY_MUTATIONS = new Set(["push", "pop", "shift", "unshift", "splice", "sort", "reverse"]);

class PropertySource {
  constructor(private readonly onInactive: () => void) {}

  private update: (() => void) | undefined;
  private subscribe = createSubscriber((update) => {
    this.update = update;
    return () => {
      this.update = undefined;
      this.onInactive();
    };
  });

  read() {
    this.subscribe();
  }

  invalidate() {
    this.update?.();
  }
}

export class ChangeTracker<T extends object> {
  private state: T;
  private proxy: T;

  private originalObjects = new Map<string, any>();
  private originalArrays = new WeakMap<object, unknown[]>();
  private proxyCache = new WeakMap<object, any>();
  private reverseProxyCache = new WeakMap<object, object>();
  private ownedNodes = new WeakSet<object>();

  private dirtyObjectProps = new Map<string, Set<string>>();
  private dirtyArrays = new Set<object>();

  private version = $state(0);
  private revision = 0;
  private publication = $state(0);
  private publicationRevision = 0;
  private propertySources = new WeakMap<object, Map<PropertyKey, PropertySource>>();

  constructor(initialData: T) {
    // This tracker already owns a proxy and a reactive version. Wrapping the
    // backing tree in $state as well materializes a second proxy for every
    // offscreen rule during indexing/rebasing. Keep detached plain data here;
    // only mounted consumers subscribe through the tracked proxy reads.
    this.state = structuredClone(initialData);
    this.init(structuredClone(initialData));
    this.proxy = this.createProxy(this.state);
  }

  private resetIndexes() {
    this.originalObjects.clear();
    this.originalArrays = new WeakMap();
    this.proxyCache = new WeakMap();
    this.reverseProxyCache = new WeakMap();
    this.ownedNodes = new WeakSet();
    this.dirtyObjectProps.clear();
    this.dirtyArrays.clear();
    this.propertySources = new WeakMap();
  }

  private init(snapshot: any) {
    this.resetIndexes();
    this.indexAndLink(this.state, snapshot);
  }

  /** Build a detached imported editor without touching the published tracker.
   * The caller owns immutable baseline/draft snapshots and must check its live
   * editor revision again before swapping the returned tracker into the UI. */
  static async fromDraftAsync<Item extends { id: string }>(
    baseline: Item[],
    draft: Item[],
    options: { signal?: AbortSignal; budgetMs?: number } = {},
  ): Promise<ChangeTracker<Item[]>> {
    const budget = new CooperativeBudget(options.budgetMs ?? 4, options.signal);
    const tracker = new ChangeTracker<Item[]>([]);
    try {
      const initialPause = budget.checkpoint();
      if (initialPause) await initialPause;
      tracker.state = await cloneCooperatively(baseline, budget);
      const snapshot = await cloneCooperatively(baseline, budget);
      tracker.resetIndexes();
      await tracker.indexAndLinkAsync(tracker.state, snapshot, budget);
      const incoming = tracker.idObjects(await cloneCooperatively(draft, budget));
      const target = tracker.idObjects(tracker.state);
      const existing = new Map(target.map((item) => [item.id, item]));
      const next = incoming.map((item) => existing.get(item.id) ?? item);
      for (let index = 0; index < next.length; index++) target[index] = next[index];
      target.length = next.length;
      for (let index = 0; index < incoming.length; index++) {
        if (target[index] !== incoming[index])
          await tracker.replayDraftAsync(target[index], incoming[index], budget);
        const pause = budget.checkpoint();
        if (pause) await pause;
      }
      await tracker.checkArrayStructureAsync(target, budget);
      // New imported roots also own their nested arrays, even without a saved
      // baseline. Index ownership without accepting them as saved objects.
      await tracker.markOwnedAsync(tracker.state, budget);
      const finalPause = budget.checkpoint();
      if (finalPause) await finalPause;
      tracker.proxy = tracker.createProxy(tracker.state);
      tracker.notify();
      return tracker;
    } finally {
      budget.close();
    }
  }

  /** Prepare one bulk insertion without publishing intermediate rule arrays.
   * Inputs stay immutable until this promise resolves; any concurrent tracker
   * mutation invalidates the prepared commit. Existing array identity and its
   * saved membership baseline are retained for normal editing and reversions. */
  async preparePrepend<Item extends object>(
    array: Item[],
    items: readonly Item[],
    options: { signal?: AbortSignal; budgetMs?: number } = {},
  ): Promise<{ validate(): void; commit(): void }> {
    const target = this.reverseProxyCache.get(array) ?? array;
    if (!Array.isArray(target) || !this.ownedNodes.has(target))
      throw new Error("Bulk insertion requires an array owned by this tracker");
    const revision = this.revision;
    const length = target.length;
    const budget = new CooperativeBudget(options.budgetMs ?? 4, options.signal);
    let committed = false;
    try {
      const initialPause = budget.checkpoint();
      if (initialPause) await initialPause;
      const incoming = await cloneCooperatively(items, budget);
      await this.markOwnedAsync(incoming, budget);
      const validate = () => {
        if (options.signal?.aborted) throw new DOMException("Preparation cancelled", "AbortError");
        if (committed || this.revision !== revision)
          throw new Error("Draft changed during bulk insertion");
      };
      validate();
      return {
        validate,
        commit: () => {
          validate();
          committed = true;
          if (!incoming.length) return;
          // Move existing raw entries in place instead of allocating another
          // complete array. Publication stays synchronous: yielding mid-shift
          // would expose an incomplete list through its retained proxy.
          target.length = length + incoming.length;
          Array.prototype.copyWithin.call(target, incoming.length, 0, length);
          for (let index = 0; index < incoming.length; index++) target[index] = incoming[index];
          this.checkArrayStructure(target);
          this.invalidateArray(target);
          this.notify();
        },
      };
    } finally {
      budget.close();
    }
  }

  /** Replace array membership in one synchronous publication, retaining the
   * tracked array and its saved order. Avoid argument-spread limits for sorting
   * or restoring a complete 100k-rule collection in WebKit. */
  replaceArray<Item extends object>(array: Item[], items: readonly Item[]): void {
    const target = this.reverseProxyCache.get(array) ?? array;
    if (!Array.isArray(target) || !this.ownedNodes.has(target))
      throw new Error("Bulk replacement requires an array owned by this tracker");
    const next: Item[] = [];
    let changed = target.length !== items.length;
    for (let index = 0; index < items.length; index++) {
      const item = this.detachValue(items[index]) as Item;
      next.push(item);
      if (target[index] !== item) changed = true;
    }
    if (!changed) return;
    for (let index = 0; index < next.length; index++) target[index] = next[index];
    target.length = next.length;
    this.checkArrayStructure(target);
    this.invalidateArray(target);
    this.notify();
  }

  private async markOwnedAsync(node: unknown, budget: CooperativeBudget): Promise<void> {
    if (!node || typeof node !== "object") return;
    this.ownedNodes.add(node);
    for (const child of Array.isArray(node) ? node : Object.values(node)) {
      if (child && typeof child === "object") await this.markOwnedAsync(child, budget);
      const pause = budget.checkpoint();
      if (pause) await pause;
    }
  }

  private async indexAndLinkAsync(
    state: unknown,
    snapshot: unknown,
    budget: CooperativeBudget,
  ): Promise<void> {
    if (!state || typeof state !== "object") return;
    this.ownedNodes.add(state);
    if (Array.isArray(state)) {
      const saved = Array.isArray(snapshot) ? snapshot : [];
      const ids: unknown[] = [];
      for (const item of saved) {
        ids.push(isIdObject(item) ? item.id : item);
        const pause = budget.checkpoint();
        if (pause) await pause;
      }
      this.originalArrays.set(state, ids);
      for (let index = 0; index < state.length; index++) {
        if (saved[index]) await this.indexAndLinkAsync(state[index], saved[index], budget);
        const pause = budget.checkpoint();
        if (pause) await pause;
      }
      return;
    }
    const saved = snapshot && typeof snapshot === "object" ? snapshot : {};
    if (isIdObject(state)) this.originalObjects.set(state.id, { ...saved });
    for (const [key, value] of Object.entries(state)) {
      if (value && typeof value === "object")
        await this.indexAndLinkAsync(value, Reflect.get(saved, key), budget);
      const pause = budget.checkpoint();
      if (pause) await pause;
    }
  }

  private async checkArrayStructureAsync(array: unknown[], budget: CooperativeBudget) {
    const originals = this.originalArrays.get(array);
    if (!originals) return;
    let dirty = originals.length !== array.length;
    if (!dirty) {
      for (let index = 0; index < array.length; index++) {
        const item = array[index];
        if (!sameValue(originals[index], isIdObject(item) ? item.id : item)) {
          dirty = true;
          break;
        }
        const pause = budget.checkpoint();
        if (pause) await pause;
      }
    }
    if (dirty) this.dirtyArrays.add(array);
    else this.dirtyArrays.delete(array);
  }

  private async replayDraftAsync(
    target: IdObject,
    draft: IdObject,
    budget: CooperativeBudget,
  ): Promise<void> {
    // Only fromDraftAsync calls this method, with its private detached clone.
    // New nodes can be adopted directly; the saved snapshot remains separate.
    if (target === draft) return;
    for (const key of new Set([...Object.keys(target), ...Object.keys(draft)])) {
      const current = target[key];
      const desired = draft[key];
      if (Array.isArray(current) && Array.isArray(desired)) {
        if (current === desired) continue;
        if (current.every(isIdObject) && desired.every(isIdObject)) {
          const byId = new Map<string, IdObject>();
          for (const item of current) {
            byId.set(item.id, item);
            const pause = budget.checkpoint();
            if (pause) await pause;
          }
          const next: IdObject[] = [];
          for (const item of desired) {
            const existing = byId.get(item.id);
            const restored: unknown = this.originalObjects.get(item.id);
            next.push(
              existing ??
                (isIdObject(restored) ? await cloneCooperatively(restored, budget) : item),
            );
            const pause = budget.checkpoint();
            if (pause) await pause;
          }
          for (let index = 0; index < next.length; index++) {
            current[index] = next[index];
            const pause = budget.checkpoint();
            if (pause) await pause;
          }
          current.length = next.length;
          for (let index = 0; index < desired.length; index++) {
            const item = desired[index];
            const restored: unknown = this.originalObjects.get(item.id);
            if (!byId.has(item.id) && restored)
              await this.indexAndLinkAsync(current[index], restored, budget);
            if (current[index] !== item) await this.replayDraftAsync(current[index], item, budget);
            const pause = budget.checkpoint();
            if (pause) await pause;
          }
        } else {
          for (let index = 0; index < desired.length; index++) {
            current[index] = desired[index];
            const pause = budget.checkpoint();
            if (pause) await pause;
          }
          current.length = desired.length;
        }
        await this.checkArrayStructureAsync(current, budget);
        if (this.dirtyObjectProps.get(target.id)?.has(key)) this.checkObjectField(target, key);
      } else if (!sameValue(current, desired)) {
        if (Object.hasOwn(draft, key)) target[key] = desired;
        else delete target[key];
        this.checkObjectField(target, key);
      }
      const pause = budget.checkpoint();
      if (pause) await pause;
    }
  }

  private indexAndLink(stateNode: any, snapshotNode: any) {
    if (!stateNode || typeof stateNode !== "object") return;
    this.ownedNodes.add(stateNode);

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

  private detachValue(value: unknown, seen = new WeakMap<object, unknown>()): unknown {
    if (!value || typeof value !== "object") return value;
    const raw = this.reverseProxyCache.get(value);
    if (raw) return raw;
    if (this.ownedNodes.has(value)) return value;
    if (seen.has(value)) return seen.get(value);
    if (Array.isArray(value)) {
      const result: unknown[] = [];
      seen.set(value, result);
      this.ownedNodes.add(result);
      for (const item of value) result.push(this.detachValue(item, seen));
      return result;
    }
    const prototype = Object.getPrototypeOf(value);
    if (prototype !== Object.prototype && prototype !== null) return structuredClone(value);
    const result: Record<string, unknown> = Object.create(prototype);
    seen.set(value, result);
    this.ownedNodes.add(result);
    for (const [key, item] of Object.entries(value)) {
      Object.defineProperty(result, key, {
        value: this.detachValue(item, seen),
        enumerable: true,
        writable: true,
        configurable: true,
      });
    }
    return result;
  }

  private trackProperty(target: object, property: PropertyKey) {
    // Validation, serialization and indexing run outside reactive consumers.
    // They must not allocate one source for every field of a 100k-rule tree.
    if (!$effect.tracking()) return;
    void this.publication;
    let properties = this.propertySources.get(target);
    if (!properties) {
      properties = new Map();
      this.propertySources.set(target, properties);
    }
    let source = properties.get(property);
    if (!source) {
      const trackedProperties = properties;
      const created: PropertySource = new PropertySource(() => {
        // Scrolling through every rule must not retain sources for every field
        // ever mounted. An old reset's teardown cannot remove a new mapping.
        if (trackedProperties.get(property) !== created) return;
        trackedProperties.delete(property);
        if (!trackedProperties.size && this.propertySources.get(target) === trackedProperties)
          this.propertySources.delete(target);
      });
      properties.set(property, created);
      source = created;
    }
    source.read();
  }

  private invalidateProperty(target: object, property: PropertyKey, keysChanged = false) {
    const properties = this.propertySources.get(target);
    if (!properties) return;
    properties.get(property)?.invalidate();
    if (keysChanged) properties.get(OBJECT_KEYS)?.invalidate();
  }

  private invalidateArray(target: object) {
    const properties = this.propertySources.get(target);
    if (!properties) return;
    for (const source of properties.values()) source.invalidate();
  }

  // Validation first reads every rule through its proxy. Share the traps so
  // that scan creates one proxy per node, without a handler and six closures
  // for every offscreen object.
  private readonly proxyHandler: ProxyHandler<any> = {
    ownKeys: (tgt) => {
      this.trackProperty(tgt, OBJECT_KEYS);
      return Reflect.ownKeys(tgt);
    },
    has: (tgt, prop) => {
      this.trackProperty(tgt, prop);
      return Reflect.has(tgt, prop);
    },
    getOwnPropertyDescriptor: (tgt, prop) => {
      this.trackProperty(tgt, prop);
      return Reflect.getOwnPropertyDescriptor(tgt, prop);
    },
    get: (tgt, prop, receiver) => {
      this.trackProperty(tgt, prop);
      if (Array.isArray(tgt) && typeof prop === "string" && ARRAY_MUTATIONS.has(prop)) {
        return (...args: any[]) => {
          const rawArgs = args.map((arg) => this.detachValue(arg));
          const res = Reflect.get(tgt, prop, receiver).apply(tgt, rawArgs);
          this.checkArrayStructure(tgt);
          this.invalidateArray(tgt);
          this.notify();
          return res;
        };
      }
      const val = Reflect.get(tgt, prop, receiver);
      return typeof val === "object" && val !== null ? this.createProxy(val) : val;
    },
    set: (tgt, prop, val) => {
      const raw = this.detachValue(val);
      const present = Object.hasOwn(tgt, prop);
      if (present && Object.is(Reflect.get(tgt, prop), raw)) return true;
      const res = Reflect.set(tgt, prop, raw);
      if (!res) return false;
      if (Array.isArray(tgt)) {
        if (typeof prop === "string" && (prop === "length" || !isNaN(Number(prop))))
          this.checkArrayStructure(tgt);
        this.invalidateArray(tgt);
      } else {
        if (typeof prop === "string") this.checkObjectField(tgt, prop);
        this.invalidateProperty(tgt, prop, !present);
      }
      this.notify();
      return res;
    },
    deleteProperty: (tgt, prop) => {
      if (!Object.hasOwn(tgt, prop)) return true;
      const res = Reflect.deleteProperty(tgt, prop);
      if (!res) return false;
      if (Array.isArray(tgt)) {
        this.checkArrayStructure(tgt);
        this.invalidateArray(tgt);
      } else {
        if (typeof prop === "string") this.checkObjectField(tgt, prop);
        this.invalidateProperty(tgt, prop, true);
      }
      this.notify();
      return res;
    },
  };

  private createProxy(target: any): any {
    if (typeof target !== "object" || target === null) return target;
    if (this.proxyCache.has(target)) return this.proxyCache.get(target);

    const proxy = new Proxy(target, this.proxyHandler);
    this.proxyCache.set(target, proxy);
    this.reverseProxyCache.set(proxy, target);
    return proxy;
  }

  private checkObjectField(target: any, prop: string) {
    const id = target.id;
    if (!id || !this.originalObjects.has(id)) return;

    const originalVal = this.originalObjects.get(id)[prop];
    const currentVal = target[prop];

    let props = this.dirtyObjectProps.get(id);
    if (!sameValue(currentVal, originalVal)) {
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

    let dirty = originals.length !== array.length;
    if (!dirty) {
      for (let i = 0; i < originals.length; i++) {
        if (!sameValue(originals[i], array[i]?.id ?? array[i])) {
          dirty = true;
          break;
        }
      }
    }

    if (dirty) this.dirtyArrays.add(array);
    else this.dirtyArrays.delete(array);
  }

  private clearDirtyField(id: string, prop: string) {
    const props = this.dirtyObjectProps.get(id);
    if (!props) return;

    props.delete(prop);
    if (props.size === 0) {
      this.dirtyObjectProps.delete(id);
    }
  }

  private notify() {
    // A mutation can be invoked by a binding inside an effect. Publishing its
    // revision must not subscribe that effect to every later tracker mutation.
    this.version = ++this.revision;
  }

  private publish() {
    this.publication = ++this.publicationRevision;
    this.notify();
  }

  private removeIndexedState(node: any) {
    if (!node || typeof node !== "object") return;

    if (Array.isArray(node)) {
      this.originalArrays.delete(node);
      for (const item of node) {
        this.removeIndexedState(item);
      }
      return;
    }

    if (node.id) {
      this.originalObjects.delete(node.id);
    }

    for (const key in node) {
      if (typeof node[key] === "object") {
        this.removeIndexedState(node[key]);
      }
    }
  }

  private collectLiveState(node: any, objects: Map<string, object>, arrays: Set<object>) {
    if (!node || typeof node !== "object") return;

    if (Array.isArray(node)) {
      arrays.add(node);
      for (const item of node) {
        this.collectLiveState(item, objects, arrays);
      }
      return;
    }

    // Only dirty IDs need a live object for reconciliation. Do not build a
    // second index of every unchanged rule during a baseline update.
    if (node.id && this.dirtyObjectProps.has(node.id)) {
      objects.set(node.id, node);
    }

    for (const key in node) {
      if (typeof node[key] === "object") {
        this.collectLiveState(node[key], objects, arrays);
      }
    }
  }

  private pruneDirtyState() {
    const liveObjects = new Map<string, object>();
    const liveArrays = new Set<object>();

    this.collectLiveState(this.state, liveObjects, liveArrays);

    for (const [id, props] of this.dirtyObjectProps) {
      const target = liveObjects.get(id);
      if (!target) {
        this.dirtyObjectProps.delete(id);
        continue;
      }
      // Reconcile the final live object, which may have moved elsewhere while
      // its old parent was restored. A saved clone must not clear its edits.
      for (const prop of Array.from(props)) this.checkObjectField(target, prop);
    }

    for (const array of Array.from(this.dirtyArrays)) {
      if (!liveArrays.has(array)) {
        this.dirtyArrays.delete(array);
        continue;
      }

      this.checkArrayStructure(array);
    }
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
    const _ = this.publication;
    return this.proxy;
  }

  get isDirty() {
    const _ = this.version;
    return this.dirtyObjectProps.size > 0 || this.dirtyArrays.size > 0;
  }

  private idObjects(value: unknown): IdObject[] {
    if (
      !Array.isArray(value) ||
      !value.every(isIdObject) ||
      new Set(value.map((item) => item.id)).size !== value.length
    ) {
      throw new Error("Draft rebasing requires an array of objects with unique IDs");
    }
    return value;
  }

  private containsDirtyState(node: unknown): boolean {
    if (!node || typeof node !== "object") return false;
    if (Array.isArray(node)) {
      return this.dirtyArrays.has(node) || node.some((item) => this.containsDirtyState(item));
    }
    if (isIdObject(node) && this.dirtyObjectProps.has(node.id)) return true;
    return Object.values(node).some((value) => this.containsDirtyState(value));
  }

  /** Root objects affected by a local edit, including nested rules and membership.
   * Reordering the root collection alone does not make every object a conflict. */
  get dirtyRootIds(): ReadonlySet<string> {
    const _ = this.version;
    const items = this.idObjects(this.state);
    const originalIds = new Set(this.originalArrays.get(this.state) ?? []);
    const currentIds = new Set(items.map((item) => item.id));
    const dirty = new Set<string>();
    for (const item of items) {
      if (!originalIds.has(item.id) || this.containsDirtyState(item)) dirty.add(item.id);
    }
    for (const id of originalIds) {
      if (typeof id === "string" && !currentIds.has(id)) dirty.add(id);
    }
    return dirty;
  }

  /** Replay a retained draft on the freshly indexed server object. Keeping its
   * existing rule arrays/objects lets normal edits and reversions keep working. */
  private replayDraft(target: IdObject, draft: IdObject) {
    for (const key of new Set([...Object.keys(target), ...Object.keys(draft)])) {
      const current = target[key];
      const desired = draft[key];
      if (sameValue(current, desired)) continue;
      if (Array.isArray(current) && Array.isArray(desired)) {
        if (current.every(isIdObject) && desired.every(isIdObject)) {
          const byId = new Map(current.map((item) => [item.id, item]));
          const next = desired.map((item) => {
            const existing = byId.get(item.id);
            return existing ?? structuredClone(this.originalObjects.get(item.id) ?? item);
          });
          for (let index = 0; index < next.length; index++) current[index] = next[index];
          current.length = next.length;
          for (let index = 0; index < desired.length; index++) {
            const item = desired[index];
            const restored = this.originalObjects.get(item.id);
            if (!byId.has(item.id) && restored) this.indexAndLink(current[index], restored);
            this.replayDraft(current[index], item);
          }
        } else {
          const next = structuredClone(desired);
          for (let index = 0; index < next.length; index++) current[index] = next[index];
          current.length = next.length;
        }
        this.checkArrayStructure(current);
        if (this.dirtyObjectProps.get(target.id)?.has(key)) this.checkObjectField(target, key);
      } else {
        if (Object.hasOwn(draft, key)) target[key] = structuredClone(desired);
        else delete target[key];
        this.checkObjectField(target, key);
      }
    }
  }

  /** Replace the editable collection without accepting it as saved. Reusing
   * indexed objects preserves field-level changes even when IDs stay the same. */
  replaceDraft(newData: T) {
    const incoming = this.idObjects(structuredClone(newData));
    const target = this.idObjects(this.state);
    const current = new Map(target.map((item) => [item.id, item]));
    const next = incoming.map(
      (item) => current.get(item.id) ?? structuredClone(this.originalObjects.get(item.id) ?? item),
    );
    for (let index = 0; index < next.length; index++) target[index] = next[index];
    target.length = next.length;
    for (let index = 0; index < incoming.length; index++) {
      const item = incoming[index];
      const restored = this.originalObjects.get(item.id);
      if (!current.has(item.id) && restored) this.indexAndLink(target[index], restored);
      this.replayDraft(target[index], item);
    }
    this.checkArrayStructure(target);
    this.pruneDirtyState();
    this.publish();
  }

  /** Accept an imported server baseline without saving unrelated local drafts.
   * The caller must reject imports that also edit dirtyRootIds. This deliberately
   * retains whole dirty roots rather than guessing how conflicting edits merge. */
  rebase(newData: T) {
    const incoming = this.idObjects(structuredClone(newData));
    const local = this.idObjects($state.snapshot(this.state));
    const dirty = this.dirtyRootIds;
    const originalIds = new Set(this.originalArrays.get(this.state) ?? []);
    const localById = new Map(local.map((item) => [item.id, item]));
    const incomingIds = new Set(incoming.map((item) => item.id));
    const deleted = new Set([...dirty].filter((id) => !localById.has(id)));
    for (const id of dirty) {
      if (originalIds.has(id) && localById.has(id) && !incomingIds.has(id)) {
        throw new Error(`Cannot rebase a local draft removed from the server: ${id}`);
      }
    }

    const orderedIds = this.dirtyArrays.has(this.state)
      ? local
          .filter((item) => dirty.has(item.id) || incomingIds.has(item.id))
          .map((item) => item.id)
      : incoming.filter((item) => !deleted.has(item.id)).map((item) => item.id);
    const included = new Set(orderedIds);
    for (const item of incoming) {
      if (!included.has(item.id) && !deleted.has(item.id)) {
        orderedIds.push(item.id);
        included.add(item.id);
      }
    }

    // Clone before reset: replaying local fields must never change the caller's
    // accepted server response or the store's independent save baseline.
    this.reset(incoming as unknown as T);
    const target = this.idObjects(this.state);
    const byId = new Map(target.map((item) => [item.id, item]));
    const next = orderedIds.map((id) => {
      const saved = byId.get(id);
      const draft = dirty.has(id) ? localById.get(id) : undefined;
      if (!saved) return draft!;
      if (draft) this.replayDraft(saved, draft);
      return saved;
    });
    for (let index = 0; index < next.length; index++) target[index] = next[index];
    target.length = next.length;
    this.checkArrayStructure(target);
    this.publish();
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
    const detached = structuredClone(newData);
    const snapshot = structuredClone(detached);

    if (Array.isArray(this.state) && Array.isArray(detached)) {
      this.state.length = 0;
      for (const item of detached) this.state.push(item);
    } else {
      const s = this.state as any;
      Object.keys(s).forEach((k) => delete s[k]);
      Object.assign(s, detached);
    }

    this.init(snapshot);
    this.proxy = this.createProxy(this.state);
    this.publish();
  }

  acknowledgeUpdate(object: any, persistedFields?: Record<string, any>) {
    if (!object || !object.id) return;
    const target = this.reverseProxyCache.get(object) || object;
    const originalSnapshot = this.originalObjects.get(object.id);
    const fieldEntries = Object.entries(persistedFields ?? {});

    if (fieldEntries.length === 0) {
      if (originalSnapshot) {
        this.removeIndexedState(originalSnapshot);
      }

      this.clearDirtyStateFor(target);

      const snapshot = $state.snapshot(target);
      this.indexAndLink(target, snapshot);
      this.pruneDirtyState();
      this.notify();
      return;
    }

    const nextOriginalSnapshot = structuredClone(originalSnapshot ?? { id: object.id });

    for (const [field, value] of fieldEntries) {
      const previousValue = target[field];
      this.clearDirtyStateFor(previousValue);

      if (originalSnapshot) {
        this.removeIndexedState(originalSnapshot[field]);
      }

      target[field] = structuredClone(value);
    }

    const snapshot = $state.snapshot(target);

    nextOriginalSnapshot.id = snapshot.id;

    for (const [field] of fieldEntries) {
      nextOriginalSnapshot[field] = snapshot[field];
    }

    this.originalObjects.set(object.id, nextOriginalSnapshot);

    for (const [field] of fieldEntries) {
      this.indexAndLink(target[field], snapshot[field]);
      this.clearDirtyStateFor(target[field]);
      this.clearDirtyField(object.id, field);
    }

    this.pruneDirtyState();
    this.publish();
  }

  acknowledgeNewItem(array: any[], item: any, position: "start" | "end" = "end") {
    const targetArray = this.reverseProxyCache.get(array) || array;
    if (!this.originalArrays.has(targetArray)) return;
    if (!item || !item.id) return;

    const originalIds = this.originalArrays.get(targetArray)!;
    const snapshot = $state.snapshot(item);
    const targetItem =
      (Array.isArray(targetArray)
        ? targetArray.find((entry: unknown) => isIdObject(entry) && entry.id === item.id)
        : undefined) ??
      this.reverseProxyCache.get(item) ??
      item;

    this.originalObjects.set(item.id, snapshot);
    this.indexAndLink(targetItem, snapshot);

    if (position === "start") {
      originalIds.unshift(item.id);
    } else {
      originalIds.push(item.id);
    }

    this.clearDirtyStateFor(snapshot);

    this.checkArrayStructure(targetArray);
    this.notify();
  }

  acknowledgeDelete(array: any[], itemId: string) {
    const targetArray = this.reverseProxyCache.get(array) || array;
    if (!this.originalArrays.has(targetArray)) return;

    const originalIds = this.originalArrays.get(targetArray)!;

    const index = originalIds.indexOf(itemId);
    if (index !== -1) {
      originalIds.splice(index, 1);
    }

    if (this.dirtyObjectProps.has(itemId)) {
      this.dirtyObjectProps.delete(itemId);
    }

    if (this.originalObjects.has(itemId)) {
      const originalSnapshot = this.originalObjects.get(itemId);
      this.clearDirtyStateFor(originalSnapshot);
    }

    if (this.originalObjects.has(itemId)) {
      this.originalObjects.delete(itemId);
    }

    this.checkArrayStructure(targetArray);
    this.notify();
  }

  private clearDirtyStateFor(node: any) {
    if (!node || typeof node !== "object") return;

    if (Array.isArray(node)) {
      this.dirtyArrays.delete(node);
      for (const item of node) {
        this.clearDirtyStateFor(item);
      }
    } else {
      if (node.id) {
        this.dirtyObjectProps.delete(node.id);
      }
      for (const key in node) {
        if (typeof node[key] === "object") {
          this.clearDirtyStateFor(node[key]);
        }
      }
    }
  }
}
