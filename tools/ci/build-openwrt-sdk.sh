#!/usr/bin/env bash
# Source fallback for the OpenWrt package workflow (not an Entware toolchain).
# Ubuntu prerequisites are installed by the caller only when this path is used.
# Build the cross-toolchain, kernel support and SDK, not all firmware/packages.
set -euo pipefail

fail() { echo "OpenWrt source SDK: $*" >&2; exit 1; }
if [ "$#" -ne 6 ]; then
  echo "usage: $0 VERSION TARGET SUBTARGET PACKAGE_ARCH SDK_ROOT LOG_DIR" >&2
  exit 2
fi
version=$1
target=$2
subtarget=$3
expected_arch=$4
sdk_root=$(realpath "$5")
log_dir=$(realpath "$6")
[[ "$version" = snapshot || "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "invalid version"
for name in "$target" "$subtarget" "$expected_arch"; do
  [[ "$name" =~ ^[A-Za-z0-9][A-Za-z0-9_.+-]*$ ]] || fail "invalid target/architecture: $name"
done
[ "$(id -u)" -ne 0 ] || fail "run the build as an unprivileged user, not root"
jobs=${OPENWRT_SOURCE_JOBS:-2}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail "OPENWRT_SOURCE_JOBS must be positive"

# The parent workflow exports mt-c's version and target settings. Do not allow
# these to alter OpenWrt's toolchain or dependency package versions.
for name in ${!PKG_@}; do unset "$name"; done
unset COMMIT COMMITS_SINCE_TAG PRERELEASE_DATE TAG PLATFORM TARGET CROSS_COMPILE \
  SYSROOT STAGING_DIR ARCH MAKEFLAGS MFLAGS MAKEOVERRIDES

work=$(mktemp -d "${sdk_root}.source.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
[[ "$work" != *[[:space:]]* ]] || fail "OpenWrt requires a build path without whitespace"
source_repo=https://github.com/openwrt/openwrt.git
source_ref="refs/tags/v${version}"

if [ "$version" = snapshot ]; then
  # Do not replace a failed snapshot archive with today's main branch. Resolve
  # the revision advertised alongside that target's published SDK instead.
  base_url="https://downloads.openwrt.org/snapshots/targets/$target/$subtarget"
  curl --retry 3 --retry-delay 5 --connect-timeout 30 --max-time 120 -fsSL \
    "$base_url/version.buildinfo" -o "$work/version.buildinfo"
  revision=$(tr -d '\r\n' < "$work/version.buildinfo")
  [[ "$revision" =~ ^r[0-9]+-([0-9a-f]{7,40})$ ]] || fail "snapshot revision unavailable; refusing unpinned main"
  short_sha=${BASH_REMATCH[1]}
  curl --retry 3 --retry-delay 5 --connect-timeout 30 --max-time 120 -fsSL \
    "https://api.github.com/repos/openwrt/openwrt/commits/$short_sha" -o "$work/commit.json"
  source_ref=$(python3 - "$work/commit.json" "$short_sha" <<'PY'
import json, re, sys
from pathlib import Path
sha = json.loads(Path(sys.argv[1]).read_text()).get("sha", "")
if not re.fullmatch(r"[0-9a-f]{40}", sha) or not sha.startswith(sys.argv[2]):
    raise SystemExit("Snapshot revision could not be resolved to its exact commit")
print(sha)
PY
  )
fi

src="$work/openwrt"
git init -q "$src"
git -C "$src" remote add origin "$source_repo"
fetched=false
for attempt in 1 2 3; do
  if timeout 300 git -c http.version=HTTP/1.1 -C "$src" fetch --depth=1 origin "$source_ref"; then
    fetched=true
    break
  fi
  if [ "$attempt" -lt 3 ]; then sleep $((attempt * 10)); fi
done
[ "$fetched" = true ] || fail "could not fetch $source_ref; refusing another version"
git -C "$src" checkout --detach FETCH_HEAD
source_sha=$(git -C "$src" rev-parse --verify HEAD)
[[ "$source_sha" =~ ^[0-9a-f]{40}$ ]] || fail "invalid source commit"
if [ "$version" = snapshot ]; then
  [ "$source_sha" = "$source_ref" ] || fail "snapshot commit mismatch"
else
  # Preserve the release tag: older SDK generators use it to pin the base feed.
  git -C "$src" update-ref "$source_ref" "$source_sha"
fi
printf 'version=%s\ncommit=%s\ntarget=%s/%s\npackage_arch=%s\n' \
  "$version" "$source_sha" "$target" "$subtarget" "$expected_arch" | tee "$log_dir/source-revision.txt"

cd "$src"
[ -f "target/linux/$target/Makefile" ] && [ -f "target/linux/$target/$subtarget/target.mk" ] ||
  fail "target $target/$subtarget is absent in $source_ref"
# Kconfig uses underscores for punctuation in target identifiers.
target_symbol=${target//[-.]/_}
subtarget_symbol=${subtarget//[-.]/_}
cat > .config <<CONFIG
CONFIG_TARGET_${target_symbol}=y
CONFIG_TARGET_${target_symbol}_${subtarget_symbol}=y
CONFIG_DEVEL=y
CONFIG_BUILDBOT=y
CONFIG_SDK=y
CONFIG_ALL=n
CONFIG_ALL_NONSHARED=n
CONFIG_ALL_KMODS=n
CONFIG_AUTOREMOVE=n
CONFIG_BPF_TOOLCHAIN_NONE=y
CONFIG_SDK_LLVM_BPF=n
CONFIG_VERSIONOPT=y
CONFIG_VERSION_FILENAMES=y
CONFIG_VERSION_NUMBER="${version/snapshot/SNAPSHOT}"
CONFIG
make defconfig
# Invalid Kconfig symbols can silently select a default target. Check the
# resolved configuration BEFORE spending time on the cross-toolchain.
grep -Fxq "CONFIG_TARGET_${target_symbol}=y" .config || fail "target was not selected"
grep -Fxq "CONFIG_TARGET_${target_symbol}_${subtarget_symbol}=y" .config || fail "subtarget was not selected"
resolved_arch=$(sed -n 's/^CONFIG_TARGET_ARCH_PACKAGES="\([^"]*\)"$/\1/p' .config)
[ "$resolved_arch" = "$expected_arch" ] || fail "source target produces '$resolved_arch', expected '$expected_arch'"
grep -Fxq 'CONFIG_LIBC="musl"' .config || fail "source toolchain is not musl"
grep -Fxq 'CONFIG_SDK=y' .config || fail "SDK output is not enabled"
cp .config "$log_dir/source.config"

# Separate invocations enforce ordering even with parallel make: SDK packaging
# requires a completed toolchain and kernel build, not just extracted headers.
for stage in tools/install toolchain/install target/linux/compile; do
  echo "Source SDK stage: $stage ($jobs jobs)"
  make -j"$jobs" "$stage" V=s
done

# Full OpenWrt firmware builds stage the host package manager as part of
# package/compile. An SDK-only source build does not. Without this target,
# the extracted SDK fails much later: fakeroot: .../host/bin/apk: not found.
if grep -Fxq 'CONFIG_USE_APK=y' .config; then
  package_manager=apk
else
  package_manager=opkg
fi
host_stage="package/system/$package_manager/host/compile"
echo "Source SDK stage: $host_stage ($jobs jobs)"
make -j"$jobs" "$host_stage" V=s
[ -x "staging_dir/host/bin/$package_manager" ] ||
  fail "host package manager $package_manager missing after $host_stage"

echo "Source SDK stage: target/sdk/compile ($jobs jobs)"
make -j"$jobs" target/sdk/compile V=s
mapfile -t archives < <(find "bin/targets/$target/$subtarget" -maxdepth 1 -type f \
  \( -name 'openwrt-sdk-*.tar.zst' -o -name 'openwrt-sdk-*.tar.xz' \) | sort)
[ "${#archives[@]}" -eq 1 ] || fail "expected one source-built SDK archive, found ${#archives[@]}"
# A local build is not byte-identical to a release binary. Record its own
# digest; never claim it matches the prebuilt archive's published SHA-256.
sha256sum "${archives[0]}" | tee "$log_dir/source-sdk.sha256"
mkdir "$work/extracted"
case "${archives[0]}" in
  *.tar.zst) tar --use-compress-program=unzstd -xf "${archives[0]}" -C "$work/extracted" ;;
  *.tar.xz) tar -xJf "${archives[0]}" -C "$work/extracted" ;;
esac
mapfile -t extracted < <(find "$work/extracted" -mindepth 1 -maxdepth 1 -type d -name 'openwrt-sdk-*')
[ "${#extracted[@]}" -eq 1 ] || fail "source build has no unique SDK directory"
[ -f "${extracted[0]}/Makefile" ] && [ -d "${extracted[0]}/staging_dir" ] || fail "incomplete SDK output"
[ -x "${extracted[0]}/staging_dir/host/bin/$package_manager" ] ||
  fail "source SDK archive lacks required $package_manager host executable"
# The caller applies the SAME architecture/ELF/package checks to both paths.
mv -- "${extracted[0]}" "$sdk_root/"
echo "Source-built SDK ready: $version $target/$subtarget ($source_sha)"
