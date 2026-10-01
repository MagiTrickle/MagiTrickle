# Toolchains and ABI matrix

The authoritative target list is **generated from `config/entware/*.config`
and `config/openwrt/*.config`** — never hand-maintained. Regenerate with:

```sh
for f in config/*/*.config; do
  echo "== $f"; cat "$f"
done
```

Snapshot at audit time: 7 Entware targets, 33 OpenWrt targets (40 total).

## Entware (glibc)

Rules (spec §9.1): use the Entware glibc toolchain/sysroot for each target;
never musl, never host headers/libs, never static glibc.
Toolchains: Entware builds with its own GCC toolchains
(http://bin.entware.net / entware/toolchain-* docker images provide
`toolchain-<arch>` tarballs with sysroot).

| TARGET | GOARCH (today) | C triplet (Entware toolchain) | Endian | Float ABI | Min kernel | libc | Notes |
|---|---|---|---|---|---|---|---|
| aarch64-3.10 | arm64 | aarch64-openwrt-linux-gnu | LE | hard (fp default) | 3.10 | glibc 2.27 (Entware std) | |
| aarch64-3.10_kn | arm64 | aarch64-openwrt-linux-gnu | LE | hard | 3.10 | glibc | + `entware_kn`: Keenetic RCI, ignored ifaces, ndm hook, dep socat |
| armv7-3.2 | arm GOARM=7,softfloat | arm-openwrt-linux-gnueabi | LE | **soft-float** (GOARM softfloat ⇒ `-mfloat-abi=soft` expected — verify against Entware armv7sf toolchain) | 3.2 | glibc | |
| mips-3.4 | mips softfloat | mips-openwrt-linux-gnu | **BE** | soft | 3.4 | glibc | big-endian! |
| mips-3.4_kn | mips softfloat | mips-openwrt-linux-gnu | BE | soft | 3.4 | glibc | `_kn` |
| mipsel-3.4 | mipsle softfloat | mipsel-openwrt-linux-gnu | LE | soft | 3.4 | glibc | |
| mipsel-3.4_kn | mipsle softfloat | mipsel-openwrt-linux-gnu | LE | soft | 3.4 | glibc | `_kn`; default `.config.example` target |

Action items (Phase 1): pin exact Entware toolchain tarball versions +
sysroot glibc version per target and record dynamic linker paths
(`/opt/lib/ld-*`); Entware userspace lives under `/opt`, binaries link with
`-Wl,--dynamic-linker=/opt/lib/ld-…` and rpath `/opt/lib` (verify against an
existing Entware C package, e.g. how Entware builds `iptables`).
Kernel 3.2/3.4 floors: avoid post-3.4 syscalls or provide fallbacks —
epoll ✓ (2.6), timerfd ✓ (2.6.25), signalfd ✓, eventfd ✓,
`getrandom` ✗ (3.17+ — need /dev/urandom fallback),
`renameat2`/`copy_file_range` — do not use.

`_kn` differences are **software** (build tag features + socat dep), same
ABI as the non-`_kn` sibling.

## OpenWrt (musl)

Rules (spec §9.2): official OpenWrt SDK per release/target; use
`TARGET_CC/TARGET_CFLAGS/TARGET_LDFLAGS` + staging sysroot; never host
glibc. OpenWrt arch names below are package-arch names, each maps to an SDK
(target/subtarget) — pin the SDK release in Phase 1 (current feed targets
OpenWrt 24.10 for opkg and 25.12+ (apk) — CI builds one binary per package
arch, installable on any device of that arch).

