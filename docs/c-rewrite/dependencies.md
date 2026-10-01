# Dependency evaluation for the C backend

Target constraints: GPL-3.0-or-later project; glibc (Entware) **and** musl
(OpenWrt); archs incl. mips/mipsel (BE/LE, softfloat), armv5–v8, aarch64,
x86, riscv64, loongarch64; small RAM (routers with 128–512 MB); packages
must be installable from Entware/OpenWrt feeds or vendored.

Scoring criteria per candidate: license, feed availability
(Entware / OpenWrt), vendorability, glibc+musl support, arch support,
RAM/CPU/alloc behaviour, thread safety, API complexity, maturity/security/
fuzzing history, cross-compile friction, linking mode, verdict.

Verdicts are **preliminary** — final selection requires the spike
prototypes listed at the bottom (per §10 of the master spec).

## DNS message parsing

| Candidate | License | Feeds | Notes | Verdict |
|---|---|---|---|---|
| **Own minimal parser** | n/a | n/a | We need: header, question, RR iteration, A/AAAA/CNAME extraction, name decompression (loop/depth-guarded), TTL rewrite-free passthrough, pack of small synthetic answers (fake PTR, AAAA-stripped repack with compression). ~1–1.5k LOC + fuzz targets. No allocation on hot path (arena/stack). | **Preferred.** The proxy forwards raw bytes in the common case; full RFC coverage is not needed. Heavy libs bring more attack surface than they remove. |
| ldns | BSD-3 | OpenWrt: yes; Entware: yes | Full-featured, allocation-heavy (per-RR mallocs), older codebase | Fallback only |
| libknot (knot-dns) | GPL-2+ | OpenWrt: knot pkgs exist | Excellent wire API (knot_pkt), но тянет большой проект, cross-компиляция тяжелее | Fallback |
| c-ares | MIT | yes/yes | resolver client, not a message toolkit for proxying | No |

Risk to manage for own parser: compression pointer loops, bounds, label
lengths, count lies — covered by dedicated fuzz targets (§21 of spec) and
the differential harness against miekg/dns behaviour.

## HTTP server

Requirements: HTTP/1.1, chunked bodies not required (client sends small
JSON), keep-alive nice-to-have, unix socket + TCP listeners, ~10 routes,
static files, tiny RAM.

| Candidate | License | Feeds | Notes | Verdict |
|---|---|---|---|---|
| **Own minimal HTTP/1.1 server** (epoll loop shared with DNS) | n/a | n/a | Bounded request line/headers/body; enough for the API + static files; no TLS (matches Go version — plain HTTP). ~1.5–2k LOC + fuzz. | **Preferred**, pending spike |
| libmicrohttpd | LGPL-2.1+ | OpenWrt: yes; Entware: yes | Mature, select/epoll modes; LGPL linking OK for GPL app; thread-pool default modes allocate per-conn | Strong fallback / alternative if spike shows own server too costly |
| civetweb | MIT | not in feeds (vendor) | threads-per-conn model, bigger | No |
| mongoose | GPLv2 *or commercial* | vendor | license mixing OK (GPLv3 compat GPLv2? — GPLv2-only is **incompatible** with GPLv3) | **No (license)** |
| lighttpd/uhttpd as external | — | — | changes deployment model (extra process) | No |

## JSON

| Candidate | License | Feeds | Notes | Verdict |
|---|---|---|---|---|
| **cJSON** | MIT | OpenWrt: yes; Entware: yes | tiny, DOM-style, ubiquitous, easy vendoring; fine for ≤100 KB payloads | **Preferred** |
| jansson | MIT | yes/yes | nicer API, slightly bigger | Acceptable alternative |
| yyjson | MIT | vendor | fastest, larger single file | Overkill |
| parson | MIT | vendor | tiny but less maintained | No |

JSON payloads here are small (groups/rules lists); throughput irrelevant;
pick by availability + API safety. Need exact float/int and string escaping
parity with Go `encoding/json` for contract tests (normalize in tests).

## YAML

