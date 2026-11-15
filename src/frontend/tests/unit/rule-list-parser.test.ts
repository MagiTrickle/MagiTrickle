import { strict as assert } from "node:assert";

import {
  boundedRuleListLines,
  buildImportedRules,
  importedRuleChunks,
  parseRuleList,
  RULE_LIST_CHUNK_PARTS,
  RULE_LIST_TEXT_LIMIT,
  RuleListParserSession,
  ruleListPreview,
} from "../../src/modules/groups/rule-list-parser";

Deno.test(
  "rule list parsing retains separators, detection order, comments and invalid preview lines",
  () => {
    const parsed = parseRuleList(
      " # comment\nexample.test,192.168.1.1\n2001:db8::/32\n/regex/\n[\n",
      "auto",
    );
    assert.deepEqual(parsed.entries, [
      { rule: "example.test", type: "namespace" },
      { rule: "192.168.1.1", type: "subnet" },
      { rule: "2001:db8::/32", type: "subnet6" },
      { rule: "/regex/", type: "regex" },
    ]);
    assert.deepEqual(parsed.stats, { namespace: 1, subnet: 1, subnet6: 1, regex: 1, INVALID: 1 });
    assert.equal(parsed.lines[0].type, null);
    assert.equal(parsed.lines[5].type, "INVALID");
    assert.equal(parsed.lines[6].type, null);
    assert.equal(parsed.totalLines, 7);
  },
);

Deno.test(
  "explicit types validate separately and deduplicate by trimmed type and pattern in source order",
  () => {
    const text = " example.test\nexample.test\n*.example.test\n[";
    const namespaces = parseRuleList(text, "namespace");
    assert.deepEqual(namespaces.entries, [{ rule: "example.test", type: "namespace" }]);
    assert.deepEqual(namespaces.stats, { namespace: 2, INVALID: 2 });
    const wildcards = parseRuleList(text, "wildcard");
    assert.deepEqual(wildcards.entries, [
      { rule: "example.test", type: "wildcard" },
      { rule: "*.example.test", type: "wildcard" },
    ]);
    assert.deepEqual(wildcards.stats, { wildcard: 3, INVALID: 1 });
  },
);

Deno.test(
  "100k rule previews stay bounded while complete imported rules remain unique and ordered",
  () => {
    const text = Array.from({ length: 100000 }, (_, index) => `item${index}.example.test`).join(
      "\n",
    );
    const parsed = parseRuleList(text, "auto");
    const preview = ruleListPreview(parsed);
    assert.equal(preview.lines.length, 500);
    assert.equal(preview.totalLines, 100000);
    assert.deepEqual(preview.stats, { namespace: 100000 });
    assert.equal(Object.hasOwn(preview, "entries"), false);
    let id = 0x10000000;
    const rules = buildImportedRules(parsed, () => (id++).toString(16));
    assert.equal(rules.length, 100000);
    assert.equal(new Set(rules.map((rule) => rule.id)).size, 100000);
    assert.equal(rules[0].rule, "item0.example.test");
    assert.equal(rules.at(-1)!.rule, "item99999.example.test");
    assert.equal(
      rules.every((rule) => rule.enable && rule.name === "" && rule.type === "namespace"),
      true,
    );
    assert.equal(Object.hasOwn(parsed.entries[0], "id"), false);
  },
);

Deno.test("imported rule IDs retry collisions before exposing a duplicate identity", () => {
  const ids = ["11223344", "11223344", "22334455"];
  const rules = buildImportedRules(
    parseRuleList("first.test\nsecond.test", "auto"),
    () => ids.shift()!,
  );
  assert.deepEqual(
    rules.map((rule) => rule.id),
    ["11223344", "22334455"],
  );
});

Deno.test("editor line counting stops at its visible height limit and ignores empty tokens", () => {
  assert.equal(boundedRuleListLines("", 20), 1);
  assert.equal(boundedRuleListLines(" \n, \n first.test,second.test\n", 20), 2);
  assert.equal(
    boundedRuleListLines(Array.from({ length: 100000 }, () => "example.test").join("\n"), 20),
    20,
  );
});

Deno.test("100k imported rules are generated only as ordered bounded chunks are pulled", () => {
  const count = 100000;
  const parsed = parseRuleList(
    Array.from({ length: count }, (_, index) => `item${index}.example.test`).join("\n"),
    "namespace",
  );
  let allocated = 0;
  const chunks = importedRuleChunks(parsed, () => (allocated++).toString(16).padStart(8, "0"));
  assert.equal(allocated, 0);
  let received = 0;
  let finished = false;
  const ids = new Set<string>();
  for (const chunk of chunks) {
    assert.equal(finished, false);
    assert.equal(chunk.total, count);
    assert.ok(chunk.parts.length <= RULE_LIST_CHUNK_PARTS);
    assert.ok(
      chunk.parts.reduce((length, part) => length + part.rule.length, 0) <= RULE_LIST_TEXT_LIMIT,
    );
    for (const part of chunk.parts) {
      assert.equal(part.complete, true);
      assert.equal(part.rule, `item${received++}.example.test`);
      assert.equal(part.type, "namespace");
      assert.equal(ids.has(part.id), false);
      ids.add(part.id);
    }
    // A pull does not allocate IDs or objects for the rest of the batch.
    assert.equal(allocated, received);
    finished = chunk.done;
  }
  assert.equal(finished, true);
  assert.equal(received, count);
});

