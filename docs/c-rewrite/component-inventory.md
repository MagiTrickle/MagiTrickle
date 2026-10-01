# Component Inventory — Go backend → C module mapping

Columns: state owned / concurrency, external deps, syscalls of note,
test coverage today, proposed C module, migration risk (L/M/H).

## Root package `magitrickle` (src/backend/*.go)

| File | Purpose | Public API | State/concurrency | Deps | Tests | C module | Risk |
|---|---|---|---|---|---|---|---|
| `app.go` | App core: group/subscription CRUD, snapshots | `App`, `New`, `Config`, `UserGroups`, `ClearGroups`, `AddGroup`, `RemoveGroupBy*`, `WithSubscriptions`, `ReplaceSubscriptions`, `AddSubscription`, `RemoveSubscriptionByID`, `ListInterfaces`, `DnsOverrider` | owns all app state; `stateMu` RWMutex, `subscriptionSyncMu`, `enabled` atomic | zerolog | indirect (tests/) | `core/` | M |
| `config.go` | YAML load/save, overlay defaults, legacy duration normalization, color validation | `LoadConfig`, `SaveConfig` | writes `App.config` (unlocked — known race), groups under locks | yaml v2, regexp2 | subscriptions_sync_test partially | `config/` | **H** (byte-level YAML compat) |
| `start.go` | Startup wiring/ordering, main loop, logging setup | `Start`, `ForceCommitIPTables` | orchestrates everything; errChan | netlink, zerolog | none | `main/` + `core/lifecycle` | **H** (ordering, shutdown) |
| `dns.go` | DNS hooks: logging, fake PTR, AAAA drop, cache+match+ipset | hooks (internal) | reads config, ruleSetSnapshot | miekg/dns | none | `dns/hooks` | **H** (hot path) |
| `rule_set.go` | RuleSet lifecycle: ipset+iptables+route, Sync diff | `RuleSet` + methods | per-set mutex + atomic enabled | netlink | tests/ (fake) | `rules/` + `netfilter/` | **H** |
| `netlink.go` | link/addr subscriptions + dispatch | `subscribe*`, `handleLink/Addr` | channels | vishvananda/netlink | none | `netlink/` | M |
| `subscriptions.go` | subscription sync engine, auto-update loop | `SyncSubscriptionRuleSets`, `StartSubscriptionAutoUpdate`, `SyncSubscriptionByID`, `SyncDueSubscriptions` | under both mutexes; rollback logic | zerolog | `subscriptions_sync_test.go` | `subscriptions/` | M |
| `cmd/magitrickled/main.go` | PID file, signals, run | `main` | — | zerolog | none | `main/` | L |

## `app/` (interfaces for API layer + tests)

`Main` and `RuleSet` interfaces, subscription error values, `SubscriptionSyncResult`.
No state. C module: `include/magitrickle/*.h` public headers. Risk L.

## `api/`

| File | Purpose | Notes | Tests | C module | Risk |
|---|---|---|---|---|---|
| `http.go` | HTTP server, auth gating, static skin serving | chi, MIME map, index fallback, placeholder | none | `http/` + `static_server/` | M |
| `unixsocket.go` | same router on unix socket, no auth | socket unlink dance | tests/ (via socket? no — in-proc) | `unix_api/` | L |
| `v1/router.go` | route tree, ID middlewares (via header smuggling) | `groupIdx` header hack — do NOT copy | tests/ | `http/router` | M |
| `v1/handlers.go` | groups/rules CRUD, interfaces, save, netfilterd hook | async SaveConfig after response | tests/ partial | `http/handlers` | M |
| `v1/subscription_handlers.go` | subscriptions CRUD/sync | 409/502 mapping | none | `http/handlers` | M |
| `v1/converters.go`, `subscription_converters.go` | model↔DTO, defaults (enable=true, color) | duplicate color regex | none | `http/dto` | L |
| `v1/types/*` | JSON DTOs | omitempty details matter | none | `http/dto` | L |
| `auth/*` | shadow/passwd lookup, MD5/SHA crypt, JWT HS256, secret file | timing-safe compare; 20-year exp | **none** | `auth/` | **H** (crypto correctness) |
| `utils/helpers.go`, `types/error.go` | JSON write/read, error shape | charset suffix | — | `http/` | L |

## `models/`

| File | Purpose | Notes | Tests | C module | Risk |
|---|---|---|---|---|---|
| `rule.go` | rule types + matching + lazy regex compile | matching semantics = core contract | `rule_test.go` (good) | `rules/match` | **H** (regexp2 parity) |
| `group.go`, `subscription.go`, `config.go`, `interface.go` | plain data | yaml tags; `enable` default false | partial | `config/model` | L |

