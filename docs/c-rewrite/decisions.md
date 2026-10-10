# Decision log (C rewrite)

Format: ID, status (proposed/accepted/superseded), context, decision,
consequences. Phase 0 entries are **proposed** unless marked otherwise —
final acceptance happens when the implementing phase starts.

## D-01 — Source layout `src/backend-c/`

Status: proposed. Adopt the spec's suggested tree (include/magitrickle/ +
src/<module> + tests/{unit,integration,differential,fuzz,benchmarks}).
Go backend stays at `src/backend/` untouched until Phase 9. Makefile gains
parallel `build_backend_c` until switchover.

## D-02 — Event-driven core, epoll, small fixed thread count

Status: proposed. Single epoll loop thread for DNS UDP/TCP + timers +
signals; a small worker pool (N=2 default, configurable) only for
blocking/slow work: iptables fork+exec, libcurl transfers, shadow-file
crypt (SHA-crypt with high rounds is CPU-heavy — must not stall DNS).
Netfilter ipset adds via netlink are non-blocking-fast; measure first
(Phase 5) before moving them off-loop. Justification vs alternatives to be
re-validated by Phase 1 spike per spec §12 (not chosen "because the spec
said epoll": the workload is thousands of tiny independent exchanges —
thread-per-request is the Go model we're explicitly replacing to cut RSS).
Every queue bounded with drop counters; overflow policy per queue
documented in code and in `current` docs.

## D-03 — DNS parser: own minimal implementation

Status: proposed. See dependencies.md. Raw pass-through preserved for the
common path (bytes in → bytes out), parse only what hooks need — mirrors
Go behaviour incl. "hooks force parse" semantics. Full fuzz coverage
mandatory before Phase 3 exit.

## D-04 — YAML via libyaml with Go-yaml-v2 compatibility shims

Status: **accepted** (Phase 1 spike, `src/backend-c/spikes/yaml_emit/`).
libyaml event API with indent=2, width=-1 воспроизводит вывод yaml.v2
**байт-в-байт** (895-байтовый config-fixture идентичен), включая стили:
plain для обычных скаляров, single-quoted для строк с `[`/`#`/`*`,
flow `[]` для пустых списков. Требуемые shims: явный порядок ключей,
duration-строки (`5s`, `1h0m0s`), выбор quote-стиля по правилам yaml.v2
(нужен небольшой классификатор скаляров в Phase 2). Duration parsing
(строки + int=наносекунды + legacy ms/s нормализация) — на стороне load.

## D-05 — JSON via cJSON

Status: **accepted** (Phase 6). Dynamically linked (`libcjson`, MIT,
confirmed available in both OpenWrt and Entware feeds per
dependencies.md), same pattern as libyaml/PCRE2/libmnl — not vendored.
`include/magitrickle/json.h` wraps it with two helpers matching
`api/utils/helpers.go`'s shape: `mt_json_error()` builds `{"error":"..."}`
(Go's `types.ErrorRes`), `mt_json_dump()` serializes compact (no
whitespace), matching Go's default un-indented `json.Marshal` output.
Byte-identical serialization with Go (e.g. Go's default HTML-escaping of
`<`/`>`/`&` in strings) is explicitly NOT a goal — the API compatibility
contract is verified by structural/normalized comparison (parse both
sides, compare values), per migration-plan.md's Phase 6 description; a
JSON parser reads an escaped and a literal `<` identically, so this only
affects wire bytes, never observable behaviour.

## D-06 — HTTP server: own bounded HTTP/1.1 on the shared loop

Status: proposed, pending Phase 1 spike vs libmicrohttpd. If the spike
shows >2 weeks of hardening effort, fall back to libmicrohttpd (LGPL,
in feeds). Hard limits (header 8 KB, body 1 MB, conns 64, timeouts) —
documented hardening, covered by tests.

## D-07 — Regex: PCRE2 with corpus-proven parity + explicit failure mode

Status: **accepted** (Phase 1 spike, `src/backend-c/spikes/regex_corpus/`).
Corpus of 47 cases (repo tests + user-style patterns + .NET constructs):
44/47 identical with `PCRE2_CASELESS|PCRE2_UTF|PCRE2_UCP` vs
`regexp2.IgnoreCase`. Divergences (frozen in `known_divergences.tsv`):
1. POSIX classes `[[:alpha:]]` — regexp2 не поддерживает (тихо не матчит),
   PCRE2 матчит корректно. Итог: С-версия «чинит» ранее сломанные паттерны.
2. Possessive quantifiers (`a*+`) — regexp2 compile_error, PCRE2 работает.
   Ранее нерабочие правила начнут работать.
3. .NET balancing groups (`(?<-name>…)`) — regexp2 работает, PCRE2
   compile_error. Единственная реальная потеря; для доменного матчинга
   экзотика. Политика: явная ошибка при загрузке, без автопереписывания.
Lookahead/lookbehind, backrefs, named groups (обе формы `(?<n>`/`(?'n'`),
inline options, `\A/\z/\Z`, `\p{L}`, atomic groups, conditionals — parity
подтверждён. Match/depth limits включены (1e6/1e4) как hardening.
Phase 2: прогнать корпус, расширенный реальными пользовательскими списками.

## D-08 — ipset/route/link via libmnl; iptables via save/restore exec

Status: proposed. Keeps the proven Go architecture (contract §7); fake
executable seam retained for tests; transcripts from
`utils/iptables/iptables_test.go` become shared fixtures.

## D-09 — HTTP client via libcurl

Status: proposed. https subscriptions need real TLS; manual redirect loop
to preserve exact 301/302-only, ≤5, loop-detect semantics. Response size
bound added (hardening).

## D-10 — Config snapshot swap instead of Go's unlocked config mutation

Status: proposed. Go mutates `App.config` fields during SIGHUP reload with
no lock (data race, benign today). C uses immutable config snapshot +
atomic pointer swap; observable behaviour unchanged.

## D-11 — Process exit codes

Status: proposed. Go exits 0 even when `Start` fails (error only logged).
Init systems (rc.func, procd respawn) treat death as restartable either
way. C will exit non-zero on startup failure — divergence flagged for
review in Phase 1 (procd respawn behaviour must be re-checked; if it causes
respawn storms on permanent config errors, keep exit 0 for parity).

## D-12 — Rule-set snapshots (RCU-light) for the match path

Status: proposed. Match path reads an immutable snapshot (refcounted);
writers build+swap. No lock-free structures beyond atomic pointer + rc.

## D-13 — UPX decision deferred to measurements

Status: accepted (spec). Not applied automatically to C builds; Phase 8
measures startup/RSS impact.

## D-14 — Language/base: C11 + documented GNU/Linux extensions in platform layer

Status: accepted (spec). Extensions (epoll, timerfd, signalfd, pktinfo,
accept4) live under `src/platform/` and are listed in its header docs.

## D-15 — `subnet`/`subnet6` stay out of DNS matching

Status: accepted (verified in code). They are materialized into ipset by
sync only; `IsMatch` returns false. The C rule engine keeps this split
(contract §6).

## D-16 — Benchmark scope on shared CI host

Status: accepted. 1000-concurrency and on-device cells deferred (noise /
no hardware); documented in benchmark-methodology.md; device runs are a
Phase 5/8 obligation, scripts already parameterized (`GROUP_ENABLE=1`).

## D-17 — No locking in the cache/matching hot path (single event-loop thread)

Status: accepted (Phase 4). D-02/D-12 anticipated RCU-style snapshots for
a *possibly* multi-threaded reader set. In the implemented architecture
(D-02) the DNS proxy, the records cache, and rule matching all run
exclusively on the single `mt_loop` thread — there are no concurrent
readers to guard against, so `dns_cache` and `rulesnap` take no locks and
the "atomic swap" of a rule-set snapshot is just a plain pointer store
from that same thread. This satisfies the Phase 4 exit criterion ("no
global lock in match path") by construction rather than by adding
synchronization primitives that would have nothing to protect against
yet. When Phase 7 introduces a worker thread for subscription fetch
(libcurl calls, D-02), the *build* step may run there, but the resulting
snapshot's *publication* must be marshalled onto the loop thread via
`mt_loop_post` before any reader sees it — at that point this decision
gets revisited with real cross-thread publication (refcounting or a
generation counter), not before.

## D-18 — Rule-set snapshot matches per-group as an aggregate OR, not Go's per-rule loop

Status: accepted (Phase 4). Go's `dns.go` processes each group's rules in
config order with two different early-exit idioms: A/AAAA records use
`break Rule` (stop the whole group after the first matching rule),
CNAME records use `continue Rule` (keep checking every rule in the group,
independently deciding per rule whether to act). Both idioms feed the
*same* action inputs regardless of which specific rule fired — A/AAAA
always adds the same (IP, TTL) pair, CNAME always adds the same cached
address set with the same per-address remaining TTL — so the *final
observable state* (what ends up in a group's ipset) is identical whether
you evaluate "did any enabled rule in this group match any candidate
name" once, or replay Go's rule-by-rule loop. The C rule-set snapshot
(`rulesnap.h`) therefore builds one aggregate `mt_matcher_t` per enabled
group (OR over its enabled rules) and asks it once per (group, DNS
record) pair.

The one thing this changes: Go's CNAME path can invoke `AddIPv4Subnet`/
`AddIPv6Subnet` multiple times for a single response when several rules
in the same group independently match different aliases (each call is
idempotent — same subnet, same TTL, `Replace: true` — so it is pure
redundant netlink traffic, explicitly called out as an optimization
target in spec §16 "уменьшить... количество netlink round trips"). The C
path collapses this to one call. No difference in resulting ipset
contents; fewer redundant operations. Documented here per the "не
исправляй несовместимое поведение молча" rule rather than left as a
silent divergence.

Disabled groups are excluded from the snapshot entirely (not just
skipped at match time): Go's `RuleSet.AddIPv4Subnet` no-ops whenever the
group's runtime/model enable flag is off, so a disabled group can never
produce an observable action regardless of whether its rules match —
there is nothing to gain from building or querying an index for it.

Subscription-derived synthetic groups are not part of the snapshot yet
(subscription sync lands in Phase 7); only `mt_config_t.groups` (user
groups) participate today.

## D-19 — Netfilter mutation is single-threaded; no per-object locking; insertion-ordered iteration

Status: accepted (Phase 5). Go's `iptables.IPTables`, `netfilterTools.IPSet`
and `IPSetToLink` each guard every method with `sync.Mutex` (some also
`atomic.Bool`) because multiple goroutines can share one instance
(HTTP API handlers, the DNS hot path, and `start.go`'s init loop all call
into the same objects concurrently in Go). The C backend instead commits
to running all netfilter mutation — iptables engine commits, ipset
create/add/del, ipset-to-link enable/disable, rtnetlink rule/route calls
— on the single event-loop thread (extending D-17's DNS-hot-path
reasoning to the whole netfilter layer); `mt_ipt_t`, `mt_ipset_t`,
`mt_ipset_to_link_t`, `mt_ruleset_t` and `mt_rtnl_t` therefore carry no
internal lock. Callers must not share one instance across threads without
external synchronization; when a future phase adds worker threads (e.g.
Phase 7's subscription fetch), calls into these objects must be
marshalled onto the loop thread via `mt_loop_post`, not called directly
from the worker.

A related, purely representational difference: Go stores per-table
per-chain registrations in `map[string]map[string]chain`, whose iteration
order is randomized by the Go runtime on every `Commit()`; `mt_ipt_t`
(engine.c) uses insertion-ordered arrays instead. This is not an
observable behavioural difference — within one priority bucket a given
chain's own compiled commands stay contiguous, and the relative order
between *different* chains/tables in the iptables-restore transcript is
insignificant to iptables-restore (distinct named chains/tables are
independent) — but it is called out here per the "no silent incompatible
fix" rule, matching the pattern of D-18.

## D-20 — ipset: dedicated per-group netlink socket; wire protocol pinned to version 6

Status: accepted (Phase 5). Two related choices in `netfilter/ipset.c` /
`ipset_nl_real.c`:

1. Go's `netfilterTools.Helper` hands every `IPSet` the same package-level
   netlink handle (`vishvananda/netlink`'s internal `pkgHandle`), so all
   groups share one socket. The C port instead gives each `mt_ipset_t`
   its own dedicated socket via `mt_ipset_nl_real_new()` (one per
   enabled group, since `mt_ruleset_t` owns its `mt_ipset_t`). This keeps
   `mt_ipset_t`'s existing (already-tested) ownership contract simple —
   `mt_ipset_free` owns and closes exactly the transport it was
   constructed with — at the cost of one extra file descriptor per group
   versus Go. On any device this backend targets, group counts are small
   (tens, not thousands), so the extra fd count is not expected to be
   material; this is an accepted, documented inefficiency rather than a
   behavioural divergence.
2. The NFNETLINK wire format (attribute set, flag bits, `hash:net`
   revision 0) is ported from what `github.com/vishvananda/netlink`
   actually emits, which pins `IPSET_PROTOCOL` to `6` — not the current
   kernel UAPI header's default of `7`. This is deliberate: the C backend
   must speak the same wire protocol the Go binary already sends in
   production, not whatever a newer kernel header advertises as current.

Both choices are untestable against a real kernel in the CI/dev sandbox
(no `ip_set` module available, confirmed Phase 0 and reconfirmed Phase 5);
only the fake in-memory transport (`tests/unit/fake_ipset_nl.c`) exercises
`mt_ipset_t`'s logic today. On-device validation remains a prerequisite
before this is trusted on a real router (see phase-5-report.md).

## D-21 — rtnetlink: verify functional equivalence on real kernel state, not byte-mirroring

Status: accepted (Phase 5). Unlike ipset (kernel module unavailable in
every sandbox tried so far) and iptables (fully differential-tested
against Go's fake-executable transcripts), plain rtnetlink — `ip rule`,
`ip route`, `ip link`, `ip addr` — works in this sandbox once `iproute2`
is installed. `rtnl.c` (rule add/del, blackhole route add/del, interface
route add/del with gateway diffing, link-by-name, gateway-for-iface,
mark/table allocation) is therefore built as idiomatic, standard
rtnetlink requests rather than byte-for-byte mirroring every attribute
`vishvananda/netlink` happens to send — including some that look like
incidental zero-values (e.g. an always-present `RTA_OIF=0` attribute on
non-interface routes) that reflect library internals more than protocol
requirements. Correctness was instead verified by exercising the real
kernel: creating rules/routes/links via the C code and independently
inspecting the resulting state with `ip rule show` / `ip route show
table N` / `ip link show`, confirming it matches what the equivalent `ip
rule add` / `ip route add` commands would produce. This is a narrower
compatibility claim than byte-mirroring (spec: "не заявляй о
совместимости только на основании визуального сходства кода") — it is a
claim about observed kernel state, backed by the real-kernel runs
recorded in phase-5-report.md, not about wire-format identity with Go's
netlink library.

One genuine behavioural gotcha found this way (not a Go-compatibility
issue, a C-side bug caught by this testing approach): a single-reply
`RTM_GETLINK` "get" request must use `NLM_F_REQUEST` only. Adding
`NLM_F_ACK` (as an early draft did) makes the kernel send an extra
trailing `NLMSG_ERROR` ack that a non-dump "read one reply, stop" loop
never consumes, permanently desynchronizing every subsequent read on that
socket. Fixed in `rtnl.c`'s `mt_rtnl_link_by_name()`; `NLM_F_ACK` stays
reserved for mutating (add/del) and dump requests.

## D-22 — Ruleset sync(): linear-scan subnet sets instead of a hash map

Status: accepted (Phase 5). Go's `RuleSet.sync()` (rule_set.go) builds
`map[IPv4Subnet]IPSetTimeout` / `map[IPv6Subnet]IPSetTimeout` to
dedupe/diff the desired ipset contents against the current one, giving
O(1) average lookup. `mt_ruleset_sync()` (netfilter/ruleset.c) uses a
plain growable array with linear-scan lookup for both the "new desired
subnets" list and the diff against the current ipset listing, making a
full sync O(n²) in the number of distinct subnets touched (config
subnet/subnet6 rules plus resolved domain addresses in the records
cache). This is an accepted scalability tradeoff, not a claimed
improvement (spec: "не заявляй об улучшении производительности без
измерений") — `sync()` only runs at daemon startup and after API-driven
group/rule mutations (Phase 6), never per-DNS-response, and expected
group/domain counts on the router hardware this backend targets are
small (tens to low hundreds), so the quadratic factor is not expected to
be material in practice. It has not been benchmarked at scale; if a
future phase needs `sync()` to handle large domain counts (e.g. a
subscription-derived group with thousands of rules, Phase 7), this
should be revisited with a real hash table rather than assumed fine.

## D-23 — Own vendored MD5/SHA-256/SHA-512/HMAC/JWT (auth crypto)

Status: accepted (Phase 6), confirms the "Own vendored single-file
implementations" verdict already recorded in dependencies.md. `api/auth/
crypt.go` and `api/auth/jwt.go` are themselves already from-scratch
reimplementations in Go (not calls into a system crypt(3) or a JWT
library) — Poul-Henning Kamp's MD5-crypt and Ulrich Drepper's SHA-256/512-
crypt reference algorithms, plus a minimal hand-rolled HS256 JWT. The C
port (`src/crypto/{md5,sha256,sha512,hmac,base64,crypt,jwt}.c`,
`include/magitrickle/{hash,crypt,jwt}.h`) follows the Go source closely
— this is the one case in the whole rewrite where a near line-by-line
port is the *correct* choice rather than a spec violation: these are
fixed, previously-specified cryptographic algorithms where byte-exact
arithmetic is the entire point (RFC 1321 MD5, FIPS 180-4 SHA-2, RFC 2104
HMAC), not incidental control-flow that should be reworked idiomatically.
No OpenSSL/mbedtls dependency is introduced for these three primitives,
matching the explicit rejection of that option in dependencies.md ("linking
the whole app to a TLS lib for 3 primitives").

Verified two ways: (1) unit tests against published NIST/RFC test
vectors for the raw primitives (empty string, "abc", the two-block NIST
SHA-256 vector, RFC 4231 HMAC test case 1); (2) a **differential** check
against the real Go implementation — `tests/unit/test_crypt.c` and
`tests/unit/test_jwt.c` use vectors captured by temporarily adding a
`TestGenVectors`/`TestGenJWTVectors` test file to `api/auth` in the Go
tree, running it with `go test`, and copying the printed output/tokens
into the C test's expected strings (the temporary Go test file was never
committed). All vectors match byte-for-byte, including the JWT tokens
(satisfying migration-plan.md's explicit "JWT byte-compatible" bar) and
crypt(3) hashes for both default and custom `rounds=N` salts.

Hardening beyond Go (spec: bounded memory): `mt_crypt_password` caps the
password length at `MAX_PASSWORD_LEN` (4096 bytes) before the SHA-crypt
pseq/sseq scratch buffers are allocated; Go imposes no such bound, but no
real password reaches anywhere near this size, so behaviour is unchanged
for every real input.

## D-06 (revisited) — Own bounded HTTP/1.1 server, confirmed

Status: **accepted** (Phase 6), resolving the "proposed, pending Phase 1
spike" note left on the original D-06 entry. `src/api/httpd.c` /
`include/magitrickle/httpd.h` implement a small, purpose-built HTTP/1.1
server on the shared `mt_loop` (epoll) rather than adopting libmicrohttpd
— the API surface is fixed and small (~25 routes under `/api/v1`, JSON
bodies "≤100 KB" per dependencies.md, plus whole-file static serving),
so a general-purpose HTTP library added little beyond what a few hundred
lines of purpose-built parsing/routing already covers, and building it in
means the connection state machine reuses the exact non-blocking
accept/partial-read/partial-write pattern already established for the
DNS proxy's TCP path (`src/dns/proxy.c`) instead of introducing a second
concurrency model.

Supports HTTP/1.1 keep-alive (one request read to completion, response
written, then the next request read on the same connection) and
`Connection: close`/HTTP/1.0 single-request mode; does **not** support
chunked request bodies (every real client here — the frontend's
fetch/axios calls, and the contract tests — sends fixed `Content-Length`
JSON, never chunked transfer-encoding).

Bounded hardening explicitly allowed by compatibility-contract.md §2
("C version may add bounded limits... keep normal-size behaviour
identical"): `MT_HTTPD_MAX_HEADER_BYTES` (8 KiB), `MT_HTTPD_MAX_BODY_BYTES`
(1 MiB), `MT_HTTPD_MAX_CONNS` (64, matching the number originally proposed
for D-06), and a 30 s per-connection idle timeout — none of which Go's
zero-value `http.Server` has, and none of which any real request from the
frontend or the differential contract tests come close to.

One deliberate, documented behavioural broadening vs. chi (Go's router):
a path pattern like `/api/v1/groups/{groupID}` matches both
`/api/v1/groups/{groupID}` and a trailing-slash variant, because segment
splitting ignores leading/trailing/duplicate slashes on both the pattern
and the incoming path before comparing. Strict chi (no `RedirectSlashes`
middleware configured in `api/v1/router.go`) would 404 a mismatched
trailing slash where this server accepts it. This can only ever accept a
request Go would reject, never the reverse, so no legitimate client can
be broken by the difference — flagged here per the "no silent
incompatible fix" rule rather than left undocumented.

Verified with a real client/server integration test
(`tests/unit/test_httpd.c`): runs the actual epoll loop on a background
pthread while the test thread drives it with plain blocking sockets over
both a TCP listener and a Unix socket listener, covering query-string
parsing, path-param extraction, POST body round-trip, the not-found
fallback, middleware short-circuiting (401), keep-alive across multiple
requests on one connection, and Unix-socket routing parity — all under
ASan/UBSan with zero findings.

## D-24 — Auth: platform paths module, and calendar-correct JWT expiry without timegm(3)

Status: accepted (Phase 6). Two small additions supporting
`src/api/auth.c` (port of `api/auth/{passwd,secret,password,middleware,
handlers}.go`):

1. `include/magitrickle/paths.h` — the C backend had no equivalent of
   Go's build-tag-conditional `constant/path_{default,entware,openwrt}.go`
   yet (only `main.c`'s single `MT_CONFIG_PATH` `#ifndef` override
   existed). Added `MT_APP_SHARE_DIR`/`MT_APP_STATE_DIR`/`MT_SOCK_PATH`/
   `MT_PASSWD_FILE`/`MT_SHADOW_FILE` with defaults matching
   `path_default.go` and the same `#ifndef`-override hook, so Phase 8
   packaging can wire per-platform `-D` flags exactly like it will for
   `MT_CONFIG_PATH`. (This per-platform wiring is now done — see D-49;
   until D-49 every C build used the non-Entware/non-OpenWrt defaults,
   which shipped the wrong `/var/...` paths in Entware packages.)
2. Go's `issueToken` computes the JWT expiry via
   `issuedAt.AddDate(jwtYears, 0, 0)` — 20 *calendar* years, correctly
   handling leap years (e.g. Feb 29 rolling to March 1 when the target
   year isn't a leap year). Reproducing this without pulling in `timegm(3)`
   (a GNU/BSD extension; project convention confines GNU extensions to
   `src/platform/`, and auth.c isn't there) uses Howard Hinnant's
   public-domain `days_from_civil`/`civil_from_days` integer arithmetic
   (`mt_auth_add_years_utc`, exposed for testing). Verified against six
   vectors captured from the real Go `time` package
   (`tests/unit/test_auth.c`), including the Feb-29-2080-to-March-1-2100
   non-leap-target-year edge case — byte-identical results.

Also carries forward the shared-primitive extraction already implied by
D-23: `mt_id_random()` (config/id.c) and the new secret generator both
need cryptographically random bytes, so the `/dev/urandom` read-loop was
pulled out into `src/util/rand.c`/`include/magitrickle/rand.h` (a
behaviour-preserving refactor of already-tested code, not a new
decision in itself, noted here for traceability).

Two internal functions (`mt_auth_load_password_hash`,
`mt_auth_authenticate`, `mt_auth_verify_token`) each have a
`..._from(shadow_path, passwd_path, ...)` sibling taking explicit file
paths, used only by `tests/unit/test_auth.c` to point at temp fixture
files instead of mutating the sandbox's real `/etc/shadow` — the
production entry points always pass the real `MT_SHADOW_FILE`/
`MT_PASSWD_FILE`. The in-memory app-secret cache
(`mt_auth_load_secret`) is loaded once per process lifetime (matching
Go's `sync.Once`), which is why a test exercising "two different
`state_dir`s in one process get two different secrets" isn't
meaningful here — Go itself never has more than one `state_dir` per
process either.

## D-25 — App layer: cfg->groups as the live group registry, not a Go-style reconstruction

Status: accepted (Phase 6). `include/magitrickle/app.h` / `src/api/app.c`
port the group/interface/config-save slice of `app.go`'s `App` struct
(`UserGroups`/`AddGroup`/`ClearGroups`/`RemoveGroupByIndex`/
`RemoveGroupByID`/`ListInterfaces`/`SaveConfig`/`ForceCommitIPTables`),
promoting what main.c (Phase 5) built ad hoc into something Phase 6's
HTTP handlers can drive at runtime.

Go's `models.AppConfig` has no `Groups` field at all — group data lives
solely in `a.userRuleSets` (each `*RuleSet` wraps a `*models.Group` via
`spec.Model`), and `SaveConfig()` reconstructs a fresh `[]*models.Group`
from `rs.Model()` on every save. The C port instead keeps `cfg->groups`
(an array of pointers, so appending/removing entries reallocates the
pointer array but never invalidates the pointees) as the single live,
mutable group registry: `mt_ruleset_t` already stores a `const
mt_group_t *` pointing straight at a `cfg->groups` entry (this was
already true from Phase 5), so `mt_app_add_group`/`mt_app_remove_group_*`
mutate `cfg->groups` and the app's parallel `mt_ruleset_t*` array
together, index-for-index, and `mt_app_save_config` is then just a
direct call to the existing `mt_config_save_file` — no Go-style
reconstruction step needed. Two new `mt_config_t` helpers back this:
`mt_config_remove_group_by_index` and `mt_config_clear_groups`.

Faithfully ports two behavioural asymmetries visible in `app.go` that are
easy to miss: `ClearGroups` calls `Disable()` on every group before
clearing, but `RemoveGroupByIndex`/`RemoveGroupByID` do **not** — the Go
callers (e.g. `handlers.go`'s `DeleteGroup`) call `Disable()` themselves
first when the group is enabled. `mt_app_clear_groups` disables;
`mt_app_remove_group_by_index`/`by_id` don't — Phase 6's Groups/Rules
handlers (next task) must disable before removing, exactly like Go's
handlers do.

`ListInterfaces` is ported via `getifaddrs()` deduped by name (matching
`net.Interfaces()`), filtered by `IFF_POINTOPOINT` unless
`show_all_interfaces` is set (matching `interfaces.filterManaged`) — the
default/non-`entware_kn` `IgnoredInterfaces` list is empty in Go, so
there's nothing further to filter on this platform. The Keenetic-RCI
friendly-name lookup (`entware_kn`-only, HTTP calls to `127.0.0.1:79`)
is out of scope here: every interface's `name` field is empty, matching
Go's `DummyRouterSpecificAPI` (the same fallback every non-`entware_kn`
build already uses) — deferred to Phase 8 packaging alongside the rest
of the `entware_kn` build-tag surface.

Verified in `tests/unit/test_app.c`, including a rollback test that
exercises `mt_app_add_group`'s real failure path: adding an
`enable:true` group while the app is marked "running" attempts a real
`ipset create` via libmnl, which fails in this sandbox (no `ip_set`
kernel module — the same Phase-0/Phase-5 finding), and the test confirms
the group is removed from both the ruleset list and `cfg.groups` after
the rollback, not left half-added.

## D-26: Groups/Rules HTTP handlers (`groups.h`/`groups.c`)

Ports `api/v1/handlers.go`'s group+rule handlers and
`api/v1/converters.go` (compatibility-contract.md §2's groups/rules row)
onto the Phase 6 `mt_httpd_t` + `mt_app_t` layers. DTO JSON is
built/parsed directly with cJSON inside `groups.c` rather than a separate
public type — mirrors `converters.go` living in the same Go package as
`handlers.go`; nothing outside this module needs the wire shape.

**Mutable access to a live group.** Go's `RuleSet.Model()` returns
`*models.Group`, the *same* object the caller mutates in place (e.g.
`PutGroup`'s `GroupFromReq(req, groupWrapper.Model())`). `mt_ruleset_t`
stores its group as `const mt_group_t *` (ruleset.c is read-only over
it), so a new accessor, `mt_ruleset_group_mut()`, was added to
`ruleset.h`/`ruleset.c` — it returns the same borrowed pointer without
the `const` qualifier. This is a legitimate cast, not a const-away hack:
the pointee is genuinely mutable (owned by `cfg->groups`, per D-25), only
`ruleset.c`'s own accessor was const for its internal safety. Used by
`handle_put_group`/`handle_put_rule`/etc. to edit a group/rule's fields
in place, preserving its `cfg->groups[i]` identity — critical, since
every `mt_ruleset_t` borrows that exact pointer and swapping it out from
under the ruleset would require re-pointing the ruleset too.

**Building vs. mutating in place.** Unlike Go, `group_from_req()` in
`groups.c` always returns a brand-new `mt_group_t*` rather than mutating
`existing` directly (matching Go's signature `GroupFromReq(req,
existing)` in spirit, not by aliasing the same memory). For `PutGroup`,
where the live group's identity must be preserved, the new object is
then transplanted onto the live one field-by-field
(`group_move_into()`, which frees the live group's old contents, moves
the built object's pointers over, and frees the now-empty built shell
with a plain `free()` — not `mt_group_free()`, which would double-free
the pointers just moved). For `CreateGroup`/`PutGroups`, the freshly
built object becomes the live one directly via `mt_app_add_group`
(which always takes ownership).

**Lenient vs. strict rule-ID reuse — a real Go distinction, not a
divergence.** Go has two different ID-matching code paths for rules,
faithfully kept as two separate C functions:
- `rule_from_req()` (lenient, mirrors `RuleFromReq`): used for
  `CreateRule`, and for a `GroupReq`'s nested `"rules"` array
  (`CreateGroup`/`PutGroup`/`PutGroups`). An `"id"` that doesn't match
  any baseline rule is *not* an error — Go silently assigns a fresh
  random ID instead of failing.
- `rule_from_req_strict()`: used only by `PutRules` (the group-level
  bulk rule replace), which has its own inline found-tracking loop in
  Go, distinct from `RuleFromReq` — an unmatched `"id"` here is
  `MT_ERR_NOENT` → HTTP 404 ("rule not found"), and a body missing the
  top-level `"rules"` key is 400 ("no rules in request").

`PutRule` (single-rule update by path) matches Go exactly by *not*
calling either converter: it mutates the path-resolved rule's fields in
place and never reads the body's own `"id"` field at all (Go: `rule :=
groupWrapper.Model().Rules[ruleIdx]; rule.Name = req.Name; ...`).

**Documented gap: subscription rule-set sync.** Go's `PutGroups` also
calls `h.app.SyncSubscriptionRuleSets()` after replacing the group list.
Subscriptions aren't wired into `mt_app_t` yet (that's Phase 6 task
#36), so this call is a no-op here for now — noted with a code comment
at the call site in `handle_put_groups`, to be revisited once
subscriptions land.

**Middleware-index smuggling not replicated.** Go's chi router resolves
`groupID`/`ruleID` path params in nested middleware and smuggles the
resulting index to inner handlers via a request header
(`r.Header.Set("groupIdx", ...)`). The C router's `{name}` path-param
support makes this unnecessary: each handler resolves `groupID`/`ruleID`
directly off the request (`resolve_group`/`resolve_rule`), producing the
same 400 (invalid hex id) / 404 (not found) behavior without the
header-passing indirection — an implementation simplification with no
observable behavior change.

Verified in `tests/unit/test_groups.c` (10 tests, HTTP-level against a
real `mt_httpd_t` + `mt_app_t` over the background-loop-thread +
blocking-client harness already established by `test_httpd.c`/
`test_auth.c`): empty list, create with color normalization and default
`enable`, invalid/unknown group and rule ids, `PutGroup` preserving rules
when the body omits `"rules"` and rejecting an ID mismatch, delete,
bulk `PutGroups` reusing group/rule IDs across a replace while dropping
unreferenced groups, full rule CRUD, and `PutRules`' strict-vs-lenient
ID validation. The app under test is never marked "running", so
enable/disable/sync are no-ops regardless of a group's `enable` field —
the real-netfilter-backed failure path through these handlers is a thin,
already-tested pass-through of `mt_ruleset_t`/`mt_app_t` return codes
(see `test_app.c`'s `add_group_while_running_rolls_back_on_failure`),
so it isn't re-exercised at the HTTP layer here.

## D-27: System endpoints, static skin serving, and main.c HTTP wiring

Ports the remaining pieces of `api/v1/handlers.go` not covered by D-26
(`ListInterfaces`, `SaveConfig`, `NetfilterDHook` — `system.h`/`system.c`)
and `http.go`'s wildcard static-file fallback (`staticfiles.h`/
`staticfiles.c`), then wires everything built across Phase 6 so far into
`main.c` — this is the first point at which the C daemon actually serves
the HTTP/Unix-socket API end to end, not just in unit tests.

**Security fix found while building this task: `mt_http_req_path()`
wasn't actually cleaning `..`/`.` segments.** httpd.h's contract
(written during task #31) already *claimed* `mt_http_req_path` returns a
"path-cleaned (matches Go's `path.Clean`)" string, but the task #31
implementation only percent-decoded the path — it never collapsed dot
segments. That gap was harmless until this task, since nothing yet
joined the request path onto a filesystem root, but `staticfiles.c`'s
skin-file serving does exactly that. A request like
`GET /../../../../etc/passwd` would have joined onto
`<skins_dir>/<skin>/../../../../etc/passwd` and, depending on nesting
depth, could have escaped the skin directory entirely. Fixed by adding
`path_clean()` to `httpd.c` (Go `path.Clean` semantics for a rooted
path: `.` segments are dropped, `..` pops the last kept segment or is
simply dropped at/above the root — since the input is always absolute,
this can never escape upward) and applying it to every parsed request
path before it's exposed via `mt_http_req_path()`. Verified directly:
`test_staticfiles.c`'s `path_traversal_is_contained_under_skin_root`
sends exactly that request against a real skin fixture directory and
asserts a plain in-skin 404, not real `/etc/passwd` contents.

**Static file serving (`staticfiles.c`)** ports `http.go`'s `r.Get("/*",
...)` handler closely, including its exact up-to-2-stat retry loop
(directory → append `/index.html` → retry once, matching Go's
`for i := 0; i < 2; i++`) and its three response shapes: normal file
(content-type by extension: html/css/js/ico/png/svg, else
`text/plain`), missing file (404 JSON `{"error":...}`), and missing
file *at `/` specifically* (404 with Go's exact
`noSkinFoundPlaceholder` HTML string). Registered only as the TCP
`mt_httpd_t` instance's not-found fallback (`mt_httpd_set_not_found`),
never on the Unix socket instance — matches `unixsocket.go`, which
mounts only the v1 API router. Non-GET requests to an unmatched path
get a plain 404 here rather than chi's default text/plain 404 page;
documented as a cosmetic, status-code-level-only difference (D-05
already established that byte-identical bodies aren't a goal).

**`main.c` refactor: `mt_app_t` replaces the ad hoc `mt_ruleset_t**`
array.** Phase 5's `main.c` built and owned its own raw ruleset array
inline. Since the Phase 6 HTTP handlers (D-26, this entry) need a real,
running `mt_app_t*` to mutate, `struct daemon`'s `rulesets`/`n_rulesets`
fields are gone, replaced by `mt_app_t *app` (plus `mt_httpd_t
*http_tcp`/`*http_unix`). `find_ruleset`/`on_link_up`/`on_addr_change`
now go through `mt_app_find_group_by_id`/`mt_app_user_group_count`/
`mt_app_user_group_at` instead of iterating the old array directly. The
startup sequence still enables+syncs every initially-configured group
itself (via `mt_app_user_group_at`) *before* calling
`mt_app_set_running(app, true)` — exactly the ordering app.h's own
header comment already prescribed (mirrors Go's `Start()` CAS happening
before that same loop), so a group added later through the HTTP API
gets its own immediate enable+sync via `mt_app_add_group`, while the
initial set is brought up directly by `main()`.

**HTTP/Unix-socket topology matches Go exactly:** the Unix socket
(`MT_SOCK_PATH`) always starts and always mounts the full v1 router
(groups/rules/system/auth — matches `unixsocket.go`, which has no
enabled-gate and never installs the auth middleware); the TCP WebUI
only starts when `cfg.app.http_web.enabled`, and only the TCP instance
gets `mt_auth_middleware` and the static-file not-found fallback
(matches `http.go`). `SaveConfig`'s path/version (needed by every
`?save=true` handler and the `/system/config/save` endpoint) come from
the same `config_path`/`MT_VERSION` `main()` already resolves at
startup — no new state.

**Verified two ways.** Unit-level: `test_system.c` (4 tests: blackhole
prepended with `name` omitted per Go's `omitempty`, config save
round-trip through a real temp file, no-op-without-a-path, netfilter.d
hook success + malformed-body 400) and `test_staticfiles.c` (7 tests
including the path-traversal-containment test above), both against a
real `mt_httpd_t` over the same background-loop-thread harness as
`test_groups.c`. End-to-end: built and ran the actual
`magitrickled-c` binary against a scratch config (remap53 disabled,
`showAllInterfaces: true`, no groups) and drove it with real `curl`
over both the TCP WebUI and the Unix socket: `GET /groups` (empty),
`POST /groups` with `enable:false` (200, persisted), the *same* POST
with the default `enable:true` (500 — the sandbox's known-missing
`ip_set` kernel module, same root cause as `test_app.c`'s rollback
test, and confirmed the failed group was *not* left in `GET /groups`
afterward), `GET /system/interfaces`, `GET /auth`, `GET /` (the
no-skin-installed HTML placeholder, since no skin is built into this
scratch environment), `POST /system/config/save`, and
`POST /system/hooks/netfilterd`; `SIGTERM` produced a clean shutdown
log and left no `MT__`-prefixed iptables chains behind afterward.

Full suite (24 unit-test binaries, up from 22), `make static_analysis`
(clang-tidy + cppcheck), and `make sanitize` (ASan+UBSan) all clean
after these changes.

## D-28: Subscriptions CRUD endpoints (non-fetch)

Ports the pure config-mutation slice of `api/v1/subscription_handlers.go`
and `subscription_converters.go`: `GET`/`PUT`/`POST
/api/v1/subscriptions` and `DELETE /api/v1/subscriptions/{id}`
(`subscriptions_api.h`/`subscriptions_api.c`), plus `mt_app_t` additions
(`mt_app_subscription_count`/`_at`/`find_by_id`,
`mt_app_add_subscription`, `mt_app_replace_subscriptions`,
`mt_app_remove_subscription_by_id`) and two `mt_config_t` helpers
(`mt_config_remove_subscription_by_index`, `mt_config_clear_subscriptions`
— the subscription-side mirrors of D-25's group equivalents).

**Deliberately not implemented (need libcurl, Phase 7 scope): `POST
/api/v1/subscriptions/{id}/sync` and `GET /api/v1/subscriptions/rules?url=`.**
Both call Go's `subscriptions.FetchList` over HTTP(S), which this C port
has no dependency for yet. Neither route is registered, so a request to
either falls through to the normal 404 path rather than being faked or
stubbed with a fake success. This was the plan's own instruction for
this task, called out again here for the historical record.

**Much simpler than group CRUD, for a real reason: subscriptions have no
runtime `mt_ruleset_t`/netfilter counterpart in the C port yet.** Go's
`App.AddSubscription`/`ReplaceSubscriptions`/`RemoveSubscriptionByID`
each rebuild `a.subscriptionRuleSets` (via
`syncSubscriptionRuleSetsLocked`) and can fail/roll back if that rebuild
fails — subscription-derived rule sets are matched during DNS resolution
exactly like user groups (`app.ruleSetSnapshot` concatenates both), and
subscriptions get real ipset/iptables state through the same
`groupruntime.BuildRuntimeRuleSet` path groups use. None of that exists
on the C side yet (only the subscription *config model* and the
subscription-list-parsing logic from Phase 2's `subparse.c` do), so
`mt_app_add_subscription`/`mt_app_replace_subscriptions`/
`mt_app_remove_subscription_by_id` are plain, unconditionally-successful
`cfg->subscriptions` array mutations with no rollback path to speak of —
a real, load-bearing gap (not yet DNS-matchable or netfilter-backed),
documented here so it isn't mistaken for parity with groups.

**No per-subscription GET/PUT and no in-place mutation, unlike
groups — because Go doesn't have them either.** `router.go`'s
`/subscriptions` route tree has no `GET`/`PUT /{subscriptionID}`; only
bulk `GET`/`PUT`, single-create `POST`, single-delete `DELETE`, plus the
two fetch-requiring routes above. Every subscription mutation in Go goes
through `SubscriptionFromReq`, which always builds a *new*
`models.Subscription` (Go's `App` doesn't expose anything like
`RuleSet.Model()` for subscriptions to mutate in place) — so unlike
`mt_ruleset_group_mut` (D-26), no mutable-accessor equivalent was needed
here at all.

**Two faithfully-preserved Go quirks, kept rather than "fixed":**
- **`?save=` defaults are the *opposite* of the groups handlers.**
  Groups: `r.URL.Query().Get("save") == "true"` (opt-in, defaults to not
  saving). Subscriptions: `r.URL.Query().Get("save") != "false"`
  (opt-out, defaults to saving). This is a real asymmetry already
  present in the Go handlers, not something introduced by this port —
  `maybe_save` in `subscriptions_api.c` implements the opt-out form
  exactly, and the header doc on `mt_subs_ctx_t` calls it out explicitly
  so a future reader doesn't "fix" it into consistency with groups.
- **`ensureUniqueSubscriptionIDs`/`ensureUniqueSubscriptionRuleIDs` are
  silent fixups, not validation.** Despite returning `error` in Go,
  neither function can actually produce one — a zero or duplicate ID
  (checked only against entries processed so far, matching Go's
  incrementally-built `dup` map) is silently replaced with a fresh random
  ID. Ported as `void`-returning fixup functions in
  `subscriptions_api.c` for the same reason. Also preserved: `Interval`
  is *never* seeded from an existing subscription during
  `SubscriptionFromReq`/`subscription_from_req` (only ever taken from the
  request body, defaulting to 0) — asymmetric next to
  `LastUpdate`/`LastCheck`, which *are* seeded from `existing`, but that
  asymmetry is genuinely how Go's converter reads, so it's kept rather
  than "corrected."

Verified in `tests/unit/test_subscriptions_api.c` (7 tests: empty list,
create requires a URL, create defaults (`enable` true, empty `rules`),
duplicate-ID create → 409, delete + 404-on-unknown + 400-on-malformed-id,
`PUT` missing-key/missing-URL → 400, and a bulk-replace test that
captures a real server-assigned rule ID from a prior `GET` -- confirming
along the way that a client-supplied nested-rule ID is silently
discarded on *creation* (no baseline to match against), exactly like
`RuleFromReq` for groups -- then reuses that captured ID plus the
subscription's own ID across the `PUT`, checks `lastUpdate` was seeded
from the pre-existing record, and confirms an unreferenced subscription
is dropped). Also smoke-tested end-to-end against the real
`magitrickled-c` binary (create, list, missing-URL 400) alongside the
groups/system smoke test from D-27.

25 unit-test binaries (up from 24), static analysis, and sanitizers all
clean.

## D-29: HTTP API contract differential suite (Go daemon vs magitrickled-c)

Adds a new differential suite (`tests/differential/http_contract/
contract.py` + `tests/differential/run_http_diff.sh`, wired into the
master `run_diff.sh` as its final step) that runs the *real* Go daemon
binary and the real `magitrickled-c` binary against byte-identical
scratch configs and drives each through the same fixed sequence of 37
HTTP/Unix-socket requests covering everything built in Phase 6 tasks
#31-36 (auth status, full group/rule CRUD including bulk `PUT`,
`system/interfaces`/`config/save`/`hooks/netfilterd`, subscriptions
CRUD, and a Unix-socket spot check), then diffs the two resulting
traces textually. This is the first suite in the project that stands up
two full, real daemon processes rather than invoking one-shot CLI
oracles — a different shape of test from every earlier phase's
differential work, so several new problems had to be solved:

**Go's config path has no CLI override.** Every earlier differential
suite drives Go through small single-purpose `oracle_go`
tools/`App.LoadConfig`+`SaveConfig` calls with paths passed as
arguments. The real Go daemon (`cmd/magitrickled`) has no such
flexibility: `cfgFileLocation` is `constant.AppStateDir +
"/config.yaml"`, a hard-coded constant, unlike the C daemon's `--config`
flag. `run_http_diff.sh` therefore backs up whatever is currently at
`/var/lib/magitrickle/config.yaml` (this sandbox already had a stale
`0.99.0` file left over from the `config` suite's own oracle runs),
overwrites it with the scratch config for the Go run, and restores the
backup (or removes the file if none existed) in a `trap ... EXIT`
cleanup — the C run, by contrast, just uses `--config` against a
separate scratch path, so it never touches the real file at all. Stale
`/var/run/magitrickle.{pid,sock}` files are also cleared before each
run (Go's `cmd/magitrickled` refuses to start with a stale PID file
pointing at another process image, and both backends need a fresh
socket path to bind).

**Random IDs required a "learn, then redact" design, not just
normalization.** A raw byte-diff between two independent backends will
never match wherever either one generates a random ID (group/rule/
subscription IDs are 4 random bytes, hex-encoded) — but simple
normalization isn't enough either, because a later request needs the
*real* ID to address the right resource (e.g. `PUT
/groups/{id}/rules/{ruleId}` needs the actual rule ID a `POST` just
returned, which is a different random value on each backend). Solved
with `contract.py`'s `Trace.step(..., learn=callback)`: the callback
inspects a step's parsed response *before* that step's own trace line is
printed, registers the real ID against a stable per-slot placeholder
(e.g. `<RULE-R2>`), and the redaction is applied everywhere that value
subsequently appears — including retroactively in the very response
that introduced it, and in the literal request path of later steps
(`GET/PUT/DELETE .../rules/<real-id>` prints as `.../rules/<RULE-R2>`).
Group- and subscription-level top-level IDs are the opposite case:
those ARE supplied explicitly and ARE expected to be honored literally
by both backends (a brand-new object's own client-supplied ID is used
as-is), so they're deliberately left unredacted — verifying that literal
echo is exactly the point.

**One request body deliberately avoids a known, already-documented,
intentional divergence rather than tripping over it.** The first draft
of this suite created a subscription with `"enable": true` and got a
real divergence: Go's `AddSubscription` builds and enables a real
netfilter-backed subscription rule set (`syncSubscriptionRuleSetsLocked`
→ the same `RuleSet` machinery groups use), which failed in this sandbox
trying to link the "eth0" ipset (a real, if incidental, environment
limitation); `mt_app_add_subscription` (D-28) has no such runtime
counterpart yet and trivially succeeds. That gap is real and already
documented in D-28 — this suite isn't the place to re-litigate it, so
the subscription in the test sequence uses `"enable": false`, keeping
both backends on the config-only path this suite is actually meant to
verify (subscription netfilter parity has no C-side implementation to
compare against yet).

**Error message *text* is normalized away, not compared.** Any
`{"error": "..."}` body has its message value redacted to a fixed
placeholder before comparison (e.g. Go's `"subscription id conflict"`
vs the C port's generic `mt_err_str(MT_ERR_EXIST)` → `"already exists"`
for the same 409) — consistent with D-05's original position that
byte-identical serialization (and, by extension, exact wording) was
never the contract; status codes and shape are.

Verified by running the full suite twice in a row (idempotent — no
leftover iptables `MT_`-prefixed chains or stray socket/PID files
between or after runs, confirmed via `iptables -L`/`-t nat -L`) and as
part of a complete `run_diff.sh` invocation covering every earlier
phase's differential suite alongside this one, all green.

## D-30: Frontend Playwright e2e suite against magitrickled-c

Adds `tests/differential/run_e2e_diff.sh` (Phase 6, task #38): builds
the production frontend (`npm run build`), serves it as the `default`
skin from a real `magitrickled-c`, and runs the *actual*
`tests/e2e/*.spec.ts` suite (45 tests, unmodified) against it via a new
`playwright.c-backend.config.ts`. All 45 pass, run twice in a row from a
clean state with no leftover `/usr/share/magitrickle`, socket/PID
files, or iptables `MT_` chains afterward.

**Why this mostly exercises `staticfiles.c`, not the HTTP API.** Nearly
every spec in `tests/e2e/` intercepts its own API calls via Playwright's
`page.route()` (see e.g. `groups.spec.ts`'s `beforeEach`), which takes
priority over whatever a real server would return — so which backend is
actually running underneath barely matters for those assertions. What
*does* matter, and what this suite genuinely exercises for the first
time against a real, unmodified, production Svelte build (rather than
the hand-written HTML/CSS/JS fixtures in `tests/unit/test_staticfiles.c`,
D-27), is: does `magitrickled-c` serve `index.html` at `/`, the JS
bundle and CSS with correct content-types, and font/image assets
correctly enough for the app to actually boot and become interactive in
a real browser. The HTTP *API* contract itself is D-29's job
(`run_http_diff.sh`), not this suite's.

**Two environment-specific fixes were needed, neither of which touched
`tests/e2e/*.spec.ts` itself:**
- **Chromium executable path.** This environment pre-installs a fixed
  Chromium revision at `/opt/pw-browsers/chromium-1194/...`, but
  `@playwright/test`'s installed version expects a different
  auto-downloaded revision path (`chromium_headless_shell-1223/...`,
  which doesn't exist here and mustn't be fetched — see the session's
  own environment notes). `playwright.c-backend.config.ts` sets
  `launchOptions.executablePath` explicitly rather than modifying
  `playwright.config.ts` (the existing dev-server config, left alone).
- **Origin-locked clipboard permission grant.** `groups.spec.ts`'s two
  clipboard tests call `context.grantPermissions([...], { origin:
  "http://localhost:5173" })` — a literal, pre-existing hardcoded origin
  in the test file. A permission grant's origin must match the page's
  actual origin exactly, so `run_e2e_diff.sh` serves the C daemon on
  `localhost:5173` (not `127.0.0.1:18099`, tried first and found to fail
  exactly these two tests) purely to land on the same origin the test
  already assumes — again, no edits to the spec itself.

Both fixes were necessary to get a correct, unmodified upstream test
suite running against a new backend in a specific sandbox, not
adaptations of the tests to the C port's behavior — no C-port-specific
behavior difference was found or needed accommodating.

**Backup/restore for the real `/usr/share/magitrickle/skins`
directory**, mirroring D-29's `/var/lib/magitrickle/config.yaml`
backup/restore pattern for the same reason: `MT_APP_SHARE_DIR` is a
compile-time default in `paths.h` (real per-platform overrides are
Phase 8 packaging work), so this suite writes to the real path rather
than a build flag, and must not clobber anything already installed
there.

## D-31: Subscription list fetch via libcurl (`sub_fetch.h`/`fetch.c`)

Ports `subscriptions/fetch.go`'s `FetchList` (Phase 7, task #40) onto
libcurl rather than hand-rolled sockets/TLS — dependencies.md already
called this out as the one place hand-rolling is out of the question
("TLS is non-negotiable for https subscription URLs"). `libcurl4-openssl-dev`
was not preinstalled in this sandbox (only the runtime `.so`); installed
via `apt-get install libcurl4-openssl-dev` to get headers for
development — the actual OpenWrt/Entware feed packages are the real
dependency target (already listed in dependencies.md's table), this was
only a local dev-environment gap.

**Exact redirect semantics via libcurl's URL API, not its own redirect
following.** `CURLOPT_FOLLOWLOCATION` is left off; the C port implements
the same manual loop Go's `FetchList` does, because libcurl's automatic
following (a) follows more status codes than Go's custom loop (which
deliberately only treats 301/302 as "redirect to follow" — any other
3xx, including 303/307/308, is treated as a plain non-2xx terminal
failure, a genuine Go quirk kept here rather than "fixed"), and (b)
doesn't expose the same loop-detection/hop-count semantics. Each hop:
`curl_url()`/`curl_url_set(CURLUPART_URL)` validates the URL and
extracts scheme+host (mirrors Go's `url.Parse` + non-empty
scheme/host check); only `http`/`https` are accepted, at the original
URL and at every redirect target. `CURLINFO_REDIRECT_URL` (queried after
a `FOLLOWLOCATION`-off request that received a redirect status) already
resolves a relative `Location` header to an absolute URL exactly like
Go's manual `parsed.ResolveReference(location)` step, so no separate
relative-URL-resolution code was needed. Loop detection uses a small
fixed-capacity array (bounded by `MT_SUB_FETCH_MAX_REDIRECTS + 1` —
tiny, so a linear scan beats introducing a hash set), seeded with the
original URL before the loop starts, matching Go's `visited` map's
initial seed. The `redirects >= maxFetchRedirects` bounds check is
ordered exactly as Go's is (checked *before* marking the new location
visited and advancing), so the boundary behavior matches precisely: up
to 5 redirects succeed, a 6th is rejected as MT_ERR_LIMIT before ever
being fetched — verified directly with two dedicated redirect chains in
`test_sub_fetch.c` (`allows_exactly_five_redirects` /
`rejects_six_redirects`).

**`MT_SUB_FETCH_MAX_BODY_BYTES` (8 MiB) is new C-side hardening, not a
behavior port.** Go's `io.ReadAll(resp.Body)` has no size limit at all;
migration-plan.md's own Phase 7 line item calls for a "size bound" as a
deliberate addition (mirrors the same "C version may add bounded
limits... document as hardening" allowance already used for the HTTP
server in Phase 6, D-06 revisited). Enforced in the libcurl write
callback (returning a short write count aborts the transfer with
`CURLE_WRITE_ERROR`, mapped to `MT_ERR_LIMIT`), verified with a
dedicated test serving a body just over the cap.

**`curl_global_init`/`curl_global_cleanup` are the caller's
responsibility** (`mt_sub_fetch_global_init`/`_cleanup`), not called
implicitly inside `mt_sub_fetch_list` — libcurl's own documented
contract requires global init to happen once, non-concurrently, before
any thread performs a transfer; main.c will call this once at startup
(wired in a later Phase 7 task alongside the rest of the daemon startup
sequence, not yet done as of this task).

Verified with `tests/unit/test_sub_fetch.c` (11 tests, against a real
local `mt_httpd_t` server acting as the stub subscription-list host —
same background-loop-thread harness as `test_httpd.c` — covering plain
200, empty body, 301, 302, redirect loop, the 5-vs-6-redirect boundary,
non-2xx, oversized body, unsupported scheme, malformed URL, and
connection-refused). Full suite (26 unit-test binaries), static
analysis, and sanitizers all clean.

## D-32: Subscription runtime rule sets, DNS-snapshot integration, and a Phase-6 DNS-matching regression fix

**Synthesis over generalization, mirroring Go's own `Spec` design.**
Go's `RuleSet` operates over `rulesets.Spec`, a small carrier type built
either directly from a `*models.Group` (user groups) or synthesized
fresh on every rebuild from a `*models.Subscription`
(`subscriptionAsRuntimeRuleSet`). C's `mt_ruleset_t` is hardwired to
`const mt_group_t *` (Phase 5, unmodified since, already tested). Rather
than generalizing `ruleset.c` into a Spec-like abstraction, the
subscription side mirrors Go's own approach: `mt_sub_runtime_group()`
(`subscriptions/runtime.c`) synthesizes an independent `mt_group_t*`
from a `mt_subscription_t` (id, name — falling back to
`"subscription:<id>"` when empty, matching Go — interface, enable
computed as `sub->enable && iface non-empty`, and one `mt_rule_t` per
subscription rule), which is then fed through the existing, unmodified
`mt_ruleset_new()` exactly like a real config-file group would be.
`color` is intentionally left NULL: only the HTTP JSON layer reads it,
and synthesized groups never reach that layer.

**Ownership: `mt_ruleset_new()` borrows its group pointer** (ruleset.h's
pre-existing documented contract — "callers keep the owning `mt_config_t`
alive for the ruleset's lifetime"), so a synthesized group can't be
freed right after building its ruleset. `mt_app_t` keeps a parallel
owned array, `sub_synth_groups`, alongside `sub_rulesets`, freed in
lockstep (`sub_rulesets_push`'s realloc only bumps `cap_sub_rulesets`
once *both* parallel array reallocs have succeeded, so a partial-realloc
failure can never leave the two arrays believing they have different
capacities). Subscription rulesets live in this array, separate from
`cfg->groups`' rulesets, because Go always rebuilds subscription rule
sets wholesale (`buildSubscriptionRuleSetsLocked` disables+recreates
every one on any subscription-list-or-rules change, never edits in
place) — `rebuild_subscription_rulesets()` in `app.c` does the same:
disable+free the old array, synthesize+build fresh for every
subscription, roll back to nothing enabled and log on failure exactly
like Go's `syncSubscriptionRuleSetsLocked` failure path.

**`mt_app_add_subscription`/`_replace_subscriptions`/
`_remove_subscription_by_id` all rebuild-wholesale and roll back the
`mt_config_t` mutation (not just the rulesets) on failure**, mirroring
Go's `AddSubscription`/`ReplaceSubscriptions`/`RemoveSubscriptionByID`
exactly: mutate `cfg`, rebuild; on failure, undo the `cfg` mutation and
rebuild again (logging if even that fails), so the app is never left
with a `cfg`/ruleset-array mismatch. `mt_app_remove_subscription_by_id`
changed signature from `bool` to `mt_err_t` + `bool *out_found` (mirrors
Go's `(bool, error)` return) so the HTTP handler can distinguish 404
(not found) from 500 (found, rebuild failed) — the one existing caller
(`subscriptions_api.c`'s `handle_delete_subscription`) was updated in
the same change.

**Real Phase-6 regression found and fixed: the DNS-matching snapshot was
never rebuilt after startup.** `mt_ruleset_snapshot_build(&cfg)` was
called exactly once, at daemon startup in `main.c`, and never again —
every Phase-6 HTTP-driven group/rule mutation updated netfilter (via
`mt_ruleset_t`'s own enable/disable/sync) but never touched DNS
matching, so a group or rule created purely through the HTTP API would
never resolve traffic into it. This was a genuine bug in already-shipped
Phase 6 code, not a hypothetical concern, found by tracing every call
site of `mt_ruleset_snapshot_build`/`mt_dns_pipeline_set_snapshot`
across `main.c`/`app.c`/`groups.c`. Fixed with a new public
`mt_app_republish_dns_snapshot(app)`: rebuilds the snapshot from
`app->cfg` and calls `mt_dns_pipeline_set_snapshot` on the pipeline
handed to `mt_app_create` via the new (optional, NULL-able)
`mt_app_deps_t.pipeline` field. Called internally by every `mt_app_t`
group/subscription mutator (`mt_app_add_group`, `mt_app_clear_groups`,
`mt_app_remove_group_by_index`, the subscription mutators above), and
explicitly by `groups.c`'s in-place-mutation HTTP handlers
(`handle_put_group`, `handle_put_rules`, `handle_create_rule`,
`handle_put_rule`, `handle_delete_rule`), which mutate a live group
in place via `mt_ruleset_group_mut` rather than going through
`mt_app_t`'s own mutators. The group side had exactly the same bug as
the subscription side, so both are fixed together rather than patching
subscriptions alone. Confirmed against Go's own `LoadConfig()` (groups
and subscription rule sets are both built, unenabled, while
`a.enabled` is still false) and `Start()` (a single flat loop over
`ruleSetSnapshot()` — groups+subscriptions together — enables and syncs
everything uniformly) that this mirrors Go's actual startup/mutation
model.

**DNS-matching snapshot extended to include subscriptions**
(`rules/snapshot.c`): a shared `append_snapshot_entry()` helper
(factored out of the existing per-group loop) is now called once per
enabled user group and once per enabled subscription (via a synthesized
`mt_sub_runtime_group()`, discarded immediately after its id/name/rules
are copied into the snapshot entry). Subscription rules of type
`subnet`/`subnet6` need no special-casing: `mt_matcher_add()` already
treats those as `RK_NEVER` (never matches a domain name), the same
semantics Go's own matcher uses, so they flow through the identical
matcher-building code as any group rule.

**`main.c` wiring**: `app_deps.pipeline` now passed through to
`mt_app_create`. `find_ruleset()` (used by the DNS match sink) now
falls back to `mt_app_find_subscription_ruleset_by_id` when a match's
`group_id` isn't a user group. `on_link_up`/`on_addr_change` now also
iterate subscription rulesets via
`mt_app_subscription_ruleset_count`/`_at` (confirmed against Go's
`netlink.go`, where `handleLink`/`handleAddr` both iterate
`ruleSetSnapshot()` — groups and subscriptions together — not just
groups). A new enable+sync loop over the initial subscription rulesets
runs right after the existing group-ruleset loop, before
`mt_app_set_running(true)`, mirroring the same "build unenabled at
config-load time, enable+sync everything in one flat pass at Start()"
sequence Go uses.

**Regression test**: `tests/unit/test_groups.c` gained
`rule_created_via_http_is_dns_matchable` — creates a group with a
domain rule purely via `POST /api/v1/groups`, then hand-builds a
synthetic `mt_dns_msg_t` with a matching A-record answer and calls
`mt_dns_pipeline_handle_message()` directly, asserting the match sink
fires with the created group's id. Before this task's fix this would
have failed (the snapshot handed to the pipeline would still have been
the empty one built from the config file at daemon startup).
`test_rulesnap.c` gained 4 tests covering subscription
inclusion/exclusion (enabled+interface, no interface, disabled, and
both groups+subscriptions participating together).

Verified: full suite (27 unit-test binaries, including the new
regression tests), `static_analysis` (clang-tidy+cppcheck, 0 warnings),
and `sanitize` (ASan+UBSan) all clean; `magitrickled-c` and
`mt-configtool` rebuild cleanly with the new `main.c`/`app.c`/`app.h`
signatures.

## D-33: Subscription sync flows (`mt_app_sync_subscription_by_id`/`mt_app_sync_due_subscriptions`) and the blocking-fetch tradeoff

**Ports `subscriptions.go`'s `SyncSubscriptionByID`/`SyncDueSubscriptions`
onto `mt_app_t`, reusing the Phase 2 parse primitives
(`mt_sub_refresh_rules`/`mt_sub_same_rules`/`mt_sub_is_due`, already
shipped and differentially tested) and the Phase 7 fetch primitive
(`mt_sub_fetch_list`) with the Phase 7 rebuild/rollback machinery
(`rebuild_subscription_rulesets`) from D-32.** `mt_app_sync_subscription_by_id`
mirrors Go field-for-field: `LastCheck`/`URL` update unconditionally on
any successful fetch, `LastUpdate`/`Rules` (and the ruleset rebuild) only
when the refreshed rules actually differ (`mt_sub_same_rules`) — matching
the observation that a URL-only change never needs a ruleset rebuild in
either language, since neither `RuleSet`/Go nor `mt_sub_runtime_group`/C
read the subscription's URL when building matcher state. On a rebuild
failure, every mutated field is rolled back to its pre-sync value and
the rulesets rebuilt again from that (rollback failure logged, not
returned, matching `mt_app_add_subscription`'s established pattern);
`*out_changed` is still set `true` in that branch, faithfully matching
Go's own literal `true` return there ("a change was attempted", even
though it was rolled back). `mt_app_sync_due_subscriptions` mirrors the
two-phase due-scan/apply split (`IsDue` filter, then per-subscription
fetch+refresh, then one shared rebuild+rollback for whatever actually
changed across the whole batch) exactly, including the detail that a
subscription's `LastCheck` still advances even when its content turned
out unchanged (so it won't be immediately re-fetched next tick), while
only an *actual* rules change anywhere in the batch triggers the shared
rebuild.

**No result-copy struct in the API.** Go's `SyncSubscriptionByID` returns
an `app.SubscriptionSyncResult{URL, LastUpdate, Rules}` value because its
caller (the HTTP handler) runs in a different goroutine and needs a
snapshot immune to concurrent mutation. This port's callers are all
on the same single event-loop thread as the sync call itself (see
below), so nothing can mutate `cfg` between `mt_app_sync_subscription_by_id`
returning and the caller's next statement — `mt_app_find_subscription_by_id`
called immediately after a successful sync already IS "target". This
avoids a whole deep-copy/free lifecycle for a value that would be stale
the instant a future worker-thread redesign made the fetch genuinely
concurrent.

**New `MT_ERR_UPSTREAM` error code**, kept deliberately distinct from
`MT_ERR_IO`/`MT_ERR_PROTO`/`MT_ERR_LIMIT` (any of which `mt_sub_fetch_list`
itself can return depending on the failure mode). Go collapses every
fetch failure into a single `app.ErrSubscriptionFetch` sentinel via
`fmt.Errorf("%w: ...")` specifically so the HTTP layer can map it to one
status (502) regardless of cause; `mt_err_t` has no wrapping, so
`mt_app_sync_subscription_by_id` does the same collapsing explicitly —
logs the underlying `mt_sub_fetch_list` error, then returns
`MT_ERR_UPSTREAM` — so a fetch failure can never be confused with a
ruleset-rebuild failure (which reuses whatever `mt_ruleset_enable`/
`_sync`/`MT_ERR_NOMEM` code the rebuild itself produced) by whatever
HTTP status-mapping code consumes this in the next Phase 7 task.

**Known, documented limitation: both sync functions perform a *blocking*
libcurl fetch on the calling thread**, and today that thread is always
the single event-loop thread — `mt_httpd_t`'s handler contract
(`handle_complete_request` in `httpd.c`) builds and writes the HTTP
response synchronously from a stack-local `mt_http_res_t` before
returning, with no support for a handler to defer its response, so an
HTTP-triggered sync (the next Phase 7 task) and the auto-update timer
callback (the task after that) will both stall DNS resolution and HTTP
serving for up to `MT_SUB_FETCH_TIMEOUT_SECONDS` on a slow or hung
upstream. This is a real architectural gap from Go's model (where
`SyncSubscriptionByID`/`SyncDueSubscriptions` block only their own
goroutine, never DNS-serving or other HTTP-request goroutines) and was
already anticipated in D-02 ("a small worker pool ... only for
blocking/slow work: ... libcurl transfers") and D-17 ("when Phase 7
introduces a worker thread for subscription fetch, the snapshot's
publication must be marshalled onto the loop thread via `mt_loop_post`").
Implementing that worker-thread + deferred-HTTP-response redesign was
scoped out of this task given the size of the `httpd.c` refactor it
would require (heap-allocating response state, extending the connection
lifecycle to survive an async completion, handling the connection
closing mid-fetch); it is called out here explicitly, as a documented
divergence, rather than silently shipped as if it matched Go's
non-blocking behavior. This mirrors the precedent already accepted for
iptables fork+exec in Phase 5/6 (also invoked synchronously on the loop
thread, also not yet moved to a worker pool) — subscription fetch simply
makes the same tradeoff far more visible, since a hung fetch can block
for the full 15-second timeout rather than a fork+exec's usual
sub-100ms cost. Left as a candidate revisit for a later phase, not
silently dropped.

Verified with a new `tests/unit/test_sub_sync.c` (9 tests, against a
real local `mt_httpd_t` server acting as the stub subscription-list
host, same background-loop-thread harness as `test_sub_fetch.c`):
first sync fetches and reports changed; a same-content resync reports
unchanged but still advances `last_check`; a different-content resync
(via `url_override`) updates both URL and rules and reports changed;
unknown subscription id is `MT_ERR_NOENT`; an empty URL with no
override is `MT_ERR_INVAL`; a fetch returning non-2xx is
`MT_ERR_UPSTREAM` and leaves the subscription untouched; due-vs-not-due
filtering in the batch sync (interval/last_check arithmetic); a
due-but-content-unchanged batch sync still advances `last_check` without
reporting a change; and a batch sync with nothing due is a no-op. A
forced-rebuild-failure/rollback test was not added at this layer (no
netfilter mock is currently wired through `mt_app_t`'s own tests to
force `rebuild_subscription_rulesets` to fail deterministically) — the
rollback code itself is structurally identical to
`mt_app_add_subscription`/`_replace_subscriptions`'s already-established
pattern from D-32. Full suite (28 unit-test binaries), `static_analysis`
(clang-tidy+cppcheck, 0 warnings), and `sanitize` (ASan+UBSan, 0
findings) all clean.

## D-34: Subscription sync/rules-preview HTTP endpoints (`POST /subscriptions/{id}/sync`, `GET /subscriptions/rules`)

**Wires `mt_app_sync_subscription_by_id` (D-33) and `mt_sub_fetch_list`/
`mt_sub_parse_rules` directly onto the two routes Phase 6/D-28 explicitly
deferred**: `POST /api/v1/subscriptions/{subscriptionID}/sync`
(`SyncSubscription`) and `GET /api/v1/subscriptions/rules?url=`
(`GetSubscriptionRules`, a preview-only endpoint — parses but never
persists). Error mapping mirrors Go's `switch { errors.Is(...) }` in
`SyncSubscription` exactly: `MT_ERR_NOENT`→404, `MT_ERR_INVAL`→400,
`MT_ERR_UPSTREAM`→502 (the new code from D-33, purpose-built for this
mapping), anything else→500. The sync response is built by re-reading
the subscription via `mt_app_find_subscription_by_id` immediately after
a successful sync rather than threading a result struct through the
API (see D-33's rationale — safe because nothing else runs between the
two calls on this single-threaded event loop). `sub_rule_to_json`/
`sub_rules_to_json_array` (already used by the non-fetch subscription
handlers) are reused as-is for both new endpoints' `rules` arrays, since
`SubscriptionRuleRes`'s JSON shape (`id`/`rule`/`type`/`enable`) is
identical for a real subscription rule and a freshly-fetched, not-yet-
saved preview rule.

**The sync endpoint's save condition differs from every other
subscriptions handler's `maybe_save()`**: Go's `SyncSubscription` only
calls `SaveConfig()` when `changed && save != "false"`, whereas
create/delete/put always save on success regardless of any "did
anything actually change" concept (they have none). Reusing the
existing `maybe_save()` helper unconditionally would have broken this
exact gate, so `handle_sync_subscription` inlines the `save`-query-
param check itself, guarded by `changed` from
`mt_app_sync_subscription_by_id`'s out-param.

**Found empirically, not just theoretically: writing
`tests/unit/test_subscriptions_api.c`'s new sync tests hit the exact
blocking-fetch hazard flagged in D-33.** The first attempt put the stub
subscription-list server on the *same* `mt_loop`/thread as the
subscriptions-API server under test (following `test_sub_fetch.c`'s
single-loop pattern). Every sync-endpoint test then hung until the
client socket's own 2-second read timeout: the sync handler blocks that
shared loop thread inside `curl_easy_perform()`, so the same thread that
would need to run `epoll_wait` to accept/service the stub server's
incoming connection from libcurl is busy — the fetch can only ever
un-block via its own `MT_SUB_FETCH_TIMEOUT_SECONDS` timeout, not by
completing. Fixed by giving the stub server its own separate
`mt_loop`/thread (matching `test_sub_fetch.c`'s and `test_sub_sync.c`'s
*actual* client/server thread separation, where the fetch is always
issued from a thread other than the one serving the stub). This is the
same hazard D-33 already named for the real daemon (HTTP handlers and
the future auto-update timer both running on the *one* production event
loop) — this test failure is direct, reproducible evidence of it, not
speculation.

**Differential suite extended** (`tests/differential/http_contract/contract.py`):
a small stdlib `http.server.HTTPServer` stub (`start_stub_sub_server`,
port 18099) now runs for the duration of one `contract.py` invocation,
serving a fixed subscription list reachable identically by the real Go
daemon and `magitrickled-c`. New steps: `GET /subscriptions/rules`
(missing url → 400; a real fetch+parse; a connection-refused → 502) and
`POST /subscriptions/{id}/sync` (unknown id → 404; a real sync via
`url` override; a re-sync of unchanged content). Two new redaction
concerns this introduced, both handled the same way the suite already
redacts learned dynamic IDs and error text (module docstring): (1) the
freshly-fetched rules' random per-backend IDs are learned and redacted
via the existing `learn=` callback mechanism (`<SUBRULE-N>`/
`<PREVIEWRULE-N>` placeholders); (2) `lastUpdate` now genuinely reaches
the live wall-clock time on a real sync (previously always 0, since no
prior step ever triggered an actual sync) — the Go run and the C run
happen minutes apart (separate process spawns), so a literal timestamp
comparison would spuriously fail on real time skew having nothing to do
with behavior. Added a recursive `_redact_live_timestamps` pass
(applied in `Trace._canonical` alongside the existing error-body
redaction) that replaces any non-zero `lastUpdate` value with a stable
placeholder, while leaving a literal `0` alone — so the suite still
verifies the "unsynced vs. synced" transition point, just not the exact
epoch value. Ran `run_http_diff.sh` end-to-end in this sandbox (root +
real iptables available): 44 steps, byte-identical trace, `HTTP
contract: OK`. Also updated the stale Phase-6-era comment explaining why
subscriptions stay `enable:false` in this suite — no longer "the C
port has no subscription-ruleset runtime" (Phase 7/D-32 built that);
now it's "avoid an `eth0`-dependent real-netfilter side effect
unrelated to what this suite checks."

Verified: full suite (28 unit-test binaries, including 7 new HTTP-level
tests in `test_subscriptions_api.c` against a real two-loop harness),
`static_analysis` (0 warnings), `sanitize` (0 findings), and the
extended `run_http_diff.sh` (44/44 steps identical, Go vs C) all clean.

## D-35: Subscription auto-update timer (timerfd, 1-minute tick) and SIGHUP config reload

**Auto-update**: a straightforward port of `StartSubscriptionAutoUpdate`
onto `mt_loop_add_timer`. Go's `time.NewTicker(time.Minute)` preceded by
one immediate `SyncDueSubscriptions` call becomes a single
`mt_loop_add_timer(d.loop, 0, MT_SUBSCRIPTION_AUTO_UPDATE_INTERVAL_MS, ...)`
— `initial_ms=0` fires almost immediately (`loop.c` already special-cases
zero to a 1ns `it_value`, since an all-zero `itimerspec` disarms a
timerfd rather than firing it) and then every 60s after, so one timer
registration reproduces "fire now, then every minute" without a separate
manual bootstrap call. The callback calls `mt_app_sync_due_subscriptions`
and — mirroring Go's `if changed { SaveConfig }` — saves the config file
only when it reports a change, via `mt_app_save_config` (the same
maybe-save shape used by the HTTP subscription handlers, since
`mt_app_sync_due_subscriptions` itself never saves — D-33). Runs on the
same event-loop thread as DNS/HTTP, so it inherits the blocking-fetch
tradeoff already named in D-33/D-34: a due subscription with a slow
upstream stalls the whole daemon for the fetch's duration, once a
minute. `mt_sub_fetch_global_init()`/`_cleanup()` (D-31 had deferred
wiring this) are now called at daemon startup/shutdown — load-bearing
for the first time in Phase 7, since this timer (and the sync HTTP
endpoints, D-34) are the first production code paths that actually
invoke `mt_sub_fetch_list`.

**SIGHUP reload**: replaces the previous "reload not implemented yet"
stub with a real port of Go's `case syscall.SIGHUP: app.LoadConfig()`.
Re-reads the config file into a scratch `mt_config_t` (fresh
`mt_config_init_defaults` + `mt_config_load_file`, exactly like
startup); a missing file is a no-op (matches Go's
`errors.Is(err, os.ErrNotExist) { return nil }`), any other parse error
is logged and also a no-op (nothing is applied) rather than leaving the
live config half-migrated.

Two things are actually applied, deliberately not everything Go's
`LoadConfig` touches:

1. **Groups and subscriptions**, gated on `mt_config_t`'s
   `groups_present`/`subscriptions_present` flags (already tracked since
   Phase 2 for exactly this "was the YAML key present at all" distinction
   — mirrors Go's `cfg.Groups != nil`/`cfg.Subscriptions != nil` checks).
   Groups: `mt_app_clear_groups` (disable + empty) then one
   `mt_app_add_group` per freshly-parsed group, transferring ownership of
   each `mt_group_t*` out of the scratch config as it's consumed (NULLing
   the source slot so the scratch config's later `mt_config_clear` can't
   double-free it) — mirrors `LoadConfig`'s own "disable all, rebuild
   fresh" loop, including stopping at the first `addGroupLocked` failure
   and leaving whatever was already re-added in place (Go doesn't roll
   those back either). Subscriptions: one `mt_app_replace_subscriptions`
   call with the whole freshly-parsed array (ownership of the array and
   every element transferred the same way) — this already IS a wholesale
   replace with its own rollback-on-rebuild-failure (D-32), unlike
   groups, which have no such primitive and are reloaded one at a time
   instead.
2. **The specific handful of app-level settings Go's own runtime code
   re-reads live from `a.config` on every DNS request/record**
   (`dns.go`): `DisableFakePTR`/`DisableDropAAAA` (checked per-message)
   and `Netfilter.IPSet.AdditionalTTL` (added to every matched record's
   TTL). Traced every `a.config.*` read site in `dns.go`/`start.go`
   specifically to answer "what does Go's own reload actually change
   live, versus just update in an already-inert copy" before deciding
   this split — `Host`/`Upstream`/`MaxIdleConns`/`MaxConcurrent`/`Timeout`,
   the netfilter chain/table prefixes, `StartMarkTableIndex`,
   `DisableIPv4`/`DisableIPv6`, the `link` list, and `LogLevel` are all
   read exactly once in `start.go` to construct already-running
   subsystems (the proxy listener, the netfilter helper, the port-53
   remap) — Go's `LoadConfig` updates `a.config`'s in-memory copies of
   these too, but nothing reads them again afterward, so they are no
   more "live" in Go than in this port. New setters
   (`mt_dnsproxy_set_disable_flags`, `mt_dns_pipeline_set_additional_ttl`)
   were added specifically to make the two genuinely-live settings behave
   identically in C — not a shortcut, a deliberate parity fix, verified
   manually (see below). Every other field is left as a documented scope
   boundary rather than silently ignored.

**Verified manually against the real daemon binary** (no dedicated
`main.c` test binary exists; this exercises code paths the unit suite
can't reach): started `magitrickled-c` with an empty config, appended a
group to the config file on disk, sent `SIGHUP`, and confirmed
`GET /api/v1/groups` immediately showed the reloaded group over HTTP;
repeated with a second reload back to empty plus a subscription add,
then a clean `SIGTERM` shutdown -- all under the sanitize build
(ASan+UBSan instrumented `magitrickled-c`), zero findings across two
reload cycles, the auto-update timer's immediate first tick, and
shutdown. Also re-ran the full unit suite (28 binaries), `static_analysis`
(0 warnings), `sanitize` (0 findings), and `run_http_diff.sh` (44/44
steps, Go vs C byte-identical, confirming `mt_sub_fetch_global_init`
wired into startup didn't change any observable behavior) — all clean.

## D-36: End-to-end fault-injection + soak test — a real leak found and fixed

**New tooling**: `tools/bench/run_c_subscription_fault_soak.sh` +
`tools/bench/subscription_fault_stub.py`, migration-plan.md's Phase 7
"end-to-end: full daemon ... stub upstream + stub subscription server
... fault injection" line item. Runs the real `magitrickled-c` binary
against a stub DNS upstream (`dnsstub`, Phase 3/4 tooling, sustained
load via `dnsload`) and a stub subscription-list HTTP server that
deliberately exercises every `mt_sub_fetch_list`/
`mt_app_sync_due_subscriptions` failure mode on a short (4s) auto-update
interval for the whole run — a redirect loop, a non-2xx status, an
oversized body, a connection-refused target — alongside one subscription
whose content genuinely alternates on every fetch (forcing a real
rebuild of the subscription-ruleset array and a DNS-snapshot republish
on roughly every other tick). SIGHUP is sent periodically mid-run to
exercise config reload concurrently with in-flight DNS traffic and due
subscription fetches. All subscriptions use `enable:true` with
`interface:""` — mirrors `mt_sub_is_due`/`mt_sub_runtime_group`'s
actual semantics exactly (`IsDue` only checks Enable/URL/Interval, never
Iface; the synthesized ruleset's own `enable` is forced false whenever
Iface is empty, per `subscriptionAsRuntimeRuleSet`) — so each
subscription is genuinely due for repeated auto-update cycles while its
ruleset never attempts real netfilter work, keeping the soak focused on
the fetch/sync/reload code under test.

**Bounded duration stands in for the plan's 24h host soak**, documented
here rather than silently substituted: this sandbox has no facility for
an unattended 24h run. `DURATION` defaults to 90s, enough for ~20 real
fetch cycles per stub endpoint at the 4s interval above — short in wall
time, not in exercised-code-paths.

**A genuine, previously-undetected memory leak was found and fixed**:
running the soak under the sanitize build (ASan+UBSan) reported
`LeakSanitizer: ... byte(s) leaked` at process exit, traced through
`load_subscription`/`mt_subscription_add_rule` in `yaml_load.c` — but
that was only the *allocation* site LSan reports, not the actual bug
site. Isolated with a series of standalone repros (loading the same
saved config file repeatedly via `mt_config_load_file` alone: clean;
adding `mt_app_replace_subscriptions` cycles without a real sync: clean;
adding a real `mt_app_sync_due_subscriptions` call with actual content
changes between reload cycles: **leaked, deterministically, every
time**) down to `mt_app_sync_due_subscriptions`'s success path in
`app.c`: when a due subscription's rules actually changed, the loop
correctly saves the subscription's *old* rules pointer into a per-
subscription `rollback_entry_t` (for the failure/rollback path, which
already frees them correctly) before overwriting `sub->rules` with the
newly-fetched array — but the **success** path only ever did
`free(rollback)`, freeing the bookkeeping array itself while never
freeing `rollback[i].rules` (the now-superseded old array) for any
subscription whose rules had actually changed. Every real "auto-update
found new content" cycle silently leaked one `mt_sub_rule_t` array (plus
each entry's `strdup`'d `rule`/`type` strings) forever. Notably,
`mt_app_sync_subscription_by_id` (the single-ID sibling function, also
task #42) already got this right — it calls
`free_sub_rule_array(prev_rules, prev_n_rules)` on its own success path
— so this was a narrow, single-function omission, not a
systemic pattern. Fixed by adding the equivalent free loop over every
`rollback[i]` with `rules_replaced == true` right before
`mt_app_sync_due_subscriptions`'s final `free(rollback)`.

This is exactly the class of bug integration/fault-injection testing at
this stage is meant to catch: it required (a) a real repeated content
change for the same subscription across (b) more than one due-sync
cycle, a combination no existing unit test happened to drive twice in a
row for the *batch* sync path specifically (`due_but_unchanged_...`
tests a same-content resync; `due_subscriptions_are_fetched_...` tests
one cycle) — plain code review of the (structurally reasonable-looking)
rollback bookkeeping did not surface it either. Added a regression test,
`tests/unit/test_sub_sync.c`'s `due_subscriptions_repeated_content_changes_leak_nothing`:
a toggling stub endpoint drives `mt_app_sync_due_subscriptions` through
two real content-change cycles for one subscription. It always passed
functionally (the bug only orphaned memory, never corrupted the live
subscription state) — its value is purely as `make sanitize` coverage,
verified by confirming it caught the leak before the fix (via the same
standalone repro) and passes clean after.

**Also confirmed, and deliberately left alone: a pre-existing Go bug,
faithfully reproduced.** Early soak runs (before pinning a real
`MT_VERSION` for the test) showed every `SIGHUP` reload failing with
`MT_ERR_STATE` ("config unsupported version") after the very first
auto-update save. Root cause: `mt_app_save_config`/`mt_app_sync_due_subscriptions`'s
save-on-change writes `configVersion: <MT_VERSION>`, and `MT_VERSION`
defaults to `"unattached"` in this Makefile with no override anywhere in
this repo's CI (`.github/workflows/*.yml`, `config/*/*`) — so any real
build of this daemon would write a `configVersion` that its own
`mt_config_load_file`'s `strncmp(version, "0.", 2) != 0` check (a
faithful port of Go's `strings.HasPrefix(cfg.ConfigVersion, "0.")`,
`config.go`) then rejects on the next load. Confirmed this is **not** a
C-vs-Go divergence: Go's own `constant.Version = "unattached"` (same
default, same no-CI-override situation) plus its byte-identical
`HasPrefix` check means the *real* Go daemon would hit the exact same
self-inflicted failure the moment any code path calls `SaveConfig()`
with an unversioned build — `SyncDueSubscriptions` calling `SaveConfig()`
on a changed subscription is exactly such a path. Per the master spec
("port Go's behavior, bugs included, unless told otherwise" — never fix
unrelated Go defects silently as a side effect of the C port), this was
left unmodified in both the sync/save code and the version-check code;
the soak test itself was simply built with `MT_VERSION=0.7.0` (matching
the version string already used elsewhere in this repo's differential
scratch configs) so this out-of-scope, pre-existing defect doesn't mask
verification of the actual Phase 7 reload/fault-injection logic. Not
silently patched, not silently ignored — documented here as a real,
verified, out-of-scope finding.

**A minor startup race, found and fixed in the test script itself, not
the daemon**: the first soak-script draft started the stub subscription
server and the real daemon back-to-back with only a fixed `sleep 0.3`
in between. Since the daemon's very first auto-update tick fires almost
immediately at startup (`mt_loop_add_timer`'s `initial_ms=0`), a slow
stub-server bind occasionally lost the race, showing up as spurious
`i/o error` fetch failures on cycle 0 only (harmless -- self-corrected
on the next tick -- but a misleading signal in the summary counts).
Fixed with an explicit `curl`-based readiness poll against the stub
server (matching `run_http_diff.sh`'s `wait_for_port` pattern) before
starting the daemon, instead of the fixed sleep.

Verified: the fixed `magitrickled-c` (both the optimized build and the
ASan+UBSan sanitize build) ran the full fault-injection soak cleanly —
daemon alive throughout, multiple real SIGHUP reloads under concurrent
DNS load and due-subscription fetches, sustained DNS load at ~20-30k
rps, clean `SIGTERM` shutdown, **zero LeakSanitizer/ASan/UBSan findings**
(confirmed on a 90s run with 4 SIGHUP reloads and ~20+ real subscription
sync cycles including repeated content changes — the exact scenario
that leaked before the fix). Also re-ran the full unit suite (28
binaries, including the new regression test in `test_sub_sync.c`),
`static_analysis` (0 warnings), `sanitize` (0 findings), and
`run_http_diff.sh` (44/44 steps Go vs C byte-identical) — all clean
after the fix.

## D-37: `build_backend` gains a real C path — `BACKEND` switch, not a project-wide cutover

**A new `BACKEND` variable (`go` default, `c` opt-in) drives the root
Makefile's `build_backend`**, mirrored in `BACKEND_SOURCES`/
`BACKEND_DEPENDENCIES`/`BACKEND_BUILD_PROPERTIES` so the existing
stamp-based incremental build correctly invalidates on either a Go or a
C source-tree change (not both, and not neither) and on any `BACKEND`/
`C_CROSS_COMPILE`/`C_SYSROOT` change. `BACKEND=c` invokes `src/backend-c`'s
own Makefile with `BUILD=$(UNIQUE_NAME)` (so per-target build artifacts
stay isolated exactly like the existing `.build/$(PLATFORM)_$(TARGET)/`
scheme) and copies its `magitrickled-c` output to the same
`$(COMPILE_DIR)/magitrickled` path the Go build already produces —
`prepare_files`/packaging needed **zero** changes to consume either
backend's output, confirmed by running both paths back to back on host.

**Deliberately NOT a project-wide cutover.** `BACKEND` defaults to `go`,
so every existing `config/*/*.config` target — and CI's entire build
matrix — keeps building exactly as before. Migration-plan.md's Phase 8
exit criterion ("`make build_backend` switches to C for all 40 targets")
requires a real cross-toolchain + sysroot (providing libyaml, cJSON,
PCRE2, libmnl, libcurl built for that target's libc) for every one of the
40 `config/*/*.config` targets — infrastructure this sandbox cannot
provision: `downloads.openwrt.org` is blocked by the session's egress
policy (confirmed via the agent-proxy status endpoint: a genuine 403
policy denial, not a transient failure — per the proxy's own guidance,
not retried or routed around), and no Entware toolchain image registry
is reachable either. Flipping the *default* to `c` without real per-target
sysroots would silently break the 40-target CI matrix the moment this
lands — the opposite of the spec's "components move one at a time behind
contracts." Two new variables, `C_CROSS_COMPILE`/`C_SYSROOT`, make this a
per-target, explicit opt-in instead: a target only builds via C once
someone supplies its real toolchain prefix and a sysroot with the 5 libs
— exactly the "flip Recipe" the plan anticipated, just not exercisable
end-to-end for the full matrix from inside this sandbox.

**`src/backend-c/Makefile`'s cross-build path no longer restricts itself
to the dependency-free core.** Previously `ifneq ($(CROSS_COMPILE),) all:
core` (i.e. only the 5-dep-free subset) built under cross-compilation at
all, explicitly deferred to "Phase 8 provides the feed sysroots." Now
`all` always targets the full daemon + tools, matching the host build —
a `CROSS_COMPILE`+`SYSROOT` invocation either produces the complete
`magitrickled-c` (given a sysroot with the 5 deps) or fails at the exact
missing-header boundary, rather than silently succeeding with a partial,
non-shippable core-only build. `core` remains available as an explicit
opt-in for anyone who genuinely only wants the dependency-free subset
(e.g. an early sysroot-less smoke build).

**Verified the mechanism, not the full matrix**: `make BACKEND=c
build_backend` on host (no `C_CROSS_COMPILE`) produced a correct
host-native `magitrickled-c` at `$(COMPILE_DIR)/magitrickled`; `make
BACKEND=c C_CROSS_COMPILE=mipsel-linux-gnu- build_backend` (a real cross
compiler available in this sandbox, though not the exact Entware
toolchain) correctly cross-compiled every object file with
`mipsel-linux-gnu-gcc` and failed at exactly the expected point — a
missing `curl/curl.h`, since no sysroot was supplied — proving the
`CROSS_COMPILE`/`SYSROOT` plumbing threads correctly from the root
Makefile through to `src/backend-c`'s `CC`/`--sysroot` flags. Re-ran the
default (`BACKEND=go`, unset) path end to end afterward, including the
UPX step, to confirm zero regression to the existing production build.
Full sysroot-backed cross validation is task-scoped separately (see the
next decision entry).

## D-38: Fixing real GitHub Actions CI failures (host build, mipsel cross_build, frontend unit tests)

**A live CI run on `check-c.yml`/`check.yml` (GitHub Actions, not this
sandbox) surfaced three failures**, none of them hypothetical, requiring
targeted fixes rather than another docs-only note:

1. **`check-c.yml`'s `build_test` job failed with `libmnl/libmnl.h: No
   such file or directory`.** Its "Install tooling" step only ever
   installed `libpcre2-dev libyaml-dev` — never updated when Phase 5
   added `libmnl` (ipset via netlink), Phase 6 added `cJSON`, or Phase 7
   added `libcurl`. This was a **pre-existing gap that predates Phase 8**,
   not something D-37 introduced — the host build has linked against all
   5 feed deps since Phase 7 at the latest, and the CI tooling-install
   step was simply never kept in sync. Fixed by adding `libmnl-dev
   libcjson-dev libcurl4-openssl-dev` to both the `build_test` job's
   "Install tooling" step and the `differential` job's "Install
   libraries" step (the latter's `run_diff.sh` also builds the full
   daemon to run the HTTP contract suite against it).

2. **`check-c.yml`'s `cross_build` job (mipsel-linux-gnu, no sysroot)
   failed to compile at all — a real regression from D-37.** D-37's
   `all` target change made cross builds always attempt the full daemon
   regardless of whether a `SYSROOT` was supplied, so the long-standing
   `cross_build` job — which only ever installs `gcc-mipsel-linux-gnu`,
   by design, to prove a dependency-free skeleton cross-compiles — now
   failed at the first missing feed header (`libmnl/libmnl.h`, same as
   (1)) instead of succeeding as it always had. Fixed by re-conditioning
   `all`: `CROSS_COMPILE` set **and** `SYSROOT` empty stays core-only
   (restoring the pre-Phase-8 behavior this job depends on); `SYSROOT`
   supplied (or no `CROSS_COMPILE` at all, i.e. host) still builds the
   full daemon, preserving D-37's actual point — a per-target opt-in via
   the root Makefile's `BACKEND=c`/`C_CROSS_COMPILE`/`C_SYSROOT`.
   D-37's rationale comment ("all always targets the full daemon,
   matching the host build") was simply wrong about what "always" could
   safely mean here; corrected in the Makefile's own comment too.

   While re-verifying this job end to end, found a **second,
   independent, longer-standing bug**: the job's own final assertion,
   `file build/cross-mipsel-linux-gnu/magitrickled-c | grep -q MIPS`, has
   named a binary that a core-only cross build has never produced —
   `core: $(CORE_OBJS)` compiles objects only, no link step — since
   Phase 3 introduced `CONFIG_SRCS` (the point at which "core" stopped
   being the whole daemon). This predates D-37 by five phases and was
   never exercised as a real pass/fail signal in this sandbox (git log
   shows `check-c.yml` untouched since Phase 3). Fixed properly rather
   than patched around: `mt-dnstool`/`mt-cachetool` link against
   `$(CORE_OBJS)` only (no `DEP_LIBS`), so they are real, fully linked,
   dependency-free binaries a sysroot-less cross build **can** produce.
   The core-only `all` branch now also links these two; the CI assertion
   now checks `mt-dnstool` (confirmed `ELF 32-bit LSB executable, MIPS,
   MIPS32 rel2 ... for GNU/Linux`) instead of the unreachable
   `magitrickled-c`.

   Linking `mt-dnstool` for mipsel then exposed a **third, genuinely new**
   issue: `undefined reference to __atomic_fetch_add_8`/`__atomic_load_8`
   from `src/dns/proxy.c`, which uses 8-byte C11 atomics for
   counters/deadlines. 32-bit targets (mipsel, arm, ...) lack a native
   64-bit atomic instruction, so gcc lowers those built-ins to libatomic
   calls that the linker won't pull in implicitly. Fixed by adding
   `-latomic` to `LDLIBS` unconditionally — confirmed present for both
   the host toolchain and every cross toolchain available in this sandbox
   (`gcc-aarch64-linux-gnu`, `gcc-arm-linux-gnueabihf`,
   `gcc-mipsel-linux-gnu`, `gcc-riscv64-linux-gnu`), and harmless on
   64-bit hosts where it's linked but unused. This is exactly the kind of
   thing Phase 9's real-toolchain validation needs to re-confirm for the
   actual Entware/OpenWrt musl/glibc toolchains, since libatomic
   packaging varies more across embedded toolchains than it does across
   Ubuntu's cross packages.

3. **`check.yml`'s `test_frontend` job failed before running a single
   test**: `deno.json` deserialization error, `invalid type: string
   "auto", expected a boolean`, for the `nodeModulesDir` field. This file
   is untouched by the C rewrite (last changed March 2026, well before
   Phase 0) — a pre-existing frontend/CI drift, not a rewrite-caused
   regression, but still blocking the CI this task was asked to fix.
   `nodeModulesDir: "auto"` requires a Deno version new enough to parse
   the string-enum form; whatever `denoland/setup-deno@v1` resolves
   `deno-version: v1.x` to in the live CI environment does not. Rather
   than chase which exact Deno version the action currently resolves (not
   verifiable from this sandbox — outbound access to Deno's/GitHub's
   release assets is blocked by the same egress policy as D-37's
   OpenWrt-download block), fixed the config itself: `nodeModulesDir:
   true` is the boolean form of the same "auto-manage a local
   `node_modules`" behavior and has always been accepted, so it parses
   correctly regardless of which Deno 1.x patch is actually installed.

   **Follow-up, same job, next real CI run**: past config parsing, `deno
   test` then failed with `Module not found ".../tests/mocks/
   setup-svelte-runes". Maybe add a '.ts' extension or run with
   --unstable-sloppy-imports` — `change-tracker.test.ts` and
   `groups-store-mutations.test.ts` import
   `"../mocks/setup-svelte-runes"` with no extension, and three more unit
   test files import from `src/` the same way. This is an established,
   deliberate convention here, not an oversight: `deno.json`'s lint
   config already excludes the `no-sloppy-imports` rule repo-wide (i.e.
   someone already turned off the *lint warning* for exactly this
   pattern), it just never turned on the matching *runtime* resolution
   behavior, and nothing had exercised `deno test` far enough to notice
   before this task's fixes got past the `nodeModulesDir` failure.
   Fixed by adding `--unstable-sloppy-imports` to the `test:unit` script
   in `package.json` (the flag Deno's own error message names) rather
   than editing `deno.json`'s `unstable` array, since the CLI flag has an
   unambiguous 1:1 meaning across Deno versions and avoids relying on
   this sandbox being able to confirm the exact accepted string for that
   array (same unverifiable-Deno-version situation as above). Confirmed
   every extensionless import in `tests/unit/` and `tests/mocks/`
   resolves to a real, existing `.ts` file, so sloppy-imports resolution
   has a legitimate target in every case, not just the one the CI log
   happened to hit first.

**Verification**: host build (`make CFLAGS_EXTRA=-Werror`), `make test`
(28/28 binaries pass), `make sanitize` (0 ASan/UBSan findings), `make
static_analysis` (0 warnings), and `tests/differential/run_diff.sh`
(all suites OK, including the 44-step HTTP contract) all re-run clean
after these fixes. The mipsel `cross_build` job's exact commands
reproduced locally end to end: compiles, links `mt-dnstool`, and `file`
confirms real MIPS object code. `deno.json`/`package.json` re-validated
as parseable JSON, and every extensionless test import was confirmed to
resolve to a real file by hand (see above). Frontend Deno test
*execution* itself (beyond config/import resolution) and Playwright e2e
were not re-run in this sandbox — no Deno binary reachable here, the
same egress-policy constraint noted throughout this entry and D-37 — so
these two fixes address the exact two failures the live CI log showed,
in the order CI hit them, but a third, later failure in the same job
remains possible and unverified until the next live run.

## D-39: Packaging — ipk/apk Depends for the C backend (task #48)

**`prepare_files` needed zero changes.** It already copies whatever
landed at `$(COMPILE_DIR)/magitrickled` (D-37 made both backends produce
exactly that path), and copies the frontend `dist/` and `files/{common,
entware,entware_kn,openwrt}` the same way regardless of `BACKEND` — so
the only real packaging gap was that `Depends:`/apk `depends:` never
listed the 5 libraries the C binary links against (libyaml, cJSON,
PCRE2, libmnl, libcurl) but the Go binary doesn't need.

**Two new root-Makefile variables, `C_DEPS_IPK` (comma-separated, for
both ipk `Depends:` branches) and `C_DEPS_APK` (space-separated, for
apk's `-I "depends:..."`)**, both listed next to `C_CROSS_COMPILE`/
`C_SYSROOT`. Package **names** (`libyaml`, `libpcre2`, `libmnl`,
`libcurl`, `libcjson`) are this project's best-effort reading of each
feed's lib-prefixed, unversioned naming convention (matching the style
of the existing `iptables-nft`/`kmod-ipt-*` entries already in this
file) — **not verified against a live Entware or OpenWrt feed index**
from this sandbox, same egress block as D-37/D-38's OpenWrt-download
denial. Flagged in both the Makefile comment and here; must be confirmed
against a real feed index (or `opkg`/`apk` search on a real device)
before a `BACKEND=c` package ships to users.

**`package_ipk`'s Entware branch** appends `, $(C_DEPS_IPK)` to its
existing shell-built `$DEPS` when `BACKEND=c` (same pattern already used
there for the `_kn`→`socat` conditional). **The OpenWrt branch** was a
single static `echo` before this — converted to the same shell-variable
style so it could gain the same conditional without duplicating the
whole dependency list per backend. **`package_apk`'s `-I "depends:..."`**
uses a `$(if $(filter c,$(BACKEND)),...)` Make-level conditional instead
(no per-target shell logic needed there), appending `$(C_DEPS_APK)`.

**Verified by building real packages, not just reading the diff**: ran
`make package_ipk` for both an Entware target (`mipsel-3.4_kn`) and an
OpenWrt target (`aarch64_cortex-a53`), each under `BACKEND=go` (default)
and `BACKEND=c`, and inspected the extracted `control` file from the
resulting `.ipk` in every case — `BACKEND=go` Depends lines are
byte-identical to before this change (zero regression); `BACKEND=c`
correctly appends the 5 new deps in each platform's existing format
(comma-separated). `apk mkpkg` itself isn't installed in this sandbox,
so `package_apk` was checked with `make -n` (recipe expansion only,
which is where the `$(if ...)` conditional resolves) for both `BACKEND`
values — `BACKEND=go`'s `-I "depends:..."` line is unchanged from
before; `BACKEND=c`'s correctly appends `libyaml libpcre2 libmnl libcurl
libcjson`.

## D-40: Cross-compile validation with available toolchains (task #49) — real full-daemon builds on 3 architectures, run under QEMU

**Went further than "prove the plumbing"** (D-37 already did that: `mipsel-linux-gnu-` reaching the expected missing-`curl.h` failure with no deps supplied). This task's goal was to prove that once the 5 feed deps genuinely **are** resolvable for a real target architecture, `src/backend-c`'s build produces a correct, running `magitrickled-c` — not just an object file that compiles.

**Method — real Debian/Ubuntu multiarch packages of the 5 libs, not stubs.** `downloads.openwrt.org` and any Entware toolchain/feed mirror remain blocked (same confirmed 403 policy denial as D-37/D-38). `ports.ubuntu.com` (Ubuntu's official secondary-architecture archive — arm64/armhf/riscv64/ppc64el/s390x/i386, **no mips/mipsel**, dropped since ~18.04) **is** reachable. For each of arm64, armhf, and riscv64: `dpkg --add-architecture <arch>` + a `ports.ubuntu.com` source scoped to `Architectures: arm64 armhf riscv64` (added to `/etc/apt/sources.list.d/`, a sandbox-local change, not committed) let `apt-get install libyaml-dev:<arch> libpcre2-dev:<arch> libmnl-dev:<arch> libcjson-dev:<arch>` install cleanly via Debian's multiarch mechanism (shared `/usr/include`, arch-specific `/usr/lib/<triplet>/`) — these are the actual Debian/Ubuntu builds of the same 4 libraries the real feeds ship, not test doubles.

**`libcurl4-openssl-dev:<arch>` doesn't install via dpkg at all**: it conflicts with the host's own already-installed amd64 `libcurl4-openssl-dev` on a non-multiarch-safe shared path (`/usr/bin/curl-config`, byte-different between architectures, and dpkg refuses to let two package instances of the same name own one non-diverted path). Worked around by `apt-get download` + `dpkg -x` (unpack without registering) into a scratch directory for the headers/`.a`, then completing the runtime `.so` symlink chain from the separately-installable (non-conflicting) `libcurl4t64:<arch>` runtime package. This is a sandbox-specific workaround for a packaging quirk, not a divergence in the shipped project — it produces byte-identical headers/libs to what `apt` would have installed had the conflict not existed.

**`--sysroot` doesn't fit how these particular cross-toolchains work.** Ubuntu's `gcc-{aarch64,arm,riscv64}-linux-gnu` packages are built for the host's own multiarch layout (`/usr/include` shared, `/usr/lib/<triplet>/` per-arch) and resolve against it **by default, with no `--sysroot` needed** — confirmed by a minimal 5-header/5-lib smoke test that linked clean with zero extra flags beyond `-I`/`-L` for the one library (`curl`) the dpkg conflict kept out of the standard multiarch path. Passing `SYSROOT=` (i.e. `--sysroot=`) instead, as a real OpenWrt/Entware SDK toolchain would expect, redirects **all** default search paths under that root — tried this too, and it broke resolution of libcurl's transitive shared-library dependencies (OpenSSL, libssh, libnghttp2, libldap, libgssapi_krb5, libpsl, librtmp — none of which had been copied into the assembled sysroot), producing dozens of `undefined reference` errors at link time, not a missing-file failure. So this validation used `src/backend-c/Makefile`'s existing `CFLAGS_EXTRA`/`LDFLAGS` extension points (already used by CI for `-Werror`) rather than `SYSROOT`, which is the accurate way to drive *these* toolchains — but it is **not** the same invocation shape a real OpenWrt SDK (`--sysroot`-rooted, self-contained `staging_dir`) or Entware toolchain would need. `SYSROOT`'s plumbing correctness itself was already proven separately in D-37 (threads through to `--sysroot=` correctly, fails at the expected point with no deps supplied) — that remains the state of the art for the `--sysroot` shape specifically; a full daemon build through a genuine `--sysroot` tree with all transitive deps present is still unverified.

**Results — full daemon (`magitrickled-c`) built, linked, and ran under user-mode QEMU for all 3 architectures attempted:**

| Arch | Toolchain | Full daemon build | `--help` under QEMU | Unit tests run under QEMU |
|---|---|---|---|---|
| `aarch64` (arm64) | `aarch64-linux-gnu-gcc` (Ubuntu, glibc) | ✅ links clean | ✅ starts, logs, reaches DNS-proxy-init code path | ✅ 8 binaries / 86 assertions, all pass (`test_yamlio`, `test_match`, `test_dnswire`, `test_dns_cache`, `test_ipset`, `test_iptables`, `test_jwt`, `test_crypt`) |
| `armhf` (arm, EABI5) | `arm-linux-gnueabihf-gcc` (Ubuntu, glibc) | ✅ links clean | ✅ same as above | not run (time-boxed; build+link+daemon-start already exercises the 32-bit ARM ABI + `-latomic` path D-38 fixed) |
| `riscv64` | `riscv64-linux-gnu-gcc` (Ubuntu, glibc) | ✅ links clean | ✅ same as above | ✅ 3 binaries / 29 assertions pass (`test_yamlio`, `test_match`, `test_jwt`) |

All three: real ARM64/ARM32/RISC-V64 machine code (confirmed via `file`), dynamically linked against the real cross-arch `.so`s, executed by `qemu-{aarch64,arm,riscv64}` (installed via `qemu-user-static`/`qemu-user`) — not skipped, not stubbed. The daemon's `--help`/startup path logs correctly and reaches `failed to start DNS proxy: system error` (expected — no config file, no network capability inside the emulated process), i.e. real initialization code executes correctly on foreign-architecture machine code, not just "the linker didn't complain."

**What this does and doesn't prove.** Entware is glibc-based (confirmed in `toolchains.md`'s Phase 0 audit), so the `arm64`/`armhf` results are a reasonably close proxy for Entware's `aarch64-3.10`/`aarch64-3.10_kn`/`armv7-3.2` targets specifically — same libc family, same general ABI shape — though the exact glibc version, kernel minimum, and Entware's own library builds remain unverified. **Every OpenWrt target is musl**, not glibc — the `riscv64`/`arm64`/`armhf` results validate the ISA/ABI/instruction-generation path (real code, real relocations, real calling convention) but say nothing about musl-specific behavior (allocator, threading primitives, locale/DNS resolver internals all differ from glibc). No claim is made that any OpenWrt target is validated beyond that ISA level.

**Coverage of the real 40 `config/*/*.config` targets** (build-matrix policy from `toolchains.md`: no silent drops, every target gets a concrete status):

- **Validated (glibc proxy, ISA+full-daemon-link+run level)**: `entware/aarch64-3.10`, `entware/aarch64-3.10_kn`, `entware/armv7-3.2` (via arm64/armhf); `openwrt/riscv64_generic`, all 4 `openwrt/aarch64_*`, all 11 `openwrt/arm_*` (via arm64/armhf/riscv64 ISA-level proxy only — musl-specific behavior unverified per above).
- **Validated (skeleton/core-only, unchanged from D-37/D-38)**: `entware/mipsel-3.4`, `entware/mipsel-3.4_kn` — Ubuntu dropped MIPS architecture support (no `ports.ubuntu.com` packages for mips/mipsel), so the 5 libs can't be obtained the same way; building them from source with `mipsel-linux-gnu-gcc` was judged out of scope for this pass.
- **Completely unverified, no attempt this pass**: `entware/mips-3.4`, `entware/mips-3.4_kn` (big-endian MIPS, no toolchain available at all in this sandbox); `openwrt/mips64_*` (2), `openwrt/mips64el_*` (1), `openwrt/mips_*` (3), `openwrt/mipsel_*` (4) — same MIPS-toolchain gap; `openwrt/loongarch64_generic` — no cross-toolchain available via `apt`; `openwrt/i386_*` (2) — multilib/32-bit x86 not attempted, though likely low-risk given `openwrt/x86_64` and the host's own native `amd64` builds already exercise the same ISA family; `openwrt/x86_64` itself — never cross-compiled specifically, but the *host* `BACKEND=c` build (exercised continuously throughout Phases 1-8) already covers this ISA and ABI natively (glibc vs musl caveat still applies).
- Real on-device or emulated-image install/upgrade verification (this phase's stated exit criterion: "install/upgrade verified on real Entware + OpenWrt devices, or emulated images") was not attempted — no real device or OpenWrt/Entware disk image was available in this sandbox; that gap is unchanged from every prior phase's report.

**Verification performed**: for each of the 3 architectures, built the full `magitrickled-c` (plus `mt-configtool`/test binaries where attempted) directly via `src/backend-c/Makefile`, confirmed real target machine code via `file`, and executed the binaries under the matching `qemu-user` interpreter with `LD_LIBRARY_PATH` pointed at the assembled library directory. No source or Makefile changes were needed for this task — it exercises the mechanism D-37 already built. Sandbox-local apt/dpkg state (foreign architectures, `ports.ubuntu.com` source, downloaded `.deb`s) is not part of the repository and was left in place only for the duration of this validation.

## D-41: UPX keep/drop for the C backend — measured, not assumed (task #51)

**Measured, not just followed the a-priori prediction.** `toolchains.md`'s Phase 0 note already expected "drop UPX" for the C port; this task backs that with real numbers rather than treating the prediction as sufficient on its own (per the master spec's "performance claims only with numbers attached").

**Setup**: host `x86_64` `magitrickled-c` (266,520 bytes) vs the same binary run through `upx -9 --lzma` (98,652 bytes, 37.01% ratio — the same flags the Go path already uses). Both variants exec identically far into the same startup path (config-defaults log line → DNS proxy init → early exit in this sandbox's environment) — a real, non-trivial amount of initialization work (arg parsing, logging setup, YAML config-defaults construction, socket/epoll setup attempts), not a no-op.

**Results**: wall-clock startup (30-run averages, two independent batches) — **13.4 ms plain vs 21.6 ms UPX-compressed, a consistent ~8 ms / ~62% latency tax on every single process start**, from LZMA decompression happening unconditionally at `exec()`. Peak RSS (`/usr/bin/time -v`, 10-run averages) — **10.84 MB plain vs 10.83 MB UPX**, statistically indistinguishable. At this binary size (hundreds of KB — the C rewrite's whole point, vs Go's ~10 MB static binary), RSS is dominated by the dynamically-linked shared libraries the binary pulls in (glibc, libyaml, PCRE2, libmnl, cJSON, libcurl and libcurl's own TLS/auth chain), not by the executable's own pages — so UPX's classically-cited "whole image resident, non-shareable" RAM cost is real in principle but doesn't move the needle at this size, while the decompression latency cost is unconditional and un-amortized (paid on every restart, not just once).

**Decision: drop UPX for the C backend.** No net benefit (168 KB of on-disk savings, irrelevant next to a package already dominated by the frontend's `dist/` assets) against a real, repeatable latency cost with zero corresponding RSS win. **No Makefile change was required to act on this** — the root `Makefile`'s `build-backend-$(UNIQUE_NAME)` recipe (introduced in D-37) only invokes `upx` inside the `BACKEND=go` branch; the `BACKEND=c` branch has never called it. This task's job was to confirm that already-correct state with numbers, not to change behavior.

## D-42: Upgrade/downgrade continuity, host-level (task #50) — a real cross-backend round trip, and a real version-string gotcha it surfaced

**No real router available, so this simulates at the state/config layer on host**, exactly as the task scoped it: both binaries pointed at the *same real* `/var/lib/magitrickle/{config.yaml,auth_secret}` (the actual production paths both backends default to — confirmed identical in D-37's `MT_CONFIG_PATH`/`MT_APP_STATE_DIR` review), with a real Go-daemon-stop → C-daemon-start → C-daemon-stop → Go-daemon-start cycle (upgrade then downgrade), driven entirely through each daemon's real HTTP API — not a mocked or unit-level check.

**Sequence and results (all real, root, this sandbox's real kernel netfilter):**
1. Go starts fresh (hand-written `config.yaml`, one group, `auth_secret` pre-seeded with a random 32-byte secret) — clean startup, `GET /api/v1/groups` returns the group correctly.
2. Go stopped (`SIGTERM`, clean exit) — `config.yaml`/`auth_secret` byte-identical to before start (Go never rewrites config on a clean shutdown with no mutations, as expected).
3. **C started on Go's on-disk state** — clean startup, `GET /api/v1/groups` returns byte-identical JSON to what Go returned, and `auth_secret` is untouched (`diff` clean) — C's `mt_auth_load_or_create_secret`-equivalent path correctly loads the existing secret rather than regenerating it, mirroring Go's `loadOrCreateSecret`'s load-if-present semantics from `secret.go`.
4. **Mutated the group through C's own HTTP API** (`PUT /api/v1/groups/{id}?save=true`, renaming it) — persisted to disk correctly. (First attempt without `?save=true` correctly did *not* persist — this is the documented draft-vs-save API contract from D-34, not a bug; confirmed by re-reading `groups.c`'s `maybe_save()`, gated on the `save` query param exactly like the Go handlers it mirrors.)
5. C stopped cleanly (`SIGTERM`).
6. **Go restarted on C's on-disk state** — clean startup, logged `added group ... name=RenamedByC` (the name C wrote), `GET /api/v1/groups` confirms it, and `auth_secret` is *still* byte-identical to the value from step 1 — full round trip, zero state loss, zero unwanted regeneration, in either direction.

**A real gotcha found and diagnosed, not a new bug — a live reproduction of D-36's already-documented quirk.** The first attempt at this sequence used binaries built the way every other dev/test build in this branch has been built: no real `MT_VERSION`/`PKG_VERSION` injected, so both defaulted to the literal string `"unattached"`. That round-tripped fine Go→C, but **C's config-save path wrote `configVersion: unattached`** (correctly — it saves whatever version string the binary was built with, exactly like Go's `SaveConfig` writes `constant.Version`), and Go's *own* loader then refused to read that file back (`config.go`: `if !strings.HasPrefix(cfg.ConfigVersion, "0.") { return ErrConfigUnsupportedVersion }`) — the identical pre-existing Go quirk D-36 already found and deliberately left alone (real production builds always inject a real `0.x.y`-shaped version via the root Makefile's `PKG_VERSION`, so `"unattached"` never reaches a real package; it's a dev-build-only artifact of testing without that injection). This is the first time it's been reproduced via an actual binary-swap upgrade/downgrade, rather than the auto-update-reload path D-36 found it on — same root cause, same "not fixing it" call, now confirmed to matter in exactly the scenario this task is about. **Rebuilt both binaries with a real `MT_VERSION=0.99.0`/`-ldflags -X constant.Version=0.99.0` for the actual test** (matching the differential fixtures' own `0.99.0`/`0.7.0` convention) — the whole sequence above is the *clean*, representative result, run after that fix. Anyone building/testing this exact flow needs to remember to inject a real version string, exactly as production packaging already does and dev/CI builds today do not.

**What could not be tested here: netfilter-state (iptables chains + ipset) cleanup/rebuild continuity across the backend swap.** This sandbox's `ipset` is non-functional at the kernel level — `ipset create test hash:ip` fails with `Kernel error received: Invalid argument` even as root, and there's no `modprobe` available to attempt loading `ip_set` (the binary doesn't exist in this container). This blocks *either* backend from successfully enabling a group at all (both backends' enable-path calls ipset init before touching iptables, so nothing gets left behind to test cleanup against) — not a Go-vs-C difference, a sandbox environment gap in the same category Phase 5's own report already hedged ("netns integration tests ... in CI where kernel allows"). What *is* covered, at the code level, for this exact scenario: Phase 5's iptables engine has full transcript-parity differential tests against the real Go fake-executable corpus (chain patch/override/delete semantics identical byte-for-byte), the `netfilter/cleaner.c` startup-cleanup module was purpose-built for "stale state from a prior run" (exactly the shape of an upgrade/downgrade transition, just not specifically tested as "prior run was the *other* implementation"), and both backends use identical `MT_`/`mt_` chain/ipset naming conventions by construction (confirmed in this task's own test config and every prior phase's fixtures) — so the cleaner's logic has no way to distinguish "stale state from my own prior run" from "stale state from the other backend's prior run." That said, this specific live cross-backend netfilter transition was not exercised end-to-end here, and remains a gap for real on-device or emulated-image verification (same gap D-40 already named for install/upgrade verification generally).

**Verification performed**: real daemon processes (not mocks), real HTTP requests (`curl`), real file `md5sum`/`diff` comparisons of `config.yaml`/`auth_secret` at every transition, real process signals (`SIGTERM`) for clean shutdown. All test state (`config.yaml`, `auth_secret`, both scratch binaries) was removed from `/var/lib/magitrickle` and `/tmp` after the test — nothing left behind in the sandbox's real state directory.

## D-43: Phase 9 profiling (task #53) — no hotspot needed fixing, and why

**Profiled a real, non-trivial DNS+cache+matching workload** (host
`magitrickled-c`, 1000 namespace rules, `valgrind --tool=callgrind`,
63,164 real UDP queries against a real `dnsstub` upstream — chosen over
`perf` because this sandbox's kernel (6.18.5) has no matching
`linux-tools` package, so hardware perf counters aren't available;
`callgrind`'s instruction-count profiling doesn't need them).

**Instruction-count breakdown**: malloc/free family combined
(`_int_malloc`/`_int_free`/`calloc`/`free`/`malloc`/`malloc_consolidate`/
`unlink_chunk`/`realloc`/...) accounts for **~44% of instructions** —
the single largest category, expected for a workload that builds
per-query name strings and RR structures on the heap rather than
reusing fixed buffers. DNS-specific hot functions (`rd_name`
7.06%, `mt_dns_name_to_string` 4.47%, `wr_bytes` 3.85%,
`mt_dns_msg_parse` 2.67%, `parse_rr_section` 2.40%, `dom_find`/
`rev_find` — cache lookups — 1.68%/1.63% combined) are all real,
expected DNS-pipeline work, not a surprise. `yaml_parser_*`/
`mt_config_load_buffer` (~2.3% combined) is one-time startup config
load, diluted into the whole-run total, not a per-query recurring cost.
**No pathological scaling, no lock contention (single event-loop
thread, D-17), no busy-loop, no O(n) scan standing out** — matches the
benchmark's own evidence (task #54): throughput is flat across
100/1000/10000 rules (namespace reverse-trie, D-18), confirming the
matcher is not the bottleneck at any rule count tested.

**Decision: no code change.** The malloc-heavy profile is architecturally
expected, not a defect, and the real wall-clock numbers already measured
(task #54) settle whether it matters in practice: at the same 1000-rule
cell, the C daemon sustains **~2x Go's UDP throughput at less than half
Go's CPU usage** (e.g. concurrency=10: C ~42k rps at ~66% CPU vs Go's
~20k rps at ~148% CPU) and uses roughly half Go's RSS. Whatever the
per-query allocator overhead costs in absolute instruction count, it does
not prevent the C rewrite from substantially outperforming the reference
implementation it must match — "fixing" it (e.g. a per-request arena
allocator) would add real complexity (allocator lifetime, use-after-free
risk surface) for a gain that the numbers don't show is needed, which is
exactly the kind of premature optimization the project's own ground
rules (§4/§23: no complexity without a shown need) argue against. Noted
here as a legitimate future optimization candidate *if* a real
performance requirement ever demands it — not acted on speculatively.

## D-44: Phase 9 long soak + final sanitizer/fuzz reruns (task #55)

**Soak** (`docs/c-rewrite/soak-c-phase9/summary.txt`): 180 s bounded soak
(host-limited substitute for a real 24 h run, same disclosure pattern as
D-36's 90 s fault-injection soak) — 500 namespace rules, group disabled
(this sandbox's `ipset` is non-functional, D-42/D-43; not a Go-vs-C
difference), sustained real load: **3,023,131 UDP + 389,421 TCP queries,
zero errors, zero timeouts**. RSS sampled every 15 s for the full
duration: **flat at exactly 31,928 KB across all 13 samples and the
final read** — not just "no leak," genuinely zero measured growth despite
3.4M+ queries touching the bounded records cache (100,000-domain rotating
pattern, well above the cache's 65,536-domain default cap, so both the
cap-rejection path and the 30 s expiry sweep were exercised, not just the
common case). `VmHWM` (peak RSS) equals steady-state RSS — no transient
spike either.

**Final full-suite reruns** (after the soak, on a freshly cleaned build —
sequenced this way specifically to avoid repeating the concurrent-rebuild
mistake that corrupted this same phase's first benchmark attempt, see
task #54's history): `make test` (28/28 unit binaries pass), `make
sanitize` (0 ASan/UBSan findings), `make fuzz FUZZ_RUNS=200000` (2
targets, 400,000 total runs, 0 crashes), `tests/differential/run_diff.sh`
(all suites OK, including the 44-step HTTP contract) — all clean, no
regressions from anything landed earlier in Phase 9 (the interfaces fix,
D-43).

## D-45: Go backend removal (task #57) — rationale, CI build gate, differential-harness retirement

**Status: accepted.** With the parity checklist signed off (D-56/
parity-checklist.md) and the user's explicit authorization to proceed,
`src/backend` (Go), its Go module dependencies, and its Go-only CI steps
were removed in this change, per migration-plan.md Phase 9's own text:
"separate change: remove `src/backend` (Go), Go deps, Go CI steps; update
Makefile, CLAUDE.md, AGENTS.md, README, docs." This is a single,
cleanly-revertible commit — `git revert` restores Go in full if a gap is
later found that the parity checklist missed.

**What changed, mechanically:**

- Root `Makefile`: the Phase 8 `BACKEND=go|c` switch is gone. `build_backend`
  always builds `src/backend-c`. `CROSS_COMPILE`/`SYSROOT` (renamed from
  `C_CROSS_COMPILE`/`C_SYSROOT`) and `DEPS_IPK`/`DEPS_APK` (renamed from
  `C_DEPS_IPK`/`C_DEPS_APK`) are now the only names, unconditional. All 40
  `config/*/*.config` files had their `GOOS=`/`GOARCH=`/`GOMIPS=`/`GOARM=`/
  `GO386=` lines stripped — they carry only `PLATFORM=`/`TARGET=` now.
- `.github/workflows/check.yml`: the Go `check`/`test_backend` jobs are
  gone; only `test_frontend` remains.
- `.github/workflows/check-c.yml`: the `differential` job no longer sets
  up Go — it runs the same `run_diff.sh`, now Go-independent (below).
- `.github/workflows/build.yml`: the "Set up Go" step is gone. **Critical
  correctness note**: Go cross-compiled correctly and automatically for
  every one of the 40 packaging targets via `GOOS`/`GOARCH`, using only
  the Go toolchain (no per-target sysroot needed for a static Go binary).
  C has no equivalent — a real target build needs a matching
  cross-toolchain *and* a sysroot with `libyaml`/`libpcre2`/`libmnl`/
  `libcurl`/`libcjson` built for that target's libc (Entware glibc /
  OpenWrt musl), and no such toolchain+sysroot pipeline is wired into this
  CI (D-37/D-38 already found the upstream Entware/OpenWrt toolchain
  mirrors blocked; D-38's `ports.ubuntu.com` workaround covers only
  arm64/armhf/riscv64 dev-lib *headers*, not a full per-target
  cross-toolchain). Leaving `CROSS_COMPILE`/`SYSROOT` empty and just
  running `make` in CI, as the old Go-based workflow implicitly could,
  would silently produce a host-x86_64 binary mislabeled with that
  target's architecture in the package filename — a correctness bug, not
  a build failure, and a strictly worse outcome than a build that visibly
  fails. Rather than accept that risk to keep the CI matrix "green," the
  "Check cross-toolchain availability" step unconditionally sets
  `ready=false` for every target with a `::notice::` explaining why, and
  gates the actual build/package/upload steps on it. This is a deliberate,
  disclosed regression in CI *build* coverage (40 targets go from
  "packaged in CI" under Go to "not yet automated" under C) traded for
  correctness — not a silent drop. Local `make` for a real device target
  still works exactly as before once a real `CROSS_COMPILE`/`SYSROOT` is
  supplied by hand; only the CI matrix's blind default is gated.
- `src/backend-c/tests/differential/`: the entire suite hard-depended on
  Go as a live comparison oracle (`oracle_go`, `cache_oracle_go`,
  `dns_gen_go`, `dns_oracle_go`, all via `go.mod` `replace magitrickle =>
  ../../../../backend`), plus two Phase 1 spikes
  (`spikes/regex_corpus/oracle_go`, `spikes/yaml_emit/fixture_go`). Before
  deleting `src/backend`, its live output for every suite (HTTP contract,
  config-fixture load/save + missing-file defaults, rule matching,
  subscription parsing, DNS wire dump/stripaaaa/ptrcheck, records cache,
  regexp2 corpus, yaml.v2 emit) was captured one last time and frozen
  under `tests/differential/golden/` and
  `spikes/{regex_corpus,yaml_emit}/golden/` — byte-identical to C's own
  output at capture time (the last thing D-44's final full-suite rerun
  verified before this change). `run_diff.sh`, `run_http_diff.sh`, and
  both spikes' `run.sh` were rewritten to diff the C tool's live output
  against these golden snapshots instead of a live Go run. All Go oracle
  directories were deleted. `tests/differential/corpus/dns_corpus.hex`
  (previously regenerated per-run by the now-deleted `dns_gen_go`, and
  accordingly gitignored) is now a committed, frozen fixture — the
  `.gitignore` entry for it was removed.

  This means these suites can no longer catch a *new* Go-vs-C divergence
  (there is no more Go to diverge from) — they are now regression tests
  against C's own previously-verified-correct behavior, same role
  `parity-checklist.md` already assigns to the frozen contract text. A
  real behavioral regression in the C backend still shows up as a diff
  against golden/; it just can't be cross-checked against a live
  reference anymore. This is the expected, disclosed trade-off of
  completing the rewrite, not an oversight.

- The `match`/`subparse`/`dns`/`cache` live-oracle corpus comparisons were
  also converted to the same golden-file pattern (rather than dropped)
  even though `tests/unit/test_match.c`, `test_subparse.c`,
  `test_dnswire.c`, and `test_dns_cache.c` already carry an independent,
  hardcoded port of the same Go test corpora — the differential corpus
  files exercise the tools' CLI/stdin plumbing (`mt-configtool`,
  `mt-dnstool`, `mt-cachetool`) end-to-end in a way the unit tests don't,
  so retiring them outright would have been a real (if small) coverage
  loss, not just redundant cleanup.

**Verification after removal**: with `src/backend` fully absent from the
working tree, `make CFLAGS_EXTRA=-Werror` (clean build), `make test`
(unit tests), `make sanitize` (ASan+UBSan), and
`sudo -E env "PATH=$PATH" sh tests/differential/run_diff.sh` (full
regression suite incl. the rewritten golden-based ones) were all rerun
from scratch and pass — confirming nothing in the retained test
infrastructure was silently depending on Go being present.

**Not done in this change, left as an explicit known gap** (already
disclosed in parity-checklist.md and D-43): no real on-device Entware/
OpenWrt verification has ever been performed in any phase of this
rewrite (sandbox has no such hardware or reachable toolchain/feed
mirror), and the Keenetic RCI hook lookup noted in parity-checklist.md
was deferred rather than found. Both predate this change and are
unaffected by it — they are pre-existing residual risk on the C
implementation itself, not something Go removal introduces or worsens.

## D-46: Restore CI packages for Entware Keenetic targets after the C cutover

**Status: accepted; source-built SDK implementation superseded by D-48.**
D-45 correctly prevented host-x86_64 binaries from
being mislabeled as embedded targets, but its unconditional gate also made
all three previously shipped Keenetic (`*_kn`) packages disappear. These
targets do not require a distinct Keenetic ABI: per `toolchains.md`, `_kn`
only enables software behavior and packaging files on top of the matching
Entware ABI.

The three `_kn` matrix jobs now build against pinned revisions of the
official Entware build system and `entware-packages` feed. Entware builds
its glibc 2.27 cross-toolchain plus the development staging files for
libyaml, PCRE2, libmnl, cJSON, and libcurl; the project then passes that
compiler, compiler sysroot, target flags, and `/opt` staging library path
to the C backend. Before packaging, the workflow checks ELF machine, byte
order, and the `/opt/lib` program interpreter for every output. This makes
an accidental native runner binary a hard failure rather than a publishable
artifact. The SDK and the official builder image are cached, while their
source revisions remain pinned by SHA.

The fresh Entware tree is bootstrapped in its required phase order
(`tools/install`, `toolchain/install`, `target/compile`) before compiling
the selected library packages. Invoking a leaf package directly does not
establish that ordering and can enter `package/libs/toolchain` with an empty
toolchain staging directory. Only the three required external feed sources
are linked; unrelated missing-dependency warnings from the rest of the
packages feed are therefore excluded from the job. A failed parallel phase
is repeated with `-j1 V=sc` so CI preserves the underlying command failure.

The Entware runtime dependency is recorded as `cJSON`, matching the actual
official feed package name; `libcjson` remains the OpenWrt package name.
The remaining non-`_kn` Entware and OpenWrt jobs retain D-45's explicit
gate until real SDK provisioning is added for them.

## D-47: CI compatibility fixes after runner/tooling updates

**Status: accepted.** Frontend CI moves from Deno 1.x to 2.x and unit tests
use `@std/testing`'s BDD functions instead of Deno's incomplete
`node:test` compatibility layer. The application replaces the deprecated
`lucide-svelte` package with its drop-in successor `@lucide/svelte`; the
removed GitLab brand icon is retained locally with the same SVG path. The
Deno-only config no longer contains the unsupported `moduleResolution`
option.

ASan also exposed a real server-lifecycle leak: accepted keep-alive
connections were owned only by epoll callbacks, so stopping the loop before
the peer closed left the connection and read buffer allocated.
`mt_httpd_t` now tracks accepted connections and closes all of them during
destroy, while preserving the existing close path during normal operation.

Finally, the HTTP regression trace no longer freezes interface names from
the machine that produced the golden snapshot. The response must still be
well-formed, begin with the synthetic `blackhole` interface, and contain at
least one real host interface; only those host-specific names are replaced
with a stable placeholder before comparison.

## D-48: Use a prebuilt Entware SDK for Keenetic packages

**Status: accepted; supersedes D-46's source-built CI SDK pipeline.**
The corrected D-46 bootstrap order successfully produced the MIPS and
MIPSEL SDKs, but each matrix job spent about 44 minutes rebuilding an
unchanged GCC/glibc toolchain before reaching the project build. That is
unnecessary CI latency and made iteration on the remaining packaging error
impractical.

The three `_kn` jobs now use `ownik/gh-action-entware-sdk`, pinned to the
verified commit behind its `v1` tag. The action downloads the latest
prebuilt `ownik/entware-sdk` release asset for the selected base Entware
architecture, verifies the release-provided SHA-256 digest, and caches the
archive. These SDK artifacts are explicitly an unofficial distribution of
the Entware SDK; the compiler ABI remains Entware GCC 8.4.0/glibc 2.27.

Because the action consumes an Entware/OpenWrt feed package rather than
exporting an SDK path to later workflow steps, CI creates a small temporary
feed containing the backend sources, already-built frontend, packaging
payload, and `tools/ci/entware-package/Makefile`. That Makefile compiles the
C backend with the SDK's `TARGET_CROSS`, target flags, and `/opt` dependency
staging directory, enables `MT_ENTWARE_KN`, and installs the existing
Keenetic init/NDM payload into the generated IPK. The workflow still extracts
the daemon from the package and verifies ELF architecture, byte order, and
the `/opt/lib` dynamic interpreter before upload.

The temporary source archive uses the normal OpenWrt/Entware `PKG_SOURCE`
and `file://` download path (including a versioned top-level directory), so
the action's mandatory `package/check` target can validate it. Target flags
are passed through the backend's additive `CFLAGS_EXTRA`/`LDFLAGS_EXTRA`
hooks; assigning `CFLAGS` on the make command line would suppress the
backend's own `-Iinclude` and feature defines. `WITH_DEPS=1` selects the
full daemon build because the feed SDK already supplies its compiler
sysroot implicitly; without that opt-in the backend correctly treats a
generic sysroot-less cross compiler as core-only.

The normal package path declares `libatomic` as an explicit runtime
dependency. The temporary SDK-feed package instead links only libatomic
statically while keeping glibc dynamic. This is especially material on
32-bit MIPS, where 64-bit C11 atomics are provided by libatomic rather than
native instructions.

The published SDK archive contains the staged completed toolchain, but
drops both its generated system IPKs and the completion stamps under
`build_dir/.../toolchain`. Consequently any ordinary package dependency
makes the build system try to rebuild GCC and fail on a removed
`.prepared_*_check` prerequisite. The temporary feed restores the
prepared/configured/built stamp chain for the pinned GCC 8.4/glibc 2.27 SDK
while its Makefile is loaded. Entware can then package libc/libgcc from the
staged toolchain and build normal feed dependencies without rebuilding the
SDK. The hashes are deliberately explicit so an incompatible future SDK
fails rather than silently reusing a stale stamp.

## D-49: Wire per-platform filesystem paths into the build (fixes Entware `/opt` paths)

**Status: accepted.** Closes the gap left open in D-24: `paths.h` had
the `#ifndef`-override hook for `MT_APP_SHARE_DIR`/`MT_APP_STATE_DIR`/
`MT_SOCK_PATH`/`MT_PASSWD_FILE`/`MT_SHADOW_FILE`, but nothing ever passed
the per-platform `-D` flags, so **every** build — including the shipped
Entware/Keenetic `_kn` packages — baked in the host defaults
(`/var/lib/magitrickle/config.yaml`, `/var/run/magitrickle.sock`,
`/etc/{passwd,shadow}`). On Entware, where the package installs
everything under `/opt`, the daemon looked in the wrong place: it never
found its config (logging "config file /var/lib/magitrickle/config.yaml
not found, using defaults") and listened on `/var/run/magitrickle.sock`
while the `_kn` `netfilter.d/100-magitrickle` hook talks to
`/opt/var/run/magitrickle.sock`, so the Keenetic integration was broken.

The fix mirrors Go's `entware`/`openwrt` build tags without introducing a
new mechanism:

- `paths.h` now selects its default set on `MT_PLATFORM_ENTWARE` /
  `MT_PLATFORM_OPENWRT` (else host defaults), each value copied verbatim
  from the old `constant/path_{entware,openwrt,default}.go`. Every macro
  keeps its own `#ifndef` guard so a command-line `-D` still wins.
- `main.c`'s `MT_CONFIG_PATH` now derives from `MT_APP_STATE_DIR`
  (`MT_APP_STATE_DIR "/config.yaml"`), matching Go's
  `cfgFileLocation = AppStateDir + "/config.yaml"`, so the config file
  follows the platform state dir instead of being pinned separately.
- `src/backend-c/Makefile` maps a passed-through `PLATFORM=entware|openwrt`
  to `-DMT_PLATFORM_ENTWARE` / `-DMT_PLATFORM_OPENWRT` (same shape as the
  existing `ENTWARE_KN` → `-DMT_ENTWARE_KN`).
- Both build entry points pass `PLATFORM`: the root `Makefile`'s
  `build_backend` (covers OpenWrt and local `entware` builds) and the CI
  Keenetic packager `tools/ci/entware-package/Makefile`'s `Build/Compile`
  (`PLATFORM=entware`, alongside its existing `ENTWARE_KN=1`).

Host builds and the whole test/differential suite pass no `PLATFORM`, so
they keep the `/var/...` defaults (no golden churn). Verified by
preprocessor expansion for all three platforms (byte-identical to the Go
constants) and by building the dependency-free `core`+tools for
`PLATFORM=entware` and host.

## D-50: Real OpenWrt SDK CI for three well-known package archs

**Status: accepted, confirmed via real CI iteration.** Mirrors D-48's
Entware approach (real SDK, real feed-package build, real `.ipk`) for
OpenWrt, but OpenWrt has no equivalent to `ownik/gh-action-entware-sdk`
and its 24 package archs each need a specific SDK `target/subtarget`
that isn't 1:1 with the package-arch name (multiple hardware targets can
share one package arch; `toolchains.md`'s own OpenWrt table already had
unfilled `Triplet` columns for most rows). Rather than guess all 24
mappings from memory and risk silently pulling the wrong ABI,
`build.yml`'s toolchain-check step marks three **well-known, stable**
target/subtarget pairs `ready=true`: `x86_64` → `x86/64`, `mips_24kc` →
`ath79/generic`, `mipsel_24kc` → `ramips/mt7621` (all three are among the
most common, best-documented OpenWrt targets and have been stable across
many releases). A fourth candidate, `aarch64_generic` → `armvirt/64`, was
tried and removed: the first real CI run 404'd on
`https://downloads.openwrt.org/releases/24.10.1/targets/armvirt/64/sha256sums`,
so that mapping is wrong for this OpenWrt release. Since it can't be
verified from this sandbox (proxy blocks `downloads.openwrt.org`) and
isn't worth guessing again blindly, `aarch64_generic` was reverted to the
default gated case rather than given a second guess. Every other OpenWrt
package arch keeps the existing "not yet automated" notice — same
explicit-gate principle as D-45, not a silent drop.

**Mechanism** (`tools/ci/openwrt-package/Makefile`, new): a real OpenWrt
SDK for `OPENWRT_VERSION=24.10.1` (matches the version `toolchains.md`
already committed the feed to) is downloaded per matching target/
subtarget from `downloads.openwrt.org`, its `sha256sums` file is fetched
first specifically to discover the exact SDK archive filename (gcc
version and compression suffix vary by target) rather than hardcoding
one, and the download is verified against that digest before
extraction — matching D-48's Entware SDK verification discipline. The
daemon's own source is staged as an in-tree SDK package (`package/net/
magitrickle/`, `PKG_SOURCE_URL:=file://...`, same as the Entware feed
package), `./scripts/feeds install` pulls in `+libyaml +libpcre2 +libmnl
+libcurl +libcjson` plus the netfilter runtime deps, and `make package/
magitrickle/{clean,compile}` builds it through the SDK's own toolchain
and dependency graph — reusing exactly the same
`CROSS_COMPILE=$(TARGET_CROSS)`/`WITH_DEPS=1`/`CFLAGS_EXTRA`/
`LDFLAGS_EXTRA` hooks into `src/backend-c/Makefile` that D-37/D-48
already established, no new backend-side mechanism. Unlike Entware,
OpenWrt's standard `/usr` install paths need no custom dynamic-linker
override (that was specifically for Entware's nonstandard `/opt/lib`).
Feed dependency package names (`libyaml`, `libpcre2`, `libmnl`,
`libcurl`, `libcjson`, `iptables-nft`, ...) reuse the exact names the
root Makefile's `DEPS_IPK`/`DEPS_APK` already assumed for OpenWrt (D-39's
"best guess, not verified against a live feed index" caveat carries
forward unchanged — this task doesn't newly introduce that risk, just
inherits it).

**What real CI caught that this sandbox couldn't** (its proxy denies
`downloads.openwrt.org`, same confirmed policy block as D-37/D-38's
Entware mirror lookups, so none of this could be dry-run locally — only
the Makefile's syntax and the workflow YAML's structure were checked
locally before the first push). Two real bugs surfaced from actual
GitHub Actions runs, both now fixed:

1. The `aarch64_generic` → `armvirt/64` mapping (404, described above).
2. `x86_64` and `mips_24kc` both failed identically, after the SDK
   download/verification and `./scripts/feeds install` steps succeeded,
   with `include/magitrickle/json.h:18:10: fatal error: cjson/cJSON.h:
   No such file or directory` during `make package/magitrickle/compile`.
   Root cause: `./scripts/feeds install <pkg>` only symlinks a package's
   build recipe into the SDK's package tree — it does not build it or
   stage its headers/libs into `staging_dir`, and a direct `make
   package/<name>/compile` does not walk the `DEPENDS` graph to build
   prerequisites first (unlike a full image `make`). Fixed by adding an
   explicit `make package/libyaml/{clean,compile}
   package/libpcre2/{clean,compile} package/libmnl/{clean,compile}
   package/curl/{clean,compile} package/libcjson/{clean,compile} V=s`
   step before `make package/magitrickle/{clean,compile}` in
   `build.yml`, compiling exactly the compile-time (header/lib)
   dependencies — the remaining `DEPENDS` (kernel modules, iptables CLI
   binaries we only fork/exec) are runtime-only and need no such
   pre-compile step.
3. That dependency pre-compile step then failed itself: `make[1]: ***
   No rule to make target 'package/libyaml/clean'`. The default failure
   reporter only grepped for error-like patterns, which a missing
   package's warning doesn't match, so `feeds-install.log` was never
   shown — fixed first by always dumping it in full on failure. The
   real content it revealed: `libyaml`/`libpcre2`/`libcurl` all resolved
   fine (feeds install follows OpenWrt's virtual/provides aliases —
   `libyaml`→`yaml`, `libpcre2`→`pcre2`, `libcurl`→`curl`), but
   `WARNING: No feed for package 'libcjson' found` — cJSON is not
   published in any OpenWrt feed for this release under any name, unlike
   Entware's opkg feed (D-48) where it already existed. Fixed by
   vendoring cJSON as a new in-tree SDK package,
   `tools/ci/openwrt-package-libcjson/Makefile`, staged into
   `$sdk_dir/package/libs/libcjson/` the same way `magitrickle`'s own
   package is staged into `package/net/magitrickle/` — no `feeds
   install` needed, since OpenWrt's package scanner walks the whole
   `package/` tree regardless of feed origin (confirmed: `magitrickle`'s
   own first-party package was already being found this way). The
   package builds cJSON's amalgamated `cJSON.c` directly via a manual
   `Build/Compile` (skip cJSON's own CMake build to keep the OpenWrt
   package Makefile simple) into a versioned `.so`, installing headers
   under `/usr/include/cjson/` to match `#include <cjson/cJSON.h>`.
   Source is pulled via `PKG_SOURCE_PROTO:=git` pinned to tag `v1.7.18`
   rather than a hash-verified tarball, since this sandbox's proxy also
   blocks `github.com` (confirmed: a direct `curl` to a GitHub release
   tarball 403'd here too) — no way to fetch a checksum to embed, so the
   git tag itself is the integrity anchor, same idea as this project's
   existing commit-pinned GitHub Actions (`ownik/gh-action-entware-sdk`).
   `libcjson` was removed from the `./scripts/feeds install` argument
   list (it would otherwise re-print the same "no feed" warning
   harmlessly, but there's no reason to ask feeds for a package that
   isn't a feed package) while staying in the explicit dependency
   pre-compile step from finding 2, since our own package still declares
   `+libcjson` in `DEPENDS` and still needs it compiled first.

`mipsel_24kc` independently confirmed finding 3's exact same
`No rule to make target 'package/libyaml/clean'` failure once pulled.

4. The cJSON-vendoring fix from finding 3 landed correctly (confirmed:
   the "has a dependency on 'libcjson', which does not exist" warning is
   gone from `defconfig.log`/`feeds-install.log` on all three targets
   post-fix), but it exposed the same "DEPENDS name vs. real package
   name" mismatch for two more packages: `feeds-install.log` had already
   shown `Installing package 'yaml' from packages` and `Installing
   package 'pcre2' from base` — `feeds install` resolves the `+libyaml`/
   `+libpcre2` DEPENDS entries as virtual/provides aliases, but the
   actual buildable package name (and thus `make package/<name>/...`
   target) is `yaml`/`pcre2`, not `libyaml`/`libpcre2`. The explicit
   pre-compile step in finding 2 used the DEPENDS names verbatim, which
   is why it kept hitting the identical `package/libyaml/clean` error
   even after the cJSON fix. Fixed by changing the pre-compile targets
   to `package/yaml/{clean,compile}` and `package/pcre2/{clean,compile}`
   (`libmnl` and `curl` keep their DEPENDS names as-is — confirmed no
   rename for those in the same log).

5. Findings 3-4's fixes worked: the real daemon build (`Prepare OpenWrt
   SDK + package`) succeeded on all three targets for the first time —
   a genuine `.ipk` was produced under `$sdk_dir/bin/packages` and
   found by the `Collect and verify package` step's `find`. That step
   then failed itself: `ar: <package>.ipk: file format not recognized`.
   Root cause: unlike Entware's `.ipk` (a classic `ar` archive containing
   `debian-binary`/`control.tar.gz`/`data.tar.gz`), OpenWrt's own
   `opkg-build` produces the `.ipk` as a **plain tar** with the same
   three members, no `ar` wrapper — so `ar p` can't read it. The
   Entware branch of this same verification step already had a fallback
   for exactly this (`if ar t ...; then ar p ...; else tar -tf ...`),
   added for Entware's own SDK output; the OpenWrt branch was written
   without porting that same fallback. Fixed by adding the identical
   `ar`-then-`tar`-fallback to the OpenWrt branch.

This entry's fixes (findings 3-5) are unverified from this sandbox (no
network to actually run the SDK/feeds/build steps) and, like every
other network-touching change in this entry, are expected to need at
least one more real-CI round-trip before landing clean — though finding
5 is the first time the actual daemon binary itself successfully built
for OpenWrt, which is the hard part; the verification-step fix is
comparatively low-risk.

Currently produces only `.ipk` (OpenWrt ≤24.10, opkg); `.apk` (OpenWrt
≥25.12) is not wired up for any target. See D-51 for the subsequent
expansion to the remaining package archs and Entware's non-Keenetic
targets.

## D-51: Expand CI to the remaining Entware and OpenWrt package archs

**Status: accepted, expect substantial iteration.** With D-50's three
OpenWrt package archs confirmed working end-to-end (real SDK build +
real `.ipk` produced and verified), this activates CI for the rest of
`config/entware/*.config` and `config/openwrt/*.config` rather than
leaving them indefinitely gated.

**Entware — low risk, reuses D-48 verbatim.** The three non-Keenetic
targets (`aarch64-3.10`, `mips-3.4`, `mipsel-3.4`) and one never-tried
before (`armv7-3.2`) use the exact same `ownik/gh-action-entware-sdk`
mechanism as the already-working `_kn` targets — same SDK, same feed
package flow, just without Keenetic's NDM integration. This required
making `tools/ci/entware-package/Makefile` target-aware
(`MAGITRICKLE_IS_KN:=$(filter %_kn,$(MAGITRICKLE_TARGET))`, mirroring
the root Makefile's own `$(filter %_kn,$(TARGET))` gate at
`Makefile:77`): only `_kn` targets get `ENTWARE_KN=1`, the NDM
`netfilter.d` hook, and the `+socat` dependency; plain Entware targets
install the existing `files/entware/etc/init.d/S99magitrickle` (which
sources the standard Entware `rc.func`, present on real non-Keenetic
Entware installs, unlike the self-contained Keenetic variant) — the
same distinction the non-CI `make package_ipk` path already draws
(`Makefile:256-257`). `armv7-3.2` is the one genuinely new risk here:
unlike the other three archs, no `_kn` variant of it has ever built in
this CI, so its own SDK asset availability is unverified.

**OpenWrt — higher risk, target/subtarget mappings are best-effort.**
Of the 21 remaining package archs (excluding the already-gated
`aarch64_generic`), this attempted 18. This section originally listed
them by a priori confidence level before any of it had run in real CI;
that framing is now replaced below by what real CI actually confirmed.

**Confirmed working (real CI, all 15):** `i386_pentium-mmx`/
`i386_pentium4` → `x86/generic`; `loongarch64_generic` →
`loongarch64/generic`; `aarch64_cortex-a53` → `bcm27xx/bcm2710`;
`aarch64_cortex-a72` → `bcm27xx/bcm2711`; `arm_arm1176jzf-s_vfp` →
`bcm27xx/bcm2708`; `arm_cortex-a15_neon-vfpv4` → `ipq806x/generic`;
`arm_cortex-a9`/`arm_cortex-a9_neon` → `mvebu/cortexa9`; `arm_xscale` →
`ixp4xx/generic`; `arm_cortex-a7` → `mediatek/mt7623`;
`arm_cortex-a7_neon-vfpv4` → `sunxi/cortexa7`; `arm_cortex-a9_vfpv3-d16`
→ `bcm53xx/generic`; `mips64_octeonplus` → `octeon/generic`;
`mipsel_74kc` → `bcm47xx/mips74k`.

**Confirmed wrong (real CI, all 3, reverted to gated):**
`riscv64_generic` (tried `riscv64/generic`), `mips64el_mips64r2`
(tried `loongson64/generic`), and `mipsel_mips32` (tried
`bcm63xx/generic`) all 404'd identically on their SDK's `sha256sums`
lookup for `OPENWRT_VERSION=24.10.1` — the *a priori* "high confidence"
label given to `riscv64_generic` (assumed, wrongly, to be as clean a
1:1 mapping as `loongarch64_generic`) turned out no better calibrated
than the "medium confidence" ones; real CI is the only actual signal
that mattered here. No stronger alternative target/subtarget came to
mind for any of the three, so — matching D-50's `aarch64_generic`
precedent — they're reverted to the default gated case rather than
guessed again.

**Still explicitly gated, never attempted**: `aarch64_cortex-a76`,
`arm_arm926ej-s`, `arm_cortex-a5_vfpv4`, `arm_cortex-a7_vfpv4`,
`arm_cortex-a8_vfpv3`, `arm_fa526`, `mips64_mips64r2`,
`mipsel_24kc_24kf` — either too many plausible hardware families share
the CPU baseline to pick one with any confidence, or (for
`mips_4kec`/`mips_mips32`, also gated) the only candidate cores that
came to mind (`ramips`, `bcm47xx`) are little-endian, which would fail
the `Collect and verify package` step's own `mips_*` → big-endian
assertion for a package arch name that (unlike `mipsel_*`) doesn't
carry an "el".

**Entware**: all four (`aarch64-3.10`, `mips-3.4`, `mipsel-3.4`,
including the previously-untested `armv7-3.2`) confirmed working in the
same real CI run — the D-48 mechanism ported over with zero surprises.

Net result: **25 of the 28 real-build-attempt targets pass in CI**
(7 Entware + 18 OpenWrt, on top of D-50's original 3), 3 OpenWrt archs
reverted to gated after a confirmed-wrong guess, 11 OpenWrt archs remain
gated as never attempted.

## D-52: Build `.apk` (OpenWrt ≥25.12) packages in CI via a self-built `apk-tools`

**Status: accepted, expect iteration.** `Makefile`'s own `package_apk`
target (non-CI, source-build path) has produced valid `.apk` metadata
logic since Phase 8, but `apk mkpkg` — OpenWrt's APKv3 packaging
subcommand — was never actually run anywhere in this project; Phase 8's
own verification could only dry-run it (`make -n`) because `apk-tools`
isn't installed in any sandbox this project has had. Checked again for
this task: no `apk-tools` package exists in Ubuntu's apt repos (the
`build` job's runner), so CI needs to build it from source, same as it
already does for the SDKs themselves.

**Mechanism.** A new `apk_tools` job (parallel with `prepare`, not
matrix'd — `apk`/`mkpkg` runs as a *host* tool that assembles the
package archive, it's never cross-compiled) clones Alpine's own
`apk-tools` (APKv3/`mkpkg` was contributed upstream into the mainline
project by OpenWrt's own developers rather than staying a separate
fork), builds it via its `meson`/`ninja` build system, and uploads the
resulting `apk` binary as a workflow artifact — built once, reused by
every OpenWrt matrix job via `actions/cache` + `actions/download-artifact`
rather than rebuilt 18+ times. Each OpenWrt `build` job downloads it
only when `openwrt_target` is set (Entware jobs never need it).

**Packaging step reuses the already-produced `.ipk`'s own `data.tar.gz`**
as the `.apk`'s file tree (`apk mkpkg -F <dir>`) instead of re-deriving
`Package/install` logic a second time — the ipk's data archive already
*is* the target root filesystem tree, so extracting it and pointing
`mkpkg` at that directory is exactly equivalent to what a from-scratch
install-root construction would produce, with far less duplicated
logic. `conffiles`/`post-install`/`pre-deinstall`/`post-upgrade`
handling copies `files/openwrt/_apk/*` verbatim — the same files the
non-CI `make package_apk` path already uses (`Makefile:241`), so this
introduces no new packaging metadata, only a new place that invokes it.
The ECDSA signing key is a fresh ephemeral one generated per CI run
(`openssl ecparam ... -genkey`, ceremony identical to `Makefile`'s own
`$(BUILD_KEY_APK_SEC)` target) — packages are unsigned-by-any-persistent-
key and installed with `--allow-untrusted`, matching the README's own
install instructions; there is no distributed public key to verify
against, by design, same as before this change.

**Failure isolation — got this wrong on the first attempt.** Originally
wrapped `.apk` packaging in `if ( set -eu; ...; ); then :; else
::warning; fi`. The first real CI run hit two real bugs:

1. `apk` failed at runtime: `error while loading shared libraries:
   libapk.so.3.0.0: cannot open shared object file` — the `apk_tools`
   job only shipped the `apk` executable, not the `libapk.so` it
   dynamically links against. Fixed by also finding and shipping
   `libapk.so*` from the build tree, and invoking `apk` with
   `LD_LIBRARY_PATH` pointed at the artifact directory.
2. Worse: despite that failure, the `if (subshell); then` still took
   its *success* branch, so `package_2_path` got set to a `.apk` path
   that was never created — and since `Upload package artifact 2` has
   no failure isolation of its own (`if-no-files-found: error`), this
   **broke the job for targets whose `.ipk` had already built
   successfully**, a real regression this change introduced into
   previously-working targets. Root cause: bash's `errexit`
   suppression for a command being tested by `if` can extend into a
   subshell that re-enables `set -e` inside it — a genuine, if
   obscure, bash gotcha, not something guessable without hitting it.
   Fixed by abandoning the `if (subshell)` idiom entirely: `set +e`,
   run the subshell directly (capturing `$?` into a variable), `set
   -e` again, then branch on the captured exit code. Also added a
   `test -s "$apk_package"` inside the subshell itself as a
   belt-and-suspenders check, independent of `apk mkpkg`'s own exit
   code, before ever reporting success.

With both fixes, `.apk` packaging failing now only emits a `::warning`
annotation and leaves `package_2_path`/`package_2_name` unset (the
existing `if steps.package_outputs.outputs.package_2_path != ''` guards
on the upload steps handle that cleanly) — it cannot fail the job or
take down the already-produced, already-verified `.ipk`, which is what
was intended from the start. `apk-tools`' own build was otherwise
confirmed working in that same run (the `apk_tools` job itself
succeeded); this was purely an artifact-completeness bug plus a bash
control-flow bug in the consuming step, not a problem with building
apk-tools itself. Unverified from this sandbox as always; expect this
to need at least one more real-CI confirmation that packaging actually
succeeds end-to-end now, not just that it fails safely.

**Not done here**: `PKG_VERSION_APK` (apk's version-string dialect —
opkg/deb-style `~git<date>.<hash>` pre-release suffixes aren't valid apk
version syntax) was already computed by the root `Makefile` but not
previously exported by `_return_export_dynamic_env`; added that one
line so the CI step can read it from `.build/openwrt-package.env`
without duplicating the `sed` transform in the workflow YAML.

## D-53: Second pass on D-51's "too ambiguous" OpenWrt archs — less conservative, per explicit request

**Status: accepted, expect iteration — several of these are lower
confidence than D-51's original batch.** D-51 left 8 OpenWrt package
archs explicitly gated as "too many plausible hardware families share
this CPU baseline to pick one with confidence." Asked to reconsider
rather than leave them gated, since the cost of a wrong guess here (a
fast 404 on SDK download, same as D-51's 3 misses) is low relative to
the value of not leaving things gated by default caution alone.

Re-examined all 8 with a lower confidence bar than D-51 used, since
`arm_*` package archs carry no byte-order verification risk in
`Collect and verify package` (only `aarch64_*`/`mips_*`/`mipsel_*`/
`x86_64` are checked there) — the only real risk left for any `arm_*`
guess is "does this SDK target/subtarget exist for
`OPENWRT_VERSION=24.10.1`", the same fast-fail 404 category as D-51's
misses, not a slow build-then-fail:

- `aarch64_cortex-a76` → `bcm27xx/bcm2712` (Raspberry Pi 5, the specific
  board this CPU baseline was clearly named after — missed on the
  first pass despite being fairly obvious in retrospect).
- `arm_arm926ej-s` → `gemini/generic` (Cortina/Storlink Gemini SoCs,
  e.g. D-Link DNS-313 — ARM926EJ-S is essentially the *only* core this
  OpenWrt target family uses, making it a cleaner match than most of
  D-51's own "medium confidence" picks were).
- `arm_fa526` → `gemini/generic` too — the Gemini SoC family's older
  members (SL2312-era) are believed to predate the SL351x/ARM926EJ-S
  generation and use the FA526 core instead; reusing the same
  target/subtarget as `arm_arm926ej-s` on the theory that OpenWrt's
  `gemini` target builds one kernel image spanning both. Lower
  confidence than the other picks here — this is the one most likely
  to turn out wrong.
- `arm_cortex-a5_vfpv4` → `at91/sama5` (Microchip/Atmel SAMA5 SoCs).
- `arm_cortex-a7_vfpv4` (no NEON) → `realtek/rtl930x` (Realtek's
  ARM-based switch SoCs, e.g. Zyxel GS1900 series — cost-optimized
  switch silicon plausibly lacking full NEON, unlike most other
  Cortex-A7 implementations which do have it).
- `mips64_mips64r2` → `octeon/generic` — reuses the exact same
  target/subtarget as `mips64_octeonplus` (already confirmed working
  and big-endian in D-51's real CI run). Deliberately redundant: no
  other big-endian MIPS64 OpenWrt target came to mind, and Octeon is
  now an *empirically confirmed* big-endian target rather than a guess,
  which is a stronger basis than most of this entry's other picks.
- `mipsel_24kc_24kf` (little-endian, hardware-FPU 24Kc) → `lantiq/xrx200`
  (Lantiq/Intel/MaxLinear VDSL SoCs) — lowest confidence of this batch;
  not certain `xrx200` actually has hardware FPU rather than the
  soft-float baseline the plain `mipsel_24kc`/`ramips` mapping already
  covers. **Confirmed wrong via real CI**: the SDK downloaded and the
  daemon built fine, but the resulting binary is BIG-endian MIPS (per
  the verification step's own `readelf -h` check) — Lantiq's whole
  MIPS lineup is apparently big-endian, contradicting the "mipsel_"
  name. Reverted to gated; no better little-endian 24Kc+FPU candidate
  came to mind.

**Still gated, no change**: `arm_cortex-a8_vfpv3` — unlike every other
arch above, no real OpenWrt target name came to mind at all for
Cortex-A8 (TI Sitara/BeagleBone-class SoCs aren't a router/NAS target
family in current OpenWrt), so this one stays gated rather than
fabricate a target/subtarget out of nothing. (D-54 later found a
candidate — `sunxi/cortexa8` — see that entry.)

**Confirmed via real CI**: 6 of this entry's 7 attempted mappings
passed (`aarch64_cortex-a76`, `arm_arm926ej-s`, `arm_fa526`,
`arm_cortex-a5_vfpv4`, `arm_cortex-a7_vfpv4`, `mips64_mips64r2`) — only
`mipsel_24kc_24kf` (above) turned out wrong, and for a reason (real
endianness mismatch) that couldn't have been predicted from a 404
alone. The "lower confidence bar" framing above turned out more
accurate than it looked going in: even the admittedly-weakest guesses
(`gemini/generic` reused for both ARM926EJ-S and FA526) held up.

## D-54: Close out the remaining gated OpenWrt archs — every config now attempts a real build

**Status: accepted, weakest-confidence entry in this series — expect
the highest miss rate of any batch so far.** After D-53, 4 OpenWrt
package archs were still gated: `aarch64_generic` (D-50's confirmed
404 on `armvirt/64`), `mips_4kec`/`mips_mips32` (no big-endian
candidate found), and `arm_cortex-a8_vfpv3` (no candidate at all).
Asked again to close these out rather than leave anything gated by
default caution.

- **`arm_cortex-a8_vfpv3`** → `sunxi/cortexa8` — Allwinner A10/A13
  (single-core Cortex-A8, e.g. Olimex OLinuXino boards), an older
  sibling of the already-working `sunxi/cortexa7` (A20) subtarget used
  for `arm_cortex-a7_neon-vfpv4`. Missed on the first two passes
  despite `sunxi` already being in use for a related core.
- **`aarch64_generic`**: retried under the theory that OpenWrt renamed
  its generic/QEMU-class aarch64 target from `armvirt` to `armsr`
  (subtarget `armv8`) at some point after the version this project's
  training knowledge was current for. If that memory is right, this
  fixes D-50's 404; if wrong, it just 404s again the same way and
  reverts to gated — no worse off than leaving it gated would have been.
- **`mips_4kec`/`mips_mips32`**: no OpenWrt target for the older
  4KEc-class AR71xx generation, or a plain big-endian generic mips32
  baseline, came to mind at all — every real candidate considered
  (`ramips`, `bcm47xx`) is little-endian, confirmed by D-51's own real
  CI (`mipsel_74kc` → `bcm47xx/mips74k` passed, proving that family's
  endianness). Rather than leave these flatly gated with nothing tried,
  both reuse `ath79/generic` — the one OpenWrt target this project has
  *empirically confirmed* big-endian via `mips_24kc`'s own passing CI
  run. This is explicitly a "best available reuse," not a claimed
  correct match: it should link and pass this repo's own byte-order
  verification, but ath79's real `-march` default may assume 24Kc/74Kc
  instruction extensions that genuine 4KEc-class or plain-mips32
  hardware lacks, which no CI check here can catch (no real 4KEc device
  or emulator to test boot/run on) — a binary that builds clean could
  still crash with an illegal-instruction trap on real hardware. If
  that turns out to matter, revisit rather than trust the green
  checkmark blindly.

**Confirmed via real CI: all 4 of this entry's mappings passed** —
`arm_cortex-a8_vfpv3` (`sunxi/cortexa8`), `aarch64_generic`
(`armsr/armv8` — the rename theory was right), `mips_4kec`, and
`mips_mips32` (both `ath79/generic`) all built and verified clean.
Combined with D-53's 6/7, **only `mipsel_24kc_24kf` (D-53) failed** out
of the entire second-pass batch — and for a real, specific reason
(confirmed big-endian binary from a `mipsel_`-named arch), not a vague
404. The weakest-confidence framing this entry opened with did not
hold up: the actual miss rate across D-53+D-54 combined was 1/11, far
lower than D-51's own 3/21 despite deliberately looser reasoning.

With this, every OpenWrt package arch config in the repo has attempted
a real CI build at least once — none remain gated purely because no
mapping was ever tried. Four package archs are gated with a confirmed
reason instead: `riscv64_generic`, `mips64el_mips64r2`, and
`mipsel_mips32` (D-51, SDK 404) plus `mipsel_24kc_24kf` (D-53, wrong
endianness).

## D-55: Bound OpenWrt matrix concurrency and retry network setup

**Status: accepted.** A full matrix run launched all newly enabled
OpenWrt jobs together; 17 otherwise independent targets failed while
cloning the same upstream feeds with transient GitHub HTTP 504 errors.
These were infrastructure failures, not target or compiler failures.

The package matrix is limited to six concurrent jobs. OpenWrt SDK and
checksum downloads use curl's retry-all-errors mode, and
`scripts/feeds update -a` is retried four times with increasing backoff.
No target is dropped or silently skipped: an exhausted retry budget still
fails the corresponding job and publishes its captured feed log.

## D-56: Package both OpenWrt release lines from their own SDKs

**Status: accepted.** OpenWrt 25.12 replaced opkg/`.ipk` with apk/`.apk`,
so one SDK can no longer produce a package both release lines can
install. Each OpenWrt arch config is therefore built twice — once
against the 24.10 SDK for `.ipk`, once against the 25.12 SDK for
`.apk` — which retires the earlier scheme of repacking the `.ipk`
payload with a host-built `apk-tools` (that produced a package no real
25.12 router had ever verified, and needed a separate job just to build
`apk mkpkg`).

Four things this exposed, each fixed where it belongs:

- **The project's `PKG_*` variables leaked into the SDK build.** The
  packaging step exports `PKG_VERSION`, `PKG_REVISION` and friends from
  `make _return_export_dynamic_env` to name the source tarball. Those
  names are generic enough that OpenWrt's own recipes picked them up:
  every kernel module inherited our `~git<date>.<sha>` suffix and apk
  rejected the result outright (`info field 'version' has invalid
  value`). They are unset again before make is invoked inside the SDK;
  the values needed after that are re-read from
  `.build/openwrt-package.env`.

- **Release SDKs ship `CONFIG_AUTOREMOVE=y`.** It empties a package's
  build directory the instant the package finishes compiling, so
  verifying the freshly built ELF at
  `<pkg>/src/backend-c/build/openwrt-<arch>/magitrickled-c` failed on
  every arch even though the package itself was built correctly.
  Verification reads `<pkg>/.pkgdir/magitrickle/usr/bin/magitrickled`
  instead — the staged install tree AUTOREMOVE deliberately preserves,
  and the exact bytes the package is packed from — falling back to the
  build output for SDKs that keep their build directories.

- **PCRE2 stays a normal shared runtime dependency.** Patching the feed
  recipe to `-DBUILD_SHARED_LIBS=OFF` for a static link breaks the
  recipe's own `Package/libpcre2/install`, which copies
  `libpcre2-{8,posix}.so*`. cJSON remains the one vendored static
  dependency, because no OpenWrt feed carries it (D-50).

- **A missing SDK is a skip, not a failure.** A release only publishes
  the targets it still supports, so a 404 on its `sha256sums` means the
  arch-to-target mapping does not apply to that release — reported as a
  warning naming the target/subtarget, consistent with how an unmapped
  arch is already handled, instead of turning the whole matrix red.

Doubling the matrix also doubled its wall-clock cost, so the feed setup
was trimmed to what the package actually resolves against: `base` and
`packages` only (no luci/routing/telephony/video), with `base` demoted
from `src-git-full` to a shallow `src-git` clone. That removes a
full-history clone of openwrt.git per job, which is what made D-55 hold
the matrix to six concurrent jobs; it now runs twelve.

## D-57: Let the SDK state its own arch, and un-gate on that

**Status: accepted.** Every OpenWrt arch here is reached through a
hand-written package-arch-to-target/subtarget guess, and D-51/D-53/D-54
established the two ways a guess goes wrong: it 404s, or it names a real
target that builds a different architecture. Only the first was visible.
The second produced `mipsel_24kc_24kf` as a big-endian binary (D-53),
caught by luck — the readelf checks only cover `aarch64_*`, `mips_*`,
`mipsel_*` and `x86_64`, so the same mistake on any other arch would
have shipped silently.

The SDK already answers the question authoritatively:
`CONFIG_TARGET_ARCH_PACKAGES` in its own `.config` is the package arch
it builds. Comparing that against the config's arch, immediately after
extraction, covers every arch and fails in seconds instead of ten
minutes. That check made the first real mismatch obvious by inspection:
`mips64_mips64r2` and `mips64_octeonplus` were both mapped to
`octeon/generic`, and one SDK cannot produce two package archs.

With a wrong guess now either skipped (D-56's 404 handling) or rejected
loudly, guessing became cheap enough to retry the archs D-51 left
gated:

- `mips64_mips64r2` → `malta/be64` and `mips64el_mips64r2` →
  `malta/le64`. malta is OpenWrt's QEMU MIPS target and the only one
  spanning all four word-size/endianness combinations, one per
  subtarget. D-51's `loongson64/generic` guess for the little-endian
  one 404'd.
- `mipsel_mips32` → `bcm47xx/generic`. The same target's `mips74k`
  subtarget already builds `mipsel_74kc` here, so the target is known
  good and known little-endian; D-51 guessed `bcm63xx/generic` and
  404'd.
- `riscv64_generic` → `sifiveu/generic`. D-51 guessed
  `riscv64/generic`, which is a package arch name, not a target name.

`mipsel_24kc_24kf` stays gated. It is the one case where a wrong guess
costs a whole job to reject rather than a 404 to skip, and no
little-endian 24Kc-with-FPU target is known.

A mismatch is reported and skipped rather than failed, for the same
reason a 404 is: not shipping a package is the right outcome, and it is
the mapping that is broken, not the build. This does cost coverage the
matrix appeared to have. D-54 knowingly reused `ath79/generic` for
`mips_4kec` and `mips_mips32` as a "best available" match, and ath79
builds `mips_24kc` — so those two archs were publishing packages built
with 24Kc flags under a 4KEc/mips32 name, exactly the illegal-
instruction risk D-54 called out. They now report the mismatch and
publish nothing until a real target is found. Three of this repo's
mappings pointed a second arch at an SDK another arch already claimed
(`mips_4kec`, `mips_mips32`, `mips64_mips64r2`); one SDK cannot produce
two package archs, and nothing before this check could see that.

## D-58: The guessed arch-to-target table was wrong 11 times out of 28

**Status: accepted.** D-57's arch check made it safe to ask the release
what it actually publishes, so D-58 asked. The `discover` job walks both
release trees and reads `arch_packages` out of every subtarget's
`profiles.json` — the field is the package arch that subtarget's SDK
builds, so inverting it gives the table this workflow had been guessing
since D-51.

Eleven of the twenty-eight mapped archs were wrong, and not narrowly:

| arch | guessed | actually builds | correct target |
|---|---|---|---|
| `arm_arm926ej-s` | `gemini/generic` | `arm_fa526` | `at91/sam9x` |
| `arm_cortex-a7` | `mediatek/mt7623` | `arm_cortex-a7_neon-vfpv4` | `mediatek/mt7629` |
| `arm_cortex-a7_vfpv4` | `realtek/rtl930x` | `mips_24kc` | `at91/sama7` |
| `arm_cortex-a9` | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | `bcm53xx/generic` |
| `arm_cortex-a9_neon` | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | `imx/cortexa9` |
| `arm_cortex-a9_vfpv3-d16` | `bcm53xx/generic` | `arm_cortex-a9` | `mvebu/cortexa9` |
| `arm_xscale` | `ixp4xx/generic` | `armeb_xscale` | `kirkwood/generic` |
| `i386_pentium-mmx` | `x86/generic` | `i386_pentium4` | `x86/geode` |
| `mips_4kec` | `ath79/generic` | `mips_24kc` | `realtek/rtl838x` |
| `mips_mips32` | `ath79/generic` | `mips_24kc` | `bmips/bcm6368` |
| `mips64_mips64r2` | `octeon/generic` | `mips64_octeonplus` | *(none)* |

Three of them were shipping something categorically different from the
name on the package: `arm_cortex-a7_vfpv4` was a MIPS binary, `arm_xscale`
was big-endian ARM, and `arm_cortex-a9`/`arm_cortex-a9_vfpv3-d16` were
each other's. The readelf checks could not see any of it — they only
cover `aarch64_*`, `mips_*`, `mipsel_*` and `x86_64`, and every one of
these is an `arm_*` or `i386_*` arch.

D-57's own guesses scored 2 of 4: `mipsel_mips32` → `bcm47xx/generic`
and `riscv64_generic` → `sifiveu/generic` were right; `malta` was not a
lucky memory but a wrong one, since no release in either line publishes
a malta target at all.

Two archs differ between the release lines and are mapped to the release
that has them, letting D-56's 404 handling skip the other:
`mips_4kec` (`realtek/rtl838x`, 24.10 only) and `riscv64_generic`
(`sifiveu/generic`, 25.12 only).

`mipsel_24kc_24kf`, gated since D-53 for want of a candidate, is
`pistachio/generic`. That leaves two archs with no mapping anywhere:
`mips64_mips64r2` and `mips64el_mips64r2` are declared by no published
subtarget in either release, so neither release SDK can build them.
(They are built from snapshots instead — see D-60, which also corrects
this entry's original claim that no router could install such a
package.)

## D-59: The arch check was a no-op, and two mislabeled packages shipped

**Status: accepted.** D-57 added a check comparing the SDK's own
`CONFIG_TARGET_ARCH_PACKAGES` against the arch the config is named for,
and D-58 relied on it to handle archs that exist in only one release
line. The first full run with both in place shows it never fired: the
SDK's `.config` does not carry that symbol, `sdk_arch` came out empty,
and `[ -n "$sdk_arch" ]` turned the mismatch test into a silent pass.

Two packages in run 85 are wrong as a result:

- `mips_4kec / 25.12.5` — `realtek/rtl838x` moved to `mips_24kc` in
  25.12, so this is a `mips_24kc` binary in a package labeled
  `mips_4kec`. It should have skipped.
- `riscv64_generic / 24.10.4` — 24.10 calls the arch `riscv64_riscv64`
  and renamed it to `riscv64_generic` only in 25.12. The binary is
  right, but a 24.10 device's opkg looks for `riscv64_riscv64` and will
  refuse a package labeled `riscv64_generic`.

Both are exactly what the check was written to prevent, and both are
per-release differences of the kind D-58 said it would delegate to it.

The check now reads `staging_dir/target-<arch>_<libc>` when `.config`
yields nothing — every SDK has that directory — normalizing its `+`
separators to the `_` the package arch uses. It also prints the arch it
found and which source it came from, and warns when neither source
yields one instead of continuing quietly. No mapping changes are needed:
with a working check, `mips_4kec` on 25.12 and `riscv64_generic` on
24.10 skip on their own.

The wider lesson is the one D-58 already paid for once: a check whose
failure mode is silence is indistinguishable from a check that passes.
Both of these had been reported here as protections that were working.

### D-59a: the working check then rejected mips64_octeonplus

The fix above did what it was meant to -- `mips_4kec / 25.12.5` and
`riscv64_generic / 24.10.4` both skipped in run 87, the latter reporting
`SDK sifiveu/generic builds package arch 'riscv64_riscv64', not
'riscv64_generic'` after 83 seconds instead of building for ten minutes
-- but it also cost two packages that had been building correctly.

`staging_dir` spells the MIPS64 ABI into the path while the package arch
omits it: octeon's directory is `target-mips64_octeonplus_64_musl`
against a package arch of `mips64_octeonplus`, so stripping only the
libc left `mips64_octeonplus_64` and the comparison failed. Dropping a
bare `64` token instead is not an option -- it would eat the one in
`x86_64` -- so the comparison accepts one trailing `_64`/`_32`/`_n32`
when the value came from `staging_dir`, and nothing extra when it came
from `.config`.

Both the defect and its cause were visible only because the check now
prints the arch it found and where it read it from. That line is the
reason this took one run to diagnose rather than a bisect.

## D-60: The last two archs come from snapshots, not from a static toolchain

**Status: accepted.** D-58 said no router could install a package for
`mips64_mips64r2` or `mips64el_mips64r2`. That conflated two separate
things: no release publishes an SDK for those archs (true, and the
reason nothing built them here), and no device accepts such a package
(false). opkg installs a package whose `Architecture` matches what the
device's own firmware was built for, and an image built from source for
one of those archs lists exactly that in its arch list. The audience is
people running self-built images — which is precisely who the Go-era
packages for these archs served.

That made the next question worth asking before reaching for a
static-musl toolchain and building the five dependencies from source:
does an SDK for them exist somewhere other than the release lines? It
does. Snapshots build far more targets than releases, and the discovery
job — extended to walk `/snapshots/targets` alongside both release
trees — found both:

| arch | 24.10.4 | 25.12.5 | snapshot |
|---|---|---|---|
| `mips64_mips64r2` | — | — | `malta/be64` |
| `mips64el_mips64r2` | — | — | `malta/le64` |

D-57's original guess of `malta/be64` and `malta/le64` was therefore
right about the target and wrong only about the tree: malta is alive,
just not published in any release line.

These two archs build from the snapshot SDK and produce `.apk`, since
snapshots follow the apk line. The matrix gives them a single snapshot
job each rather than two release jobs that would only 404
(`SNAPSHOT_ONLY_ARCHS`). The snapshot URL carries no version and cannot
be pinned, so these packages move with the tree — acceptable here, since
no release firmware has these archs to install them onto in the first
place.

The same run settled the other two per-release cases, which look alike
and are not:

`mips_4kec` is absent from 25.12 *and* from snapshots. It is genuinely
retired: `realtek/rtl838x` declared it in 24.10 and builds `mips_24kc`
in 25.12, so the devices that used it are served by the `mips_24kc`
package this workflow already builds. Nothing is missing, and forcing a
`mips_4kec` build on 25.12 would produce a copy of that package under a
name no opkg will ever ask for — the mislabeling D-59 fixed.

`riscv64_generic` is present in snapshots, confirming that 24.10's
`riscv64_riscv64` is a rename rather than an absence — but a rename with
consequences. 24.10's riscv64 devices (`d1`, `sifiveu`, `starfive`) ask
opkg for `riscv64_riscv64`, and no config carried that name, so they had
no package at all. `config/openwrt/riscv64_riscv64.config` closes that
gap: both names map to `sifiveu/generic`, and the arch check skips
whichever name the release line does not use.

## D-61: A skipped job is a failure unless the absence is a known one

**Status: accepted.** Every way of not producing a package used to end
in `exit 0` with `sdk_available=false`: an unmapped config, a 404 on the
SDK, an arch mismatch, an arch that could not be determined at all. The
job went green and the run went green, so a package missing from a
release looked exactly like a package that was never meant to exist.
That is how D-58's eleven wrong mappings stayed invisible for as long as
they did, and it is the wrong default for a workflow whose output is
release assets.

Only three absences are legitimate, and they are now named explicitly:

| arch | line | why |
|---|---|---|
| `mips_4kec` | 25.12 | retired; `realtek/rtl838x` builds `mips_24kc` there, and those devices are served by that package |
| `riscv64_generic` | 24.10 | the arch is called `riscv64_riscv64` on that line |
| `riscv64_riscv64` | 25.12 | the arch is called `riscv64_generic` on that line |

Those three report a notice and skip. Everything else now fails the job:
a 404 for an arch that is not a known absence, an arch mismatch outside
that list, an undeterminable arch (shipping unverified is the D-59
failure), and a config with no mapping at all -- every config in
`config/` has one, so hitting the default case means a config was added
without it.

This also answers the release question that prompted it: with skips
loud, a run that stays green has produced every package it was supposed
to, and a missing arch cannot reach a release quietly. The expected
asset count is 68 -- 7 Entware, 59 from the release lines, 2 from
snapshots -- against 71 jobs and the three absences above.

## D-62: Port the Keenetic RCI interface-alias lookup that Phase 9 scoped out

**Status: accepted; closes the last deferred parity item.** Phase 9's
parity checklist recorded the Keenetic RCI friendly-name lookup
(Go's `internal/interfaces/keenetic_router_specific.go`,
`GetIfaceAliases`) as "deliberately deferred", on the reasoning that it
"needs an actual Keenetic router's RCI service to develop and verify
against safely; this sandbox has none".

**That reasoning was wrong, and the checklist even contained the
evidence against it**: the same paragraph noted that Go's own test
covers this lookup "with a mocked HTTP server". Go never needed a real
router to develop or test it either. Deferring on hardware grounds was
therefore not a real constraint — the identical mock-server approach was
available the whole time, and is what this entry uses.

**The cost was user-visible.** On real `entware_kn` hardware the WebUI's
interface picker listed bare kernel names (`nwg0`, `nwg1`) where the Go
build showed the Keenetic labels the user actually set (`Home VPN`).
`mt_iface_info_t.name` was hardwired empty on every platform. This was
reported from a live router, not caught by any test here — a reminder
that "matches Go's default-platform behavior" is not the same claim as
"matches Go on the platform the feature exists for", and the checklist
conflated the two.

**Implementation** (`src/interfaces/keenetic_rci.c`, new). Preserves
Go's protocol exactly, because RCI's shape is not negotiable:

1. `GET /rci/show/interface` → object keyed by RCI interface id, values
   carrying `description` and `interface-name`. Non-object values are
   skipped per entry, mirroring Go's `json.Unmarshal`-error `continue`.
2. One batched `POST /rci/` carrying an array element per interface id.
   The response is an array matched back **positionally** — RCI does not
   echo the id, so this ordering coupling is the contract's most
   breakable point and gets its own unit test.
3. Alias selection: trimmed `description`, else trimmed
   `interface-name`, else skip; also skip when the result equals the
   system name (nothing there the user isn't already seeing).

Trimming is applied both at parse time and again in
`mt_kn_build_aliases`. That looks redundant but is deliberate: Go
trimmed at the alias-building step, and keeping it there makes that
exported function correct on its own rather than only when fed by this
file's own parsers. A unit test asserting a whitespace-only label is
what surfaced the difference.

**Platform gating** matches Go's build tags rather than inventing a new
mechanism: the module compiles everywhere, but `mt_kn_get_iface_aliases`
performs RCI calls only under `-DMT_ENTWARE_KN` and otherwise returns an
empty set without touching the network — the direct analogue of Go's
`DummyRouterSpecificAPI` under `!entware_kn`. `mt_app_list_interfaces`
treats any failure as "no aliases" and logs at debug, exactly as Go's
`interfaces.List` did; an unreachable RCI must never fail the interface
list itself.

**Hardening beyond Go**: a 1 MiB cap on an RCI response body. Go had
none. This is a local, trusted endpoint, but an unbounded read into a
router's small RAM is a worse failure mode than losing aliases.

**Verification**: `tests/unit/test_keenetic_rci.c` — 11 cases porting
Go's own test assertions (batch request shape, positional matching,
`description`/`interface-name` precedence) plus the edges Go's table
left implicit (response/request length mismatch in both directions,
whitespace-only and self-equal labels, malformed and non-object
payloads, empty list short-circuit). The libcurl transport itself,
which unit tests do not reach, was exercised under ASan+UBSan against a
stub RCI server: aliases resolved correctly, the self-equal entry was
skipped, and an unreachable endpoint degraded to `MT_ERR_UPSTREAM` with
an empty set and no leak. Both the default and `ENTWARE_KN=1` builds
pass the full suite; `make static_analysis` is clean (the two cppcheck
notes on this file are `style`-category, which this project's gate does
not enable, and one of them applies equally to the pre-existing
`write_cb` in `subscriptions/fetch.c`).
## D-63: Netfilter table writes on `_kn` move to a committer thread that restarts rather than reports

**Status: accepted.** Keenetic firmware's own userspace netfilter
implementation replaces a table whole and atomically, then runs the
`netfilter.d` hook, which POSTs `/api/v1/system/hooks/netfilterd`.
Answering that hook the way the Go original did — commit in place,
chain by chain, on the thread handling the request — races with the
firmware's next rewrite: some of what was written is gone before the
rest lands, and the iptables errors that produces come back as an HTTP
failure the caller can do nothing with.

On `-DMT_ENTWARE_KN` builds the write is owned by a dedicated thread
(`netfilter/committer.c`, `nfcommit.h`):

- The hook handler calls `mt_nfcommit_request()` and returns. It never
  waits and never reports: requests arriving during a rebuild fold into
  a single following pass, so one firmware rewrite (one event per table)
  costs one rebuild rather than three.
- A request arriving mid-write aborts it. The cancellation token
  (`cancel.h`) is polled by `mt_ipt_commit` between transfers and joins
  the `poll()` set in `executable_real.c`, so a raised token also kills
  the running `iptables-save`/`iptables-restore`. `iptables-restore`
  applies each table in one `setsockopt` at its `COMMIT` line, so a
  killed child leaves whole tables applied or not applied.
- A pass drops every chain and jump of ours from the kernel *first* and
  only then stages the port remap and each enabled group, writing the
  result in one commit per family. The outcome therefore depends only on
  the current group set, never on what an aborted pass left behind.
- Nothing fails outward. `MT_ERR_CANCELED` and `MT_ERR_AGAIN` (raced
  writes, classified from iptables' stderr in `executable_real.c`) are
  logged at debug and retried immediately; anything else is logged at
  warn and retried with a growing backoff.

Other platforms keep committing in place: nothing rewrites the tables
there, and the in-place path stays both correct and simpler.

**Relationship to D-19.** D-19 put all netfilter mutation on the loop
thread and gave `mt_ipt_t`/`mt_ruleset_t` no internal locking. That
premise is narrowed, not abandoned: the committer thread only ever
*reads* the ruleset registry, and the loop thread remains its only
writer. The `nf_mu` mutex in `api/app.c` (recursive, taken by every
mutating `mt_app_*` entry point and by the rebuild) exists so the
registry cannot be rewritten, or an iptables engine driven, while a
rebuild is in flight. The DNS hot path is the other reader and stays
lock-free exactly as D-17 requires — two readers never conflict.
A mutating entry point asks the committer to restart *before* taking the
lock, so the loop thread waits for an abort rather than for a whole
rebuild, and the pass that follows picks up the change it just made.

**Tested by** `test_cancel.c` (token semantics), `test_nfcommit.c`
(request coalescing, mid-write abort and restart, retry-until-quiet,
shutdown while a write is parked) and `test_nfrebuild.c` (recovery after
a simulated firmware wipe, no duplicated jumps across repeated passes,
removal of chains left by a previous run, other writers' rules
preserved, aborted pass writes nothing and converges on the next one).

## D-64: Parse ipset nesting flags from the raw netlink attribute type

**Status: accepted.** `mnl_attr_get_type()` returns a normalized type
with `NLA_F_NESTED` and `NLA_F_NET_BYTEORDER` removed. The original C
list parser tested `NLA_F_NESTED` on that normalized value, so it
discarded every `IPSET_ATTR_ADT`, `IPSET_ATTR_DATA`, and
`IPSET_ATTR_IP` container. Both IPv4 and IPv6 sets consequently appeared
empty even when the kernel dump contained entries.

Nesting is now detected through the raw `struct nlattr::nla_type` field,
while attribute identity continues to use libmnl's normalized accessor.
`test_ipset_nl_parser.c` constructs real nested libmnl messages for both
families, including CIDR and network-byte-order timeout attributes, and
passes them through the production parser without requiring an ipset
kernel module.

The committer cancellation token is also serialized across raise/clear.
That preserves its documented invariant under concurrency: the fd is
readable whenever the atomic flag is raised. Condition-variable
initialization no longer destroys an uninitialized attribute object on
its rare failure path.

## D-65: Do not put diagnostic pragmas between libmnl loop macros and their bodies

**Status: accepted after live Entware validation.** An on-device comparison
against the original Go daemon on `mipsel-3.4_kn` found three failures that
had appeared unrelated:

- all 14 enabled groups reused the first fwmark/table instead of allocating
  distinct values;
- a ruleset sync read every existing IPv4/IPv6 ipset member as an all-zero
  address, so obsolete members were not deleted;
- link-up notifications could not recover the interface name and therefore
  did not dispatch.

The common cause was the placement of `#pragma GCC diagnostic pop` between
`mnl_attr_for_each*()` and its following compound statement. The pragmas
were intended only to suppress the macro's pointer-difference-to-int
`-Wconversion`, and host GCC 13 executed the loops normally. The Entware
GCC 8.4 MIPS binary instead contained an empty loop over every valid
attribute and ran the apparent body only after `mnl_attr_ok()` had failed.
Disassembly and a live netlink trace both confirmed that control flow.

The production code no longer uses libmnl's iteration macros.
`nlattr_iter.c` walks aligned attributes with `size_t` lengths and explicit
structural validation; the ipset parser, rtnetlink gateway and mark/table
scanners, and link watcher all share it. This is a platform-independent
fix: those sources are compiled for every Entware and OpenWrt target, not
only `_kn`.

`test_nlattr_iter.c` covers complete top-level and nested traversal plus
truncation. `test_ipset_nl_parser.c` now also uses the address-attribute
shape observed in the kernel LIST reply (no `NLA_F_NET_BYTEORDER`) and
checks multiple entries in one dump. The real router remains the final
verification for allocator, sync deletion, and link reconnect behavior.

The same audit found that main constructed the port-remap chain as literal
`DNSOR`, bypassing the frozen `<ChainPrefix>DNSOR` contract. Port remap now
accepts the configured prefix and owns the concatenation; a custom-prefix
unit test prevents default-only coverage from hiding this again.


## D-66: Repair PR1 lifetime/reload regressions and remove blocking subscription I/O

**Status: implemented; supersedes D-33's event-loop blocking limitation.**
Review of PR1 at `7cf213665291c33dc691318af5db79934d2add6a` found eight
regressions. The fixes preserve YAML names, success JSON, rule matching and
netfilter naming; frozen golden files are deliberately unchanged.

- Subscription replacement builds its array through the same model allocator
  as append. Allocating exactly N pointers was incompatible with the append
  allocator's minimum-eight/geometric capacity assumption.
- Deleted epoll watches are invalidated immediately and freed only after the
  current ready batch. One-shot timers are reclaimed after their callback,
  unless the callback already removed them. Fired timerfds no longer leak.
- TCP DNS clients receive a request-read deadline at accept, replaced by a
  separate processing deadline after the complete request (including local
  PTR replies). Proxy destruction also closes outstanding exchanges.
- Reload parses an overlay onto a deep copy of CURRENT app settings, not
  defaults, and publishes stored settings as well as live flags. Auth, skin
  and interface filtering read the updated config. Missing/null subscriptions
  clears them; absent groups preserves groups. Failed parsing preserves state.
  Already-created listeners, helper prefixes and port-remap retain startup
  settings until restart, as in Go. Owned startup strings prevent dangling
  references when the reloadable config strings are replaced.
- DNS upstream accepts a hostname through getaddrinfo. Resolution currently
  occurs once at startup, selecting the resolver's first address; automatic
  re-resolution/address failover is not implemented. Listener/upstream address
  families are independent; tests exchange actual DNS traffic through localhost.

Production subscription network I/O now uses two workers and a bounded queue
of 32 accepted jobs (including active/completed jobs). Workers only see owned
URL/body buffers; a nonblocking pipe returns completions to the event loop.
All subscription, ruleset, cache and HTTP mutation remains on the loop owner.
Blocking compatibility entry points remain for embeddings/tests, not daemon
callbacks. Synchronous sync rejects an already-pending async sync; synchronous
batch sync skips pending entries.

Each subscription incarnation has a nonserialized revision. Delete/recreate,
PUT replacement or SIGHUP during a fetch invalidates the old completion. A
second sync of an already-pending subscription and an obsolete completion
return HTTP 409 rather than overwriting newer edits; saturation returns 503.
The normal 200/400/404/502 response shapes are preserved. Deferred responses
own a weak connection token and can finish safely after peer disconnect or
server destruction. Destroy the fetcher before app/handler contexts; shutdown
cancels queued transfers, joins workers, and completes each accepted job once.

**Intentional scheduling difference:** auto-update applies and saves each
successful changed subscription on completion, rather than atomically applying
one fully downloaded batch. A failed source no longer delays unrelated sources.
Rebuild rollback remains per application. Queue overflow is retried next minute
with a rotating starting index so permanently failing early URLs cannot starve
later subscriptions. Fetch redirect policy, per-hop timeout and body cap are
unchanged. Disk saves, parsing and netfilter apply are still synchronous; this
change removes network waiting, not every possible source of loop latency.

Regression coverage is in `test_migration_regressions.c` and
`test_async_subscriptions.c`; the existing subscription HTTP API tests now run
through the production asynchronous path. Coverage includes arbitrary replacement
sizes followed by append, same-batch event invalidation, timerfd reclamation,
partial TCP requests, DNS cleanup and hostname exchange, reload/save/restart,
slow upstream served by the same event loop, stale fetches, duplicate sync,
queue saturation/fairness, shutdown and deferred response after server teardown.
Physical Keenetic/OpenWrt integration remains a separate on-device validation.

Static-analysis follow-up separates list unlinking from resource release in
HTTP/DNS bulk teardown, making the lifetime invariant explicit to Clang's
analyzer. The RCI size limit is explicitly widened at the size_t comparison;
port-remap prefix copying includes its terminator before appending the suffix.
These changes do not alter the wire or configuration contracts.


## D-67: Indexed large-rule processing and compact saves (groups and subscriptions)

**Status: implemented; large-list regression tests added.** The reported list
had 33,048 lines / 572,927 bytes and downloaded on the router in 0.398 seconds.
The device-side parsing/apply time was not profiled, and its anomalous local
403 response is not claimed resolved. Independent source review and local
benchmarks established quadratic loops and an incompatible 1 MiB request cap.

### Shared processing

- An owning byte-key index replaces linked-list deduplication, repeated ID
  searches, subscription refresh/sameRules scans and subnet reconciliation.
  First-occurrence ordering and existing ID/type/enable/TTL semantics remain.
- Group request arrays traverse cJSON's linked list once, with a single ID
  index per baseline. Rule replacement uses the geometric model allocator so
  a subsequent append is safe at arbitrary sizes.
- Namespace trie nodes use geometric child vectors and hash indexes only above
  eight children. Tiny nodes do not allocate full hash tables. YAML group-ID
  conflict checks and app group insertion are indexed, including after restart.
- Parser and group-import IDs use per-operation random batches, checked for
  entropy failure, zero/duplicate IDs and bounded retry; no shared PRNG state.
- Bulk group PUT stages and validates all input before changing live state,
  publishes one DNS snapshot, and reuses identical runtime groups when ordering
  is unchanged. Reordering deliberately rebuilds to preserve ordering semantics.
  Apply failure retains the old configuration and attempts runtime rollback;
  netfilter rollback itself is best effort and errors are logged.

### Scheduling and resource bounds

- Production subscription workers perform fetch **and parse**, returning owned
  detached rule arrays. Live model reconciliation/application and HTTP response
  completion remain loop-owned. Existing revision checks reject obsolete syncs.
- At most four fetch/parse jobs are admitted, including queued and completed
  results, inside the existing two-worker/32-job pool. Lists remain limited to
  8 MiB downloaded bytes, now also 100,000 unique rules / 4,096 bytes per line.
  Cancellation and parser/entropy/OOM errors do not publish partial lists.
- Rule mutation routes (POST/PUT under groups or subscriptions) allow up to
  16 MiB input; unrelated endpoints retain their 1 MiB cap. Each listener has
  a 32 MiB aggregate in-flight body reservation, released on keep-alive reset,
  errors, disconnect or timeout. Large TCP requests authenticate before body
  allocation. Excess size is JSON 413; budget exhaustion is JSON 503.
- Disk serialization, matcher publication and kernel/netfilter application are
  still synchronous. No guarantee of constant-time saves or router latency is
  made; regex-heavy lists and slow storage/kernel operations have their own cost.

### Additive API and matching frontend

- `GET /subscriptions/rules?summary=true` returns `{count, types}`. The original
  full-preview response remains available when the parameter is absent.
- `POST /subscriptions?fetch=true` accepts URL/settings without `rules` or a
  client ID, fetches/parses before inserting and saving, and returns canonical
  `{subscription}`. Fetch failure leaves no phantom subscription. Failed initial
  persistence attempts removal and returns an explicit error (including rollback
  failure). The old explicit-rules POST remains supported.
- PUT accepts `ruleChanges` mutually exclusive with an explicit rules array.
  It clones the current rules and applies only the named changes. Each change
  includes previous field values; a stale/deleted ID or stale value is rejected
  before publishing any new state. An empty change list is a metadata-only edit.
  Group changes include name/type/pattern/enable and a `previous` object;
  subscription changes include type/enable, pattern identity and prior values.
- The frontend uses compact changes for subscription saves and group edits
  without membership/order changes. New/imported/reordered groups use the full
  ordered representation under the bounded larger route limit. Group saves use
  the canonical response (including server-assigned IDs), not client-only IDs.
- Preview never echoes tens of thousands of rules back during subscription
  creation. Source content can change between preview and creation; the creation
  response is authoritative. No unbounded preview cache is introduced.
- Failed persistence returns an error instead of falsely acknowledging a save.
  For edits/deletes the in-memory state may already have changed; it is explicitly
  not claimed persisted and the UI keeps its dirty state for retry. The legacy
  success JSON of existing endpoints and YAML schema are unchanged.

### Validation and reproducibility

`make test` and an ASan+UBSan build of the complete C unit suite pass locally.
The tests exercise 50k-rule parsing/refresh, stable unique IDs, entropy failure,
limits, cancellation, bounded workers, HTTP preview/create/edit/persistence,
50k domain-group bulk/strict/compact writes, stale edits, invalid-batch retention,
append after replacement, and HTTP body budget/keep-alive cleanup. Frontend
check/build pass locally; frontend unit/e2e tests are included for GitHub CI.
Local Chromium cannot navigate localhost in this environment, so local e2e
success is not asserted. Golden config/matcher/subscription/DNS/cache checks
passed; the local full daemon HTTP differential test cannot start without
iptables, and remains delegated to the existing GitHub workflow.

See `tools/bench/large-rules.c` / `make bench_large_rules` and
`docs/c-rewrite/large-rules-benchmark.txt` for reproducible CPU measurements.
These are Linux x86_64 host measurements, **not Keenetic/Entware benchmarks**.
Real SDK package validation and on-router netfilter timing remain separate.

## D-68 — Distinguish runtime apply from failed persistence for retry (2026-10-01)

The sparse edit preconditions introduced in D-67 deliberately remain strict.
A failed disk write after a successful bulk PUT used to leave the editor's old
baseline in place, so a retry replayed already-applied changes and received 409.

Only a bulk group/subscription PUT whose apply phase completed successfully and
whose subsequent configuration save failed now returns HTTP 500 with additive
fields `code: "PERSISTENCE_FAILED"`, `applied: true` and the canonical `groups`
or `subscriptions` collection. The original `error` field is preserved. A full
group import may generate IDs, so the response includes those server IDs rather
than asking the editor to guess them. Validation/apply/network failures do not
carry this acknowledgement. Existing success responses and YAML are unchanged.

The frontend accepts only a well-formed, explicitly marked HTTP 500 response,
advances its optimistic rule baseline to that acknowledged runtime snapshot,
and keeps a separate `persistencePending` flag. Save remains available and the
unload warning stays active even without further edits. Retrying sends compact
metadata/empty changes; another edit uses the applied baseline. A generic 500,
409, malformed acknowledgement or transport error does not move the baseline.
No error is silently changed to success; the pending flag clears only after a
successful save response. Concurrent stale edits remain rejected.

Regression coverage includes repeated disk errors, retry followed by YAML reload,
canonical group IDs on failed full imports, and browser state for both editors.
The browser tests separate runtime/disk fixtures and enforce preconditions;
real HTTP handlers and filesystem failures are covered by C tests.

## D-69 — Package identity mt-c, mutually exclusive with magitrickle (2026-10-01)

The user requested a distinct installed package name to prevent accidental
co-installation with upstream (or earlier C builds named `magitrickle`). Root
packaging and both SDK recipes now emit `mt-c`. IPK metadata declares
`Conflicts: magitrickle`; APK uses the equivalent negative dependency
`!magitrickle`, and SDK recipes use `CONFLICTS:=magitrickle`. There is deliberately
no `Provides`/`Replaces` or forced installation over another package's files.

CI source archives, package directories, build targets and collected artifact
names follow the new identity. Direct APK inventories and lifecycle hooks use
`mt-c`, while the daemon `magitrickled`, init service, config/YAML paths, WebUI
assets and routing identifiers retain their existing names and contracts.

This is an explicit package migration, not an in-place package-name upgrade.
README requires export/backup outside package-owned directories, stopping the
old service, removing the old package and then installing the matching new
artifact; installation hooks may restart the service. Do not promise config
retention across removal without a backup or automatically remove dependencies.

`python3 tools/tests/package_identity.py` validates real IPK archives built with
synthetic payloads, APK staging/arguments/hooks, and evaluates both SDK package
identity declarations. This is not a real cross-build or an on-router migration
test. The supported target matrix is unchanged.

## D-70 — Reusable routing profiles and ordered local failover (2026-10-07)

User-approved new behavior: Settings manages an unbounded collection of named
profiles; Groups and Subscriptions select a saved profile or a direct interface
with one ordinary select. Root YAML `profiles` owns ordered interface lists;
optional group/subscription `profile` references stable IDs. Existing `interface`
is deliberately retained as a compatibility shadow of the first configured
candidate, never the runtime-active backup. Profile references are authoritative;
missing references are errors, not permission to silently use the shadow. Old
configs remain a valid subset and existing no-profile golden YAML stays unchanged.
Older binaries may drop unknown fields on save; no downgrade preservation layer.

Each profile uses first-available ordering with automatic return and a fixed final
blackhole. Availability is LOCAL link/address/gateway readiness, per IPv4/IPv6,
not a claim about remote peer/Internet health. No probes, strategy/failback field,
nested profiles or implicit global chain are introduced. Link-down/deletion, address and route events reconcile all candidates.
The original five-second retry was replaced with event-driven reconciliation,
kernel-state comparisons and bounded retries only after observed failures.
Routes replace atomically at the same table/metric; blackhole is kept installed.
Own policy tables are excluded from upstream gateway discovery to avoid retaining
stale gateways through the daemon's copied routes. Profile edits preserve the
existing mark/table/ipset, and synchronize compatibility shadows only after apply.

Profile CRUD is additive GET/PUT `/api/v1/profiles`, protected like existing routes.
Referenced deletions are rejected. Validation precedes mutation; profile apply and
config reload retain the old model registry for rollback. Kernel rollback errors
are logged and surfaced through failed apply, not claimed to be fully transactional.
Persistence-after-apply failures use D-68's explicit acknowledgement/retry pattern.
Group export/import carries profile dependencies and remaps ID collisions once for
all consumers. UI drafts survive navigation and selects remain single-purpose.

See `docs/routing-profiles.md` for schema, semantics and limitations. Tests cover
YAML/shadow compatibility, validation/scaling, API/ref integrity, deterministic
reload failure, failover selection, real kernel route transitions in an isolated
namespace, and browser create/select/edit/retry flows. This is not an on-router
Keenetic/OpenWrt test or a remote-health-monitoring implementation.

## D-71 — Diagnose iptables restore errors and remove goto references (2026-10-08)

Status: **accepted**. Keenetic reported repeated `iptables-restore: line N failed`
while netfilter.d committer retries held the netfilter mutex. The old generic
error message did not include the command corresponding to N, so the on-device
root cause cannot be established from historical logs alone. Failed restore
now logs only bounded, escaped context around the reported line and the active
table (no file persistence or full ruleset dump); errors and retries are not
suppressed. Routing reconciliation treats short `MT_ERR_AGAIN` lock contention
as DEBUG, reporting prolonged contention or other errors as WARN, retaining
bounded retries. Cleanup must remove references to managed chains through
both jump (`-j` / `--jump`) and goto (`-g` / `--goto`). Previously it used
a substring search for `-j <prefix>`, which could miss goto references and
produce a permanent `-X` failure on older iptables backends. This intentional
hardening preserves the contract of removing all owned chains without touching
unrelated chains or rules. Regression tests cover goto cleanup with the checked
fake and real kernel netfilter backends, plus bounded failure diagnostics.

## D-72 — Preserve existing user chains under iptables-restore --noflush (2026-10-08)

Status: **accepted**. Kernel logs on Keenetic reported repeat failures at the
`mangle` table's `COMMIT`, including batches deleting owned chains. The
generation layer also had a concrete destructive bug: `mt_ipt_commit()`
emitted `:CHAIN - [0:0]` for EVERY chain with pending operations. With
`iptables-restore --noflush`, declarations of **already existing**
user-defined chains still flush their contents. This could erase unrelated
firmware rules while trying to delete one jump into an `MT_` chain, or make
a later `-D` fail because its target rule had already been flushed.
Only chains absent from `iptables-save` now receive declarations; existing
chain overrides and removals use their already explicit `-F` or `-D`.

In addition, a stale managed chain may be referenced by another stale
managed chain. Interleaving `-F A; -X A; -F B; -X B` can fail if B
references A; `mt_ipt_commit()` now writes all `-X` commands only after
the full set of removes/flushes (preserving the original order among all
other commands). These changes do not affect user rules unrelated to
mt-c, restore package artifacts or alter the profile selection algorithm.
Regression tests cover foreign user-chain preservation, managed-chain
dependencies and repeat recovery with both in-memory and real kernel
legacy/nft backends.

The exact historical `COMMIT` failure cannot be attributed exclusively to
this cause without the failed full transcript and device-side kernel state.
The bug is independently actionable and consistent with the observed failure
shape.

## D-73 — Explicit routing priority for groups and subscriptions (2026-10-09)

The user requested a common numeric priority that determines the outgoing
interface when a destination matches several enabled groups/subscriptions.
The field is `priority`, an integer from 1 through 1000, with defaults 300 for
user groups and 100 for subscriptions. Larger numbers win. This intentionally
changes overlap behavior for legacy configurations: a group now takes
precedence over a subscription with the default values, including a domain
group whose resolved address lies inside a subscription's broad cloud subnet.

The model allocators supply defaults; missing YAML/create-request fields use
them. Existing-object updates that omit priority preserve the current value,
including compact rule-delta updates. JSON responses and YAML saves explicitly
include it. New-field validation rejects nonnumeric, noninteger, null, duplicate
and out-of-range values before mutation; YAML requires an integer scalar. The
YAML prepass only validates priority inside otherwise inspectable mappings so
unrelated malformed legacy inputs retain their old validation/overlay timing.
No config-version bump, endpoint rename, mark/table allocation change, or
change to rule IDs and netfilter names is involved.

Ordering belongs in the packet path. DNS still populates every matching set:
choosing just one DNS match would ignore subnet rules and discard the fallback
membership needed when a winner is disabled or reprioritized. Subscription
runtime groups explicitly copy their subscription's priority. Group equality
includes priority so compact bulk changes cannot reuse the old model silently.
Reload cloning also preserves priority when the YAML overlay omits groups.

Priority selects the matching group/subscription's route table; D-70's profile
ordering independently selects an available interface within that table.
Reconfiguring a profile preserves its consumer's priority. An exhausted profile
retains its terminal blackhole and does not fall through to a lower-priority
group or subscription.

Each group's mangle chain sets MARK and saves CONNMARK without terminating the
parent chain. Therefore the last matching jump wins; all group/subscription
jumps are ordered together in **ascending** priority order. The new
`mt_ipt_append_ordered` patch operation also moves already-present rules, unlike
the existing Append/InsertUnique behavior. It compares desired order with the
kernel snapshot, removes only exact registered ordered rules when needed, and
reinstates the sorted subset at the first occurrence of each unique original
managed rule. Duplicate slots are discarded; extra new rules extend the last
managed slot, or append when no such slot exists. Foreign rule contents and
relative order are preserved. Ordinary append/insert/delete semantics remain
unchanged, and staged-state reset/rebuild uses the same reconciliation.

In the original 2026-10-09 implementation, equal priorities used lexical
chain-name order (the common configured prefix plus the canonical
eight-character hex ID), so the higher ID won regardless of source type.
This original rule was deterministic, not dependent on enable/list order,
and is superseded by the 2026-10-10 group-over-subscription amendment below.
Filter ACCEPT and NAT MASQUERADE rules retain their behavior; IPv4, IPv6,
blackhole routing, and the Keenetic-critical CONNMARK save rule are preserved.

The frontend binds its existing edge editor to the model, rejects invalid
values, bounds the increment/decrement buttons, defaults old responses/imports,
and includes priority in compact saves. Applied-but-not-persisted responses
continue using D-68's retry path and retain the chosen priority. Priority edits
remain local until the existing Save action is used.

Regression coverage includes API/config validation, defaults, reload, compact
priority-only updates, subscription refresh/runtime synthesis, ordered patch
updates with foreign/duplicate rules, stable ties, cancellation/failed-write
retries, and generated packet/connection marking for overlapping host/subnet
membership in both families. The latter uses an in-memory netfilter harness;
it does not establish real-kernel or on-router validation. Existing golden
YAML/HTTP fixtures are extended only with the expected default priority fields.

### D-73 amendment — Priority maximum is 999 (2026-10-10)

At the user's request, the accepted integer range is now **1 through 999**,
replacing D-73's original upper bound of 1000. Frontend validation, increment
controls, JSON API validation, YAML loading, and the API reference use this
same limit. Explicit 1000 is rejected without mutation, just like larger
values; existing configurations containing it must be edited to the accepted
range before loading. Defaults and omitted-field behavior remain as defined
in D-73; this range-only change did not alter the ordering/tie policy at the
time. The subsequent amendment below supersedes the original cross-source
tie rule. Boundary tests cover acceptance of 999 and rejection of 1000,
including preservation of live and persisted state on invalid writes.

### D-73 amendment — User groups win equal-priority ties (2026-10-10)

At the user's request, equal numeric routing priority now favors manually
configured user groups over subscription-derived rule sets. This is an
intentional, observable divergence from D-73's original higher-ID-wins
policy across source types. Precedence for overlapping enabled sets is:

1. The higher numeric `priority` wins (range 1–999), regardless of source.
2. At equal numeric priority, a user group wins over a subscription,
   regardless of either object's ID.
3. At equal numeric priority **within the same source type**, the higher
   canonical eight-character hex ID still wins, as in the original policy.

For example, group(300) beats subscription(300) regardless of IDs, but
subscription(301) beats group(300). This supersedes the original cross-source
tie policy above; the earlier 999-limit amendment did not change ties *at
the time* and must not be interpreted as restoring the previous policy.
Enable order, list order, and service restarts do not change the outcome.

The subscription-to-runtime-group converter carries a runtime-only
`from_subscription` flag; no new YAML/API field is introduced. The effective
iptables order key is `2 * priority + (from_subscription ? 0 : 1)`, sorted
ascending in the shared mangle PREROUTING chain. Because MARK/CONNMARK
updates are non-terminating, the last matching rule wins. If the keys
match, the existing stable chain-name/ID comparison breaks the tie.
Both initial enable and live/rebuild staging calculate the same key.
Stored priority values, defaults (300/100), accepted range (1–999), DNS
set membership, profile failover, and unrelated firewall rules are unchanged.

### D-73 amendment — Reject cross-source ID collisions (2026-10-10)

User groups and subscriptions use the same netfilter chain/ipset naming format
based on their eight-hex-digit ID. A group and subscription with identical IDs
would therefore share a jump target, and the ordered iptables compiler would
deduplicate the two entries before their group/subscription tie-break could
apply. This is not fixed merely by giving them distinct effective priorities.

The IDs of user groups and subscriptions must now be disjoint, irrespective of
configured priority or enabled state. A cross-source collision is rejected with
`MT_ERR_EXIST` during YAML loading, API single-object creation (HTTP 409), and
bulk group/subscription updates (HTTP 409, without changing live state).
Startup also checks programmatically constructed configurations. SIGHUP reload
rechecks after retaining groups omitted from the YAML overlay, before touching
live routes; conflicts leave the previous configuration active. Existing
configurations with cross-source duplicate IDs must assign a new unique ID to
one resource before they can be loaded.

IDs are not silently rewritten; valid existing resource IDs, netfilter
names, and the group's precedence over subscriptions on a numeric tie remain
unchanged. Neither priority values nor the tie-break calculation are changed.

Regression tests in `test_priority_source.c` cover equal-value source
precedence independent of arrival order and IDs, a strictly higher
subscription priority, live reordering, disable/re-enable and rebuild,
and same-source ID ordering with in-memory iptables transports.
