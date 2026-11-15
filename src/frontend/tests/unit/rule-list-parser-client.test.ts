import { strict as assert } from "node:assert";

import {
  RULE_LIST_TEXT_LIMIT,
  RuleListParserSession,
  type RuleListRequest,
  type RuleListResponse,
} from "../../src/modules/groups/rule-list-parser";
import { RuleListParserClient } from "../../src/modules/groups/rule-list-parser-client";

class ControlledWorker {
  onmessage: ((event: MessageEvent<RuleListResponse>) => void) | null = null;
  onerror: ((event: ErrorEvent) => void) | null = null;
  requests: RuleListRequest[] = [];
  terminated = false;
  private session = new RuleListParserSession();

  postMessage(request: RuleListRequest) {
    this.requests.push(request);
  }

  terminate() {
    this.terminated = true;
  }

  respond(map: (response: RuleListResponse) => RuleListResponse = (response) => response) {
    const request = this.requests.shift();
    assert.ok(request);
    const response = map(this.session.handle(request));
    this.onmessage?.(new MessageEvent("message", { data: response }));
    return response;
  }
}

async function prepared(text = "first.test\nsecond.test") {
  const workers: ControlledWorker[] = [];
  const client = new RuleListParserClient(() => {
    const worker = new ControlledWorker();
    workers.push(worker);
    return worker;
  });
  const preview = client.request({ type: "parse", version: 1, text, ruleType: "auto" });
  workers[0].respond();
  assert.equal((await preview).type, "preview");
  return { client, workers, worker: workers[0] };
}

Deno.test(
  "client pulls one bounded chunk at a time and exposes every rule only when complete",
  async () => {
    const count = 100000;
    const { client, worker } = await prepared(
      Array.from({ length: count }, (_, index) => `item${index}.test`).join("\n"),
    );
    let published = false;
    const result = client.request({ type: "apply", version: 1 }).then((response) => {
      published = true;
      return response;
    });
    let messages = 0;
    let done = false;
    while (!done) {
      assert.equal(worker.requests.length, 1);
      const response = worker.respond();
      assert.equal(response.type, "chunk");
      if (response.type !== "chunk") throw new Error("Expected chunk");
      done = response.done;
      messages++;
      await Promise.resolve();
      assert.equal(published, done);
    }
    assert.ok(messages > 1);
    assert.equal(worker.requests.length, 0);
    const complete = await result;
    assert.equal(complete.type, "rules");
    if (complete.type !== "rules") throw new Error("Expected rules");
    assert.equal(complete.rules.length, count);
    assert.equal(new Set(complete.rules.map((rule) => rule.id)).size, count);
    for (let index = 0; index < count; index++) {
      const rule = complete.rules[index];
      assert.equal(rule.rule, `item${index}.test`);
      assert.equal(rule.name, "");
      assert.equal(rule.type, "namespace");
      assert.equal(rule.enable, true);
    }
    client.dispose();
  },
);

Deno.test(
  "client reassembles long patterns including split surrogate pairs exactly once",
  async () => {
    const pattern = `${"x".repeat(RULE_LIST_TEXT_LIMIT - 1)}😀${"z".repeat(RULE_LIST_TEXT_LIMIT)}`;
    const { client, worker } = await prepared(`${pattern}\nexample.test`);
    const result = client.request({ type: "apply", version: 1 });
    let messages = 0;
    while (worker.requests.length) {
      worker.respond();
      messages++;
    }
    assert.equal(messages, 3);
    const complete = await result;
    assert.equal(complete.type, "rules");
    if (complete.type !== "rules") throw new Error("Expected rules");
    assert.equal(complete.rules.length, 2);
    assert.equal(complete.rules[0].rule, pattern);
    assert.equal(complete.rules[1].rule, "example.test");
    client.dispose();
  },
);

