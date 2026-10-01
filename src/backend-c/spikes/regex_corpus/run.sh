#!/bin/sh
# Regex compatibility spike: dlclark/regexp2 (Go oracle) vs PCRE2.
# Phase 1 finding, frozen in decisions.md D-07 and known_divergences.tsv:
# 3 documented divergences out of the corpus, all in obscure regex
# features (POSIX classes, possessive quantifiers, balancing groups) not
# used by any real-world domain-matching rule. Now that src/backend (Go)
# is gone, this replays PCRE2 against golden/regexp2.tsv (the regexp2
# oracle's frozen output, captured before Go was removed in Phase 9)
# instead of a live Go run. See decisions.md D-45.
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$DIR/out}"
mkdir -p "$OUT"

cc -O2 -Wall -Wextra -o "$OUT/pcre2_runner" "$DIR/pcre2_runner.c" $(pcre2-config --libs8 --cflags)

"$OUT/pcre2_runner" < "$DIR/corpus.tsv" > "$OUT/pcre2.tsv"

if diff -u "$DIR/golden/regexp2.tsv" "$OUT/pcre2.tsv" > "$OUT/divergence.diff"; then
    echo "IDENTICAL: $(grep -c . "$OUT/pcre2.tsv") cases, no divergence"
else
    echo "DIVERGENCES FOUND:"
    cat "$OUT/divergence.diff"
    exit 1
fi
