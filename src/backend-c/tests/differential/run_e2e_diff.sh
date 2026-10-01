#!/bin/sh
# Frontend e2e suite (Playwright) against the real magitrickled-c
# serving the production frontend build (Phase 6, task #38).
#
# tests/e2e/*.spec.ts mock nearly every API call at the browser layer via
# page.route() (see e.g. groups.spec.ts's beforeEach) -- the backend
# actually running underneath mostly only needs to serve the static
# bundle (index.html + JS/CSS/asset files) correctly, since Playwright's
# route interception takes priority over whatever the real server would
# return. Running this suite against magitrickled-c is therefore mainly
# a real-world exercise of staticfiles.c (D-27) against an actual built
# Svelte app, not a functional test of the HTTP API layer (that's what
# run_http_diff.sh, D-29, is for) -- but a handful of specs (smoke.spec.ts's
# page title, the initial page load every spec's beforeEach depends on)
# do require the real server to serve real, correct bytes.
#
# Requires: node/npm (frontend build), a working `make` build of the C
# daemon, and Chromium reachable by Playwright (pre-installed in this
# environment; see the session's environment notes).
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
BACKEND_C_DIR="$(cd "$DIR/../.." && pwd)"
FRONTEND_DIR="$(cd "$BACKEND_C_DIR/../frontend" && pwd)"
OUT="$DIR/out"
mkdir -p "$OUT"

# 5173 (Vite's own dev-server default) matters here for a reason beyond
# habit: groups.spec.ts's clipboard tests call
# context.grantPermissions([...], { origin: "http://localhost:5173" })
# hardcoded to that exact origin (a pre-existing test detail, not
# something this suite introduced) -- serving the C daemon on the same
# port keeps the grant's origin matching the page's real origin without
# editing the spec file.
PORT=5173
DNS_PORT=13599
SKINS_DIR="$OUT/e2e_skins"
SCRATCH_CONFIG="$OUT/e2e_scratch_config.yaml"
SOCK=/var/run/magitrickle.sock

C_PID=
cleanup() {
    [ -n "$C_PID" ] && kill "$C_PID" 2>/dev/null || true
    [ -n "$C_PID" ] && wait "$C_PID" 2>/dev/null || true
    rm -f "$SOCK"
}
trap cleanup EXIT

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

echo "== building frontend"
( cd "$FRONTEND_DIR" && npm install --no-audit --no-fund >/dev/null && npm run build >/dev/null )
rm -rf "$SKINS_DIR"
mkdir -p "$SKINS_DIR/default"
cp -r "$FRONTEND_DIR/dist/." "$SKINS_DIR/default/"

echo "== building C daemon"
( cd "$BACKEND_C_DIR" && make build/host/magitrickled-c >/dev/null )
C_BIN="$BACKEND_C_DIR/build/host/magitrickled-c"

# MT_APP_SHARE_DIR is a compile-time default (paths.h) -- real
# per-platform overrides are Phase 8 packaging work, so this suite
# reroutes the real system path to today's scratch build rather than
# needing a special dev build flag. Any pre-existing skins directory at
# the real path is preserved and restored (mirrors run_http_diff.sh's
# config-file backup/restore for the same reason).
REAL_SKINS_ROOT=/usr/share/magitrickle
REAL_SKINS_BACKUP="$OUT/e2e_real_skins.backup"
rm -rf "$REAL_SKINS_BACKUP"
if [ -d "$REAL_SKINS_ROOT/skins" ]; then
    mv "$REAL_SKINS_ROOT/skins" "$REAL_SKINS_BACKUP"
fi
restore_skins() {
    rm -rf "$REAL_SKINS_ROOT/skins"
    if [ -d "$REAL_SKINS_BACKUP" ]; then
        mv "$REAL_SKINS_BACKUP" "$REAL_SKINS_ROOT/skins"
    fi
    rmdir "$REAL_SKINS_ROOT" 2>/dev/null || true
}
trap 'restore_skins; cleanup' EXIT

mkdir -p "$REAL_SKINS_ROOT/skins"
cp -r "$SKINS_DIR/default" "$REAL_SKINS_ROOT/skins/default"

wait_for_port() {
    i=0
    while [ "$i" -lt 50 ]; do
        if curl -s -o /dev/null "http://localhost:$PORT/"; then return 0; fi
        i=$((i + 1))
        sleep 0.1
    done
    return 1
}

echo "== starting C daemon"
rm -f "$SOCK" /var/run/magitrickle.pid
"$C_BIN" --config "$SCRATCH_CONFIG" > "$OUT/e2e_daemon.log" 2>&1 &
C_PID=$!
if ! wait_for_port; then
    echo "C daemon failed to start:"; cat "$OUT/e2e_daemon.log"; exit 1
fi

echo "== running Playwright e2e suite against magitrickled-c"
STATUS=0
( cd "$FRONTEND_DIR" && \
  MT_E2E_BASE_URL="http://localhost:$PORT" \
  npx playwright test --config=playwright.c-backend.config.ts ) || STATUS=$?

exit $STATUS
