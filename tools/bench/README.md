# tools/bench — MagiTrickle backend benchmark tooling

Reproducible performance measurement of the C daemon and original Go package.
For real-device comparisons, use [router-benchmark.md](router-benchmark.md):
paired package runs, real DNS upstream, paced and capacity workloads, response
validation, daemon and generator CPU recorded separately. Published device
results: [router-raw](../../docs/c-rewrite/router-raw/2026-10-14/).

Historical host methodology: `docs/c-rewrite/benchmark-methodology.md`.
Frozen Go baseline: `docs/c-rewrite/baseline-raw/`.

## Components

- `dnsstub` — fixed-answer upstream DNS (UDP+TCP; `-cname N` adds a CNAME
  chain to every A answer).
- `dnsload` — load generator; UDP = persistent socket per worker, TCP =
  one connection per query (matches the proxy contract). JSON results on
  stdout. `-mode probe` prints time-to-first-answer (startup metric).
- `genconfig` — deterministic bench configs (N rules,
  domain/namespace/wildcard/regex/mixed). Group is `enable: false` by
  default so the Go daemon runs DNS+matching without ipset/iptables privileges.
  C omits disabled groups from matching; use the enabled-group router protocol
  for fair package comparisons (D-80). `-group-enable` enables real netfilter.
- `configbench` — historical Go `LoadConfig`/`SaveConfig` timings; now a
  separate module. Requires an original backend checkout and an explicit
  replacement path (`cd configbench; go mod edit -replace=magitrickle=/path/to/src/backend; go mod tidy`).
- `run_baseline.sh` — historical Go orchestration; requires the removed
  Go backend checkout; see header comment for
  requirements (root, free ports 3553/5399/8080) and env vars
  (`LOAD_SEC`, `REPEATS`, `SOAK_SEC`, `GROUP_ENABLE`).

## Build the independent tools

```sh
cd tools/bench
go test -race ./dnsload
go build -o ../../.build/bench/dnsload ./dnsload
go build -o ../../.build/bench/dnsstub ./dnsstub
go build -o ../../.build/bench/genconfig ./genconfig
```

The main module does not depend on the deleted Go backend. `dnsload -corpus`
reads real names, `-qps` sets aggregate offered load (0 = unlimited). Success
requires a matching NOERROR A answer; errors include timeouts and invalid
responses. RPS uses actual elapsed time. UDP uses persistent worker sockets;
TCP opens a connection for each request. Latency includes TCP connection setup, the DNS exchange and response validation. `loader_cpu_pct` comes from `getrusage`.

`run_c_baseline.sh` remains the host C runner. Build the tools above and the
host C daemon first; its historical frozen numbers use the earlier loader.
New response checks and elapsed-time accounting do not retroactively change
the old JSONL files.
