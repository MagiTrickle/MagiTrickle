/** Yield between bounded pieces of immutable JSON work. MessageChannel avoids
 * the nested setTimeout clamp in browsers, including Safari. */
export class CooperativeBudget {
  private deadline: number;
  private operations = 0;
  private channel: MessageChannel | undefined;
  constructor(
    private readonly milliseconds = 4,
    private readonly signal?: AbortSignal,
  ) {
    this.deadline = performance.now() + milliseconds;
    if (typeof MessageChannel !== "undefined") this.channel = new MessageChannel();
  }
  checkpoint(forceCheck = false): Promise<void> | undefined {
    if (this.signal?.aborted) throw new DOMException("Preparation cancelled", "AbortError");
    if ((!forceCheck && ++this.operations % 64) || performance.now() < this.deadline) return;
    return new Promise<void>((resolve) => {
      const resume = () => {
        this.deadline = performance.now() + this.milliseconds;
        resolve();
      };
      if (this.channel) {
        this.channel.port1.onmessage = resume;
        this.channel.port2.postMessage(null);
      } else setTimeout(resume, 0);
    });
  }
  close() {
    this.channel?.port1.close();
    this.channel?.port2.close();
  }
}

/** Inputs to import preparation are immutable JSON snapshots. Clone nested
 * arrays cooperatively too: a single group can itself contain 100k rules. */
export async function cloneCooperatively<T>(value: T, budget: CooperativeBudget): Promise<T> {
  const seen = new WeakMap<object, object>();
  const descriptor: PropertyDescriptor = {
    value: undefined,
    enumerable: true,
    configurable: true,
    writable: true,
  };
  async function visit(source: unknown): Promise<unknown> {
    if (!source || typeof source !== "object") return source;
    const prior = seen.get(source);
    if (prior) return prior;
    if (Array.isArray(source)) {
      const target: unknown[] = [];
      seen.set(source, target);
      for (const item of source) {
        target.push(item && typeof item === "object" ? await visit(item) : item);
        const pause = budget.checkpoint();
        if (pause) await pause;
      }
      return target;
    }
    const prototype = Object.getPrototypeOf(source);
    if (prototype !== Object.prototype && prototype !== null) return structuredClone(source);
    const target: Record<string, unknown> = Object.create(prototype);
    seen.set(source, target);
    for (const key of Object.keys(source)) {
      const item = (source as Record<string, unknown>)[key];
      // Nested visits reuse this descriptor. Assign after they finish; the
      // synchronous definition copies it before another visit can change it.
      descriptor.value = item && typeof item === "object" ? await visit(item) : item;
      Object.defineProperty(target, key, descriptor);
      const pause = budget.checkpoint();
      if (pause) await pause;
    }
    return target;
  }
  // Like structuredClone, this preserves the input's public shape and type.
  return (await visit(value)) as T;
}
