#!/bin/sh
# Runs the suite with a coverage-instrumented magitrickled and prints per-function coverage.
# Usage: ./cover.sh [go test args...]   (output in $E2E_COVER_OUT, default /tmp/e2e-cov)
set -e
cd "$(dirname "$0")"
OUT=${E2E_COVER_OUT:-/tmp/e2e-cov}
BIN=${E2E_COVER_BIN:-/tmp/e2e-covbin/magitrickled}
mkdir -p "$(dirname "$BIN")"
rm -rf "$OUT"; mkdir -p "$OUT"
(cd ../src/backend && GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build -cover -covermode=atomic -coverpkg=magitrickle/... \
	-tags entware,entware_kn -ldflags="-X magitrickle/constant.Version=0.0.0-e2e" -o "$BIN" ./cmd/magitrickled)
E2E_COVER_BIN=$BIN E2E_COVER_OUT=$OUT go test -count=1 -timeout 40m "${@:--v}" .
go tool covdata textfmt -i="$OUT" -o "$OUT.txt"
(cd ../src/backend && go tool cover -func="$OUT.txt" | tail -1)
