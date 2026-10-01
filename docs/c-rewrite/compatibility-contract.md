# Compatibility Contract — observable behaviour the C backend must preserve

Every item cites its source. "Test" = existing automated coverage;
"Gap" = no automated coverage today; "C-verify" = how the C version will be
checked (differential = run same input through Go and C and compare).

Legend for C-verify: **DIFF** = differential harness, **CT** = contract test
(HTTP/Unix), **UT** = unit test port, **IT** = integration test (netns),
**MAN** = manual/device check.

## 1. Configuration (YAML)

Source: `config/config.go`, `config/app.go`, `config.go`
(`LoadConfig`/`SaveConfig`), `constant/constant.go`, `models/*`.

| Contract | Detail | Test | C-verify |
|---|---|---|---|
| File location | `<AppStateDir>/config.yaml` (platform table in `constant/path_*.go`) | Gap | IT |
| configVersion gate | must have prefix `0.` else load fails (`ErrConfigUnsupportedVersion`); missing file = OK (defaults) | Gap | DIFF |
| Overlay semantics | every present `app.*` scalar overrides the default; absent = keep default (pointer fields) | Gap | DIFF |
| Defaults | DNS host `[::]:3553`, upstream `127.0.0.1:53`, remap53/fakePTR/dropAAAA all **enabled** (disable-flags false), maxIdleConns 10, maxConcurrent 100, timeout 5000 ms, HTTP `[::]:8080` enabled, auth disabled, skin `default`, chainPrefix `MT_`, ipset prefix `mt_`, additionalTTL 1 h, IPv4+IPv6 enabled, startMarkTableIndex 0x4D616769, link `[br0]`, showAllInterfaces false, logLevel info | Gap | DIFF |
| Duration parsing | yaml v2 accepts Go duration strings (`5s`, `1h0m0s`) and bare integers = **nanoseconds**; legacy normalization: dnsProxy.timeout < 1 ms ⇒ treat as ms; additionalTTL < 1 s ⇒ treat as s (empirically verified) | Gap | DIFF |
| Duration serialization | saved as Go duration strings (`5s`, `1h0m0s`) | Gap | DIFF |
| `enable` default | **absent `enable` on group/rule/subscription unmarshals to `false`** (plain Go bool; empirically verified). NOTE: contradicts CLAUDE.md claim of default-true; API create paths default group Enable to true instead | Gap | DIFF |
| Field names | camelCase app keys (`httpWeb`, `dnsProxy`, `disableRemap53`, …); group/rule keys lowercase; subscription uses `last_update` (snake) | Gap | DIFF |
| Save shape | full app tree always written; key order = Go struct order (`configVersion`, `app`, `groups`, `subscriptions`); `groups`/`subscriptions` always present (may be `[]`) | Gap | DIFF |
| Group color | invalid `#rrggbb` → `#ffffff`; valid → lowercased (regexp2 IgnoreCase) | Gap | DIFF |
| ID format | 4-byte IDs, 8 hex chars text form; random on create; load fails on duplicate group ID or duplicate rule ID within group | Gap (partial via handlers test) | DIFF |
| Order preservation | groups and rules keep YAML/API order everywhere (list semantics) | tests/handlers_v1_test.go partially | DIFF+CT |
| Unknown fields | silently ignored (yaml v2 default) | Gap | DIFF |
| Corrupted YAML | load returns error; daemon continues with defaults (New logs error), SIGHUP reload keeps old state | Gap | DIFF |
| Save is non-atomic | 0600 file, direct WriteFile (improvement to temp+rename allowed if bytes identical) | Gap | DIFF |
| Save on state change | v1 write handlers persist asynchronously after responding | tests (implicit) | CT |
| Live reload | SIGHUP → LoadConfig on running app: groups disabled/rebuilt only if `groups` key present; subscriptions replaced (or cleared when key absent!) and rule sets rebuilt | Gap | IT |

## 2. HTTP API and Unix socket

Source: `api/http.go`, `api/unixsocket.go`, `api/v1/router.go`,
`api/v1/handlers.go`, `api/v1/subscription_handlers.go`, `api/auth/*`,
`docs/swagger.yaml`. Existing test: `src/backend/tests/handlers_v1_test.go`
(in-process, groups PUT/GET round-trip, subnet ipset behaviour with fake
netfilter absent — subnet rules verified via RuleSet.sync fake?, см. файл).

Transports: TCP HTTP (`HTTPWeb.Host`, only when `HTTPWeb.Enabled`) and Unix
socket `<SockPath>` (always on, **never authenticated**). Same router.

Auth middleware (HTTP only): applies to paths starting `/api/` except
`/api/v1/auth`; only active when `HTTPWeb.Auth.Enabled`. Non-`/api/` paths
(static) are never authenticated. `Authorization: Bearer <jwt>` required;
failure → 401 `{"error":"Unauthorized"}`.