| TARGET (pkg arch) | GOARCH/GOARM | Triplet (musl) | Endian | FPU | Notes |
|---|---|---|---|---|---|
| aarch64_cortex-a53 | arm64 | aarch64-openwrt-linux-musl | LE | fp | mt7622/ipq etc. |
| aarch64_cortex-a72 | arm64 | aarch64-openwrt-linux-musl | LE | fp | |
| aarch64_cortex-a76 | arm64 | aarch64-openwrt-linux-musl | LE | fp | newer SDKs only |
| aarch64_generic | arm64 | aarch64-openwrt-linux-musl | LE | fp | |
| arm_arm1176jzf-s_vfp | arm GOARM=6 | arm-openwrt-linux-muslgnueabi | LE | vfp | armv6 |
| arm_arm926ej-s | arm GOARM=5 | arm-openwrt-linux-muslgnueabi | LE | soft | armv5te |
| arm_cortex-a15_neon-vfpv4 | arm GOARM=7 | …-muslgnueabihf-like (per SDK) | LE | neon-vfpv4 | |
| arm_cortex-a5_vfpv4 | arm GOARM=7 | | LE | vfpv4 | |
| arm_cortex-a7 | arm GOARM=5 | | LE | soft (no FPU variant) | note: Go uses GOARM=5 here |
| arm_cortex-a7_neon-vfpv4 | arm GOARM=7 | | LE | neon | most common (mt7620-class successors) |
| arm_cortex-a7_vfpv4 | arm GOARM=7 | | LE | vfpv4 | |
| arm_cortex-a8_vfpv3 | arm GOARM=7 | | LE | vfpv3 | |
| arm_cortex-a9 | arm GOARM=5 | | LE | soft | |
| arm_cortex-a9_neon | arm GOARM=7 | | LE | neon | |
| arm_cortex-a9_vfpv3-d16 | arm GOARM=7 | | LE | vfpv3-d16 | |
| arm_fa526 | arm GOARM=5 | | LE | soft | armv4! oldest ARM — check compiler min arch |
| arm_xscale | arm GOARM=5 | | LE | soft | armv5 |
| i386_pentium-mmx | 386 | i486-openwrt-linux-musl | LE | x87 | 32-bit x86 |
| i386_pentium4 | 386 | | LE | sse2 | |
| loongarch64_generic | loong64 | loongarch64-openwrt-linux-musl | LE | fp | newest SDKs; no UPX today |
| mips64_mips64r2 | mips64 | mips64-openwrt-linux-musl | **BE** | | n64 ABI; no UPX today |
| mips64_octeonplus | mips64 | | BE | | |
| mips64el_mips64r2 | mips64le | | LE | | |
| mips_24kc | mips softfloat | mips-openwrt-linux-musl | **BE** | soft | ath79 etc. — very common |
| mips_4kec | mips softfloat | | BE | soft | |
| mips_mips32 | mips softfloat | | BE | soft | |
| mipsel_24kc | mipsle softfloat | mipsel-openwrt-linux-musl | LE | soft | mt76x8 — very common |
| mipsel_24kc_24kf | mipsle softfloat | | LE | 24kf FPU | |
| mipsel_74kc | mipsle softfloat | | LE | soft | |
| mipsel_mips32 | mipsle softfloat | | LE | soft | |
| riscv64_generic | riscv64 | riscv64-openwrt-linux-musl | LE | fp | no UPX today |
| x86_64 | amd64 | x86_64-openwrt-linux-musl | LE | | |

Init system: procd (all OpenWrt). Package formats: `.ipk` (≤24.10, opkg)
**and** `.apk` (≥25.12) built from the same rootfs — unchanged by the C port.

## Cross-cutting ABI/portability requirements for C code

- Endianness: mips/mips64 are big-endian — all wire I/O via explicit
  byte-order helpers (`be16toh`/manual shifts); no struct-cast onto DNS or
  netlink payloads (netlink headers via libmnl handle alignment).
- 32-bit: `time_t` is 32-bit on some targets (musl 32-bit uses 64-bit
  time_t since musl 1.2 — OpenWrt ≥21 yes; Entware glibc 32-bit is
  **32-bit time_t** — avoid storing epoch in `long`, use `int64_t`).
- Unaligned access: fatal/slow on mips/armv5 — no unaligned loads
  (memcpy for multi-byte reads).
