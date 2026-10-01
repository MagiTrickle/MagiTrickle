#!/bin/sh
# HTTP API contract regression test. Originally a Go-vs-C differential
# (Phase 6, task #37): ran the real Go daemon and magitrickled-c against
# byte-identical scratch configs, drove each through the same fixed
# request sequence (http_contract/contract.py), and diffed the two
# traces -- the last such run (golden/http_contract.trace, captured
# immediately before Go was removed in Phase 9) was byte-identical
# across both backends, 44/44 steps. Now that src/backend (Go) is gone,
# this compares magitrickled-c's live trace against that golden
# snapshot instead: a real behavioral regression in the C daemon's HTTP
# surface still shows up as a diff, it just can no longer be checked
# against a live Go reference. See decisions.md D-45 for the full
# rationale and docs/c-rewrite/compatibility-contract.md +
# parity-checklist.md for what was verified while Go was still present.
#
# Requires root (real /var/lib/magitrickle/config.yaml, real iptables).
# Temporarily overwrites /var/lib/magitrickle/config.yaml -- any
# pre-existing file there is backed up and restored on exit. Also
# requires python3 (stdlib only).
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
BACKEND_C_DIR="$(cd "$DIR/../.." && pwd)"
OUT="$DIR/out"
GOLDEN="$DIR/golden/http_contract.trace"
mkdir -p "$OUT"

REAL_CONFIG=/var/lib/magitrickle/config.yaml
REAL_CONFIG_BACKUP="$OUT/http_real_config.backup"
SCRATCH_CONFIG="$OUT/http_scratch_config.yaml"
PORT=18095
DNS_PORT=13595
SOCK=/var/run/magitrickle.sock
PIDFILE=/var/run/magitrickle.pid

C_PID=
cleanup() {
    [ -n "$C_PID" ] && kill "$C_PID" 2>/dev/null || true
    if [ -f "$REAL_CONFIG_BACKUP" ]; then
        cp "$REAL_CONFIG_BACKUP" "$REAL_CONFIG"
    else
        rm -f "$REAL_CONFIG"
    fi
    rm -f "$SOCK" "$PIDFILE"
}
trap cleanup EXIT

if [ -f "$REAL_CONFIG" ]; then
    cp "$REAL_CONFIG" "$REAL_CONFIG_BACKUP"
else
    rm -f "$REAL_CONFIG_BACKUP"
fi

cat > "$SCRATCH_CONFIG" <<EOF
configVersion: 0.7.0
app:
  httpWeb:
    enabled: true
    auth:
      enabled: false
    host:
      address: "127.0.0.1"
      port: $PORT
    skin: default
  dnsProxy:
    host:
      address: "127.0.0.1"
      port: $DNS_PORT
    upstream:
      address: 127.0.0.1
      port: 53
    disableRemap53: true
    disableFakePTR: false
    disableDropAAAA: false
    maxIdleConns: 10
    maxConcurrent: 100
    timeout: 5s
  netfilter:
    iptables:
      chainPrefix: MT_
    ipset:
      tablePrefix: mt_
      additionalTTL: 1h0m0s
    disableIPv4: false
    disableIPv6: false
    startMarkTableIndex: 1298229097
  link: []
  showAllInterfaces: true
  logLevel: error
groups: []
subscriptions: []
EOF

echo "== building C daemon"
( cd "$BACKEND_C_DIR" && make build/host/magitrickled-c >/dev/null )
C_BIN="$BACKEND_C_DIR/build/host/magitrickled-c"

wait_for_port() {
    i=0
    while [ "$i" -lt 50 ]; do
        if curl -s -o /dev/null "http://127.0.0.1:$PORT/api/v1/auth"; then return 0; fi
        i=$((i + 1))
        sleep 0.1
    done
    return 1
}

echo "== running contract against C daemon"
mkdir -p /var/lib/magitrickle
cp "$SCRATCH_CONFIG" "$REAL_CONFIG"
rm -f "$SOCK" "$PIDFILE"
"$C_BIN" --config "$SCRATCH_CONFIG" > "$OUT/c_daemon.log" 2>&1 &
C_PID=$!
if ! wait_for_port; then
    echo "C daemon failed to start:"; cat "$OUT/c_daemon.log"; exit 1
fi
python3 "$DIR/http_contract/contract.py" 127.0.0.1 "$PORT" "$SOCK" > "$OUT/c.trace" 2> "$OUT/c_contract.err"
CONTRACT_STATUS=$?
kill "$C_PID" 2>/dev/null || true
wait "$C_PID" 2>/dev/null || true
C_PID=
if [ "$CONTRACT_STATUS" -ne 0 ]; then
    echo "contract run against C failed:"; cat "$OUT/c_contract.err"; exit 1
fi

echo "== diffing against golden trace"
if diff -u "$GOLDEN" "$OUT/c.trace" > "$OUT/http_contract.diff"; then
    echo "   HTTP contract: OK ($(grep -c '^STEP' "$GOLDEN") steps)"
else
    echo "   HTTP CONTRACT REGRESSION (vs golden/http_contract.trace):"
    cat "$OUT/http_contract.diff"
    exit 1
fi