Routes (all under `/api/v1`):

| Method+Path | Behaviour, status codes |
|---|---|
| GET /auth | 200 `{"enabled":bool}` |
| POST /auth | auth disabled → 404; bad JSON/missing creds → 400; bad creds → 403; ok → 200 `{"token":...}` |
| GET /groups?with_rules=bool | 200 `{"groups":[...]}` (rules omitted unless with_rules) |
| PUT /groups?save=bool | replace all groups; no `groups` key → 400; ID conflicts → 400; enable failure → 500; 200 = new groups with rules |
| POST /groups?save=bool | create; 200 group with rules |
| GET/PUT/DELETE /groups/{id} | invalid hex id → 400; unknown → 404 (middleware); PUT keeps rules if `rules` absent; ID mismatch in body → 400 |
| GET /groups/{id}/rules … | same pattern per-rule; unknown rule → 404 |
| GET /subscriptions?with_rules | 200 list |
| PUT /subscriptions?save | replace; 400 on invalid/dup; 200 `{"status":"ok"}` |
| POST /subscriptions?save&sync=bool | create (+optional immediate sync); dup ID → 409; missing url → 400 |
| GET /subscriptions/rules?url= | fetch+parse remote list; fetch error → 502 |
| POST /subscriptions/{id}/sync?save | not found → 404; invalid → 400; fetch fail → 502; 200 `{"status":"ok","subscription":{...}}` |
| DELETE /subscriptions/{id}?save | 200 / 404 |
| GET /system/interfaces | 200 `{"interfaces":[{"id","name"?}]}` |
| POST /system/config/save | 200 `{}`? (writes config; error → 500) |
| POST /system/hooks/netfilterd | body `{"type","table"}`; triggers iptables re-commit; 200 |

Details to freeze exactly (from code):

- Success body for group/rule writes is the resource JSON (see
  `api/v1/types/*`: `enable` serialized always; `rules` key omitted when
  nil via `omitempty` on the wrapper). JSON error shape:
  `{"error":"..."}`; Content-Type `application/json; charset=utf-8`.
- Group create defaults: missing `id` → random; missing `enable` → `true`;
  invalid color → `#ffffff`.
- `PUT /groups/{id}` with changed `enable` transitions the live rule set
  (disable → update → enable+sync).
- `save=true` query triggers async SaveConfig after the response.
- chi routing details: trailing-slash variants are distinct; the group
  middleware smuggles the resolved index via request header `groupIdx`
  (implementation detail — C version must reproduce *external* behaviour
  only). Test: `tests/handlers_v1_test.go`. C-verify: CT against both
  backends over HTTP and Unix socket.
- Static files: any non-`/api` GET; path cleaned, joined under
  `<share>/skins/<skin>`; dir → index.html (single retry); missing → 404
  JSON error except `/` → 404 with HTML placeholder; MIME by extension
  (html, css, js, ico, png, svg; default text/plain); whole file read into
  memory; no caching headers; no SPA fallback. Gap: no tests. C-verify: CT.
- No request size limits, no timeouts (Go http.Server zero values), no
  connection cap. C version may add bounded limits — document as hardening,
  cover with tests, keep normal-size behaviour identical.

## 3. Authentication

Source: `api/auth/*`.

- Credentials: system account lookup in shadow (fallback passwd when shadow
  missing); crypt formats MD5 `$1$`, SHA-256 `$5$`, SHA-512 `$6$` incl.
  `rounds=N` (1000–999999999); empty/`x`/`*` hash → "no password" error.
- Token: hand-rolled JWT HS256; header `{"alg":"HS256","typ":"JWT"}`;
  claims sub=login, iss=`magitrickle`, iat, exp=iat+20 years.
- Signing key = hex(HMAC-SHA256(app_secret, passwordHash)) — password change
  invalidates tokens; app_secret = 32 random bytes, base64 in
  `<state>/auth_secret` (file 0600, dir 0700), created on demand.
- Verification: parse claims unverified → load user's current hash →
  re-derive key → HMAC compare (constant-time), check sub/iss/exp.
- Gap: no tests at all. C-verify: UT (crypt vectors, JWT vectors) + CT
  (login flow round-trip Go-token→C-verify and C-token→Go-verify — the
  token format must be byte-compatible given same secret and hash).

## 4. DNS proxy

Source: `utils/dnsMITMProxy/*`, `dns.go`, `start.go`. Tests: none. C-verify:
DIFF (semantic DNS comparison) + IT + fuzz.

