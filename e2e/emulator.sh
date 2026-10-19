#!/bin/bash
# Starts/stops a disposable KN-1012 emulator instance in tmpfs.
# Usage: emulator.sh start|stop|status
# Env: EMU_DIR (router-emulator checkout), E2E_TMP (work dir in tmpfs)
set -eu
EMU_DIR=${EMU_DIR:-$HOME/Projects/router-emulator}
T=${E2E_TMP:-/dev/shm/router-e2e}
FORWARDS=${E2E_FORWARDS:-8080:80;8022:22;2222:2222;8081:8080;8053:53}
# UDP DNS from the emulated LAN "PC": the packets cross PREROUTING like those of a real client.
export LAN_EXTRA=${LAN_EXTRA:-,hostfwd=udp:127.0.0.1:8053-192.168.1.1:53}

stop() {
	if [ -S "$T/monitor.sock" ]; then
		echo quit | socat - UNIX-CONNECT:"$T/monitor.sock" >/dev/null 2>&1 || true
		sleep 1
	fi
	rm -rf "$T"
}

case "${1:-}" in
start)
	stop
	mkdir -p "$T/nand" "$T/logs"
	cp "$EMU_DIR/nand-kn1012-anon/kn1012.mtd0.full.bin" "$T/nand/"
	# base image: reference Entware stick + ipset/ip-full/bind-dig/curl/tcpdump (see prepare-base.sh)
	cp --sparse=always "${E2E_USB_BASE:-$HOME/.cache/magitrickle-e2e/usb-base.img}" "$T/usb.img"
	cd "$EMU_DIR"
	(setsid ./emulator.sh -P keenetic-kn-1012 -n "$T/nand" -w "${E2E_WAN:-offline}" -l user -u none \
		-o "lan-forwards=$FORWARDS" \
		-L "$T/logs" -m "$T/monitor.sock" -S "$T/console.sock" -- \
		-drive if=none,id=usbstick,format=raw,file="$T/usb.img" \
		-device usb-storage,drive=usbstick,removable=on,port=1 >"$T/run.log" 2>&1 &)
	for _ in $(seq 120); do
		curl -s -m 2 -o /dev/null http://127.0.0.1:8080/ && exit 0
		sleep 3
	done
	echo "router did not come up" >&2
	exit 1
	;;
stop) stop ;;
status) curl -s -m 2 -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/ ;;
*) echo "usage: $0 start|stop|status" >&2; exit 2 ;;
esac
