import type { Rule } from "../../types";
import {
  RULE_LIST_CHUNK_PARTS,
  RULE_LIST_TEXT_LIMIT,
  type RuleListJob,
  type RuleListResponse,
  type RuleListResult,
} from "./rule-list-parser";

type ParserWorker = Pick<Worker, "onmessage" | "onerror" | "postMessage" | "terminate">;
type Pending = {
  id: number;
  job: RuleListJob;
  resolve: (result: RuleListResult) => void;
  reject: (error: Error) => void;
  sequence: number;
  rules?: Rule[];
  count: number;
  partial?: { id: string; type: Rule["type"]; fragments: string[] };
};

/** Owns one cancellable parser session. Only a complete result escapes this
 * client; bounded messages arrive on demand, without flattening the full batch. */
export class RuleListParserClient {
  private worker?: ParserWorker;
  private pending?: Pending;
  private nextRequest = 0;

  constructor(private readonly createWorker: () => ParserWorker) {}

  request(job: RuleListJob): Promise<RuleListResult> {
    return new Promise((resolve, reject) => {
      if (this.pending) {
        reject(new Error("Rule list parser is already busy"));
        return;
      }
      const id = ++this.nextRequest;
      this.pending = { id, job, resolve, reject, sequence: 0, count: 0 };
      try {
        if (!this.worker) {
          const worker = this.createWorker();
          this.worker = worker;
          worker.onmessage = (event: MessageEvent<RuleListResponse>) => {
            if (this.worker === worker) this.receive(event.data);
          };
          worker.onerror = (event) => {
            if (this.worker !== worker) return;
            event.preventDefault();
            this.dispose(new Error(event.message || "Rule list parsing failed"));
          };
        }
        this.worker.postMessage({ ...job, id });
      } catch (error) {
        this.dispose(error instanceof Error ? error : new Error(String(error)));
      }
    });
  }

  dispose(error: Error = new DOMException("Rule list parsing cancelled", "AbortError")) {
    this.worker?.terminate();
    this.worker = undefined;
    this.pending?.reject(error);
    this.pending = undefined;
  }

  private receive(response: RuleListResponse) {
    const pending = this.pending;
    if (!pending || response.id !== pending.id) return;
    try {
      if (response.version !== pending.job.version)
        throw new Error("Rule list changed during import");
      if (response.type === "error") throw new Error(response.message);
      if (pending.job.type === "parse" && response.type === "preview") {
        this.pending = undefined;
        pending.resolve(response);
        return;
      }
      if (pending.job.type !== "apply" || response.type !== "chunk")
        throw new Error("Unexpected rule list response");
      if (
        response.sequence !== pending.sequence ||
        !Number.isSafeInteger(response.total) ||
        response.total < 0 ||
        response.parts.length > RULE_LIST_CHUNK_PARTS
      )
        throw new Error("Invalid rule list chunk");
      pending.rules ??= new Array<Rule>(response.total);
      if (response.total !== pending.rules.length)
        throw new Error("Rule list size changed during import");
      let textLength = 0;
      for (const part of response.parts) {
        textLength += part.rule.length;
        if (textLength > RULE_LIST_TEXT_LIMIT || pending.count >= response.total)
          throw new Error("Invalid rule list chunk size");
        let pattern = part.rule;
        if (pending.partial) {
          const partial = pending.partial;
          if (partial.id !== part.id || partial.type !== part.type)
            throw new Error("Incomplete rule list pattern");
          partial.fragments.push(pattern);
          if (part.complete) {
            // An exceptional long pattern needs one full string, but never
            // repeatedly copy its prefix or concatenate the entire rule list.
            pattern = partial.fragments.join("");
            pending.partial = undefined;
          }
        } else if (!part.complete) {
          pending.partial = { id: part.id, type: part.type, fragments: [pattern] };
        }
        if (part.complete)
          pending.rules[pending.count++] = {
            id: part.id,
            name: "",
            rule: pattern,
            type: part.type,
            enable: true,
          };
      }
      if (response.done) {
        if (pending.partial || pending.count !== response.total)
          throw new Error("Incomplete rule list transfer");
        this.pending = undefined;
        pending.resolve({
          id: response.id,
          version: response.version,
          type: "rules",
          rules: pending.rules,
        });
      } else {
        if (!response.parts.length || pending.count === response.total)
          throw new Error("Invalid rule list continuation");
        // Pulling only after assembly provides backpressure and returns to the
        // event loop between chunks, instead of queueing the entire import.
        this.worker!.postMessage({
          type: "next",
          id: response.id,
          version: response.version,
          sequence: ++pending.sequence,
        });
      }
    } catch (error) {
      this.dispose(error instanceof Error ? error : new Error(String(error)));
    }
  }
}