## `config/` (yaml DTOs)

Pointer-typed overlay structs; field names = YAML contract. Tests: none.
C module: `config/yaml`. Risk M (must match yaml v2 quirks: duration
strings, ns-integers, unknown-key tolerance).

## `constant/`

Defaults + per-platform paths (build tags `entware`, `entware_kn`,
`openwrt`) + ignored interfaces. C module: `platform/` (compile-time or
runtime-selected config). Risk L.

## `groups/`, `rulesets/`, `subscriptions/` (runtime specs)

- `rulesets/spec.go`: common Spec carrier (model group or synthetic).
- `groups/runtime_rule_sets.go`: group → Spec (RuntimeKey = hex id).
- `subscriptions/runtime_rule_sets.go`: subscription → Spec
  (RuntimeKey `sub_<id>`, display name "Subscription: …"). Tests exist.
- `subscriptions/fetch.go` (redirect policy), `parse.go`
  (tokenize/dedup/type-detect/refresh), `validate.go` (type syntax),
  `auto_update.go` (IsDue/PlanRefresh). Tests: fetch/parse/runtime — port
  the corpora.
C modules: `rules/spec`, `subscriptions/`. Risk M (type detection heuristics
must match exactly).

## `internal/interfaces/`

Interface listing + Keenetic RCI alias lookup (entware_kn tag; HTTP to
localhost:79, batch parse). Tests: keenetic (tag-gated). C module:
`platform/keenetic`. Risk M (device-only verification).

## `utils/`

| Package | Purpose | Concurrency | Syscalls | Tests | C module | Risk |
|---|---|---|---|---|---|---|
| `dnsMITMProxy` | UDP/TCP DNS proxy, conn pools, semaphore, pktinfo replies | goroutine per request; sync.Pool buffers; chan-based pool | sendmsg/recvmsg with pktinfo cmsgs, SO deadlines | **none** | `dns/transport` | **H** |
| `intID` | 4-byte hex ID | — | getrandom | none | `util/id` | L |
| `iptables` | desired-state model, save/restore batch, fake exec | RWMutex | fork/exec | `iptables_test.go` (tag `testing`, good transcripts) | `netfilter/iptables` | M |
| `netfilterTools` | ipset netlink CRUD, ipset-to-link (chains+rule+routes+mark alloc), port remap, cleaner, helper | per-object mutex+atomic | netlink (NETLINK_NETFILTER for ipset, NETLINK_ROUTE for rules/routes) | none | `netfilter/ipset`, `netfilter/routing` | **H** (raw netlink in C) |
| `recordsCache` | DNS cache maps + reverse alias index + cleanup | RWMutex | — | `records_test.go` | `dns_cache/` | M |

## External Go dependencies → C responsibility

| Go dep | Used for | C replacement candidates (см. dependencies.md) |
|---|---|---|
| miekg/dns | msg parse/pack | own minimal parser vs ldns/libknot |
| go-chi/chi | routing | own tiny router vs libmicrohttpd/civetweb/mongoose |
| dlclark/regexp2 | rule regex (.NET semantics) | PCRE2 (+compat corpus) — see decisions D-07 |
| IGLOU-EU/go-wildcard | wildcard match | trivial own implementation |
| rs/zerolog | logging | own leveled console logger |
| vishvananda/netlink | ipset, rules, routes, subscriptions | libmnl (+libnftnl-нет; raw ipset netlink) vs raw sockets |
| go.yaml.in/yaml/v2 | config | libyaml (+ own duration handling) |
| golang.org/x/net ipv4/ipv6 | pktinfo cmsgs | recvmsg/sendmsg + IP_PKTINFO/IPV6_RECVPKTINFO directly |
| stdlib net/http (client) | subscription fetch, Keenetic RCI | libcurl vs own tiny client (TLS! → libcurl+mbedtls/openssl из фида) |
| stdlib crypto | HMAC-SHA256, SHA-256/512-crypt, MD5-crypt, base64 | own impl vs libcrypt (crypt_r) + small SHA2/HMAC (либо wolfssl/mbedtls) |

## Coverage gaps (no automated tests today)

DNS proxy transport, DNS hooks, records-cache↔ipset interplay, HTTP static
serving, auth (all of it), unix socket transport, netlink watcher, config
load/save round-trip, port remap, ipset netlink layer, PID/signals.
These need harness coverage against the **Go** version first (Phase 1–2)
so the C port has a reference oracle.