- Listeners: UDP + TCP on `DNSProxy.Host` (default `[::]:3553`, dual-stack;
  IPv4-only when host address is IPv4). UDP replies are sent with source
  address = original destination (pktinfo) and same ifindex.
- TCP: 2-byte length framing; max 65535; **one query per connection**,
  server closes after the answer; read timeout = proxy timeout.
- Upstream: single upstream (`DNSProxy.Upstream`); same transport as the
  client used (UDP→UDP, TCP→TCP); idle connection pools (size
  `MaxIdleConns`) per transport; failed conns dropped, successful returned.
- Concurrency: global semaphore `MaxConcurrent` across UDP+TCP; acquisition
  blocks the accept/read loop (backpressure, no queue, no drops except
  kernel socket buffer).
- Timeout: per-request `Timeout` (context deadline on upstream I/O).
  On upstream failure/timeouts: **no response is sent to the client** (UDP
  silence, TCP close) — client retries.
- Request path: request bytes forwarded verbatim (raw), unless fake-PTR
  answers locally: single-question PTR + `!DisableFakePTR` → NXDOMAIN with
  RA=1, question echoed, no upstream contact.
- Response path: parsed; on `!DisableDropAAAA` all AAAA answers removed and
  message re-packed **with compression enabled**; ID/flags/sections
  otherwise preserved; malformed upstream response → dropped (client gets
  nothing). Non-success rcode responses are forwarded but not processed.
- Records processed for cache/ipset: A, AAAA, CNAME from the Answer section
  only (Authority/Additional ignored). AAAA IPs still enter cache+ipset
  even when stripped from the client answer.
- trimFQDN: single trailing dot removed; matching is on presentation names
  as produced by miekg/dns (wire case preserved; escapes possible for
  non-ASCII labels).
- Malformed client request: if parsing fails, request is **not** answered
  (error path) — but note parsing happens only because hooks are set.
