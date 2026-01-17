import type { Rule } from "../types";

const TYPE_PRIORITY: Record<string, number> = {
  subnet: 10, // IPv4 subnets
  subnet6: 11, // IPv6 subnets
  domain: 20, // Specific domains
  wildcard: 25, // Wildcards
  namespace: 30, // Namespaces
  regex: 40, // Regex
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

export function sortRules(rules: Rule[]): Rule[] {
  return [...rules].sort((a, b) => {
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

    if (a.type === "domain" || a.type === "wildcard") {
      const revA = getReverseDomain(a.rule);
      const revB = getReverseDomain(b.rule);
      return revA.localeCompare(revB);
    }

    return a.rule.localeCompare(b.rule);
  });
}
