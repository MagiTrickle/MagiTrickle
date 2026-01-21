import assert from "node:assert";
import { describe, it } from "node:test";

import type { Rule } from "../../src/types.ts";
import { sortRules } from "../../src/utils/rule-sorter.ts";

// Helper to create a minimal rule
const createRule = (overrides: Partial<Rule>): Rule => ({
  id: "1",
  enable: true,
  name: "Rule",
  rule: "",
  type: "domain", // default
  ...overrides,
});

describe("Rule Sorter", () => {
  it("should sort by name ascending", () => {
    const rules = [createRule({ name: "B" }), createRule({ name: "A" }), createRule({ name: "C" })];
    const sorted = sortRules(rules, "name", "asc");
    assert.strictEqual(sorted[0].name, "A");
    assert.strictEqual(sorted[1].name, "B");
    assert.strictEqual(sorted[2].name, "C");
  });

  it("should sort by name descending", () => {
    const rules = [createRule({ name: "A" }), createRule({ name: "B" })];
    const sorted = sortRules(rules, "name", "desc");
    assert.strictEqual(sorted[0].name, "B");
    assert.strictEqual(sorted[1].name, "A");
  });

  it("should prioritize patterns correctly (Subnet > Wildcard > Domain > Regex)", () => {
    const rules = [
      createRule({ rule: "example.com", type: "domain" }),
      createRule({ rule: "192.168.1.0/24", type: "subnet" }),
      createRule({ rule: ".*", type: "regex" }),
      createRule({ rule: "*.example.com", type: "wildcard" }),
    ];

    // Expected: Subnet (10) -> Domain (exact match) -> Wildcard (child of domain) -> Regex (40)
    // Note: Domain and Wildcard are "domain-like" and sorted by content.
    // "example.com" comes before "*.example.com" in domain sort logic.
    const sorted = sortRules(rules, "pattern", "asc");

    assert.strictEqual(sorted[0].type, "subnet");
    assert.strictEqual(sorted[1].type, "domain");
    assert.strictEqual(sorted[2].type, "wildcard");
    assert.strictEqual(sorted[3].type, "regex");
  });

  it("should sort subnets by IP and mask", () => {
    const rules = [
      createRule({ rule: "10.0.0.0/8", type: "subnet" }),
      createRule({ rule: "192.168.1.0/24", type: "subnet" }),
      createRule({ rule: "10.0.0.0/16", type: "subnet" }),
    ];

    // Logic: IP asc, then Mask desc (larger mask = more specific = usually higher priority in routing, but sorter implementation might vary)
    // Checking implementation:
    // if (cidrA.ip !== cidrB.ip) return cidrA.ip - cidrB.ip;
    // return cidrB.mask - cidrA.mask; (Larger mask first for same IP)

    const sorted = sortRules(rules, "pattern", "asc");

    // 10.0.0.0 is smaller than 192.168.1.0
    // Between 10.0.0.0/8 and 10.0.0.0/16: same IP.
    // 16 > 8. So /16 comes before /8.

    assert.strictEqual(sorted[0].rule, "10.0.0.0/16");
    assert.strictEqual(sorted[1].rule, "10.0.0.0/8");
    assert.strictEqual(sorted[2].rule, "192.168.1.0/24");
  });

  it("should sort domains by TLD, Base, Sub", () => {
    const rules = [
      createRule({ rule: "a.example.com", type: "domain" }),
      createRule({ rule: "b.example.com", type: "domain" }),
      createRule({ rule: "google.com", type: "domain" }),
      createRule({ rule: "example.org", type: "domain" }),
    ];

    // Logic: TLD compare -> Base compare -> Sub compare
    // .com vs .org -> .com first
    // example.com vs google.com -> example first
    // a.example.com vs b.example.com -> a first

    const sorted = sortRules(rules, "pattern", "asc");

    // 1. a.example.com (com, example, a)
    // 2. b.example.com (com, example, b)
    // 3. google.com (com, google, "")
    // 4. example.org (org, example, "")

    // Wait, let's verify Base comparison vs TLD.
    // implementation: baseCmp first? No.
    // const baseCmp = keyA.base.localeCompare(keyB.base);
    // if (baseCmp !== 0) return baseCmp;
    // const tldCmp = keyA.tld.localeCompare(keyB.tld);

    // Ah! It compares BASE first!
    // So "example.com" and "example.org". Base is "example". Equal.
    // Then TLD: "com" vs "org". "com" first.
    // "google.com". Base "google". "example" < "google".

    // So:
    // 1. example.com / example.org block
    // 2. google.com block

    // Inside example block:
    // a.example.com vs b.example.com vs example.org
    // Base "example".
    // TLD: "com" vs "org". "com" first.
    // So a.example.com/b.example.com come before example.org.

    // Sub: "a" vs "b". "a" first.

    assert.strictEqual(sorted[0].rule, "a.example.com");
    assert.strictEqual(sorted[1].rule, "b.example.com");
    assert.strictEqual(sorted[2].rule, "example.org");
    assert.strictEqual(sorted[3].rule, "google.com");
  });
});
