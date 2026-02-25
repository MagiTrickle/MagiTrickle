export type SearchHighlightSegment = {
  text: string;
  matched: boolean;
};

export function splitSearchHighlightSegments(
  value: string,
  query: string,
): SearchHighlightSegment[] | null {
  if (!value || !query) return null;

  const source = `${value}`;
  const needle = query.trim().toLowerCase();
  if (!needle) return null;

  const haystack = source.toLowerCase();
  const needleLength = needle.length;

  let cursor = 0;
  let matchIndex = haystack.indexOf(needle, cursor);
  if (matchIndex === -1) return null;

  const segments: SearchHighlightSegment[] = [];

  while (matchIndex !== -1) {
    if (matchIndex > cursor) {
      segments.push({ text: source.slice(cursor, matchIndex), matched: false });
    }

    const matchEnd = matchIndex + needleLength;
    segments.push({ text: source.slice(matchIndex, matchEnd), matched: true });

    cursor = matchEnd;
    matchIndex = haystack.indexOf(needle, cursor);
  }

  if (cursor < source.length) {
    segments.push({ text: source.slice(cursor), matched: false });
  }

  return segments;
}
