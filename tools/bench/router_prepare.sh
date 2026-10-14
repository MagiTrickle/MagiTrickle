#!/opt/bin/sh
# Run on the router: router_prepare.sh BASE ORIGINAL_BINARY MT_C_BINARY
# Both binaries are copied into a private chroot; packages are not installed.
set -eu
BASE=${1:?private benchmark directory required}
ORIGINAL=${2:?extracted original package binary required}
MT_C=${3:?mt-c package binary required}

case "$BASE" in
  /tmp/*) ;;
  *) echo 'BASE must be a fresh directory under /tmp' >&2; exit 1 ;;
esac
[ -x "$ORIGINAL" ] && [ -x "$MT_C" ]
[ ! -e "$BASE" ] || { echo 'BASE already exists; choose a fresh directory' >&2; exit 1; }
[ -x /opt/sbin/chroot ]
mkdir -p "$BASE/root/opt/var/lib/magitrickle" "$BASE/root/opt/var/run" \
    "$BASE/root/opt/bin" "$BASE/root/tmp" "$BASE/root/etc"
MOUNTED=""
rollback() {
    for dir in $MOUNTED; do umount "$BASE/root$dir" || true; done
}
trap rollback 0
for dir in /opt/lib /opt/sbin /proc /sys /dev /bin /lib; do
    mkdir -p "$BASE/root$dir"
    mount -o bind "$dir" "$BASE/root$dir"
    MOUNTED="$dir $MOUNTED"
done
cp /etc/resolv.conf "$BASE/root/etc/resolv.conf"
cp "$ORIGINAL" "$BASE/root/opt/bin/original"
cp "$MT_C" "$BASE/root/opt/bin/mt-c"
chmod 755 "$BASE/root/opt/bin/original" "$BASE/root/opt/bin/mt-c"
touch "$BASE/prepared-by-router-bench"
trap - 0
echo "Prepared $BASE; upload dnsload into this directory before running router_dns.py"