| Candidate | License | Feeds | Notes | Verdict |
|---|---|---|---|---|
| **libyaml** | MIT | OpenWrt: yes; Entware: yes | the de-facto C YAML; event/document API; battle-tested, fuzzed | **Preferred** (only serious option) |
| libcyaml | ISC | vendor | schema layer over libyaml; convenient but adds a layer to match Go quirks | Maybe for load, not for save |
| own subset parser | — | — | YAML is too footgun-rich to hand-roll | No |

Extra work regardless of choice: Go-yaml-v2 semantics shims — duration
strings (`5s`, `1h0m0s`) and bare-int nanoseconds, `!!merge` not used,
key order on save (explicit emit order), flow `[]` for empty lists,
quoting style differences (contract tests must compare **parsed** trees,
plus a byte-level snapshot test for the canonical save shape we commit to).

## Regex (rule type `regex`)

Current: dlclark/regexp2 = .NET-flavoured backtracking engine, IgnoreCase,
unanchored, **no timeout configured**.

| Candidate | License | Feeds | .NET-syntax parity | Notes |
|---|---|---|---|---|
| **PCRE2** | BSD | OpenWrt: yes; Entware: yes | High for the constructs seen in router rules (lookaround ✓, backrefs ✓, named groups ✓ (syntax variants), inline options ✓, `\d`/`\w` classes ✓). Differences: possessive/atomic details, some escapes, backtracking limits, Unicode categories naming | **Preferred**, with corpus proof + `pcre2_set_match_limit`/depth limit + optional JIT off on exotic archs |
| RE2 | BSD | C++ | no backrefs/lookaround → semantics loss | No |
| POSIX regex (libc) | — | builtin | far from .NET syntax | No |
| Oniguruma | BSD | vendor/feed | multiple syntaxes incl. ~.NET-ish (`ONIG_SYNTAX_RUBY`), good but less ubiquitous | Fallback |
| tiny-regex etc. | — | — | toy | No |

Phase 0 corpus (initial, to be extended in Phase 2 with real user lists):
patterns seen in repo tests/examples — `^[a-z]*example\.com$`,
`^ex[apm]{3}le.com$`, `example`, `^example$`, `(?i)EXAMPLE`,
`\d+\.example`, `^.*\.example\.com$`, `^.*.regex.example.com$`,
`^#[0-9a-f]{6}$` (internal color check), subscription auto-detection
regex-ish inputs. All are PCRE2-compatible. Unresolved risk: user configs in
the wild may use .NET-only constructs (balancing groups, `(?<name>` vs
`(?'name'` variants, `\G`, conditionals) — plan: compile-time validation in
C with clear error + release-notes migration guidance; **no silent
rewriting** (spec §7.5).

## Hash table / trees

| Need | Candidate | Verdict |
|---|---|---|
| domain→records cache, exact-domain rule index | **uthash** (BSD, header-only, vendored) or open-coded open-addressing table | uthash preferred for speed of development; revisit if alloc-per-node shows up in profiles |
| namespace rules | reversed-label trie (own, ~300 LOC) | own |
| subnet/subnet6 sets | not needed in hot path (subnets go straight to ipset) | none |
| TTL expiration | binary min-heap (own) or timing wheel | min-heap first; wheel only if profiling demands |

## Netlink (ipset + ip rule/route + link/addr watch)

| Candidate | License | Feeds | Notes | Verdict |
|---|---|---|---|---|
| **libmnl** | LGPL-2.1+ | OpenWrt: yes (core), Entware: yes | thin, stable ABI, used by iptables-nft itself; we hand-build ipset NFNL messages (protocol is small: create/destroy/flush/add/del/list with IPSET_ATTR_*) | **Preferred** |
| raw AF_NETLINK sockets | — | — | no dep, but re-implements mnl's alignment/TLV helpers | Fallback (keeps zero deps) |
| libipset | GPL-2 | feed has `ipset` pkg | GPLv2-only userspace lib → GPLv3 mixing problem + heavier | No |
| libnl-3 | LGPL | yes | bigger, caching layers we don't need | No |

