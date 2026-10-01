# AGENTS.md

Durable instructions for AI coding agents working in this repository.
Keep this file free of transient progress notes and benchmark numbers.

## Project structure

- `src/backend-c/` — **C11 backend (production implementation).** Entry
  `src/main/main.c`. This was rewritten from a Go implementation
  (`src/backend/`, removed in Phase 9 once parity was verified — see
  `docs/c-rewrite/`); there is no Go backend in this repository anymore.
- `src/frontend/` — Svelte 5 + TypeScript WebUI (Vite build, Deno mock
  backend in `dev/`, Playwright e2e in `tests/`). Only confirmed
  compatibility fixes are allowed, no redesigns.
- `config/entware/*.config`, `config/openwrt/*.config` — the build target
  matrix. This is the single source of truth for supported targets; never
  hand-edit a hardcoded target list elsewhere, never drop a target
  silently.
- `files/` — packaging payload (init scripts, default config, hooks) for
  common/entware/entware_kn/openwrt.
- `tools/bench/` — benchmark tooling (dnsstub, dnsload, genconfig,
  configbench, run_baseline.sh / run_c_baseline.sh). Raw results live in
  `docs/c-rewrite/baseline-raw*/`.
- `docs/c-rewrite/` — the rewrite's audit, contracts, plans, and
  phase-by-phase decisions log (kept as historical record — read
  `decisions.md` before touching behaviour that traces back to a
  documented Go-vs-C decision). `docs/swagger.yaml` — HTTP API reference.

## Build

```sh
cp .config.example .config   # choose PLATFORM/TARGET/CROSS_COMPILE/SYSROOT
make build_backend           # C daemon binary
make build_frontend          # Svelte dist/
make package                 # .ipk (Entware/OpenWrt) / .apk (OpenWrt)
make clean | make clear
```

Outputs: `.build/<PLATFORM>_<TARGET>/`, packages in `.build/`.
Do not remove or rename the user-facing make targets
(`all`, `build`, `build_backend`, `build_frontend`, `package`, `clean`,
`clear`).

## Test

```sh
cd src/backend-c
make CFLAGS_EXTRA=-Werror     # build, warnings as errors
make test                      # unit tests (greatest.h)
make sanitize                  # ASan+UBSan
make static_analysis            # clang-tidy + cppcheck
make fuzz FUZZ_RUNS=200000     # libFuzzer targets
sudo -E env "PATH=$PATH" sh tests/differential/run_diff.sh  # regression suites vs golden/

cd src/frontend
npm run check                 # svelte-check + tsc
npm run format:check          # prettier (CI)
npm run test:unit             # Deno unit tests
npm run test:e2e              # Playwright (needs built/mocked backend)
```

Benchmarks: `sh tools/bench/run_c_baseline.sh` (root; see script header for
requirements and env vars).

## Formatting / style

- C: C11; warning set and `make sanitize`/`make static_analysis` defined
  in `src/backend-c/`; GNU/Linux extensions only inside the platform
  layer.
- Frontend: Prettier via `npm run format`.

## Target platforms & compatibility constraints

- Entware (Keenetic etc.): **glibc**, `/opt` prefix, kernels ≥3.2/3.4,
  targets incl. big-endian mips and softfloat arm. `_kn` targets add the
  `entware_kn` build tag (Keenetic RCI, ignored interfaces, ndm hook,
  socat dep).
- OpenWrt: **musl**, procd init, `.ipk` (opkg, ≤24.10) and `.apk`
  (≥25.12) from the same rootfs.
- Never link Entware binaries against musl or host glibc; never build
  OpenWrt binaries against host glibc; no `-march=native`; no static
  glibc.
- Wire formats (DNS, netlink) must use explicit byte-order access — BE
  mips targets are first-class.
- Binary/package size is NOT an optimization goal.

## Backend behaviour contracts

The Go→C rewrite is complete (`docs/c-rewrite/migration-plan.md` Phase 9);
these rules keep the C backend's behaviour from silently drifting from the
contracts it was verified against:

1. Behaviour contracts live in `docs/c-rewrite/compatibility-contract.md`
   and `docs/c-rewrite/parity-checklist.md`; check them before changing
   anything observable, and record intentional divergences in
   `docs/c-rewrite/decisions.md` — never diverge silently.
2. Regression suites in `src/backend-c/tests/differential/` compare live
   C output against `golden/` snapshots frozen from the last verified
   Go-vs-C run (see `decisions.md` D-45). A real regression still shows
   up as a diff; update golden files only alongside a documented,
   intentional behaviour change.
3. No performance claims without `tools/bench` measurements attached.
4. Config YAML field names, defaults, API routes/status codes, ipset/chain
   naming (`mt_`/`MT_` prefixes), and file paths are frozen contracts.

## Git etiquette

- Run `git status` before changes; never overwrite uncommitted user work;
  never rewrite history; small, logically complete commits.
