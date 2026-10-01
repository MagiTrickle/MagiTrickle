#!/bin/sh
# C DNS proxy baseline — same methodology as run_baseline.sh (same dnsstub,
# same dnsload, same cells) so the numbers line up against the Go baseline in
# docs/c-rewrite/baseline-raw/. Results -> docs/c-rewrite/baseline-raw-c/.
#
# As of Phase 9 this is a full-pipeline comparison: transport + DNS parse +
# hooks + rule matching + cache (Phase 4) all run on every query, same as
# the Go baseline (whose group is also disabled here, so both sides skip
# ipset writes but both run real rule matching). The Phase 3-era "transport
# only" caveat that used to live here no longer applies — see
# docs/c-rewrite/phase-9-report.md for the current numbers.
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO/.build/bench"
DAEMON="$REPO/src/backend-c/build/host/magitrickled-c"
RESULTS_DIR="${1:-$REPO/docs/c-rewrite/baseline-raw-c}"
LOAD_SEC="${LOAD_SEC:-4}"
REPEATS="${REPEATS:-3}"
UPSTREAM_PORT=5399
PROXY_PORT=3553
CFG=/tmp/mt-c-bench.yaml

mkdir -p "$RESULTS_DIR"
RESULTS="$RESULTS_DIR/results.jsonl"
: > "$RESULTS"

{
  echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "kernel: $(uname -sr)"
  echo "arch: $(uname -m)"
  echo "cc: $(gcc --version | head -1)"
  echo "cpu_model: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
  echo "cpus: $(nproc)"
  echo "libc: $(ldd --version 2>/dev/null | head -1)"
  echo "git_commit: $(cd "$REPO" && git rev-parse HEAD)"
  echo "binary_size_bytes: $(stat -c %s "$DAEMON")"
  echo "note: full pipeline (transport+parse+hooks+rule matching+cache; group disabled so ipset writes are skipped, same as the Go baseline)"
} > "$RESULTS_DIR/environment.txt"

rss_kb()   { awk '/VmRSS/{print $2}' "/proc/$1/status" 2>/dev/null; }
hwm_kb()   { awk '/VmHWM/{print $2}' "/proc/$1/status" 2>/dev/null; }
threads()  { awk '/Threads/{print $2}' "/proc/$1/status" 2>/dev/null; }
cpu_ticks(){ awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null; }
measure_cpu() { t0=$(cpu_ticks "$1"); sleep "$2"; t1=$(cpu_ticks "$1"); hz=$(getconf CLK_TCK)
  awk -v a="$t0" -v b="$t1" -v s="$2" -v hz="$hz" 'BEGIN{printf "%.1f",(b-a)/hz/s*100}'; }

STUB="" DP=""
cleanup() { [ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$STUB" ] && kill -9 "$STUB" 2>/dev/null; }
trap cleanup EXIT INT TERM

pkill -9 -f magitrickled-c 2>/dev/null; pkill -9 -x dnsstub 2>/dev/null; sleep 1
"$BIN/dnsstub" -listen "127.0.0.1:$UPSTREAM_PORT" >/dev/null 2>&1 &
STUB=$!
sleep 0.3

for RULES in 100 1000 10000; do
  ( cd "$REPO/tools/bench" && go run ./genconfig -rules "$RULES" -type namespace \
      -addr 0.0.0.0 -upstream-port "$UPSTREAM_PORT" -out "$CFG" )
  "$DAEMON" --config "$CFG" >/dev/null 2>&1 &
  DP=$!
  sleep 1
  "$BIN/dnsload" -mode probe -server "127.0.0.1:$PROXY_PORT" >/dev/null 2>&1

  if [ "$RULES" = "1000" ]; then
    IDLE_RSS=$(rss_kb "$DP"); IDLE_THREADS=$(threads "$DP")
    IDLE_CPU=$(measure_cpu "$DP" 5)
    echo "{\"metric\":\"idle\",\"rules\":1000,\"idle_rss_kb\":$IDLE_RSS,\"threads\":$IDLE_THREADS,\"idle_cpu_pct\":$IDLE_CPU}" >> "$RESULTS"
  fi

  for PROTO in udp tcp; do
    for CONC in 10 100; do
      REP=0
      while [ "$REP" -lt "$REPEATS" ]; do
        CPU_FILE=$(mktemp)
        ( measure_cpu "$DP" "$LOAD_SEC" > "$CPU_FILE" ) &
        CJOB=$!
        OUT=$("$BIN/dnsload" -server "127.0.0.1:$PROXY_PORT" -proto "$PROTO" \
          -concurrency "$CONC" -duration "${LOAD_SEC}s" -ndomains "$RULES" \
          -label "rules=$RULES type=namespace rep=$REP")
        wait "$CJOB"
        CPU=$(cat "$CPU_FILE"); rm -f "$CPU_FILE"
        RSS=$(rss_kb "$DP")
        echo "$OUT" | sed "s/}$/,\"cpu_pct\":$CPU,\"rss_kb\":$RSS,\"rules\":$RULES,\"rule_type\":\"namespace\"}/" >> "$RESULTS"
        REP=$((REP+1))
      done
    done
  done
  echo "{\"metric\":\"peak\",\"rules\":$RULES,\"vmhwm_kb\":$(hwm_kb "$DP")}" >> "$RESULTS"
  kill -9 "$DP" 2>/dev/null; DP=""
  sleep 0.5
done

echo "results in $RESULTS" >&2
