#!/bin/sh
# Differential/regression test runner (C only; see decisions.md D-45).
#
# Through Phase 8, every suite here compared the real Go reference
# backend against the C implementation. src/backend (Go) was removed in
# Phase 9 once the compatibility contract was signed off
# (parity-checklist.md) — these suites now compare the C
# implementation's live output against golden/ snapshots frozen from the
# last Go-vs-C run (captured byte-identical, immediately before removal),
# so a real regression in C's own behavior still shows up as a diff.
#
# Suites:
#   regex    — PCRE2 corpus vs golden/regexp2.tsv (regexp2 oracle output,
#              frozen; 3 known divergences documented in decisions.md D-07
#              and spikes/regex_corpus/known_divergences.tsv)
#   yaml     — libyaml emit vs golden/go-yaml-v2.yaml (Phase 1 spike)
#   config   — mt-configtool resave over fixtures/*.yaml vs
#              golden/config-fixtures/*.golden.yaml (Phase 2)
#   match    — mt-configtool match corpus vs golden/match.golden.tsv
#   subparse — mt-configtool subparse corpus vs golden/subparse.golden.txt
#   dns      — mt-dnstool dump/stripaaaa/ptrcheck vs golden/dns.*.golden.txt
#   cache    — mt-cachetool script vs golden/cache.golden.txt
#   http     — Phase 6 HTTP API contract vs golden/http_contract.trace
#              (see run_http_diff.sh for what's normalized/redacted and why)
#
# Requires root for the http suite (real /var/lib/magitrickle, real
# iptables). Exit non-zero on any unexpected divergence.
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
BACKEND_C_DIR="$(cd "$DIR/../.." && pwd)"
OUT="$DIR/out"
GOLDEN="$DIR/golden"
mkdir -p "$OUT"

fail=0

echo "== differential: regex (PCRE2 vs golden regexp2.tsv)"
if ! sh "$BACKEND_C_DIR/spikes/regex_corpus/run.sh"; then
    KNOWN="$BACKEND_C_DIR/spikes/regex_corpus/known_divergences.tsv"
    ACTUAL_NORM="$BACKEND_C_DIR/spikes/regex_corpus/out/divergence.norm"
    grep -E '^[+-]' "$BACKEND_C_DIR/spikes/regex_corpus/out/divergence.diff" \
        | grep -vE '^(\+\+\+|---)' > "$ACTUAL_NORM" || true
    if [ -f "$KNOWN" ] && \
       diff -u "$KNOWN" "$ACTUAL_NORM" >/dev/null 2>&1; then
        echo "   divergences match the documented known set — OK"
    else
        echo "   UNEXPECTED regex divergence (update decisions.md D-07 if intended)"
        fail=1
    fi
fi

echo "== differential: yaml emit (libyaml vs golden go-yaml-v2.yaml)"
if ! sh "$BACKEND_C_DIR/spikes/yaml_emit/run.sh"; then
    fail=1
fi

echo "== differential: building tools"
( cd "$BACKEND_C_DIR" && make >/dev/null )
CONFIGTOOL="$BACKEND_C_DIR/build/host/mt-configtool"
DNSTOOL="$BACKEND_C_DIR/build/host/mt-dnstool"
CACHETOOL="$BACKEND_C_DIR/build/host/mt-cachetool"

echo "== differential: config load/save fixtures (vs golden)"
for fixture in "$DIR"/fixtures/*.yaml; do
    name=$(basename "$fixture" .yaml)
    golden="$GOLDEN/config-fixtures/$name.golden.yaml"
    "$CONFIGTOOL" resave "$fixture" 0.99.0 > "$OUT/$name.c.yaml"
    if ! diff -u "$golden" "$OUT/$name.c.yaml" > "$OUT/$name.diff" 2>&1; then
        echo "   REGRESSION in $name (vs golden):"
        head -20 "$OUT/$name.diff"
        fail=1
    else
        echo "   $name: OK"
    fi
done

echo "== differential: missing-file behaviour (defaults, vs golden)"
"$CONFIGTOOL" resave /nonexistent/config.yaml 0.99.0 > "$OUT/missing.c.yaml"
if ! diff -u "$GOLDEN/config-fixtures/missing.golden.yaml" "$OUT/missing.c.yaml" \
     > "$OUT/missing.diff" 2>&1; then
    echo "   REGRESSION in defaults (vs golden):"
    head -20 "$OUT/missing.diff"
    fail=1
else
    echo "   defaults: OK"
fi

echo "== differential: rule matching corpus (vs golden)"
"$CONFIGTOOL" match < "$DIR/corpus/match_corpus.tsv" > "$OUT/match.c.tsv"
if ! diff -u "$GOLDEN/match.golden.tsv" "$OUT/match.c.tsv" \
     > "$OUT/match.diff" 2>&1; then
    echo "   REGRESSION (vs golden):"
    cat "$OUT/match.diff"
    fail=1
else
    echo "   $(grep -c . "$OUT/match.c.tsv") cases: OK"
fi

echo "== differential: subscription parse corpus (vs golden)"
"$CONFIGTOOL" subparse < "$DIR/corpus/subparse_corpus.txt" > "$OUT/subparse.c.txt"
if ! diff -u "$GOLDEN/subparse.golden.txt" "$OUT/subparse.c.txt" \
     > "$OUT/subparse.diff" 2>&1; then
    echo "   REGRESSION (vs golden):"
    cat "$OUT/subparse.diff"
    fail=1
else
    echo "   $(grep -c . "$OUT/subparse.c.txt") rules: OK"
fi

echo "== differential: DNS wire corpus (vs golden)"
DNS_CORPUS="$DIR/corpus/dns_corpus.hex"
for mode in dump stripaaaa ptrcheck; do
    "$DNSTOOL" "$mode" < "$DNS_CORPUS" > "$OUT/dns.$mode.c.txt"
    if ! diff -u "$GOLDEN/dns.$mode.golden.txt" "$OUT/dns.$mode.c.txt" \
         > "$OUT/dns.$mode.diff" 2>&1; then
        echo "   DNS $mode REGRESSION (vs golden):"
        head -30 "$OUT/dns.$mode.diff"
        fail=1
    else
        echo "   dns $mode: $(grep -c '^===' "$OUT/dns.$mode.c.txt") messages: OK"
    fi
done

echo "== differential: DNS records cache (vs golden)"
CACHE_SCRIPT="$DIR/corpus/cache_script.txt"
"$CACHETOOL" < "$CACHE_SCRIPT" > "$OUT/cache.c.txt"
if ! diff -u "$GOLDEN/cache.golden.txt" "$OUT/cache.c.txt" > "$OUT/cache.diff" 2>&1; then
    echo "   REGRESSION (vs golden):"
    cat "$OUT/cache.diff"
    fail=1
else
    echo "   $(grep -c . "$OUT/cache.c.txt") queries: OK"
fi

echo "== differential: HTTP API contract (vs golden)"
if ! sh "$DIR/run_http_diff.sh"; then
    fail=1
fi

exit $fail
