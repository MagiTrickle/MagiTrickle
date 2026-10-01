# MagiTrickle C backend

Go→C migration, through Phase 4 (план и статус:
`docs/c-rewrite/migration-plan.md`; контракты:
`docs/c-rewrite/compatibility-contract.md`). The Go backend in
`src/backend/` remains the production implementation and behavioural
oracle until Phase 9.

## Layout

```
include/magitrickle/   public module headers
src/
  main/                daemon entry (skeleton: loop + signals + logging)
  core/                lifecycle (partial-init unwinding)
  logging/             leveled console logger (zerolog-compatible levels)
  platform/            Linux-only layer: epoll/timerfd/signalfd/eventfd loop
  util/                error model, bounded queue
  config/              Phase 2: models, Go-compatible duration, 4-byte IDs,
                       YAML load/save with go-yaml-v2 byte parity (libyaml),
                       atomic file write
  rules/               Phase 2: rule matching (domain/namespace/wildcard/
                       PCRE2 regex) + per-group index (hash + reversed trie)
  subscriptions/       Phase 2: list parser/type-detect/refresh/is_due
  dns/                 Phase 3: wire codec (parse/pack, decompression
                       guards, fake-PTR, AAAA-strip) + epoll MITM proxy
                       transport (UDP pktinfo, TCP one-query-per-conn,
                       upstream pools, backpressure); Phase 4: response
                       processing pipeline (dns.go handleMessage port)
  dns_cache/           Phase 4: records cache (address/alias/reverse-alias,
                       bounded domain count, 30s expiry sweep) — no
                       external deps, no locking (single loop thread)
  rules/               Phase 2: matching; Phase 4: rule-set snapshot
                       (per-group aggregate matcher, immutable, swapped on
                       the loop thread — decisions.md D-17/D-18)
  tools/               mt-configtool, mt-dnstool, mt-cachetool
                       (differential-test drivers)
tests/
  unit/                greatest.h-based unit tests
  vendor/              vendored test framework (greatest.h, ISC)
  differential/        run_diff.sh + fixtures/corpora — Go↔C parity suites
                       (config, rules, subscriptions, DNS wire vs miekg,
                       records cache vs recordsCache)
  fuzz/                libFuzzer targets (DNS parser, fake-PTR) + seeds
spikes/
  regex_corpus/        dlclark/regexp2 vs PCRE2 corpus (+known divergences)
  yaml_emit/           go-yaml v2 vs libyaml byte-shape check
```

Host deps for the config/rules layer: `libyaml-dev`, `libpcre2-dev`.
The differential config suite needs root (the Go oracle runs the real
`App.LoadConfig`/`SaveConfig` against `/var/lib/magitrickle`).

## Commands

```sh
make                   # host build -> build/host/magitrickled-c
make test              # unit tests
make sanitize          # ASan+UBSan test run
make static_analysis   # clang-tidy (.clang-tidy) + cppcheck
make fuzz FUZZ_RUNS=200000                            # libFuzzer smoke
make CROSS_COMPILE=mipsel-linux-gnu- [SYSROOT=...]   # cross skeleton
sh tests/differential/run_diff.sh                    # parity suites
```

Run the daemon (DNS proxy + records cache + rule matching; netfilter/API
land in later phases — the match "sink" only logs for now):

```sh
build/host/magitrickled-c --config /path/to/config.yaml
# functional smoke + throughput vs a stub upstream:
SCRATCH=/tmp sh ../../tools/bench/run_c_smoke.sh
# sustained-churn soak (flat RSS check):
sh ../../tools/bench/run_c_soak.sh
```

CI (`.github/workflows/check-c.yml`) builds with `-Werror`, runs tests,
sanitizers, static analysis, differential suites and a mipsel cross build.

## Rules

- C11; `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Wundef
  -Wstrict-prototypes -Wmissing-prototypes`; POSIX.1-2008 API project-wide.
- GNU/Linux-specific APIs live only under `src/platform/` (D-14).
- Every queue/cache is bounded with an explicit overflow policy and a
  dropped-counter (см. `include/magitrickle/queue.h`).
- Init/destroy симметричны; частичная инициализация разматывается через
  `mt_lifecycle` (см. `include/magitrickle/lifecycle.h`).
- Никакой работы с DNS/netfilter здесь до соответствующих фаз плана.
