# NDM interface readiness for routing profiles

Decision: approved profile-only Keenetic addition, 2026-10-14, recorded as
**D-81** in the [C rewrite decision log](c-rewrite/decisions.md).
NDM may veto an otherwise kernel-ready profile candidate: this is an intentional
routing divergence. It complements D-79's device scope but does not depend on
that include/exclude filter. Group/subscription matching, rule priorities, DNS
and direct-interface selection are unchanged. Historical rewrite decisions
remain applicable outside D-81's explicit profile-selection exception.

## Behavior

Only `_kn` builds consult NDM while choosing a member of an interface-priority
profile. A known unavailable member is skipped; the first available member is
selected and a recovered higher-priority member is selected again. Existing
kernel checks (link/address/gateway per family) still have to succeed. If none
is usable, the existing terminal blackhole remains; no ordinary WAN fallback
or global conntrack flush is added. Established flows are not promised seamless
migration to a different tunnel.

IPv4 and IPv6 are evaluated separately. Known negative `conf`, `link`, `ctrl`
or the selected family's IP layer veto a candidate. Absent/nonapplicable layers
are not inferred to be disabled. Unknown values do not erase a known negative
state. Without NDM metadata, existing kernel checks remain the fallback; this
feature is not an independent connectivity probe or a kill switch.

Modern layer events update one field, not the entire interface. Legacy events
use `up`, `link`, and `connected` together; their `change` label is not used as
an event filter. Once a modern hook has actually been observed for an interface,
companion legacy events cannot overwrite its layered state. An RCI response
containing `summary.layer` alone does not establish modern-hook support: older
firmware may expose that data but emit only legacy callbacks.

The user's WireGuard trace is a regression fixture: `ctrl=disabled` excludes
`nwg0` even while its kernel link is up; `conf=running` and subsequent pending
levels do not restore it; only the complete recovery does. This trace shows a
manual connection transition. It does not establish which notifications every
firmware emits for Ping Check failures, a dead peer or hardware offload.
MT does not introduce ICMP/HTTP availability probes.

## Events and snapshots

The package installs `100-magitrickle` in `iflayerchanged.d`,
`ifstatechanged.d`, `ifcreated.d`, and `ifdestroyed.d` under `/opt/etc/ndm`.
Four small wrappers invoke `/opt/lib/magitrickle/ndm-interface-hook`.
The existing `socat` dependency sends a bounded ASCII/JSON event to
`POST /api/v1/system/hooks/ndm-interface` on the local MT Unix socket.
This route is **not** registered on the TCP/WebUI listener. HTTP 202 acknowledges
receipt, not route installation. Hooks never run RCI or route commands.
Connection and inactivity timeouts are one second; firmware's serial hook
queue must not wait for backend reconciliation.

The backend updates its loop-owned cache and uses the existing 20 ms coalescing
route queue. NDM-only notifications reconcile affected profiles, not direct
routes or all iptables chains. Repeated unchanged availability causes no
reconciliation; unchanged chosen routes cause no route writes. Kernel
notifications retain their existing reconciliation and retry behavior.

RCI is read only for the initial snapshot, incomplete/new identity, unknown
fields, an explicit resynchronization, or detected delivery loss. There is no
normal periodic interface-state poll and no confirmation read after each
ordinary event. Failed snapshots retry with bounded backoff (1–30 seconds),
while ordinary hooks remain usable. The unrelated existing device-policy mark
service is unchanged.

A single worker reads fixed local RCI endpoints with proxy/redirects disabled,
500 ms connect and 2 second per-request timeouts, a 1 MiB body limit and at most
256 interface records. `GET /rci/show/interface` supplies state. A read-only
`POST /rci/` show/interface batch supplies missing system names, matched by
position. Names are never guessed from description, alias or NDM ID. Missing
system-name support retains NDM IDs until a hook supplies the mapping.

Each snapshot captures a generation before I/O. Newer per-field/lifecycle/name
notifications win over its result. Deletion and reassignment cannot be undone
by an old in-flight snapshot. A failed read leaves known state intact.

Hooks create `/opt/var/run/magitrickle-ndm-lost` (a directory marker) when delivery
fails. The backend watches its parent with inotify, so a lost **last** event can
trigger recovery without waiting for another interface event. The marker is
consumed at snapshot start, never at completion: a later failure cannot be
silently erased by the old read. No notification journal or unbounded queue
is created. Without a working inotify watch, recovery uses a later explicit
loss report/startup and bounded watch retries. Arbitrary silently lost firmware
events cannot be guaranteed detectable without periodic polling.

## Build and validation

All added C modules, declarations, cache fields, route hook and package scripts
are compile-time/platform gated. Generic Entware/OpenWrt have no new dependency
or payload. Both root Makefile packaging and the independent Entware SDK
install recipe carry the same hooks. No frontend or workflow files are changed.

Tests cover the supplied trace, old/new source selection, IPv4/IPv6, duplicates,
unknown/malformed input, stale snapshots, identity reuse, local HTTP receipt
while RCI is stalled, delivery-loss recovery without a following hook, and
shutdown cancellation. The route-recovery tests exercise targeted/coalesced
scheduling, retry and overflow without rebuilding iptables. The existing
real-kernel profile test additionally verifies NDM failover/failback while all
kernel links remain up, direct-interface exemption, per-family readiness, and
absence of repeated route writes. It runs only inside an isolated network
namespace; a local environment without that permission skips it.

Package tests inspect actual synthetic IPK payloads, APK staging, the evaluated
SDK install recipe, and the shell notification contract. They are not claims
of a successful cross-compiled package installation on a router. Binary
isolation is checked against built generic and Keenetic executables.

On-device acceptance: install the `_kn` package, configure two profile members,
toggle one tunnel and confirm failover/failback and blackhole when both are
unavailable. Check IPv4/IPv6 separately, then service/router restart and a real
Ping Check/peer failure if used. The old diagnostic `099-mt-interface-probe`
files may be removed manually after testing; package install does not delete
user-created scripts.

Source contract: [official OPKG hook documentation](https://support.netcraze.ru/giga/nc-1012/ru/42407-opkg-component-description.html).
