# Performance baseline — Go backend (host)

Raw data: `baseline-raw/results.jsonl`, `baseline-raw/environment.txt`,
`baseline-raw/soak_rss.csv`. Reproduce: `sh tools/bench/run_baseline.sh`
(+ `tools/bench/run_tail.sh` for configbench/soak only).
Summarize: `python3 tools/bench/summarize.py baseline-raw/results.jsonl`.

## Environment

- Date: 2026-07-22 (UTC); commit `8c8bd12` + bench tooling.
- Host: shared cloud container, Intel Xeon @2.80 GHz, 4 vCPU, x86_64,
  Linux 6.18.5, glibc 2.39 (Ubuntu 24.04), governor n/a (virtualized).
- Go 1.24.7 (CI uses 1.23 — noted; toolchain minor-version drift accepted
  for baseline v1, will pin 1.23 for the final comparison).
- Daemon: host build, no platform tags, **no UPX**, version-injected;
  binary 9.6 MB (informational only — size is a non-goal).
- Upstream: local `dnsstub` (fixed A answer), i.e. numbers measure the
  proxy+matching stack, not a real resolver.
- **Benchmark group is `enable: false`** — full DNS+cache+match path runs,
  ipset/netfilter writes are skipped (no ipset kernel module in the
  container). Netfilter-path numbers are **pending on-device runs**
  (`GROUP_ENABLE=1`).
- IPv6 unavailable in container → listeners on `0.0.0.0` (device default is
  `[::]`).
- Caveat: shared/virtualized CPU; treat absolute numbers as indicative,
  spreads are given below. 1000-concurrency cells intentionally deferred
  (loader saturates the 4 vCPU host first) — see benchmark-methodology.md.

## Startup / idle (1000 namespace rules)

| Metric | Value |
|---|---|
| exec → first DNS answer | **51.6 ms** (probe; total incl. fork ~58 ms) |
| idle RSS | 15.3 MB |
| idle PSS | 13.9 MB |
| threads | 10 |
| goroutines | not instrumented (no pprof endpoint in app) — план: временная инструментализация в Phase 1 harness |
| idle CPU (10 s) | 0.1 % |

## DNS load matrix (4 s per cell, 3 repeats, median (min–max))

cpu% = daemon process CPU (100% = 1 core); rss = after the cell.