Deno.test(
  "long patterns bound both preview text and transfer payload without truncating data",
  () => {
    const pattern = `${"x".repeat(RULE_LIST_TEXT_LIMIT - 1)}😀${"z".repeat(RULE_LIST_TEXT_LIMIT)}`;
    const parsed = parseRuleList(`${pattern}\nshort`, "regex");
    assert.equal(parsed.entries[0].rule, pattern);
    assert.equal(parsed.lines.length, 1);
    assert.equal(parsed.lines[0].text.length, RULE_LIST_TEXT_LIMIT);
    assert.equal(parsed.lines[0].text.at(-1), "…");
    assert.equal(parsed.totalLines, 2);
    assert.deepEqual(parsed.stats, { regex: 2 });
    let id = 0;
    const chunks = [...importedRuleChunks(parsed, () => String(id++))];
    assert.equal(chunks.length, 3);
    assert.ok(chunks.every((chunk) => chunk.parts.length <= RULE_LIST_CHUNK_PARTS));
    assert.ok(
      chunks.every(
        (chunk) =>
          chunk.parts.reduce((length, part) => length + part.rule.length, 0) <=
          RULE_LIST_TEXT_LIMIT,
      ),
    );
    const fragments = chunks.flatMap((chunk) => chunk.parts).filter((part) => part.id === "0");
    assert.equal(fragments.map((part) => part.rule).join(""), pattern);
    assert.deepEqual(
      fragments.map((part) => part.complete),
      [false, false, true],
    );
    assert.deepEqual(
      chunks.map((chunk) => chunk.done),
      [false, false, true],
    );
  },
);

Deno.test(
  "ID collision reservations survive chunk boundaries and empty Apply completes once",
  () => {
    const parsed = parseRuleList(
      Array.from({ length: RULE_LIST_CHUNK_PARTS + 1 }, (_, index) => `item${index}.test`).join(
        "\n",
      ),
      "namespace",
    );
    let allocated = 0;
    const chunks = importedRuleChunks(parsed, () => {
      const index = allocated++;
      return String(index === RULE_LIST_CHUNK_PARTS ? 0 : index);
    });
    const first = chunks.next().value!;
    const last = chunks.next().value!;
    assert.equal(first.parts.length, RULE_LIST_CHUNK_PARTS);
    assert.equal(first.done, false);
    assert.equal(last.parts.length, 1);
    assert.equal(last.parts[0].id, String(RULE_LIST_CHUNK_PARTS + 1));
    assert.equal(last.done, true);
    assert.equal(chunks.next().done, true);
    assert.deepEqual(
      [...importedRuleChunks(parseRuleList("# comment", "auto"))],
      [{ parts: [], total: 0, done: true }],
    );
  },
);

Deno.test("worker session rejects stale versions, changed requests and out-of-order pulls", () => {
  for (const invalid of [
    { id: 3, version: 1, sequence: 1 },
    { id: 2, version: 2, sequence: 1 },
    { id: 2, version: 1, sequence: 0 },
    { id: 2, version: 1, sequence: 2 },
  ]) {
    const session = new RuleListParserSession();
    assert.equal(session.handle({ type: "apply", id: 1, version: 1 }).type, "error");
    const preview = session.handle({
      type: "parse",
      id: 1,
      version: 1,
      ruleType: "namespace",
      text: Array.from({ length: 300 }, (_, index) => `item${index}.test`).join("\n"),
    });
    assert.equal(preview.type, "preview");
    const first = session.handle({ type: "apply", id: 2, version: 1 });
    assert.equal(first.type, "chunk");
    assert.equal(session.handle({ type: "next", ...invalid }).type, "error");
    assert.equal(session.handle({ type: "next", id: 2, version: 1, sequence: 1 }).type, "error");
    const retry = session.handle({ type: "apply", id: 4, version: 1 });
    assert.equal(retry.type, "chunk");
    session.handle({ type: "parse", id: 5, version: 2, ruleType: "auto", text: "new.test" });
    assert.equal(session.handle({ type: "next", id: 4, version: 1, sequence: 1 }).type, "error");
    assert.equal(session.handle({ type: "apply", id: 6, version: 1 }).type, "error");
    const current = session.handle({ type: "apply", id: 7, version: 2 });
    assert.equal(current.type, "chunk");
    if (current.type !== "chunk") throw new Error("Expected chunk");
    assert.equal(current.done, true);
    assert.equal(current.parts[0].rule, "new.test");
  }
});
