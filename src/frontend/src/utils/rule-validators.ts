export function isValidWildcard(pattern: string): boolean {
  return /^(?!\.)(?!.*\.$)(?!.*\.\.)(?!.*\*\*)[a-zA-Z0-9\-.*?]+$/.test(pattern);
}

export function isValidDomain(pattern: string): boolean {
  return /^(?!\.)(?!.*\.$)(?!.*\.\.)[a-zA-Z0-9\-.]+$/.test(pattern);
}

export function isValidNamespace(pattern: string): boolean {
  return isValidDomain(pattern);
}

export function isValidSubnet(pattern: string): boolean {
  let matches = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?:\/(\d{1,2}))?$/.exec(pattern)
  return !(
      !matches ||
      parseInt(matches[1]) > 255 ||
      parseInt(matches[2]) > 255 ||
      parseInt(matches[3]) > 255 ||
      parseInt(matches[4]) > 255 ||
      (matches[5] != "" && parseInt(matches[5]) > 32)
  );
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
  subnet: isValidSubnet,
};
