#!/bin/sh
set -eu

package=${1:?"usage: $0 PACKAGE.ipk"}
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

control_archive="$tmp_dir/control.tar"
if ar t "$package" >/dev/null 2>&1; then
    control_member=$(
        ar t "$package" |
            tr -d '\r' |
            awk '$0 ~ /^control\.tar(\.[a-z0-9]+)?$/ { print; exit }'
    )
    if [ -z "$control_member" ]; then
        echo "No control.tar archive in $package" >&2
        exit 1
    fi
    ar p "$package" "$control_member" > "$control_archive"
else
    control_member=$(
        tar -tf "$package" |
            awk '$0 ~ /^(\.\/)?control\.tar(\.[a-z0-9]+)?$/ { print; exit }'
    )
    if [ -z "$control_member" ]; then
        echo "No control.tar archive in $package" >&2
        exit 1
    fi
    tar -xOf "$package" "$control_member" > "$control_archive"
fi

tar -tf "$control_archive" > "$tmp_dir/control-files"

if awk '$0 ~ /(^|\/)postinst-pkg$/ { found=1 } END { exit !found }' \
    "$tmp_dir/control-files"; then
    echo "$package contains postinst-pkg; OpenWrt default_postinst would recursively execute it" >&2
    exit 1
fi

postinst_member=$(
    awk '$0 ~ /(^|\/)postinst$/ { print; exit }' "$tmp_dir/control-files"
)
if [ -z "$postinst_member" ]; then
    echo "No postinst script in $package" >&2
    exit 1
fi

tar -xOf "$control_archive" "$postinst_member" > "$tmp_dir/postinst"
default_postinst_calls=$(
    awk '
        /^[[:space:]]*default_postinst([[:space:]]|$)/ { calls++ }
        END { print calls + 0 }
    ' "$tmp_dir/postinst"
)
if [ "$default_postinst_calls" -ne 1 ]; then
    echo "$package postinst must invoke default_postinst exactly once; found $default_postinst_calls" >&2
    exit 1
fi
