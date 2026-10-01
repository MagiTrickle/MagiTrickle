#!/bin/sh
# Quick functional + throughput smoke of the C DNS proxy against dnsstub.
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO/.build/bench"
DAEMON="$REPO/src/backend-c/build/host/magitrickled-c"
CFG="${CFG:-/tmp/mt-c-smoke.yaml}"
SCRATCH="${SCRATCH:-/tmp}"

pkill -9 -f magitrickled-c 2>/dev/null
pkill -9 -x dnsstub 2>/dev/null
sleep 1

"$BIN/dnsstub" -listen 127.0.0.1:5399 > "$SCRATCH/stub.log" 2>&1 &
STUB=$!
( cd "$REPO/tools/bench" && go run ./genconfig -rules 1000 -type namespace \
    -addr 0.0.0.0 -upstream-port 5399 -out "$CFG" )
"$DAEMON" --config "$CFG" > "$SCRATCH/cdaemon.log" 2>&1 &
DAEMON_PID=$!
sleep 1

echo "stub alive: $(kill -0 $STUB 2>/dev/null && echo yes || echo NO)"
echo "daemon alive: $(kill -0 $DAEMON_PID 2>/dev/null && echo yes || echo NO)"

"$BIN/dnsload" -mode probe -server 127.0.0.1:3553 >/dev/null 2>&1 \
    && echo "probe: OK" || echo "probe: FAIL"

for proto in udp tcp; do
    for conc in 10 100; do
        echo "$proto c$conc: $("$BIN/dnsload" -server 127.0.0.1:3553 \
            -proto $proto -concurrency $conc -duration 3s -ndomains 1000)"
    done
done

# fake PTR check: query a PTR, expect NXDOMAIN with no answers
echo "fake-ptr rcode: $("$BIN/dnsload" -mode probe -server 127.0.0.1:3553 \
    -pattern '1.0.0.127.in-addr.arpa.' >/dev/null 2>&1 && echo replied)"

RSS=$(awk '/VmRSS/{print $2}' /proc/$DAEMON_PID/status 2>/dev/null)
THREADS=$(awk '/Threads/{print $2}' /proc/$DAEMON_PID/status 2>/dev/null)
echo "daemon RSS=${RSS}kB threads=${THREADS}"

kill -9 $DAEMON_PID $STUB 2>/dev/null
wait 2>/dev/null
