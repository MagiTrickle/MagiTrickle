# Netfilter recovery: stale staged-state regression

## Incident and invariant

The 2026-10-07 Entware/Keenetic report on 0.8.2.2 showed a running daemon
repeatedly submitting `-I PREROUTING 1 -j MT_DNSOR` without declaring the
missing chain. `iptables-restore` exited with status 2. Restart rebuilt the
initial state correctly, but the existing process could not recover.

D-63 requires every full rebuild to converge from the current kernel state.
An `mt_ipt_t` also retains patch/override/delete registrations in memory.
Reusing those registrations during cleanup violated that requirement: a
previous desired jump could be reinserted while cleanup deleted its target.
A failed cleanup then left a DELETE registration that poisoned later retries.
This is a recovery bug fix, not a change to D-19's ordinary commit semantics.

## Fix

Under the existing netfilter mutex, a full rebuild now:

1. Discards both engines' staged registrations before constructing cleanup.
2. Cleans only the owned rules/chains found in the current kernel snapshot.
3. Discards cleanup registrations on success, failure, or cancellation.
4. Registers base chains and rebuilds DNS remap and enabled rulesets from
   their live owners, then commits each enabled address family.

`mt_ipt_reset_staged` does no kernel I/O and preserves engine identity,
transport ownership and the cancellation token. Ordinary `mt_ipt_commit`
still retains desired state. No YAML/API/name/route/ipset format changes.
The snapshot parser also avoids NULL-pointer arithmetic on an empty result,
which the new reset test exposed under UBSan.

## Regression tests

`test_nfrebuild_recovery` has 11 cases: missing jump with a surviving DNS
chain in either family; failed cleanup followed by a firmware wipe; repeated
mixed complete/partial wipes; apply and snapshot errors; cancellation after
cleanup; a custom prefix; and an event burst with a failed first attempt.
Assertions require the DNS chain and exactly one jump, DNAT to port 3553,
no unresolved owned targets, and preservation of foreign rules.

The checked fake rejects a candidate restore with an unresolved owned
jump/goto before applying it. This is deliberately not a complete kernel
emulator: the same cases also run against real legacy and nft frontends in
isolated network namespaces in `check-netfilter.yml`. Real mode refuses to
run as non-root or in the network namespace of PID 1. Tests model coherent
firmware states; they do not pretend a real kernel can retain a jump into
a chain it has already successfully deleted.

`test_iptables_reset` adds 3 cases for all registration kinds, idempotent and
NULL reset, cancellation/transport retention, and ordinary commit retention.
Both binaries are picked up automatically by `make test` and `make sanitize`.

```sh
cd src/backend-c
make BUILD=recovery ENTWARE_KN=1 CFLAGS_EXTRA=-Werror \
  build/recovery/tests/test_nfrebuild_recovery \
  build/recovery/tests/test_iptables_reset
build/recovery/tests/test_nfrebuild_recovery -v
build/recovery/tests/test_iptables_reset -v
# Root only, disposable namespace; never run the real test on the host firewall.
sudo env MT_TEST_REAL_IPTABLES=1 unshare --net \
  timeout 90s build/recovery/tests/test_nfrebuild_recovery -v
```

CI uses `pipefail` so capturing output with `tee` cannot hide a failing test.
Host-kernel coverage is not a substitute for installation and validation of
an architecture-correct Entware build on the affected Keenetic device.