| rules | type | scenario | proto | conc | rps med (min–max) | p50 ms | p95 ms | p99 ms | cpu% | rss MB |
|---|---|---|---|---|---|---|---|---|---|---|
| 100 | mixed | match | tcp | 10 | 11530 (11362–11784) | 0.66 | 1.36 | 1.98 | 155 | 18.2 |
| 100 | mixed | match | tcp | 100 | 14389 (14186–14794) | 5.96 | 11.82 | 15.25 | 183 | 18.9 |
| 100 | mixed | match | udp | 10 | 16710 (16454–18826) | 0.54 | 1.12 | 1.66 | 165 | 17.4 |
| 100 | mixed | match | udp | 100 | 34139 (32779–34392) | 2.73 | 5.36 | 6.92 | 226 | 28.4 |
| 100 | mixed | nomatch | tcp | 10 | 9954 (9570–10128) | 0.77 | 1.58 | 2.25 | 162 | 18.1 |
| 100 | mixed | nomatch | tcp | 100 | 14110 (13527–14550) | 5.96 | 12.55 | 18.41 | 184 | 19.1 |
| 100 | mixed | nomatch | udp | 10 | 16531 (16174–17517) | 0.55 | 1.10 | 1.61 | 168 | 17.5 |
| 100 | mixed | nomatch | udp | 100 | 28205 (27599–29932) | 3.31 | 6.55 | 8.44 | 234 | 29.6 |
| 100 | namespace | match | tcp | 10 | 11198 (11092–11484) | 0.67 | 1.34 | 1.91 | 140 | 17.8 |
| 100 | namespace | match | tcp | 100 | 18373 (15177–18647) | 4.68 | 8.77 | 11.74 | 168 | 18.5 |
| 100 | namespace | match | udp | 10 | 21312 (19164–21691) | 0.43 | 0.85 | 1.24 | 143 | 17.0 |
| 100 | namespace | match | udp | 100 | 36631 (36033–40408) | 2.54 | 4.83 | 6.41 | 198 | 26.6 |
| 100 | namespace | nomatch | tcp | 10 | 13272 (13022–13532) | 0.56 | 1.13 | 1.63 | 137 | 17.9 |
| 100 | namespace | nomatch | tcp | 100 | 15306 (14801–15594) | 5.58 | 10.64 | 13.70 | 168 | 18.7 |
| 100 | namespace | nomatch | udp | 10 | 22546 (20701–22564) | 0.40 | 0.80 | 1.15 | 142 | 17.1 |
| 100 | namespace | nomatch | udp | 100 | 43382 (42210–43628) | 2.16 | 4.10 | 5.45 | 195 | 28.2 |
| 1000 | mixed | match | tcp | 10 | 6093 (5614–6116) | 1.34 | 2.93 | 3.98 | 229 | 20.2 |
| 1000 | mixed | match | tcp | 100 | 6299 (5956–6732) | 14.29 | 31.66 | 41.97 | 254 | 21.2 |
| 1000 | mixed | match | udp | 10 | 7066 (7037–7184) | 1.27 | 2.81 | 3.79 | 259 | 19.7 |
| 1000 | mixed | match | udp | 100 | 9227 (9070–9332) | 8.82 | 25.95 | 32.76 | 306 | 26.6 |
| 1000 | mixed | nomatch | tcp | 10 | 4814 (4692–4897) | 1.72 | 3.70 | 4.96 | 245 | 20.6 |
| 1000 | mixed | nomatch | tcp | 100 | 5833 (5767–6072) | 15.04 | 36.09 | 46.76 | 261 | 21.2 |
| 1000 | mixed | nomatch | udp | 10 | 6233 (6224–6866) | 1.44 | 3.12 | 4.14 | 275 | 20.4 |
| 1000 | mixed | nomatch | udp | 100 | 7446 (7272–7452) | 11.16 | 33.01 | 40.88 | 308 | 25.4 |
| 1000 | namespace | match | tcp | 10 | 11638 (11275–12650) | 0.64 | 1.32 | 1.93 | 144 | 18.2 |
| 1000 | namespace | match | tcp | 100 | 17094 (15806–17386) | 4.99 | 9.68 | 12.57 | 171 | 19.1 |
| 1000 | namespace | match | udp | 10 | 20811 (19176–20856) | 0.44 | 0.88 | 1.30 | 148 | 17.3 |
| 1000 | namespace | match | udp | 100 | 34946 (34590–35082) | 2.68 | 5.12 | 6.76 | 205 | 27.5 |
| 1000 | namespace | nomatch | tcp | 10 | 12705 (12453–12833) | 0.59 | 1.21 | 1.78 | 143 | 18.0 |
| 1000 | namespace | nomatch | tcp | 100 | 16813 (16510–17768) | 5.11 | 9.74 | 12.45 | 174 | 19.0 |
| 1000 | namespace | nomatch | udp | 10 | 20038 (18798–20289) | 0.45 | 0.90 | 1.33 | 148 | 17.8 |
| 1000 | namespace | nomatch | udp | 100 | 33094 (32414–34324) | 2.83 | 5.32 | 6.86 | 208 | 28.6 |
| 10000 | mixed | match | tcp | 10 | 1129 (1100–1237) | 7.93 | 17.61 | 23.48 | 346 | 48.6 |
| 10000 | mixed | match | tcp | 100 | 1280 (1204–1350) | 69.17 | 180.11 | 237.19 | 334 | 51.5 |
| 10000 | mixed | match | udp | 10 | 1346 (1306–1370) | 6.75 | 15.11 | 20.46 | 359 | 43.0 |
| 10000 | mixed | match | udp | 100 | 1295 (1268–1323) | 65.91 | 187.51 | 250.15 | 367 | 54.5 |
| 10000 | mixed | nomatch | tcp | 10 | 1097 (936–1132) | 8.23 | 17.32 | 22.88 | 347 | 48.6 |
| 10000 | mixed | nomatch | tcp | 100 | 977 (956–1002) | 92.50 | 241.93 | 320.34 | 352 | 51.4 |
| 10000 | mixed | nomatch | udp | 10 | 1031 (957–1032) | 8.96 | 19.25 | 25.29 | 354 | 47.1 |
| 10000 | mixed | nomatch | udp | 100 | 989 (984–1008) | 80.97 | 258.32 | 327.82 | 371 | 55.1 |
| 10000 | namespace | match | tcp | 10 | 9262 (8816–9419) | 0.85 | 1.76 | 2.52 | 176 | 28.8 |
| 10000 | namespace | match | tcp | 100 | 12437 (12271–12748) | 7.05 | 14.16 | 18.69 | 212 | 29.7 |
| 10000 | namespace | match | udp | 10 | 12497 (12154–13211) | 0.73 | 1.48 | 2.09 | 193 | 25.8 |
| 10000 | namespace | match | udp | 100 | 17580 (17376–19092) | 5.22 | 11.41 | 14.86 | 244 | 40.8 |
| 10000 | namespace | nomatch | tcp | 10 | 8323 (7901–8554) | 0.96 | 1.94 | 2.70 | 182 | 28.8 |
| 10000 | namespace | nomatch | tcp | 100 | 11435 (11311–11503) | 7.79 | 15.93 | 20.04 | 221 | 29.7 |
| 10000 | namespace | nomatch | udp | 10 | 11374 (11092–11496) | 0.79 | 1.63 | 2.26 | 199 | 29.0 |
| 10000 | namespace | nomatch | udp | 100 | 16290 (15949–16874) | 5.64 | 12.88 | 16.64 | 248 | 40.5 |
| 100000 | namespace | match | tcp | 10 | 3020 (2966–3072) | 2.88 | 6.08 | 7.99 | 299 | 107.9 |
| 100000 | namespace | match | tcp | 100 | 3152 (3122–3245) | 27.71 | 72.55 | 91.99 | 307 | 96.2 |
| 100000 | namespace | match | udp | 10 | 3445 (3411–3446) | 2.68 | 5.82 | 7.67 | 320 | 263.6 |
| 100000 | namespace | match | udp | 100 | 3824 (3477–3832) | 22.79 | 70.68 | 88.01 | 338 | 107.8 |
| 100000 | namespace | nomatch | tcp | 10 | 2737 (2728–2788) | 3.17 | 6.41 | 8.25 | 296 | 96.1 |
| 100000 | namespace | nomatch | tcp | 100 | 2909 (2900–2927) | 30.92 | 83.06 | 105.48 | 310 | 96.2 |
| 100000 | namespace | nomatch | udp | 10 | 3030 (3025–3206) | 3.04 | 6.30 | 8.46 | 320 | 323.5 |
| 100000 | namespace | nomatch | udp | 100 | 3257 (3234–3369) | 29.04 | 77.84 | 101.82 | 328 | 107.8 |

