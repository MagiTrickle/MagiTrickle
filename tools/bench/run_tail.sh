#!/bin/sh
# run_tail.sh — re-runs only the configbench + soak sections of
# run_baseline.sh (useful when the main matrix already completed).
# Appends to the existing results.jsonl.
set -eu

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$BENCH_DIR/../.." && pwd)"
RESULTS_DIR="${1:-$REPO_DIR/docs/c-rewrite/baseline-raw}"
GROUP_ENABLE="${GROUP_ENABLE:-0}"
STATE_DIR="/var/lib/magitrickle"
CFG="$STATE_DIR/config.yaml"
UPSTREAM_PORT=5399
PROXY_PORT=3553
SOAK_SEC="${SOAK_SEC:-60}"
BIN_DIR="$REPO_DIR/.build/bench"
RESULTS="$RESULTS_DIR/results.jsonl"

log() { echo "[bench] $*" >&2; }
rss_kb() { awk '/VmRSS/ {print $2}' "/proc/$1/status"; }
hwm_kb() { awk '/VmHWM/ {print $2}' "/proc/$1/status"; }

VERSION_LDFLAG="-X 'magitrickle/constant.Version=0.99.0'"
( cd "$REPO_DIR/src/backend" && go build -trimpath -ldflags="-w -s $VERSION_LDFLAG" -o "$BIN_DIR/magitrickled" ./cmd/magitrickled )
( cd "$BENCH_DIR" && go build -ldflags="$VERSION_LDFLAG" -o "$BIN_DIR/configbench" ./configbench )

STUB_PID=""; DAEMON_PID=""
cleanup() {
  [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
  [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

GEN_FLAGS=""
[ "$GROUP_ENABLE" = "1" ] && GEN_FLAGS="-group-enable"

# config load/save
for RULES in 1000 10000; do
  ( cd "$BENCH_DIR" && go run ./genconfig -rules "$RULES" -type mixed $GEN_FLAGS -out "$CFG" )
  OUT=$("$BIN_DIR/configbench")
  echo "$OUT" | sed "s/}$/,\"metric\":\"configbench\",\"rules\":$RULES}/" >> "$RESULTS"
done
log "configbench done"

"$BIN_DIR/dnsstub" -listen "127.0.0.1:$UPSTREAM_PORT" &
STUB_PID=$!
sleep 0.3

( cd "$BENCH_DIR" && go run ./genconfig -rules 10000 -type namespace $GEN_FLAGS -out "$CFG" )
"$BIN_DIR/magitrickled" >"$RESULTS_DIR/daemon.log" 2>&1 &
DAEMON_PID=$!
"$BIN_DIR/dnsload" -mode probe -server "127.0.0.1:$PROXY_PORT" >/dev/null
"$BIN_DIR/dnsload" -server "127.0.0.1:$PROXY_PORT" -proto udp -concurrency 50 \
  -duration "${SOAK_SEC}s" -ndomains 10000 -label soak > "$RESULTS_DIR/soak_load.json" &
LOAD_JOB=$!
SOAK_SAMPLES="$RESULTS_DIR/soak_rss.csv"
echo "sec,rss_kb" > "$SOAK_SAMPLES"
i=0
while [ "$i" -lt "$SOAK_SEC" ]; do
  echo "$i,$(rss_kb "$DAEMON_PID")" >> "$SOAK_SAMPLES"
  sleep 5; i=$((i+5))
done
wait "$LOAD_JOB"
cat "$RESULTS_DIR/soak_load.json" >> "$RESULTS"
echo "{\"metric\":\"soak_end\",\"rss_kb\":$(rss_kb "$DAEMON_PID"),\"vmhwm_kb\":$(hwm_kb "$DAEMON_PID")}" >> "$RESULTS"
log "soak done"

[ -f "$RESULTS_DIR/config.yaml.bak" ] && cp "$RESULTS_DIR/config.yaml.bak" "$CFG" || rm -f "$CFG"
log "tail sections complete"
