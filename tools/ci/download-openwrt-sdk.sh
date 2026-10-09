#!/usr/bin/env bash
# Download/extract a verified prebuilt SDK. Never build sources here: the caller
# decides whether an acquisition failure should use the source fallback.
set -euo pipefail
if [ "$#" -ne 3 ]; then
  echo "usage: $0 SDK_ROOT BASE_URL LOG_DIR" >&2
  exit 2
fi
sdk_root=$(realpath "$1")
base_url=$2
log_dir=$(realpath "$3")
cd "$sdk_root"
sdk_file=$(grep -oE 'openwrt-sdk-[^ ]+\.tar\.(zst|xz)' sha256sums | head -n1)
if [ -z "$sdk_file" ]; then
  echo "Could not find an SDK archive name in ${base_url}/sha256sums" | tee -a "$log_dir/download.log" >&2
  exit 1
fi
if [[ ! "$sdk_file" =~ ^openwrt-sdk-[A-Za-z0-9_.+-]+\.tar\.(zst|xz)$ ]]; then
  echo "Unsafe SDK archive filename: $sdk_file" >&2
  exit 1
fi
# Logging must not hide curl/sha256sum failures behind tee's status.
set -o pipefail

echo "SDK archive: $sdk_file" | tee -a "$log_dir/download.log"
sdk_sha=$(awk -v name="$sdk_file" '$2 == name || $2 == ("*" name) { print $1 }' sha256sums)
if [[ ! "$sdk_sha" =~ ^[[:xdigit:]]{64}$ ]]; then
  echo "Missing or ambiguous SHA-256 for $sdk_file" | tee -a "$log_dir/download.log" >&2
  exit 1
fi
sdk_part=$(mktemp "${sdk_file}.part.XXXXXX")
extract_root=$(mktemp -d "$sdk_root/.extract.XXXXXX")
trap 'rm -f -- "$sdk_part"; rm -rf -- "$extract_root"' EXIT
sdk_verified=false
for attempt in 1 2 3 4 5 6; do
  saved_bytes=$(stat -c '%s' "$sdk_part")
  echo "SDK download attempt $attempt/6 ($saved_bytes bytes saved)" | tee -a "$log_dir/download.log"
  # Keep bytes from interrupted transfers (curl 18/28) and resume
  # with an HTTP Range request. Restarting each attempt discards
  # hundreds of MB on slow/unstable OpenWrt mirrors.
  if [ "$saved_bytes" -gt 0 ] && \
    printf '%s  %s\n' "$sdk_sha" "$sdk_part" | sha256sum -c - >/dev/null 2>&1; then
    mv -- "$sdk_part" "$sdk_file"
    sdk_verified=true
    break
  fi
  if curl --http1.1 --connect-timeout 30 --max-time 600 \
    --speed-limit 1024 --speed-time 60 --continue-at - \
    -fsSL -o "$sdk_part" "${base_url}/${sdk_file}" \
    2>&1 | tee -a "$log_dir/download.log"; then
    if printf '%s  %s\n' "$sdk_sha" "$sdk_part" | \
      sha256sum -c - 2>&1 | tee -a "$log_dir/download.log"; then
      mv -- "$sdk_part" "$sdk_file"
      sdk_verified=true
      break
    fi
    # A completed transfer with the wrong digest must not be
    # resumed: restart it, and never extract unverified bytes.
    echo "SDK checksum mismatch; restarting download" | tee -a "$log_dir/download.log" >&2
    : > "$sdk_part"
  else
    curl_status=${PIPESTATUS[0]}
    if [ "$curl_status" -eq 33 ]; then
      # The mirror rejected a Range request. Retry from zero.
      echo "SDK mirror does not support resume; restarting transfer" | tee -a "$log_dir/download.log"
      : > "$sdk_part"
    fi
  fi
  if [ "$attempt" -lt 6 ]; then
    sleep $((attempt * 10))
  fi
done
if [ "$sdk_verified" != true ]; then
  echo "SDK download or SHA-256 verification failed after 6 attempts; refusing to extract" | tee -a "$log_dir/download.log" >&2
  exit 1
fi

case "$sdk_file" in
  *.tar.zst) tar --use-compress-program=unzstd -xf "$sdk_file" -C "$extract_root" ;;
  *.tar.xz) tar -xJf "$sdk_file" -C "$extract_root" ;;
  *) echo "Unrecognized SDK archive extension: $sdk_file" >&2; exit 1 ;;
esac
mapfile -t extracted < <(find "$extract_root" -mindepth 1 -maxdepth 1 -type d -name 'openwrt-sdk-*')
if [ "${#extracted[@]}" -ne 1 ] || [ ! -f "${extracted[0]}/Makefile" ] ||
   [ ! -d "${extracted[0]}/staging_dir" ]; then
  echo "SDK archive has no unique usable SDK directory" >&2
  exit 1
fi
mv -- "${extracted[0]}" "$sdk_root/"
