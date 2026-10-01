# Migration plan — Go → C, phase by phase

Ground rules (spec §4, §23): Go stays the working reference until parity;
no mechanical transliteration; components move one at a time behind
contracts; nothing is deleted until Phase 9's gate passes; every phase ends
with tests green, cross-build green, docs updated, report delivered.

## Status board

| Phase | Status |
|---|---|
| 0 — audit & baseline | **done (this branch)** |
| 1 — C foundation | **done (this branch)** — see phase-1-report.md |
| 2 — models/YAML/rules | **done (this branch)** — see phase-2-report.md |
| 3 — DNS transport/parser | **done (this branch)** — see phase-3-report.md |
| 4 — DNS processing/cache | **done (this branch)** — see phase-4-report.md |
| 5 — netfilter/netlink | **done (this branch)** — see phase-5-report.md |
| 6 — API/WebUI | **done (this branch)** — see phase-6-report.md |
| 7 — subscriptions/integration | **done (this branch)** — see phase-7-report.md |
| 8 — packaging | **done (this branch)** — see phase-8-report.md |
| 9 — optimization & Go removal | **done (this branch)** — see phase-9-report.md |

## Phase 1 — foundation

Deliverables:
- `src/backend-c/` skeleton per structure in `decisions.md` D-01
  (include/, src/{main,core,config,dns,dns_cache,rules,groups,
  subscriptions,netfilter,netlink,http,auth,unix_api,static_server,
  platform,logging,util}, tests/{unit,integration,differential,fuzz,
  benchmarks}).
- Build system: Makefile driven, C11, warning set
  (-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Wundef
  -Wstrict-prototypes -Wmissing-prototypes), `make test`, `make sanitize`
  (ASan+UBSan host), `make static_analysis` (clang-tidy + cppcheck),
  cross-build skeleton consuming `.config` (PLATFORM/TARGET) with
  toolchain descriptors per `toolchains.md`.
- Error model (int codes + `mt_err` context), logging module (zerolog-like
  console lines, levels), lifecycle framework (init/destroy pairs, partial
  init unwinding), event loop core (epoll + timerfd + signalfd), bounded
  queue primitive with overflow policy + drop counters.
- Vendored test framework; CI job compiling host + 2 representative cross
  targets (mipsel-3.4_kn glibc, mipsel_24kc musl).
- Differential harness scaffolding: runner that drives Go and C binaries
  with identical inputs and diffs canonicalized outputs.
Exit: unit tests + sanitizers green on host; skeleton cross-builds.

## Phase 2 — models, YAML, rules

- models (group/rule/subscription/appconfig) with ownership rules;
- YAML load/save (libyaml) matching contract §1 incl. duration quirks and
  canonical save shape; atomic save (tmp+fsync+rename) — bytes identical;
- rule matching: exact hash, namespace reverse-trie, wildcard compiled,
  PCRE2 regex pre-compiled with match/depth limits;
- regex compatibility corpus runner (regexp2 via small Go oracle tool vs
  PCRE2) + documented divergence list;
- subscription parse/refresh/validate/IsDue ported with existing corpora;
- differential tests: Go↔C load/save cross-products, missing fields, bad
  types, empty collections, order, disabled entries, unknown fields,
  corrupt YAML.
Exit: differential YAML/rules/subscription suites green.

## Phase 3 — DNS transport & parser

- epoll UDP sockets with pktinfo source-address replies; TCP one-query
  conns; upstream pools (bounded idle lists); request correlation with
  per-request deadline (timerfd wheel); semaphore-equivalent bounded
  in-flight budget with backpressure;
- wire parser/packer per contract §4 (header/question/RR walk,
  decompression with loop+depth guards, A/AAAA/CNAME/PTR extraction,
  AAAA-strip repack with compression, fake-PTR synthesis);
- fuzz targets: parser, decompression, TCP framing;
- differential: semantic DNS comparison (canonical parse) Go vs C across a
  recorded query/response corpus incl. malformed inputs.
Exit: fuzz smoke clean (ASan), differential DNS suite green, host bench of
proxy path within provisional thresholds.

## Phase 4 — DNS processing & cache

- records cache (addresses/alias/reverse-alias, BFS aliases, forward chain
  walk, cycle guards) with **bounded** entry count + eviction + counters
  (bounds are hardening; defaults sized so contract behaviour unchanged);
- min-heap expiration; 30 s cleanup parity;
- hook pipeline: fake PTR, AAAA drop, cache insert, alias closure,
  rule-set matching with immutable snapshot publication (build → validate →
  atomic swap → deferred free);
- soak: cache growth flat under churn.
Exit: differential cache/matching suites green; no global lock in match
path; Phase-3+4 combined bench ≥ thresholds.

## Phase 5 — netfilter & netlink

- ipset via libmnl (create/destroy/flush/add/del/list, timeouts, replace);
- iptables save/restore batching engine with chain patch/override/delete
  semantics ported against the **same fake-executable transcripts** as
  `utils/iptables/iptables_test.go`;
- ip rule/route management, mark/table allocation, port remap, startup
  cleaner;
- netlink watcher (link/addr) driving rule-set hooks;
- fake netfilter backend for unit tests; netns integration tests (veth,
  real ipset) in CI where kernel allows.
Exit: transcript parity with Go fake tests; netns ITs green; on-device
smoke on one Entware `_kn` and one OpenWrt target.

## Phase 6 — API & WebUI

- HTTP/1.1 server (own, bounded) + unix socket sharing handler core;
- route table, DTO JSON parity, auth (crypt + JWT byte-compatible), static
  skin serving per contract §2–3;
- contract test suite executed against Go and C backends (same requests,
  normalized comparison) over both transports;
- frontend `npm run test:e2e` against C backend.
Exit: contract suites + Playwright green on C.

## Phase 7 — subscriptions & full integration

- libcurl fetch with exact redirect semantics + size bound; scheduler on
  timerfd; rebuild/rollback flows; SIGHUP reload; save-after-sync;
- end-to-end: full daemon in netns with stub upstream + stub subscription
  server; restart/upgrade state checks; fault injection (upstream flaps,
  fetch failures, netlink errors).
Exit: e2e suite green; 24 h host soak flat.

## Phase 8 — packaging

- `make build_backend` switches to C for all 40 targets (matrix from
  config/, per toolchains.md); Makefile user-facing targets unchanged;
- ipk/apk packaging, init scripts, `_kn` files unchanged; Depends updated
  (drop nothing needed, add libyaml/cjson/pcre2/libcurl/libmnl);
- upgrade tests Go-pkg → C-pkg (config preserved, service restarts,
  netfilter state cleaned/rebuilt), downgrade documented;
- UPX keep/drop decision by measurement.
Exit: CI matrix green; install/upgrade verified on real Entware + OpenWrt
devices (or emulated images where hardware unavailable, marked as such).

## Phase 9 — optimization & Go removal

- profile (perf on device/qemu), fix hotspots; final benchmark comparison
  per methodology; long soak; sanitizer + fuzz smoke reruns;
- parity checklist from spec §23 signed off item by item;
- separate change: remove `src/backend` (Go), Go deps, Go CI steps; update
  Makefile, CLAUDE.md, AGENTS.md, README, docs.
Exit: Definition of Done (spec §24) satisfied with linked evidence.

## Cross-phase rules

- Differential harness runs in CI from Phase 2 onward; a failing mandatory
  suite blocks phase promotion.
- Every intentional behaviour divergence gets an entry in `decisions.md`
  and a release-note line; silent divergence is a bug.
- Performance claims only with `tools/bench` numbers attached.
