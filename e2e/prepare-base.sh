#!/bin/bash
# Creates ~/.cache/magitrickle-e2e/usb-base.img: the reference Entware stick plus the tools the tests need.
# Boots the emulator with internet access (-w user) once to run opkg.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${E2E_USB_BASE:-$HOME/.cache/magitrickle-e2e/usb-base.img}
mkdir -p "$(dirname "$out")"
EMU_DIR=${EMU_DIR:-$HOME/Projects/router-emulator}
cp --sparse=always "$EMU_DIR/usb.img" "$out"
E2E_USB_BASE=$out E2E_WAN=user "$here/emulator.sh" start
sleep 15
SSHPASS=keenetic sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p 8022 root@127.0.0.1 \
	'opkg update && opkg install ipset ip-full bind-dig curl tcpdump && sync'
T=${E2E_TMP:-/dev/shm/router-e2e}
echo quit | socat - UNIX-CONNECT:"$T/monitor.sock" >/dev/null || true
sleep 2
cp --sparse=always "$T/usb.img" "$out"
"$here/emulator.sh" stop
