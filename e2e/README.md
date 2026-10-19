# E2E tests on an emulated Keenetic KN-1012

The suite installs the real `.ipk` (entware `aarch64-3.10_kn`) into KeeneticOS running under
[router-emulator](https://github.com/stavultras/router-emulator) and drives it over SSH, the Keenetic RCI
and MagiTrickle's own API. It covers the package/init script, DNS -> ipset -> iptables/ip rule plumbing,
every rule type, subscriptions, auth, persistence, and the `ndm/netfilter.d` hook (triggered both by running
the script and by switching interfaces through RCI so that ndm calls it for real).

## Prerequisites

- built `qemu-system-aarch64` and the NAND image in `$EMU_DIR` (default `~/Projects/router-emulator`), see its guide;
- a base USB image with `ipset ip-full bind-dig curl tcpdump` on top of the reference Entware stick,
  in `~/.cache/magitrickle-e2e/usb-base.img` (`E2E_USB_BASE` to override; `prepare-base.sh` creates it, needs `-w user`);
- `go`, `make`, `socat`, `sshpass` (only for `prepare-base.sh`).

Credentials baked into the image: web/RCI `admin:E2ETests`, Entware SSH (host port 8022) `root:keenetic`.

## Running

    cd e2e
    go test -count=1 -v .                 # boots a throw-away emulator in /dev/shm, builds and installs the package
    E2E_REUSE=1 go test -run Hook -v .    # use an emulator started with ./emulator.sh start
    E2E_REUSE=1 E2E_SKIP_INSTALL=1 ...    # ... and keep the package already installed there

`magitrickled` is started by the tests themselves (not by the package), so its stdout/stderr are captured and
printed when a test fails. A scripted upstream DNS server and a list server (`helper/`, cross-compiled to arm64)
run on the router, which keeps everything offline. A full run takes about ten minutes.
