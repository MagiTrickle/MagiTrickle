#!/bin/sh
# run_baseline.sh — reproducible Go-backend baseline for the C rewrite.
#
# Runs the *unmodified* Go daemon (host build, no platform tags, no UPX)
# against a local fixed-answer upstream (dnsstub) and measures:
#   - startup time (exec -> first DNS answer)
#   - idle RSS/PSS/threads, idle CPU
#   - UDP/TCP throughput + latency percentiles across rule counts
#   - CPU under load, post-load RSS
#   - CNAME chain handling
#   - config load/save time (in-process, via configbench)
#   - short soak (RSS growth under sustained load)
#
# Requirements: root (default build uses /var/lib/magitrickle and
# /var/run/magitrickle.pid), Go toolchain, free ports 3553/5399/8080.
# ipset/iptables privileges are NOT required: the benchmark group is
# generated with enable:false, so rule matching runs but ipset writes are
# skipped. On a real router re-run with GROUP_ENABLE=1.
#
# Usage: sh tools/bench/run_baseline.sh [results_dir]
set -eu

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$BENCH_DIR/../.." && pwd)"
RESULTS_DIR="${1:-$REPO_DIR/docs/c-rewrite/baseline-raw}"
GROUP_ENABLE="${GROUP_ENABLE:-0}"
STATE_DIR="/var/lib/magitrickle"
CFG="$STATE_DIR/config.yaml"
UPSTREAM_PORT=5399
PROXY_PORT=3553
WARMUP_SEC="${WARMUP_SEC:-2}"
LOAD_SEC="${LOAD_SEC:-5}"
REPEATS="${REPEATS:-3}"
SOAK_SEC="${SOAK_SEC:-60}"

mkdir -p "$RESULTS_DIR"
BIN_DIR="$REPO_DIR/.build/bench"
mkdir -p "$BIN_DIR"

log() { echo "[bench] $*" >&2; }

# ---------------------------------------------------------------- build
# NB: a version must be injected — SaveConfig writes constant.Version as
# configVersion, and LoadConfig rejects anything not starting with "0.",
# so an unversioned dev build cannot re-load its own saved config.
VERSION_LDFLAG="-X 'magitrickle/constant.Version=0.99.0'"
log "building daemon (host, no tags, no upx)"
( cd "$REPO_DIR/src/backend" && go build -trimpath -ldflags="-w -s $VERSION_LDFLAG" -o "$BIN_DIR/magitrickled" ./cmd/magitrickled )
log "building bench tools"
( cd "$BENCH_DIR" && go build -o "$BIN_DIR/dnsstub" ./dnsstub && go build -o "$BIN_DIR/dnsload" ./dnsload && go build -ldflags="$VERSION_LDFLAG" -o "$BIN_DIR/configbench" ./configbench )

# ---------------------------------------------------------------- env info
{
  echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "kernel: $(uname -sr)"
  echo "arch: $(uname -m)"
  echo "go_version: $(go version)"
  echo "cpu_model: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //' || true)"
  echo "cpus: $(nproc)"
  echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'n/a')"
  echo "libc: $(ldd --version 2>/dev/null | head -1 || echo 'n/a')"
  echo "git_commit: $(cd "$REPO_DIR" && git rev-parse HEAD)"
  echo "binary_size_bytes: $(stat -c %s "$BIN_DIR/magitrickled")"
  echo "group_enable: $GROUP_ENABLE"
  echo "load_sec: $LOAD_SEC  repeats: $REPEATS  soak_sec: $SOAK_SEC"
} > "$RESULTS_DIR/environment.txt"
log "environment recorded"

