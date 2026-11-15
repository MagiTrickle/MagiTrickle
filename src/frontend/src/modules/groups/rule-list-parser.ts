import type { Rule } from "../../types";
import {
  isValidDomain,
  isValidNamespace,
  isValidRegex,
  isValidSubnet,
  isValidSubnet6,
  isValidWildcard,
  VALIDATOP_MAP,
} from "../../utils/rule-validators";

export type RuleListType = Rule["type"] | "auto";
export type ParsedRuleLine = {
  text: string;
  type: Rule["type"] | "INVALID" | null;
  isValid: boolean;
};
export type RuleListPreview = {
  lines: ParsedRuleLine[];
  totalLines: number;
  stats: Record<string, number>;
};
export type ParsedRuleList = RuleListPreview & {
  entries: { rule: string; type: Rule["type"] }[];
};
export type RuleListJob =
  | { type: "parse"; version: number; text: string; ruleType: RuleListType }
  | { type: "apply"; version: number };
export type RuleListRequest = { id: number } & (
  | RuleListJob
  | { type: "next"; version: number; sequence: number }
);
export type RuleListPart = Pick<Rule, "id" | "type" | "rule"> & { complete: boolean };
export type RuleListChunk = {
  parts: RuleListPart[];
  total: number;
  done: boolean;
};
export type RuleListResponse = { id: number; version: number } & (
  | { type: "preview"; preview: RuleListPreview }
  | ({ type: "chunk"; sequence: number } & RuleListChunk)
  | { type: "error"; message: string }
);
export type RuleListResult = { id: number; version: number } & (
  | { type: "preview"; preview: RuleListPreview }
  | { type: "rules"; rules: Rule[] }
);

// Bound both object count and string storage per message. UTF-16 code units
// cost at most two bytes each; even one unusually long pattern is split.
export const RULE_LIST_CHUNK_PARTS = 256;
export const RULE_LIST_TEXT_LIMIT = 32 * 1024;

export function detectRuleListType(pattern: string): Rule["type"] | null {
  if (isValidSubnet6(pattern)) return "subnet6";
  if (isValidSubnet(pattern)) return "subnet";
  if (isValidNamespace(pattern)) return "namespace";
  if (isValidDomain(pattern)) return "domain";
  if (isValidRegex(pattern)) return "regex";
  if (isValidWildcard(pattern)) return "wildcard";
  return null;
}

/** Preserve the existing separator/detection order; only the bounded preview
 * crosses back to the UI. Complete unique rules remain in its worker session. */
export function parseRuleList(
  text: string,
  ruleType: RuleListType,
  previewLimit = 500,
): ParsedRuleList {
  const lines: ParsedRuleLine[] = [];
  const entries: ParsedRuleList["entries"] = [];
  const stats: Record<string, number> = {};
  const seen = new Set<string>();
  let totalLines = 0;
  let previewLength = 0;
  for (const line of text.split(/[\n,]+/)) {
    const trimmed = line.trim();
    let type: ParsedRuleLine["type"] = null;
    if (trimmed && !trimmed.startsWith("#")) {
      type =
        ruleType === "auto"
          ? detectRuleListType(trimmed)
          : VALIDATOP_MAP[ruleType]?.(trimmed)
            ? ruleType
            : null;
      stats[type || "INVALID"] = (stats[type || "INVALID"] || 0) + 1;
      if (type) {
        const key = `${type}|${trimmed}`;
        if (!seen.has(key)) {
          seen.add(key);
          entries.push({ rule: trimmed, type });
        }
      } else type = "INVALID";
    }
    if (lines.length < previewLimit && previewLength < RULE_LIST_TEXT_LIMIT) {
      const remaining = RULE_LIST_TEXT_LIMIT - previewLength;
      const text = line.length <= remaining ? line : `${line.slice(0, remaining - 1)}…`;
      lines.push({ text, type, isValid: Boolean(type && type !== "INVALID") });
      previewLength += text.length;
    }
    totalLines++;
  }
  return { lines, totalLines, stats, entries };
}

export function ruleListPreview(parsed: ParsedRuleList): RuleListPreview {
  return { lines: parsed.lines, totalLines: parsed.totalLines, stats: parsed.stats };
}

