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

function getReverseDomain(domain: string): string {
  return domain.toLowerCase().split(".").reverse().join(".");
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
      const priorityA = TYPE_PRIORITY[a.type] ?? 99;
      const priorityB = TYPE_PRIORITY[b.type] ?? 99;

      if (priorityA !== priorityB) {
        return priorityA - priorityB;
      }

      if (a.type === "subnet") {
        const cidrA = parseCidr(a.rule);
        const cidrB = parseCidr(b.rule);

        if (cidrA.ip !== cidrB.ip) {
          return cidrA.ip - cidrB.ip;
        }
        return cidrB.mask - cidrA.mask;
      }

      if (["domain", "wildcard", "namespace"].includes(a.type)) {
        const revA = getReverseDomain(a.rule);
        const revB = getReverseDomain(b.rule);

        const cmp = revA.localeCompare(revB);
        if (cmp !== 0) return cmp;

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