Deno.test(
  "cancel drops partial rules and late worker replies cannot affect a reopened parser",
  async () => {
    const { client, worker, workers } = await prepared(
      Array.from({ length: 300 }, (_, index) => `item${index}.test`).join("\n"),
    );
    const result = client.request({ type: "apply", version: 1 });
    worker.respond();
    assert.equal(worker.requests.length, 1);
    const rejected = assert.rejects(result, { name: "AbortError" });
    client.dispose();
    await rejected;
    assert.equal(worker.terminated, true);
    const reopened = client.request({
      type: "parse",
      version: 1,
      ruleType: "namespace",
      text: "new.test",
    });
    assert.ok(workers[1].requests[0].id > worker.requests[0].id);
    worker.respond();
    assert.equal(workers[1].requests.length, 1);
    assert.equal(workers[1].terminated, false);
    workers[1].respond();
    assert.equal((await reopened).type, "preview");
    const next = client.request({ type: "apply", version: 1 });
    workers[1].respond();
    const complete = await next;
    assert.equal(complete.type, "rules");
    if (complete.type !== "rules") throw new Error("Expected rules");
    assert.deepEqual(
      complete.rules.map((rule) => rule.rule),
      ["new.test"],
    );
    client.dispose();
  },
);

Deno.test(
  "client rejects stale, reordered, oversized and incomplete chunks without exposing rules",
  async () => {
    const invalid: ((
      response: Extract<RuleListResponse, { type: "chunk" }>,
    ) => RuleListResponse)[] = [
      (response) => ({ ...response, version: 2 }),
      (response) => ({ ...response, sequence: 1 }),
      (response) => ({ ...response, total: response.total + 1 }),
      (response) => ({ ...response, parts: response.parts.slice(0, 1) }),
      (response) => ({ ...response, parts: [], done: false }),
      (response) => ({
        ...response,
        parts: [{ ...response.parts[0], rule: "x".repeat(RULE_LIST_TEXT_LIMIT + 1) }],
      }),
      (response) => ({
        ...response,
        parts: [{ ...response.parts[0], complete: false }],
      }),
    ];
    for (const change of invalid) {
      const { client, worker } = await prepared();
      const result = client.request({ type: "apply", version: 1 });
      const rejected = assert.rejects(result);
      worker.respond((response) => {
        if (response.type !== "chunk") throw new Error("Expected chunk");
        return change(response);
      });
      await rejected;
      assert.equal(worker.terminated, true);
      assert.equal(worker.requests.length, 0);
    }
  },
);

Deno.test(
  "client rejects duplicate sequences and changed totals after a partial transfer",
  async () => {
    for (const field of ["sequence", "total"] as const) {
      const { client, worker } = await prepared(
        Array.from({ length: 300 }, (_, index) => `item${index}.test`).join("\n"),
      );
      const result = client.request({ type: "apply", version: 1 });
      worker.respond();
      const rejected = assert.rejects(result);
      worker.respond((response) => {
        if (response.type !== "chunk") throw new Error("Expected chunk");
        return { ...response, [field]: response[field] - 1 };
      });
      await rejected;
      assert.equal(worker.terminated, true);
      assert.equal(worker.requests.length, 0);
    }
  },
);

Deno.test("worker failures reject a pending import and permit a new parser session", async () => {
  const { client, worker, workers } = await prepared();
  const result = client.request({ type: "apply", version: 1 });
  const rejected = assert.rejects(result, /worker failed/);
  let prevented = false;
  worker.onerror?.({
    message: "worker failed",
    preventDefault: () => {
      prevented = true;
    },
  } as ErrorEvent);
  await rejected;
  assert.equal(prevented, true);
  assert.equal(worker.terminated, true);
  const preview = client.request({ type: "parse", version: 2, text: "", ruleType: "auto" });
  workers[1].respond();
  assert.equal((await preview).type, "preview");
  const empty = client.request({ type: "apply", version: 2 });
  workers[1].respond();
  assert.deepEqual(await empty, { id: 4, version: 2, type: "rules", rules: [] });
  client.dispose();
});