Route/rule/link/addr: RTNETLINK via same libmnl. Watcher: RTMGRP_LINK +
RTMGRP_IPV4_IFADDR + RTMGRP_IPV6_IFADDR membership on one socket.

## iptables

Keep the current architecture: model desired state, diff against
`iptables-save` output, apply via `iptables-restore --noflush`
(fork/exec, batch). No linking against libiptc (unstable/deprecated ABI).
Same fake-executable test seam as Go (`Executable` vtable in C).

## HTTP client (subscription fetch, Keenetic RCI)

| Candidate | Notes | Verdict |
|---|---|---|
| **libcurl** | in both feeds; TLS via distro backend (openssl/mbedtls per feed); redirects/timeouts built-in (match: ≤5 redirects 301/302 only → custom via CURLOPT_FOLLOWLOCATION off + manual loop to preserve exact semantics) | **Preferred** — TLS is non-negotiable for https subscription URLs and hand-rolling TLS is out of the question |
| own client | no TLS | No |
| wget/uclient-fetch exec | fragile parsing, extra dep | No |

Package dependency impact: adds `libcurl` (+TLS lib) to Depends. Both feeds
carry it; RAM cost is dynamic-link shared. Acceptable; note in Phase 8.

## Crypto (auth): MD5-crypt, SHA256/512-crypt, HMAC-SHA256, base64, getrandom

| Candidate | Notes | Verdict |
|---|---|---|
| **Own vendored single-file implementations** (public-domain/BSD reference code: e.g. Ulrich Drepper's sha2-crypt reference, RFC 6234 SHA-2, RFC 2104 HMAC) | tiny, no deps, testable against Go vectors | **Preferred** |
| libc `crypt_r` | musl supports $1$/$5$/$6$; glibc (Entware) too — but behaviour/rounds parity must be verified per libc; avoids own crypt | Strong alternative for the crypt part; keep HMAC/SHA2 own |
| OpenSSL/mbedtls | already pulled by libcurl indirectly — but linking app to TLS lib for 3 primitives couples us to feed TLS choices | No |

## Logging

Own ~200 LOC leveled logger (console format similar to zerolog console
writer; levels trace..disabled). No dep.

## Test framework

| Candidate | Notes | Verdict |
|---|---|---|
| **Unity** or **greatest.h** (MIT, header-only, vendored) | trivial cross-compile, runs on host | pick one in Phase 1 (leaning greatest.h for zero-setup) |
| CMocka | in feeds, mocking support | alternative if mocking need grows |
| Check | fork-per-test, heavier | No |

Fuzzing: libFuzzer/AFL++ harnesses on host only (clang), не в feeds.
Sanitizers: host builds only.

## Summary of proposed runtime dependencies

| Dep | Link | Entware | OpenWrt | Reason |
|---|---|---|---|---|
| libyaml | dyn | ✓ | ✓ | config |
| cJSON | dyn (or vendor) | ✓ | ✓ | API JSON |
| PCRE2 (8-bit) | dyn | ✓ | ✓ | regex rules |
| libcurl (+feed TLS) | dyn | ✓ | ✓ | subscriptions https |
| libmnl | dyn | ✓ | ✓ | ipset/route netlink |
| uthash / greatest.h / small crypto | vendored headers | n/a | n/a | no runtime dep |

Everything else: POSIX + Linux (epoll, timerfd, signalfd, eventfd,
recvmsg/pktinfo, getrandom fallback to /dev/urandom for old kernels).

## Required spikes before Phase 1 sign-off

1. PCRE2 corpus run vs regexp2 (extend corpus with real-world lists).
2. libmnl ipset add/list/del on a real router kernel (3.4-era Entware
   targets!) — verify IPSET protocol revision compatibility with old
   kernels (hash:net with timeout requires ipset protocol ≥ 6 / kernel
   ≥ 3.4 with backports; `_kn` firmwares must be tested).
3. Minimal epoll DNS echo proxy on mipsel qemu — measure RSS/latency floor.
4. libyaml emit shape vs committed canonical save fixture.
