import type { Rule } from "../types";

export type SortField = "pattern" | "name";
export type SortDirection = "asc" | "desc";

const TYPE_PRIORITY: Record<string, number> = {
  subnet: 10, // IPv4 subnets
  subnet6: 11, // IPv6 subnets
  wildcard: 20,
  domain: 30,
  namespace: 31,
  regex: 40, // Regex usually at the end
};

function ipToNum(ip: string): number {
  return (
    ip.split(".").reduce((acc, octet) => {
      return (acc << 8) + parseInt(octet, 10);
    }, 0) >>> 0
  );
}

function parseCidr(value: string) {
  const parts = value.split("/");
  const ipStr = parts[0].trim();
  const maskStr = parts[1];

  if (!ipStr) return { ip: 0, mask: 0 };

  return {
    ip: ipToNum(ipStr),
    mask: maskStr ? parseInt(maskStr, 10) : 32,
  };
}

type DomainSortKey = {
  base: string;
  tld: string;
  sub: string;
  raw: string;
  isWildcard: boolean;
};

const SECOND_LEVEL_TLD_HINTS = new Set(["co", "com", "net", "org", "gov", "edu", "ac"]);

function buildDomainSortKey(rule: string): DomainSortKey {
  const trimmed = rule.trim();
  const hasWildcard = /^[*?]/.test(trimmed);
  const cleaned = rule
    .trim()
    .replace(/^[*?]+\.?/, "")
    .replace(/\.+$/, "")
    .toLowerCase();
  const parts = cleaned.split(".").filter(Boolean);

  if (parts.length < 2) {
    return {
      base: cleaned,
      tld: "",
      sub: "",
      raw: cleaned,
      isWildcard: hasWildcard,
    };
  }

  const tldPartsCount =
    parts.length >= 3 &&
    SECOND_LEVEL_TLD_HINTS.has(parts[parts.length - 2]) &&
    parts[parts.length - 1].length === 2
      ? 2
      : 1;
  const tld = parts.slice(-tldPartsCount).join(".");
  const base = parts[parts.length - tldPartsCount - 1];
  const sub = parts.slice(0, -tldPartsCount - 1).join(".");

  return {
    base,
    tld,
    sub,
    raw: cleaned,
    isWildcard: hasWildcard,
  };
}

export function sortRules(
  rules: Rule[],
  field: SortField = "pattern",
  direction: SortDirection = "asc",
): Rule[] {
  const sorted = [...rules];

  if (field === "name") {
    sorted.sort((a, b) => a.name.localeCompare(b.name));
  } else {
    sorted.sort((a, b) => {
      const domainLikeTypes = ["domain", "wildcard", "namespace"];
      const isDomainLikeA = domainLikeTypes.includes(a.type);
      const isDomainLikeB = domainLikeTypes.includes(b.type);

      if (!isDomainLikeA || !isDomainLikeB) {
        const priorityA = TYPE_PRIORITY[a.type] ?? 99;
        const priorityB = TYPE_PRIORITY[b.type] ?? 99;

        if (priorityA !== priorityB) {
          return priorityA - priorityB;
        }
      }

      if (a.type === "subnet") {
        const cidrA = parseCidr(a.rule);
        const cidrB = parseCidr(b.rule);

        if (cidrA.ip !== cidrB.ip) {
          return cidrA.ip - cidrB.ip;
        }
        return cidrB.mask - cidrA.mask;
      }

      if (isDomainLikeA && isDomainLikeB) {
        const keyA = buildDomainSortKey(a.rule);
        const keyB = buildDomainSortKey(b.rule);

        const baseCmp = keyA.base.localeCompare(keyB.base);
        if (baseCmp !== 0) return baseCmp;

        const tldCmp = keyA.tld.localeCompare(keyB.tld);
        if (tldCmp !== 0) return tldCmp;

        if (keyA.sub !== keyB.sub) {
          if (!keyA.sub) return -1;
          if (!keyB.sub) return 1;
          if (keyA.isWildcard !== keyB.isWildcard) {
            return keyA.isWildcard ? -1 : 1;
          }

          const depthA = keyA.sub.split(".").length;
          const depthB = keyB.sub.split(".").length;
          if (depthA !== depthB) return depthA - depthB;

          return keyA.sub.localeCompare(keyB.sub);
        }

        if (keyA.isWildcard !== keyB.isWildcard) {
          return keyA.isWildcard ? 1 : -1;
        }

        const rawCmp = keyA.raw.localeCompare(keyB.raw);
        if (rawCmp !== 0) return rawCmp;

        return a.type.localeCompare(b.type);
      }

      return a.rule.localeCompare(b.rule);
    });
  }

  if (direction === "desc") {
    sorted.reverse();
  }

  return sorted;
}
