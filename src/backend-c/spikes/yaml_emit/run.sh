#!/bin/sh
# libyaml emit spike: byte-compare libyaml output against Go yaml.v2 output
# for the same config data. Divergences drove the Phase 2 emitter shims
# (decisions.md D-04). Now that src/backend (Go) is gone, this replays
# libyaml against golden/go-yaml-v2.yaml (the yaml.v2 fixture's frozen
# output, captured before Go was removed in Phase 9) instead of a live
# Go run. See decisions.md D-45.
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$DIR/out}"
mkdir -p "$OUT"

cc -O2 -Wall -Wextra -o "$OUT/emit_config" "$DIR/emit_config.c" -lyaml

"$OUT/emit_config" > "$OUT/libyaml.yaml"

if diff -u "$DIR/golden/go-yaml-v2.yaml" "$OUT/libyaml.yaml" > "$OUT/divergence.diff"; then
    echo "IDENTICAL ($(wc -c < "$OUT/libyaml.yaml") bytes)"
else
    echo "DIVERGENCES:"
    cat "$OUT/divergence.diff"
    exit 1
fi
