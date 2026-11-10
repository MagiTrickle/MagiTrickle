# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MagiTrickle is a DNS-based traffic routing utility for routers (Entware/OpenWrt). It intercepts DNS queries via a MITM proxy, caches resolved IPs, matches them against domain rules, and programs iptables/ipset to route matched traffic through a specified network interface.

## Build System

The project uses a Makefile with stamp-based incremental builds. Before building, copy and configure `.config.example` to `.config`:

```sh
cp .config.example .config
# Edit PLATFORM, TARGET, GOOS, GOARCH etc.
```

Key make targets:
```sh
make all              # download deps + build + package
make build            # build backend and frontend only
make build_backend    # Go binary only
make build_frontend   # Svelte frontend only
make package          # create .ipk / .apk packages
make clean            # remove all build artifacts
make clear            # remove build dir for current PLATFORM/TARGET + frontend dist
```

Build outputs land in `.build/<PLATFORM>_<TARGET>/`. The final packages appear in `.build/`.

Two supported platforms (set via `PLATFORM` in `.config`):
- `entware` — Keenetic and similar; targets like `mipsel-3.4`, `aarch64-3.10`. The `_kn` suffix targets add `entware_kn` build tag and require `socat`.
- `openwrt` — targets like `aarch64_cortex-a53`.

Backend binary is compressed with UPX during build (skipped for riscv64, mips64, mips64le, loong64).

## Backend

**Entry point**: `src/backend/cmd/magitrickled/main.go`  
**Go module**: `magitrickle` (Go 1.23)

Run tests:
```sh
cd src/backend
go test -tags testing ./...      # all tests (the tag enables the fake iptables)
go test ./models/...             # single package
go test ./tests/ -run TestInteg  # specific integration test
```

### Architecture

The root package (`src/backend/*.go`) contains the `App` struct — the central coordinator:

- **`app.go`** — `App` struct and CRUD methods for groups and subscriptions; every change ends in `commitLocked`, which rebuilds rule sets and applies them to routing (rolling the model back if the change itself failed to apply). Published `models.Group` and `models.Subscription` values are immutable — changes replace them (`Clone`); `UpdateGroup(id, fn)` runs `fn` under the state lock so concurrent edits are not lost. `SaveConfig` refuses to write until `LoadConfig` has succeeded, so a config that failed to load is never overwritten
- **`config.go`** — `LoadConfig`/`SaveConfig`: `Start` refuses to run until a load has succeeded; load reads YAML directly into `models.Group`/`models.Subscription`, repairs hand-edit leftovers (nil entries, missing/duplicate rule IDs), validates everything (including kernel-name prefix lengths) and only then swaps settings, groups and subscriptions together; settings live in an `atomic.Pointer` snapshot (`a.cfg()`). Save marshals live data under `stateMu.RLock()`
- **`start.go`** — `App.Start()`: wires up DNS MITM proxy, iptables helper, `netfilterTools.Router`, `routing.Table`, HTTP/Unix API servers, and the netlink watcher. The main loop only reads netlink events: they are queued per interface (`interfaceEvents`, latest event wins) and passed to the Router in a separate goroutine, and routing retries run in another one (`retryLoop`: every minute and after events, with a 2s pause that merges a burst into one retry), so waiting for the Router never stops event reading
- **`dns.go`** — DNS request/response hooks; resolved addresses go to the cache and then to `routing.Table.AddAddresses`
- **`subscriptions.go`** — subscription fetching and auto-update scheduling. `subscriptionSyncMu` only serializes syncs and is held during the download instead of `stateMu`, so edits don't wait for a slow source. A download is applied only if the subscription's URL and rule objects are still the same (`sameFetchInput`); otherwise it is dropped — auto-update retries on the next tick, a manual sync returns `ErrSubscriptionChanged` (409)

Key sub-packages:

| Package | Purpose |
|---|---|
| `app/` | `Main` interface (used by the API layer) and shared errors |
| `api/` | HTTP server (chi, port 8080) and Unix socket; mounts `api/v1` |
| `models/` | Data types: `Group`, `Rule`, `Subscription`, `Config`/`AppConfig` (the config file layout); `Group`, `Rule`, `Subscription` carry YAML struct tags (no custom `UnmarshalYAML`: an absent `enable` key is loaded as `false`; the HTTP API, by contrast, treats an absent `enable` in a request as `true`) |
| `constant/` | Default config values; platform-conditional paths and ignored interfaces via build tags (`entware`, `entware_kn`, `openwrt`) |
| `subscriptions/` | Fetches, parses, and validates external domain lists |
| `rulesets/` | Domain layer: turns subscriptions and groups into `RuleSet` snapshots (only enabled rules; domain rules indexed in `Matcher`, subnets parsed into `Subnets`); `Build` decides which are active and their order, `Builder` caches rule sets per (immutable) model pointer. Knows nothing about netfilter |
| `routing/` | Glue: `Table` maps rule sets to routes, fills ipsets from the DNS cache, routes DNS answers. The only package that knows both sides. A route whose fill failed does not replace the live one. `Apply` fails only for rule sets changed since the previous call (`ApplyError.Fatal` for routing as a whole); still-broken unchanged ones are logged and retried by `App` every minute and on netlink events, so they don't block unrelated edits |
| `utils/dnsMITMProxy/` | UDP/TCP DNS proxy with request/response hooks |
| `utils/netfilterTools/` | iptables chain management, ipset CRUD, port remap (53→3553); `Router` — ordered set of routes (ipset → interface) changed in two phases. Knows nothing about groups. Host entries are keyed with CIDR 0 on both write and read (the kernel reports them as /32 or /128) |
| `utils/recordsCache/` | In-memory DNS A/AAAA/CNAME cache with TTL cleanup |
| `utils/intID/` | 4-byte ID type used for groups and rules |
| `utils/iptables/` | Low-level iptables wrapper with batched commit |

### DNS flow

1. iptables `nat PREROUTING` redirects port 53 → 3553 (configurable, can be disabled)
2. `dnsMITMProxy` forwards queries to upstream (default `127.0.0.1:53`) and intercepts responses
3. `dnsResponseHook` calls `handleMessage`, which processes A/AAAA/CNAME records
4. Matching IPs are added to the group's ipset with TTL = DNS TTL + `AdditionalTTL` (default 3600s)
5. iptables routes packets from the ipset through the group's configured interface

### Rule set order

Built-in chains (`FORWARD`, mangle `PREROUTING`, nat `POSTROUTING`) jump to a single `MT_ROUTING` chain, which jumps to the per-route chains in the order given to `Router.Apply`. A packet passes all of them and the **last** matching route's mark wins. `rulesets.Build` puts subscriptions first and user groups last, so user groups override subscriptions. Routes change in two phases, and live ipsets are never edited in place: `Router.Prepare` brings up what is needed without touching traffic, `routing.Table` fills the new (empty) ipsets, then `Router.Commit` switches everything at once and only then removes stale instances. Instances live in two slots, `<id>-0`/`<id>-1`: an interface change brings up a whole new instance in the other slot and `Commit` switches `MT_ROUTING` to it; a rule change brings up only an ipset in the other slot and `Commit` swaps it with the live one (`ipset swap`), leaving iptables alone. A failed `Commit` undoes both. Unchanged routes are left alone, and an unchanged order causes no iptables call at all. While a route is being replaced, DNS answers are matched against both the old and the new rules, each written to its own route.

### Rule types

Defined in `models/rule.go`: `domain` (exact), `namespace` (domain + subdomains), `wildcard` (`*` — any sequence, `?` — exactly one char, `.` is literal), `regex` (dlclark/regexp2), `subnet` (IPv4 CIDR), `subnet6` (IPv6 CIDR).

## Frontend

**Location**: `src/frontend/`  
**Stack**: Svelte 5, TypeScript, Vite, Prettier

```sh
cd src/frontend
npm install
npm run dev:frontend    # Vite dev server (needs separate backend or mock)
npm run dev:backend     # Deno mock backend (API mock for local UI dev)
npm run build           # production build → dist/
npm run check           # svelte-check + tsc type check
npm run format          # Prettier format
npm run format:check    # Prettier check (CI)
npm run test:e2e        # Playwright end-to-end tests
npm run test:unit       # Deno unit tests
```

For local development, run both `dev:backend` (Deno mock at `dev/backend-mock.ts`) and `dev:frontend` in separate terminals.

Built frontend is placed into the package at `usr/share/magitrickle/skins/default/` and served by the HTTP server. The active skin is set by `HTTPWeb.Skin` config (default: `"default"`).

## Configuration

Runtime config is YAML; platform-specific default paths are in `constant/path_*.go`. Default values are in `constant/constant.go`:
- DNS proxy listens on `[::]`:3553, upstream `127.0.0.1:53`
- HTTP WebUI at `[::]`:8080
- iptables chain prefix `MT_`, ipset prefix `mt_`
- Default monitored interface: `br0`

## CI

GitHub Actions (`.github/workflows/build.yml`) builds a matrix of all configs under `config/*/`. To add a new target, add a `.config` file in the appropriate `config/<platform>/` directory.