## Peak memory per block (VmHWM)

| rules | type | VmHWM |
|---|---|---|
| 100 | namespace / mixed | 28.6 / 30.1 MB |
| 1000 | namespace / mixed | 28.9 / 33.4 MB |
| 10000 | namespace / mixed | 41.8 / 74.3 MB |
| 100000 | namespace | 344.5 MB |

## CNAME chains (1000 namespace rules, UDP, conc 10)

| chain length | rps | p50 | p99 |
|---|---|---|---|
| 0 (plain A) | ~20 000 | 0.44 ms | 1.3 ms |
| 2 | 11 807 | 0.77 ms | 2.15 ms |
| 10 | 3 038 | 2.96 ms | 8.63 ms |

## Upstream down (UDP, conc 10, 1 s client timeout)

Proxy sends nothing on upstream failure (contract): 40 sent, 0 answered,
40 client timeouts, ~0% daemon CPU. No crash, no leak.

## Config load/save (in-process, 5 runs)

| rules | LoadConfig ms | SaveConfig ms |
|---|---|---|
| 1000 (mixed) | 8.3–10.0 | 15.1–34.0 |
| 10000 (mixed) | 74.6–178.3 | 118.8–350.8 |

## Soak (60 s, UDP, conc 50, 10 000 namespace rules)

