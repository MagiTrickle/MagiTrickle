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
go test ./...                    # all tests
go test ./models/...             # single package
go test ./tests/ -run TestInteg  # specific integration test
```

### Architecture

The root package (`src/backend/*.go`) contains the `App` struct — the central coordinator:

- **`app.go`** — `App` struct and CRUD methods for groups and subscriptions
- **`config.go`** — `LoadConfig`/`SaveConfig`: load reads YAML directly into `models.Group`/`models.Subscription` (no intermediate config copies); save marshals live data under `stateMu.RLock()`
- **`start.go`** — `App.Start()`: wires up DNS MITM proxy, iptables helper, HTTP/Unix API servers, and the netlink watcher
- **`dns.go`** — DNS request/response hooks; on each resolved A/AAAA record, matched IPs are added to the appropriate ipset
- **`rule_set.go`** — `RuleSet` wraps a group spec with ipset + iptables lifecycle (Enable/Disable/Sync)
- **`subscriptions.go`** — subscription-level rule set management and auto-update scheduling

Key sub-packages:

| Package | Purpose |
|---|---|
| `app/` | `Main` and `RuleSet` interfaces (used for testing and API layer) |
| `api/` | HTTP server (chi, port 8080) and Unix socket; mounts `api/v1` |
| `models/` | Data types: `Group`, `Rule`, `Subscription`, `AppConfig`; `Group`, `Rule`, `Subscription` carry YAML struct tags and custom `UnmarshalYAML` (absent `enable` defaults to `true`) |
| `config/` | YAML-serializable structs for app-level settings only (`App`, `HTTPWeb`, `DNSProxy`, etc.); groups and subscriptions are stored directly as `models.Group`/`models.Subscription` — no separate config types for them |
| `constant/` | Default config values; platform-conditional paths and ignored interfaces via build tags (`entware`, `entware_kn`, `openwrt`) |
| `groups/` | Builds `rulesets.Spec` from a `models.Group` for user-defined groups |
| `subscriptions/` | Fetches, parses, and validates external domain lists; builds subscription rule sets |
| `rulesets/` | `Spec` type — common data carrier for both user groups and subscription groups |
| `utils/dnsMITMProxy/` | UDP/TCP DNS proxy with request/response hooks |
| `utils/netfilterTools/` | iptables chain management, ipset CRUD, port remap (53→3553) |
| `utils/recordsCache/` | In-memory DNS A/AAAA/CNAME cache with TTL cleanup |
| `utils/intID/` | 4-byte ID type used for groups and rules |
| `utils/iptables/` | Low-level iptables wrapper with batched commit |

### DNS flow

1. iptables `nat PREROUTING` redirects port 53 → 3553 (configurable, can be disabled)
2. `dnsMITMProxy` forwards queries to upstream (default `127.0.0.1:53`) and intercepts responses
3. `dnsResponseHook` calls `handleMessage`, which processes A/AAAA/CNAME records
4. Matching IPs are added to the group's ipset with TTL = DNS TTL + `AdditionalTTL` (default 3600s)
5. iptables routes packets from the ipset through the group's configured interface

### Rule types

Defined in `models/rule.go`: `domain` (exact), `namespace` (domain + subdomains), `wildcard` (`*`/`?`), `regex` (dlclark/regexp2), `subnet` (IPv4 CIDR), `subnet6` (IPv6 CIDR).

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