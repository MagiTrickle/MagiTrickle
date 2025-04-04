export function isValidWildcard(pattern: string): boolean {
  return (
    /^(\*)?([a-zA-Z0-9](?![\.\-])[a-zA-Z0-9\-]*[a-zA-Z0-9])?(\.[a-zA-Z0-9](?![\.\-])[a-zA-Z0-9\-]*[a-zA-Z0-9])+$/.test(
      pattern
    ) || /^(\*)?[a-zA-Z0-9](?![\.\-])[a-zA-Z0-9\-]*[a-zA-Z0-9]$/.test(pattern)
  );
}

export function isValidDomain(pattern: string): boolean {
  return /^(([a-zA-Z0-9]|[a-zA-Z0-9][a-zA-Z0-9\-]*[a-zA-Z0-9])\.)*([A-Za-z0-9]|[A-Za-z0-9][A-Za-z0-9\-]*[A-Za-z0-9])$/.test(
    pattern
  );
}

export function isValidNamespace(pattern: string): boolean {
  return isValidDomain(pattern) || isValidDomain(pattern.replace(/^\.+/, ""));
}

export function isValidRegex(pattern: string): boolean {
  try {
    new RegExp(pattern);
    return true;
  } catch (e) {
    if (e instanceof SyntaxError) {
      return false;
    }
    return false;
  }
}

export const VALIDATOP_MAP: Record<string, (pattern: string) => boolean> = {
  regex: isValidRegex,
  wildcard: isValidWildcard,
  domain: isValidDomain,
  namespace: isValidNamespace,
};
