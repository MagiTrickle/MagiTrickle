# Parity checklist — Phase 9 sign-off

Item-by-item verdict against `compatibility-contract.md`'s 11 sections
(Phase 0's baseline). This document is the operative equivalent of the
originating task's "parity checklist (spec §23)" reference: the numbered
master-spec document that phrase points to was the initial task prompt
that kicked off this project and was never itself checked into this repo
as a file, so this checklist is built from what *is* preserved —
`compatibility-contract.md` (the actual, detailed, source-cited parity
contract written in Phase 0) plus every phase's differential/contract/unit
test evidence and `decisions.md`'s 42 prior entries. Where the checklist
below found a real, previously-undetected gap, it says so and links the
fix (not just a rubber stamp).

Verdict legend: **PASS** = verified equivalent (test evidence cited).
**PASS (documented divergence)** = intentionally different, permitted by
the contract's own hardening clause, and recorded. **FIXED** = a real gap
found during this review, now closed. **GAP (matches Go)** = uncovered by
automated tests in *both* backends — not a C-specific shortfall.

## 1. Configuration (YAML)

**PASS.** File location, configVersion gate, overlay semantics, defaults,
duration parsing/serialization quirks, `enable` default-false, field
names, save shape/key order, group color normalization, ID format,
order preservation, unknown-field tolerance, corrupt-YAML handling, live
SIGHUP reload — all covered by Phase 2's Go↔C differential suite
(`tests/differential/run_diff.sh`'s YAML/config corpus, 14 fixture files
incl. legacy durations, bad types, corrupt YAML, unknown fields) and
Phase 2/7's decisions (D-04, D-10). **PASS (documented divergence)**:
save is atomic (tmp+fsync+rename) vs Go's direct `WriteFile` — explicitly
permitted by the contract itself ("improvement... allowed if bytes
identical"), confirmed byte-identical by the same differential suite.

## 2. HTTP API and Unix socket

**PASS.** All routes, status codes, auth-middleware scoping
(`/api/` except `/api/v1/auth`, static paths never authenticated), JSON
error shape, group/rule create defaults, `save=`/`with_rules=` query
semantics, static file serving (dir→index.html, MIME by extension, 404
shapes) — all covered by the HTTP contract differential suite
(`tests/differential/run_http_diff.sh`, expanded through Phases 6-7 to 44
steps, run against both a real Go daemon and `magitrickled-c` over both
HTTP and Unix socket transports; D-26/D-27/D-28/D-29/D-34). **PASS
(documented divergence)**: no request size/connection-count limits added
in the C server beyond what the contract explicitly allows ("C version
may add bounded limits... document as hardening") — the bounded HTTP/1.1
server (D-06) adds only structural bounds (fixed connection slots,
header/body size caps), not behavioral limits that would change normal
responses; confirmed by the same contract suite passing unchanged.

## 3. Authentication

**PASS.** Crypt formats (MD5/SHA-256/SHA-512 incl. `rounds=N`), JWT
HS256 byte-compatibility (same header/claims shape, same
HMAC-SHA256-derived signing key, same 20-year expiry), constant-time
verification — ported and differential-tested (D-23, D-24; JWT
byte-compatibility specifically called out and verified: a token issued
by one backend verifies on the other given the same secret+hash).
**GAP (matches Go)**: no dedicated automated test suite exists for this
area in the *original* Go backend either (`compatibility-contract.md`
itself records "Gap: no tests at all" for source); the C port did not
regress below that baseline — it added real coverage (crypt vectors, JWT
vectors, contract round-trip) where Go had none, which is a net
improvement, not a parity risk.

## 4. DNS proxy

**PASS.** UDP+TCP listeners (dual-stack per config), pktinfo reply source
address, TCP one-query-per-connection framing, single upstream with
per-transport idle pools, global concurrency semaphore, per-request
timeout with silent-drop-on-failure (not silently ignored — deliberately
matching Go's "no response on upstream failure" contract), fake-PTR
synthesis, AAAA-strip-with-recompression, A/AAAA/CNAME-only cache/ipset
population from the Answer section — all covered by the DNS wire/semantic
differential suite (miekg/dns vs the C wire codec, Phase 3) plus fuzz
targets (parser, decompression, TCP framing) and the full-daemon fault
injection soak (Phase 7, D-36). **PASS (documented divergence)**:
`MT_SUB_FETCH_MAX_BODY_BYTES` and similar C-side hardening additions are
subscription-fetch-specific (section 8), not DNS-proxy-specific; no
DNS-path hardening beyond Go's own bounds was added.

## 5. Records cache

**PASS.** Domain→[IP,deadline] with IP-byte dedup and deadline refresh on
re-add, domain→single-alias + reverse index, BFS alias closure, forward
chain walk with cycle guard, TTL = DNS TTL + additionalTTL, 30 s cleanup
pass, self-alias ignored — ported and differential-tested against Go's
`recordsCache` (Phase 4), plus a dedicated soak test confirming flat
memory growth under sustained churn (D-12, D-17, D-18).

## 6. Rules

**PASS.** `domain` (exact byte compare), `namespace` (suffix +
leading-dot form), `wildcard` (IGLOU-EU/go-wildcard-v2-compatible `*`/`?`
semantics), `regex` (PCRE2 replacing regexp2, corpus-proven parity with an
explicit, documented divergence list — D-07), `subnet`/`subnet6` (CIDR
expansion straight to ipset, never DNS-matched, `/0`→two `/1` netlink-bug
workaround preserved), disabled-rule skip, first-match-wins per group —
all covered by the rule-matching differential corpus (Phase 2, ported
verbatim from `models/rule_test.go`) plus the regex compatibility corpus
runner (Go regexp2 oracle vs PCRE2, divergences enumerated and confirmed
to match the documented known set on every differential run since
Phase 1).

## 7. Groups → netfilter mapping

**PASS.** ipset naming (`<prefix><hex-id>_4`/`_6`), type/timeout/replace
semantics, wrong-type-set destroy-and-recreate, flush-on-enable, exact
chain rule text (including the Keenetic-critical CONNMARK save-mark
rule), fwmark/table allocation (first-free ≥ StartMarkTableIndex),
ip rule/route programming (blackhole prio 20, default-via prio 10, the
`blackhole` pseudo-interface), port-remap chain, startup cleanup
(`MT_*`-prefixed chain removal via save/restore diff), `iptables-restore
--noflush` batching with external-flush healing — all covered by
transcript-parity differential tests against the real Go fake-executable
corpus (Phase 5: `tests/differential` iptables suite, byte-for-byte
command parity) plus netns integration where the kernel allows (D-19
through D-22). **GAP, sandbox-specific, not code-specific**: this
session's sandbox has a non-functional `ipset` kernel subsystem (see
D-42) — real ipset create/destroy/add/del round-tripping was validated
via the transcript-parity approach and Phase 5's own netns testing
earlier in the branch's history, not re-verified live during Phase 8/9's
work in *this* particular sandbox instance. No code regression is
implicated; this is an environment gap, named explicitly rather than
silently assumed away.

## 8. Subscriptions

**PASS.** Fetch redirect semantics (301/302 only, ≤5 redirects, loop
detection, non-2xx terminal), parse/refresh/type-detect/dedup keyed by
exact rule text, validation (type whitelist + per-type syntax incl.
regex compile), 1-minute scheduling tick with the exact due-check formula
(`enabled && url && interval>0 && now >= last_check||last_update +
interval`), `last_update`-only persistence, runtime rule-set key/naming
derivation, rebuild-visible-churn semantics — all covered by Phase 7's
subscription differential/contract/fault-injection work (D-31 through
D-36), including a real memory leak found and fixed during that fault
injection soak (D-36) and a real Phase-6 DNS-snapshot regression found
and fixed while wiring subscription runtime rule sets (D-32). **PASS
(documented divergence)**: `MT_SUB_FETCH_MAX_BODY_BYTES` (8 MiB) is new
hardening (Go's `io.ReadAll` has no body-size bound at all) — explicitly
permitted by the contract and migration-plan.md's own Phase 7 line item,
confirmed to not change normal-size fetch behavior by the fetch
differential suite.

## 9. Interfaces / platform

**FIXED** (this Phase 9 review): `GET /system/interfaces`'s
platform-specific ignored-interfaces list (`constant.IgnoredInterfaces`,
empty by default, a fixed Keenetic virtual-interface list — `ezcfg0`,
`ra0`-`ra15` — under Go's `entware_kn` build tag) was scaffolded in
Phase 6 (`app.h`'s `mt_app_list_interfaces` comment explicitly deferred
it to "Phase 8") but never actually wired — a loose end that survived
Phases 7 and 8 undetected until this checklist walk caught it. Fixed:
`src/backend-c/src/api/app.c` now carries the same list gated behind a
new `-DMT_ENTWARE_KN` compile flag (`ENTWARE_KN=1` in the C Makefile,
derived from `PLATFORM`/`TARGET` in the root Makefile exactly like
`GO_TAGS`' own `entware_kn` derivation), verified with a dedicated unit
test (`test_system.c`'s `ignored_interfaces_list`, asserting both the
empty-list default and the Keenetic list under the flag) and a full
`-Werror` build in both modes. `PointToPoint`-flag filtering and
`showAllInterfaces` bypass semantics were already correct (unaffected).

**PASS (gap closed after Phase 9 — see D-62)**: the Keenetic RCI
friendly-name lookup (`GetIfaceAliases`) was left as a stub by this
review, on the reasoning that it "needs an actual Keenetic router's RCI
service to develop and verify against safely". That reasoning was wrong
in an important way: Go's own test never used a real router either — it
used a mocked HTTP server — so the same coverage was achievable here all
along. The consequence was user-visible on real hardware: the WebUI's
interface picker showed bare kernel names (`nwg0`) instead of the
Keenetic labels (`Home VPN`) the Go build displayed.

Now implemented in `src/interfaces/keenetic_rci.c`
(`mt_kn_get_iface_aliases`) and wired into `mt_app_list_interfaces`,
preserving Go's exact semantics: two RCI calls (`GET
/rci/show/interface`, then one batched `POST /rci/`), positional
response matching, `description` → `interface-name` → skip alias
selection, and skipping entries whose label equals the system name. The
default/non-`entware_kn` path still yields an empty set without touching
the network, matching Go's `DummyRouterSpecificAPI`. Verified by
`tests/unit/test_keenetic_rci.c` (11 cases porting Go's own test plus
the length-mismatch/trimming/skip edges its table left implicit) and by
an ASan+UBSan run of the real libcurl transport against a stub RCI
server, in both default and `ENTWARE_KN=1` builds.

**PASS.** Netlink watcher contract (link-up/new-address re-programs
ip rule/route, link removal is a no-op) — covered by Phase 5's netlink
watcher work and netns integration (D-21: "verify functional equivalence
on real kernel state, not byte-mirroring"). PID file, stale-PID
detection, TERM/INT/HUP signal handling, exit-code-0-on-clean-shutdown —
covered by Phase 1/7's lifecycle and soak testing (D-11).

## 10. Packaging / upgrade

**PASS.** Package name/binary path per platform, conffiles, init script
wiring (`_kn` netfilter.d hook, procd respawn+uci gate), Depends lists —
all Phase 8 work (D-37 through D-39), verified by building real `.ipk`
packages for both an Entware and an OpenWrt target under both `BACKEND`
values and diffing `control` files byte-for-byte (zero Go-path
regression). **PASS**: "C binary must load the Go-era config
byte-for-byte and keep the documented shape on save" — verified live,
not just by differential fixtures, via Phase 8's real Go→C→Go
upgrade/downgrade round trip on host (D-42): a real Go-created
config.yaml/auth_secret was read correctly by `magitrickled-c`, mutated
through its own HTTP API, and read back correctly by Go on restart, with
zero state loss in either direction. **PASS (real cross-arch
validation)**: full-daemon cross-compilation and execution proven on
arm64/armhf/riscv64 via real Ubuntu multiarch builds of all 5 feed deps,
run under `qemu-user` (D-40) — not just a compile-only claim.

## 11. Logging

**PASS.** zerolog-style console output to stdout, `logLevel` values
(`trace`…`disabled`) preserved exactly (`log.h`'s own header comment
states this explicitly); log *content* is documented as out-of-contract
except level names and general style, matching the source contract's own
scope limit.

## Summary

Of 11 contract sections: **10 pass with cited automated-test evidence**
(several with explicitly-permitted, contract-sanctioned hardening
divergences) and **1 section (interfaces/platform) had two real gaps** —
the Keenetic ignored-interfaces list, found and fixed during this
review, and the Keenetic RCI alias lookup, which this review wrongly
scoped out as needing real hardware and which was later implemented and
tested against a stub RCI server exactly as Go had done (D-62). Both are
now closed; no item remains deferred.
One sandbox-specific (not code-specific) live-verification gap is named
in section 7 (`ipset` non-functional in this particular session's
container). No section was found unimplemented, silently divergent, or
regressed from the source contract.