1 110 581 queries, 0 errors/timeouts, 18 510 rps sustained,
p50 2.43 ms / p95 5.53 ms / p99 7.26 ms.
RSS flat: 30.0→31.4 MB over the run (samples in `soak_rss.csv`),
VmHWM 41.9 MB. No growth trend.

## Observed bottlenecks (to drive the C design)

1. **Rule matching dominates as rule count grows.** Namespace-only:
   ~34 krps @1k rules → ~16 krps @10k → ~3.3 krps @100k (linear scan per
   answer record). Mixed set (25% regex + 25% wildcard): collapses to
   ~1 krps @10k rules with ~3.6 cores busy — regexp2 scan cost. C design
   answer: exact/namespace indexes (hash + reversed-label trie) and
   pre-compiled PCRE2 scanned only as the residual class (migration-plan
   Phase 2/4, spec §14).
2. **Per-request goroutine + GC churn** show as CPU 1.4–3.7 cores at
   moderate loads; epoll + bounded workers should cut both CPU and the
   28–55 MB under-load RSS.
3. **CNAME processing** re-walks alias closures per record; chain of 10
   costs ~6× vs chain of 2. Reverse-index approach is fine; C should keep
   it but avoid re-matching all groups per hop.
4. **Config save at 10k rules** costs up to 350 ms (marshal + write) —
   acceptable, but C should stream-emit YAML to stay in the same ballpark.
5. **100k rules**: load works but RSS up to ~350 MB peak during load
   (models + matching garbage). C target: an order of magnitude lower.

## Pending device measurements (no hardware in this environment)

- Real ipset/iptables add/list latency and netfilter-queue behaviour
  (`GROUP_ENABLE=1` runs on Keenetic/OpenWrt).
- mips/arm softfloat CPU profiles; startup on flash storage.
- 1000-concurrency cells on a dedicated host.
- Subscription update timing end-to-end (needs network).
Commands are ready in `tools/bench/`; mark results here when collected.

## Proposed acceptance thresholds for the C backend (host, same cells)

Derived from the repeat spread above (min–max within ±5–8% for most cells;
shared-host noise): a C result is a regression only if it falls outside
these bounds in the final side-by-side run (same host, back-to-back):

- Throughput: C ≥ 1.0× Go median on every namespace/domain cell;
  C ≥ 3× Go median on mixed/regex cells at ≥10k rules (that's the point of
  the rewrite; if PCRE2 cannot deliver ≥1.0×, investigate before merge).
- Latency: C p50 ≤ Go p50, C p99 ≤ 1.25× Go p99 per cell.
- Memory: idle RSS ≤ 0.5× Go (≤ 7.5 MB); under-load RSS ≤ 0.5× Go per
  cell; 60 s soak RSS drift ≤ +2% after warm-up.
- CPU: ≤ 0.7× Go cores at equal offered load (namespace cells).
- Startup: ≤ 52 ms to first answer (≤ 1.0× Go).
- Config: load/save at 10k rules ≤ 1.0× Go medians.
- Zero errors/timeouts in all cells where Go shows zero.

These are v1 thresholds; re-derive after pinning a quieter benchmark host
and before Phase 9 sign-off.
