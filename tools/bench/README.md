# tools/bench — MagiTrickle backend benchmark tooling

Reproducible performance measurement for the Go backend (baseline for the
C rewrite) and, later, for Go↔C comparisons. Methodology:
`docs/c-rewrite/benchmark-methodology.md`. Raw results:
`docs/c-rewrite/baseline-raw/`.

## Components

- `dnsstub` — fixed-answer upstream DNS (UDP+TCP; `-cname N` adds a CNAME
  chain to every A answer).
- `dnsload` — load generator; UDP = persistent socket per worker, TCP =
  one connection per query (matches the proxy contract). JSON results on
  stdout. `-mode probe` prints time-to-first-answer (startup metric).
- `genconfig` — deterministic bench configs (N rules,
  domain/namespace/wildcard/regex/mixed). Group is `enable: false` by
  default so the daemon runs the whole DNS+matching path without
  ipset/iptables privileges; `-group-enable` for real-router runs.
- `configbench` — times `LoadConfig`/`SaveConfig` in-process (uses a
  `replace` on the backend module).
- `run_baseline.sh` — full orchestration; see header comment for
  requirements (root, free ports 3553/5399/8080) and env vars
  (`LOAD_SEC`, `REPEATS`, `SOAK_SEC`, `GROUP_ENABLE`).

## Quick start (host)

```sh
sh tools/bench/run_baseline.sh              # results to docs/c-rewrite/baseline-raw
GROUP_ENABLE=1 sh tools/bench/run_baseline.sh /tmp/device-results  # on a router
```
