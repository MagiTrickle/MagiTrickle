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
    go test -count=1 -timeout 30m -v .                 # boots a throw-away emulator in /dev/shm, builds and installs the package
    E2E_REUSE=1 go test -run Hook -v .    # use an emulator started with ./emulator.sh start
    E2E_REUSE=1 E2E_SKIP_INSTALL=1 ...    # ... and keep the package already installed there

`magitrickled` is started by the tests themselves (not by the package), so its stdout/stderr are captured and
printed when a test fails. A scripted upstream DNS server and a list server (`helper/`, cross-compiled to arm64)
run on the router, which keeps everything offline. A full run takes 10-12 minutes, so pass `-timeout 30m` (the default 10m is not enough).

## Known flakiness

The suite runs against an emulated router whose ndm daemon and QEMU user-mode network have timing of their own.
In most full runs one test fails for a reason that has nothing to do with MagiTrickle: a UDP reply lost on the
forwarded port, a TCP segment that never made it through the forwarder, ndm rewriting netfilter rules at the moment
a test starts. The failure message and the daemon log tell which one it was (for example, the daemon has logged
the request it was then not able to answer). Re-run the failed test on its own, e.g.
`go test -run TestName -count=10 .`; a real bug reproduces.

## Coverage

`./cover.sh` builds an instrumented `magitrickled`, runs the suite with it and prints the total (profile in `/tmp/e2e-cov.txt`).

## CI

These tests are not run in CI: they need QEMU, a KeeneticOS NAND image and an Entware USB image that live outside
this repository. Run them locally before merging changes to the netfilter, DNS proxy, API or packaging code.
