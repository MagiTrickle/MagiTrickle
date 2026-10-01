# Benchmark methodology — comparing C vs Go fairly

## Tooling (in-repo, reproducible)

`tools/bench/` (Go module, independent of the backend except a `replace`
for configbench):

- `dnsstub` — fixed-answer upstream (UDP+TCP, optional CNAME chain of
  length N). Near-zero cost so the proxy under test dominates.
- `dnsload` — load generator: UDP (persistent socket per worker) and TCP
  (connection per query — mirrors the proxy's one-query-per-conn contract);
  fixed concurrency and duration; JSON output with sent/ok/errors/timeouts,
  rps, p50/p95/p99/max latency. `-mode probe` measures time-to-first-answer
  (startup metric).
- `genconfig` — deterministic config generator: N rules of type
  domain/namespace/wildcard/regex/mixed, benchmark group **disabled** by
  default so the full DNS+matching path runs without netfilter privileges
  (ipset writes short-circuit); `-group-enable` for on-device runs with real
  ipset/iptables.
- `configbench` — in-process load/save timing of the config engine.
- `run_baseline.sh` — orchestration: environment capture, startup, idle,
  load matrix, CNAME chains, upstream-down, config load/save, 60 s soak
  with RSS sampling. Raw results → `docs/c-rewrite/baseline-raw/`
  (`environment.txt`, `results.jsonl`, `soak_rss.csv`).

## Protocol for any Go↔C comparison

1. Same host, same kernel, same governor, same CPU set; run pairs
   back-to-back; record `environment.txt` for both.
2. Same MagiTrickle config files (produced by `genconfig` with identical
   flags), same dnsstub parameters, same dnsload parameters.
3. Warm-up ≥2 s after first successful probe; then ≥3 repeats per cell;
   report median and min–max spread; a difference smaller than the spread
   of either side is *not* a result.
4. Metrics per cell: ok-RPS, p50/p95/p99, errors+timeouts, daemon CPU%
   (delta of utime+stime over the run), post-run RSS, VmHWM.
5. Memory: VmRSS + Pss (smaps_rollup) idle and after load; soak = 60 s+
   sustained load with 5 s RSS sampling — the series must be flat for both.
6. DNS correctness during perf runs is asserted by dnsload (answers must
   parse and match the query); functional equivalence is the differential
   harness's job, not the load generator's.
7. On-device runs (Keenetic/OpenWrt): same scripts (POSIX sh; Go tools
   cross-compiled or replaced by `dnsperf` if Go tools can't run there —
   in that case use the same replacement for both backends), plus
   `GROUP_ENABLE=1` to exercise real ipset/iptables. Device measurements
   pending until hardware is available; host numbers never substitute for
   device claims.

## Load matrix (host baseline v1)

- Rules: 100 / 1000 / 10000 (namespace + mixed), 100000 (namespace).
- Scenarios: `match` (queried names hit rules) and `nomatch` (full scan).
- Transports: UDP and TCP; concurrency 10 and 100.
- CNAME chains: length 2 and 10 at 1000 rules.
- Upstream-down: unreachable upstream, measures error handling economy.
- 1000-concurrency cell is deferred to device/dedicated host runs: on the
  4-vCPU shared CI container it saturates the loader before the daemon and
  produces noise (documented limitation).

## What is explicitly NOT a benchmark goal

Binary size, package size, UPX ratios (spec §1). UPX impact on startup and
RSS *is* measured in Phase 8 before deciding to keep/drop it.

## Acceptance thresholds for later phases

Derived from measured baseline spread — see `performance-baseline.md`
§"Proposed acceptance thresholds". Thresholds are set as
"C must be ≥ X% of Go throughput / ≤ Y× Go latency / ≤ Z× Go RSS" per cell,
with the noise floor taken from the baseline's own repeat spread.
