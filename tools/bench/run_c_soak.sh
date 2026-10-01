#!/bin/sh
# Soak test: sustained DNS churn (many distinct, constantly-rotating names,
# mixed A/AAAA/CNAME chains) against the C daemon, sampling RSS at
# intervals. The records cache is bounded (mt_cache, default cap 65536
# domains) and runs a 30s expiry sweep, so RSS must stay flat over the run
# — no unbounded growth (migration-plan.md Phase 4 exit criterion).
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO/.build/bench"
DAEMON="$REPO/src/backend-c/build/host/magitrickled-c"
OUT_DIR="${1:-$REPO/docs/c-rewrite/soak-c}"
DURATION="${DURATION:-120}"
SAMPLE_INTERVAL="${SAMPLE_INTERVAL:-5}"
CFG=/tmp/mt-c-soak.yaml

mkdir -p "$OUT_DIR"
RSS_CSV="$OUT_DIR/rss.csv"
echo "sec,rss_kb,domains_seen_approx" > "$RSS_CSV"

pkill -9 -f magitrickled-c 2>/dev/null
pkill -9 -x dnsstub 2>/dev/null
sleep 1

( cd "$REPO/tools/bench" && go run ./genconfig -rules 500 -type namespace \
    -addr 0.0.0.0 -upstream-port 5399 -group-enable -loglevel error \
    -out "$CFG" )

"$BIN/dnsstub" -listen 127.0.0.1:5399 -cname 3 >/dev/null 2>&1 &
STUB=$!
sleep 0.3
"$DAEMON" --config "$CFG" >"$OUT_DIR/daemon.log" 2>&1 &
DP=$!
sleep 1

"$BIN/dnsload" -mode probe -server 127.0.0.1:3553 >/dev/null 2>&1

# sustained load in the background for the whole soak window: high
# distinct-name cardinality (100k) so the cache keeps discovering "new"
# domains, exercising both growth and the bounded-cap/eviction path, mixed
# with a UDP+TCP split.
"$BIN/dnsload" -server 127.0.0.1:3553 -proto udp -concurrency 30 \
    -duration "${DURATION}s" -ndomains 100000 \
    -pattern 'd%06d.soak.example.com.' >"$OUT_DIR/load_udp.json" 2>&1 &
LOAD1=$!
"$BIN/dnsload" -server 127.0.0.1:3553 -proto tcp -concurrency 10 \
    -duration "${DURATION}s" -ndomains 100000 \
    -pattern 'd%06d.soak.example.com.' >"$OUT_DIR/load_tcp.json" 2>&1 &
LOAD2=$!

i=0
while [ "$i" -lt "$DURATION" ]; do
    rss=$(awk '/VmRSS/{print $2}' "/proc/$DP/status" 2>/dev/null || echo 0)
    echo "$i,$rss," >> "$RSS_CSV"
    sleep "$SAMPLE_INTERVAL"
    i=$((i + SAMPLE_INTERVAL))
done

wait "$LOAD1" "$LOAD2" 2>/dev/null
final_rss=$(awk '/VmRSS/{print $2}' "/proc/$DP/status" 2>/dev/null || echo 0)
final_hwm=$(awk '/VmHWM/{print $2}' "/proc/$DP/status" 2>/dev/null || echo 0)
echo "$DURATION,$final_rss," >> "$RSS_CSV"

{
    echo "duration_sec: $DURATION"
    echo "final_rss_kb: $final_rss"
    echo "peak_rss_kb (VmHWM): $final_hwm"
    echo "load_udp: $(cat "$OUT_DIR/load_udp.json")"
    echo "load_tcp: $(cat "$OUT_DIR/load_tcp.json")"
} > "$OUT_DIR/summary.txt"

kill -9 "$DP" "$STUB" 2>/dev/null
cat "$OUT_DIR/summary.txt"
echo "raw samples: $RSS_CSV" >&2