# ---------------------------------------------------------------- helpers
STUB_PID=""
DAEMON_PID=""
cleanup() {
  [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
  [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

rss_kb()     { awk '/VmRSS/ {print $2}' "/proc/$1/status"; }
hwm_kb()     { awk '/VmHWM/ {print $2}' "/proc/$1/status"; }
threads_of() { awk '/Threads/ {print $2}' "/proc/$1/status"; }
pss_kb()     { awk '/^Pss:/ {s+=$2} END {print s}' "/proc/$1/smaps_rollup" 2>/dev/null || echo "n/a"; }
cpu_ticks()  { awk '{print $14+$15}' "/proc/$1/stat"; }

measure_cpu() { # pid seconds -> prints cpu_percent
  t0=$(cpu_ticks "$1"); sleep "$2"; t1=$(cpu_ticks "$1")
  hz=$(getconf CLK_TCK)
  awk -v a="$t0" -v b="$t1" -v s="$2" -v hz="$hz" 'BEGIN {printf "%.1f\n", (b-a)/hz/s*100}'
}

start_daemon() { # config already in place
  rm -f /var/run/magitrickle.pid
  "$BIN_DIR/magitrickled" >"$RESULTS_DIR/daemon.log" 2>&1 &
  DAEMON_PID=$!
}

stop_daemon() {
  [ -n "$DAEMON_PID" ] || return 0
  kill "$DAEMON_PID" 2>/dev/null || true
  wait "$DAEMON_PID" 2>/dev/null || true
  DAEMON_PID=""
}

# ---------------------------------------------------------------- stub
"$BIN_DIR/dnsstub" -listen "127.0.0.1:$UPSTREAM_PORT" &
STUB_PID=$!
sleep 0.3

mkdir -p "$STATE_DIR"
[ -f "$CFG" ] && cp "$CFG" "$RESULTS_DIR/config.yaml.bak"

RESULTS="$RESULTS_DIR/results.jsonl"
: > "$RESULTS"

GEN_FLAGS=""
[ "$GROUP_ENABLE" = "1" ] && GEN_FLAGS="-group-enable"

# ---------------------------------------------------------------- startup + idle (1000 rules)
( cd "$BENCH_DIR" && go run ./genconfig -rules 1000 -type namespace $GEN_FLAGS -out "$CFG" )
START_NS=$(date +%s%N)
start_daemon
STARTUP_MS=$("$BIN_DIR/dnsload" -mode probe -server "127.0.0.1:$PROXY_PORT")
END_NS=$(date +%s%N)
TOTAL_STARTUP_MS=$(( (END_NS - START_NS) / 1000000 ))
sleep "$WARMUP_SEC"
IDLE_RSS=$(rss_kb "$DAEMON_PID"); IDLE_PSS=$(pss_kb "$DAEMON_PID"); IDLE_THREADS=$(threads_of "$DAEMON_PID")
IDLE_CPU=$(measure_cpu "$DAEMON_PID" 10)
echo "{\"metric\":\"startup\",\"probe_ms\":$STARTUP_MS,\"total_ms\":$TOTAL_STARTUP_MS,\"idle_rss_kb\":$IDLE_RSS,\"idle_pss_kb\":\"$IDLE_PSS\",\"threads\":$IDLE_THREADS,\"idle_cpu_pct\":$IDLE_CPU}" >> "$RESULTS"
log "startup probe=${STARTUP_MS}ms rss=${IDLE_RSS}kB threads=$IDLE_THREADS idle_cpu=${IDLE_CPU}%"
stop_daemon

# ---------------------------------------------------------------- load matrix
for RULES in 100 1000 10000 100000; do
  for RTYPE in namespace mixed; do
    # skip the expensive combination duplicates: 100k only namespace
    [ "$RULES" = "100000" ] && [ "$RTYPE" = "mixed" ] && continue
    ( cd "$BENCH_DIR" && go run ./genconfig -rules "$RULES" -type "$RTYPE" $GEN_FLAGS -out "$CFG" )
    start_daemon
    "$BIN_DIR/dnsload" -mode probe -server "127.0.0.1:$PROXY_PORT" >/dev/null
    sleep "$WARMUP_SEC"
    for PROTO in udp tcp; do
      for CONC in 10 100; do
        # match scenario: query names hit rules; nomatch: full scan miss
        for SCEN in match nomatch; do
          PAT="d%06d.bench.example.com."
          [ "$SCEN" = "nomatch" ] && PAT="x%06d.miss.example.net."
          REP=0
          while [ "$REP" -lt "$REPEATS" ]; do
            CPU_FILE=$(mktemp)
            ( measure_cpu "$DAEMON_PID" "$LOAD_SEC" > "$CPU_FILE" ) &
            CPU_JOB=$!
            OUT=$("$BIN_DIR/dnsload" -server "127.0.0.1:$PROXY_PORT" -proto "$PROTO" \
              -concurrency "$CONC" -duration "${LOAD_SEC}s" -ndomains "$RULES" \
              -pattern "$PAT" -label "rules=$RULES type=$RTYPE scen=$SCEN rep=$REP")
            wait "$CPU_JOB"
            CPU=$(cat "$CPU_FILE"); rm -f "$CPU_FILE"
            RSS=$(rss_kb "$DAEMON_PID")
            echo "$OUT" | sed "s/}$/,\"cpu_pct\":$CPU,\"rss_kb\":$RSS,\"rules\":$RULES,\"rule_type\":\"$RTYPE\",\"scenario\":\"$SCEN\"}/" >> "$RESULTS"
            REP=$((REP+1))
          done
        done
      done
    done
    HWM=$(hwm_kb "$DAEMON_PID")
    echo "{\"metric\":\"peak\",\"rules\":$RULES,\"rule_type\":\"$RTYPE\",\"vmhwm_kb\":$HWM}" >> "$RESULTS"
    log "rules=$RULES type=$RTYPE done (VmHWM=${HWM}kB)"
    stop_daemon
  done
done

# ---------------------------------------------------------------- CNAME chains
kill "$STUB_PID" 2>/dev/null || true; wait "$STUB_PID" 2>/dev/null || true
for CHAIN in 2 10; do
  "$BIN_DIR/dnsstub" -listen "127.0.0.1:$UPSTREAM_PORT" -cname "$CHAIN" &
  STUB_PID=$!
  sleep 0.3
  ( cd "$BENCH_DIR" && go run ./genconfig -rules 1000 -type namespace $GEN_FLAGS -out "$CFG" )
  start_daemon
  "$BIN_DIR/dnsload" -mode probe -server "127.0.0.1:$PROXY_PORT" >/dev/null
  sleep "$WARMUP_SEC"
  OUT=$("$BIN_DIR/dnsload" -server "127.0.0.1:$PROXY_PORT" -proto udp -concurrency 10 \
    -duration "${LOAD_SEC}s" -ndomains 1000 -label "cname_chain=$CHAIN")
  echo "$OUT" | sed "s/}$/,\"cname_chain\":$CHAIN}/" >> "$RESULTS"
  stop_daemon
  kill "$STUB_PID" 2>/dev/null || true; wait "$STUB_PID" 2>/dev/null || true
done
"$BIN_DIR/dnsstub" -listen "127.0.0.1:$UPSTREAM_PORT" &
STUB_PID=$!
sleep 0.3

# ---------------------------------------------------------------- upstream down behaviour
( cd "$BENCH_DIR" && go run ./genconfig -rules 100 -type namespace $GEN_FLAGS -upstream-port 5998 -out "$CFG" )
start_daemon
sleep 1
OUT=$("$BIN_DIR/dnsload" -server "127.0.0.1:$PROXY_PORT" -proto udp -concurrency 10 \
  -duration "${LOAD_SEC}s" -ndomains 100 -timeout 1s -label "upstream_down")
echo "$OUT" | sed 's/}$/,"scenario":"upstream_down"}/' >> "$RESULTS"
stop_daemon
log "upstream-down scenario done"

# ---------------------------------------------------------------- config load/save
for RULES in 1000 10000; do
  ( cd "$BENCH_DIR" && go run ./genconfig -rules "$RULES" -type mixed $GEN_FLAGS -out "$CFG" )
  OUT=$("$BIN_DIR/configbench")
  echo "$OUT" | sed "s/}$/,\"metric\":\"configbench\",\"rules\":$RULES}/" >> "$RESULTS"
done
log "configbench done"

# ---------------------------------------------------------------- soak
( cd "$BENCH_DIR" && go run ./genconfig -rules 10000 -type namespace $GEN_FLAGS -out "$CFG" )
start_daemon
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
stop_daemon
log "soak done"

# restore config backup
[ -f "$RESULTS_DIR/config.yaml.bak" ] && cp "$RESULTS_DIR/config.yaml.bak" "$CFG" || rm -f "$CFG"

log "all results in $RESULTS"