- Softfloat: avoid double-heavy hot paths (TTL math in integers).
- Syscalls: floor = kernel 3.2/3.4 (Entware); guard `getrandom`,
  `SO_REUSEPORT` (3.9 — ok), `TCP_FASTOPEN` (don't use), `epoll` fine.
- No `-march=native`; per-target flags come from the SDK/toolchain.
- Dynamic linking against feed libs; `-O2`; LTO optional per-target after
  validation; `-Werror` only in CI after warning cleanup.

## Build-matrix policy

Every target listed above must either build in CI or be explicitly marked
"not yet supported" with a concrete technical reason in this file. None may
be dropped silently. Current expectation: all 40 are reachable with GCC
toolchains; the risky ones to validate early are `arm_fa526` (armv4),
`loongarch64_generic` (new SDK), `mips64*` (n64 ABI), Entware kernel-3.2/3.4
syscall floor.

The three Entware Keenetic targets (`*_kn`) are automated in
`.github/workflows/build.yml`. CI uses the prebuilt SDK selected by
`ownik/gh-action-entware-sdk` for the matching base architecture and builds
MaciTrickle through a temporary Entware feed package. The action is pinned
by commit; its SDK release archive is selected dynamically, digest-verified,
and cached by the action. These SDK artifacts are unofficial builds of the
Entware GCC 8.4.0/glibc 2.27 SDK. CI extracts the resulting daemon from the
IPK and verifies its ELF architecture, byte order, and `/opt/lib` dynamic
interpreter before upload.

Three OpenWrt package archs are automated the same way (D-50): `x86_64`
(SDK target/subtarget `x86/64`), `mips_24kc` (`ath79/generic`),
`mipsel_24kc` (`ramips/mt7621`) — chosen because these target/subtarget
pairs are among the most common and best-documented in the OpenWrt
project, unlike most of the other 21 package archs above where the
table's `Triplet` column is still blank and no specific target/subtarget
has been confirmed. `aarch64_generic` was tried against `armvirt/64` but
that pairing 404s for `OPENWRT_VERSION=24.10.1` (confirmed against real
CI, not this sandbox) — reverted to gated rather than guess again
blindly; a real aarch64 target/subtarget for this OpenWrt release still
needs to be found. CI downloads the real `OPENWRT_VERSION=24.10.1` SDK
for each confirmed target (via `tools/ci/openwrt-package/Makefile`, a
real in-tree SDK package, not a temporary feed), verifies its digest,
and builds through `./scripts/feeds install` + explicitly compiling the
compile-time feed dependencies (`libyaml`/`libpcre2`/`libmnl`/`curl` —
`feeds install` only symlinks a package's recipe, it does not build or
stage its headers, which the first real-CI run caught) + `make
package/magitrickle/compile`. The explicit compile step's make targets
had to be the SDK's real package names, not the `+libX` DEPENDS/
feeds-install names: `libyaml` resolves (as a virtual/provides alias)
to the real package `yaml`, `libpcre2` to `pcre2` (`libmnl`/`curl` keep
their DEPENDS names as-is) — confirmed from `feeds-install.log`'s own
`Installing package 'yaml' from packages` / `Installing package
'pcre2' from base` lines after the first real-CI run kept hitting `No
rule to make target 'package/libyaml/clean'` even past the cJSON fix.
cJSON isn't in any OpenWrt feed for this release at all (`WARNING: No
feed for package 'libcjson' found`, confirmed via CI), so it's vendored
as its own in-tree SDK package (`tools/ci/openwrt-package-libcjson/
Makefile`, source pinned to git tag `v1.7.18` — no hash-verified
tarball, since this sandbox can't reach `github.com` either to source a
checksum) and compiled the same explicit way as the other compile-time
deps.

D-51 subsequently activated the rest: the three non-`_kn` Entware targets
(`aarch64-3.10`, `mips-3.4`, `mipsel-3.4`) plus a fourth never-tried-before
one (`armv7-3.2`) reuse the identical `ownik/gh-action-entware-sdk`
mechanism, and 18 of the remaining 21 OpenWrt package archs got a
best-effort target/subtarget mapping (see D-51 for the confidence
breakdown and the full list of what's still explicitly gated: 8 OpenWrt
archs judged too ambiguous or endianness-mismatched to guess, plus `.apk`
(OpenWrt ≥25.12) packaging for any target).

## UPX

Today Go binaries are UPX-compressed except riscv64/mips64/mips64le/loong64.
Package size is a non-goal for the C port; UPX increases startup RSS (whole
image resident, non-shareable pages). Phase 8: measure C binary startup/RSS
with and without UPX and keep it only if it does not regress RAM (expected
outcome: drop UPX).

**Measured (Phase 8, host x86_64, `magitrickled-c`, 266,520 → 98,652 bytes,
37.01% ratio with `upx -9 --lzma`)**: 30-run average wall-clock startup
(exec → daemon's early-exit path, identical workload both variants) was
**13.4 ms plain vs 21.6 ms UPX-compressed — UPX adds ~8 ms (~62%) to every
process start**, consistent across two independent 20/30-run batches.
Peak RSS was statistically indistinguishable (10.84 MB plain vs 10.83 MB
UPX, 10-run average each) — at this binary size (hundreds of KB, not the Go
binary's ~10 MB), RSS is dominated by the dynamically-linked shared
libraries (libc, libyaml, PCRE2, libmnl, cJSON, libcurl + libcurl's own
TLS/auth dependency chain), not by the executable's own resident image, so
UPX's "whole image resident" cost barely registers against that baseline —
but the decompression-at-exec cost is pure, unconditional latency with **no
offsetting benefit**: 168 KB of on-disk savings is noise against a package
already dominated by the frontend's `dist/` assets. **Decision: drop UPX
for the C backend** — confirmed by measurement, not just the a priori
expectation above. The root Makefile already reflects this: `BACKEND=c`'s
build step (`Makefile`'s `build-backend-$(UNIQUE_NAME)` recipe) has never
invoked `upx` — only the `BACKEND=go` branch does — so no Makefile change
was needed to act on this decision, only to confirm it was the right one.