- Oversized: UDP reads up to 65535 (dns.MaxMsgSize buffer); no EDNS
  handling of its own (upstream's answer passes through).

## 5. Records cache

Source: `utils/recordsCache/records.go`. Tests: `records_test.go`
(7 s soak of cleanup). C-verify: UT port + DIFF on scenarios.

- domain → list of (IP, deadline); dedup by IP bytes; re-add refreshes
  deadline. No cap on entries or memory.
- domain → single alias (last CNAME wins) + reverse index alias→[domains].
- `GetAliases(name)` = name + transitive reverse closure (BFS, dedup).
- `GetAddresses(name)` = walk forward through alias chain (cycle-guarded)
  until first domain with any unexpired address; expired entries filtered
  at read, physically removed by 30 s cleanup pass.
- TTL stored as absolute deadlines; inserted TTL = DNS TTL + additionalTTL
  (for hook path) — RuleSet.sync recomputes remaining TTL at sync time.
- Self-alias (`a→a`) ignored.

## 6. Rules

Source: `models/rule.go`; README; tests `models/rule_test.go` (good corpus —
port verbatim). C-verify: DIFF corpus (see regex plan in
`dependencies.md` §regex and `decisions.md`).

- `domain`: exact, case-sensitive byte compare.
- `namespace`: exact or `.`-boundary suffix; also matches leading-dot form
  (`.example.com`).
- `wildcard`: `*` any run, `?` exactly one char (IGLOU-EU/go-wildcard v2),
  case-sensitive.
- `regex`: dlclark/regexp2 (.NET semantics), compiled with IgnoreCase,
  **unanchored substring match**; invalid regex matches nothing (and
  API/subscription validation may reject earlier); no match timeout is set
  (regexp2 default = no timeout → catastrophic backtracking possible;
  document, do not replicate unboundedness — see decisions D-07).
- `subnet`/`subnet6`: never match domain names. They are expanded by
  `RuleSet.sync()` directly into ipset entries: value parsed as CIDR or
  bare IP (/32, /128); IPv4 in `subnet` only, IPv6 in `subnet6` only;
  `0.0.0.0/0` and `::/0` are split into two /1 entries (upstream netlink
  bug workaround); subnet entries get **no timeout** (permanent).
- Disabled rules are skipped everywhere.
- Match order: rules in listed order, first match per group wins; each
  group is evaluated independently (an IP can join several groups' sets).

## 7. Groups → netfilter mapping

Source: `rule_set.go`, `utils/netfilterTools/*`, `utils/iptables/*`.
Tests: `utils/iptables/iptables_test.go` (fake executable, `-tags testing`),
`tests/handlers_v1_test.go`. C-verify: UT against same fake-executable
transcripts + IT in netns.

- ipset names: `<TablePrefix><hex-id>_4` / `_6`; type `hash:net`; default
  timeout 300; per-entry timeout = remaining TTL (nil ⇒ 0 = permanent);
  add with replace; list/del tolerate IPSET_ERR_EXIST.
- Existing sets of wrong type are destroyed and recreated; existing sets are
  flushed on enable.
- chains: `<ChainPrefix><hex-id>` in filter/mangle/nat with the exact rule
  text listed in current-architecture.md §5 (including the CONNMARK
  save-mark rule — Keenetic-critical).
- fwmark/table allocation: first free value ≥ StartMarkTableIndex scanning
  existing ip rules and routes (v4+v6); mark==table index.
- ip rule: fwmark→table; ip route: blackhole prio 20 always; default via
  iface prio 10 (+gateway if discoverable) when iface up; `blackhole`
  pseudo-interface = blackhole-only.
- port remap chain: `<ChainPrefix>DNSOR` in nat PREROUTING position 1,
  TCP+UDP DNAT per link address.
- startup cleanup: all `MT_*`-prefixed chains and `-j MT_*` references
  removed via save/restore diff.
- `iptables` batching: desired state compiled against `iptables-save`
  output; committed via `iptables-restore --noflush`; external flushes are
  healed on the next Commit (netfilterd hook → ForceCommitIPTables).

## 8. Subscriptions

Source: `subscriptions.go`, `subscriptions/*`. Tests: parse/fetch/runtime
tests. C-verify: UT port + DIFF (parse corpus) + CT (sync endpoints).

- Fetch: http/https, 15 s total timeout, ≤5 redirects (301/302 only),
  redirect loop detection, non-2xx → error, unlimited body (C: add bound,
  document as hardening).
- Parse/refresh/type-detect/dedup: as described in
  current-architecture.md §9; ID/enable/type preservation on refresh keyed
  by exact rule text.
- Validation (`validate.go`): type whitelist and per-type syntax checks
  (incl. regexp2 compile for regex type).
- Scheduling: 1-min tick; due = enabled && url && interval>0 &&
  now ≥ (last_check || last_update) + interval; last_check is
  process-lifetime only (restart ⇒ immediate re-check when last_update
  stale).
- Persisted fields: `last_update` only. Auto-update saves config after a
  changed sync.
- Subscription rule sets: runtime key `sub_<hex-id>`, ipset/chain names
  derive from it; rebuild = disable-all + recreate (visible netfilter churn
  during rebuild; C may improve only if observable end state identical).

## 9. Interfaces / platform

Source: `internal/interfaces/*`, `constant/*`, `netlink.go`.

- `GET /system/interfaces`: default only interfaces with FlagPointToPoint
  and not in IgnoredInterfaces; `showAllInterfaces: true` lists all.
  On entware_kn names are decorated via Keenetic RCI
  (`http://localhost:79/rci/`, batch `show/rc/interface` POST) — best
  effort. Test: keenetic test (tag-gated). C-verify: UT + MAN on device.
- netlink watcher contract: on link-up or new-address of a group's route
  interface, re-program ip rule/route for that group. Link removal: no
  action. C-verify: IT in netns (veth up/down/addr).
- PID file: `<PIDPath>`, содержит PID, stale detection via
  `/proc/<pid>/exe` basename comparison.
- Signals: TERM/INT graceful; HUP reload. Exit code 0 on clean shutdown
  (main logs error but exits 0 on failed start — C: preserve? see
  decisions D-11; init scripts rely on process death for respawn).

## 10. Packaging / upgrade

Source: root Makefile, `files/**`, CI workflow.

- Package name `magitrickle`, binary `magitrickled` at
  `/opt/bin` (Entware) or `/usr/bin` (OpenWrt).
- conffiles: Entware `/opt/var/lib/magitrickle/config.yaml`; OpenWrt
  `/etc/config/magitrickle` + `/etc/magitrickle/state/config.yaml`,
  sysupgrade keep `/etc/magitrickle/`.
- Entware init: rc.func wrapper `S99magitrickle` (PROCS=magitrickled);
  postinst restarts service. `_kn` adds ndm netfilter.d hook (socat).
- OpenWrt init: procd, respawn, uci `magitrickle.main.enabled` gate,
  runs as configured user (default root).
- Upgrade path: package replace + service restart; config preserved via
  conffiles; C binary must load the Go-era config byte-for-byte and (on
  save) keep the documented shape.
- Deps (must stay valid for the C build): Entware `libc, iptables`
  (+socat `_kn`); OpenWrt `libc, iptables-nft, iptables-mod-conntrack-extra,
  kmod-ipt-nat, kmod-ipt-ipset, ip6tables-nft`.

## 11. Logging

zerolog console output to stdout; levels trace…disabled via `logLevel`.
Log *content* is not part of the contract except: init scripts pipe stdout;
keep level names and the general "human console line" style. C-verify: MAN.