function* importedRules(parsed: ParsedRuleList, newId?: () => string): Generator<Rule> {
  const reserved = new Set<string>();
  // Keep collision-safe random IDs without 100k separate crypto calls and
  // short-lived typed arrays when applying a large native rule list.
  const random = new Uint32Array(4096);
  let offset = random.length;
  const nextId =
    newId ??
    (() => {
      if (offset === random.length) {
        crypto.getRandomValues(random);
        offset = 0;
      }
      return random[offset++].toString(16).padStart(8, "0");
    });
  for (const { rule, type } of parsed.entries) {
    let id: string;
    do id = nextId();
    while (reserved.has(id));
    reserved.add(id);
    yield { id, name: "", rule, type, enable: true };
  }
}

export function buildImportedRules(parsed: ParsedRuleList, newId?: () => string): Rule[] {
  return Array.from(importedRules(parsed, newId));
}

/** A single iterator owns ID reservations for the entire Apply. The worker
 * advances it only after the UI has consumed the previous bounded message. */
export function* importedRuleChunks(
  parsed: ParsedRuleList,
  newId?: () => string,
): Generator<RuleListChunk> {
  const total = parsed.entries.length;
  let parts: RuleListPart[] = [];
  let textLength = 0;
  let completed = 0;
  for (const { id, rule, type } of importedRules(parsed, newId)) {
    let offset = 0;
    do {
      const end = Math.min(rule.length, offset + RULE_LIST_TEXT_LIMIT - textLength);
      const complete = end === rule.length;
      parts.push({ id, type, rule: rule.slice(offset, end), complete });
      textLength += end - offset;
      offset = end;
      if (complete) completed++;
      if (parts.length === RULE_LIST_CHUNK_PARTS || textLength === RULE_LIST_TEXT_LIMIT) {
        yield { parts, total, done: completed === total };
        parts = [];
        textLength = 0;
      }
    } while (offset < rule.length);
  }
  if (parts.length || !total) yield { parts, total, done: true };
}

export class RuleListParserSession {
  private prepared?: { version: number; parsed: ParsedRuleList };
  private transfer?: {
    id: number;
    version: number;
    sequence: number;
    chunks: Generator<RuleListChunk>;
  };

  constructor(private readonly newId?: () => string) {}

  handle(request: RuleListRequest): RuleListResponse {
    const { id, version } = request;
    try {
      if (request.type === "parse") {
        this.transfer = undefined;
        this.prepared = undefined;
        const parsed = parseRuleList(request.text, request.ruleType);
        this.prepared = { version, parsed };
        return { id, version, type: "preview", preview: ruleListPreview(parsed) };
      }
      if (request.type === "apply") {
        if (this.prepared?.version !== version) throw new Error("Rule list changed during import");
        this.transfer = {
          id,
          version,
          sequence: 0,
          chunks: importedRuleChunks(this.prepared.parsed, this.newId),
        };
      }
      const transfer = this.transfer;
      if (
        !transfer ||
        transfer.id !== id ||
        transfer.version !== version ||
        (request.type === "next" && request.sequence !== transfer.sequence)
      )
        throw new Error("Rule list transfer is no longer current");
      const chunk = transfer.chunks.next();
      if (chunk.done) throw new Error("Rule list transfer ended unexpectedly");
      if (chunk.value.done) this.transfer = undefined;
      return { id, version, type: "chunk", sequence: transfer.sequence++, ...chunk.value };
    } catch (error) {
      this.transfer = undefined;
      return {
        id,
        version,
        type: "error",
        message: error instanceof Error ? error.message : String(error),
      };
    }
  }
}

/** Height saturates after a few visible lines; never split a multi-megabyte
 * textarea merely to decide that it already needs the maximum height. */
export function boundedRuleListLines(text: string, limit: number): number {
  let lines = 0;
  const tokens = /[^\n,]+/g;
  let match: RegExpExecArray | null;
  while ((match = tokens.exec(text))) {
    if (match[0].trim() && ++lines >= limit) return lines;
  }
  return Math.max(1, lines);
}
