# Routing profiles

Profiles are reusable **ordered interface chains**, configured in **Settings**.
The first interface is primary; the others are backups in list order. The daemon
chooses the first locally usable interface separately for IPv4 and IPv6. If no
candidate is usable, the group's existing policy-routing table retains its final
`blackhole` route. A recovered higher-priority candidate is selected automatically.
There is no load balancing, sticky mode, nested profile or hidden global fallback.

## Web interface

1. Open **Settings → Interface priority profiles → Add profile** and enter a name.
2. Use **Add interface** in the profile header to choose each candidate. Drag rows by
   their grip to set priority; the first row is primary. Alt+Up/Down on a focused
   grip is the keyboard alternative. The final disabled `blackhole` row stays fixed.
3. **Save Changes** makes the profile available in the existing route selector on
   **Groups** and **Subscriptions**, including subscription creation and bulk edits.
4. Select a profile or an ordinary interface and save the group/subscription.

Settings stacks independent sections with a plain heading, not another tab
bar. Each section owns its editor and save state; only interface priority profiles
are currently shown. The profile header shows usage counts beside a link icon.
Profile headers and striped tables follow the existing subscription panels.
Rows display Linux interface IDs as text, with optional friendly names. Keenetic
builds get these from RCI (description first, interface-name fallback). OpenWrt
displays Linux IDs without Keenetic labels; UCI logical names are not substituted.
Only the **Add interface** picker is a select inside a profile, and already-chosen
interfaces and `blackhole` are excluded from it.

The selector still chooses one item only. Profile creation, ordering and deletion
are exclusively in Settings. There is no fixed application limit on the number of
profiles or candidates; available system resources and request-size limits apply.
Duplicates inside a chain, an empty chain, blank names and invalid interface names
are rejected. A temporarily missing interface remains configured, not deleted.
A referenced profile cannot be deleted until its groups/subscriptions are reassigned.
Returning to Settings refreshes server-applied usage without discarding profile
edits. An in-flight older read is followed by a fresh read; a failed refresh blocks
editor actions until Retry succeeds rather than trusting stale usage counts.

Profile drafts survive tab changes. Save stays in the section toolbar; there is no
status footer or Cancel button. Validation hints appear on Save, and save failures
use the existing error notifications. If applying succeeds but saving to disk
fails, Save stays available with a retry hint. Closing the page with pending changes
still produces the usual unload warning.

## Configuration and compatibility

```yaml
configVersion: 0.8.2.2
profiles:
  - id: p_vpn
    name: VPN with fallback
    interfaces:
      - mitun0
      - tun_stable
      - nwg0
    on_unavailable: blackhole

groups:
  - id: aabbccdd
    name: GitHub
    interface: mitun0
    profile: p_vpn
    enable: true
    rules:
      - id: 00000001
        type: namespace
        rule: github.com
        enable: true

subscriptions:
  - id: 11223344
    name: Networks
    interface: mitun0
    profile: p_vpn
    enable: true
    url: https://example.test/networks.txt
    interval: 86400
```

`profile` is authoritative when present. `interface` is its **configured primary**,
kept for older binaries. It is updated automatically when a profile's first
candidate changes and is never replaced by the currently active backup. Renaming a
profile does not change its stable ID or any references.

When `profile` is absent/empty, the existing direct `interface` behavior applies;
`interface: blackhole` still blocks immediately. Old configs need no migration and
do not acquire profile fields merely by being saved. A mismatching compatibility
shadow is normalized to the profile's first candidate. A missing profile reference
is an error, not a fallback to the shadow. The only supported `on_unavailable` value
is `blackhole`; omission defaults to it.

An old binary can use the saved primary but does not understand failover profiles
and may remove profile fields when saving. Downgrade round-tripping is not promised.

Group exports include every referenced profile exactly once. Import remaps colliding
IDs and all imported references together; matching display names are not treated as
identity. An identical existing definition with the same ID is reused. Review
interface names when transferring to another router.

## What “available” means

This implementation detects **local link/routing failure**, not remote Internet
health. A profile candidate must exist, be administratively up and operational
(`UP`, or `UNKNOWN` for virtual links), and have a usable address for the relevant
IP family. Non-point-to-point links additionally require a discoverable gateway.
Tentative/DAD-failed IPv6 addresses and IPv6 link-local-only addresses are not usable
MASQUERADE source addresses. Gateway discovery excludes tables owned by this daemon
so a previously copied route cannot keep a vanished upstream gateway alive.

Link up/down/deletion, address changes and IPv4/IPv6 route updates trigger
event-driven reconciliation. Repeated notifications are coalesced for 20 ms.
No periodic route polling takes place. A failed route operation, busy rebuild,
overrun or watcher reconnection triggers one-time resynchronization with a bounded
100 ms–30 s backoff until success. Actual kernel routes are compared before
writing, so unchanged routes do not cause another write. The daemon currently
reconciles all attached routes per coalesced pass, not only the changed interface.
Candidate selection and route replacement are per IP family; IPv4 can therefore
use a different backup from IPv6 when their readiness differs.

Each enabled family prepares its own terminal route and reconciles its candidates
independently. A terminal-route lookup/install error in one family does not skip
the other family's reconciliation; the first error is still returned for retry.
This is error isolation during reconciliation, not a promise that a failed initial
attachment or profile edit is partially accepted instead of rolled back.
A conflicting non-blackhole terminal route is reported, not silently deleted.
Likewise, ambiguous or unsupported active-route snapshots can return an error;
repair does not imply permission to delete arbitrary foreign routes.

Rtnetlink dump completion must contain a valid native-endian status integer.
A nonzero `NLMSG_DONE` status, truncated completion, `NLM_F_DUMP_INTR` or receive
failure cannot publish a successful default-route snapshot. Optional trailing
extack attributes do not replace that status. The event owner retries reported
errors through the existing one-shot queue; a successful pass clears the retry.

A WireGuard/TUN device which stays `UP` while its remote peer stops responding will
**not** fail over on that basis alone. Active peer/Internet probes are not part of
this change. Existing TCP connections are not guaranteed to survive an egress/NAT
change; new connections use the newly selected route.

A profile edit reconfigures its existing route attachment without rebuilding its
ipset or changing its mark/table. The final blackhole stays installed while active
routes are replaced. Invalid API requests are rejected before mutation; apply errors
restore definitions and routing best-effort and are reported rather than hidden.
Reload similarly stages models and restores the prior registry on apply failure.

### Clarification of the D-72 restore follow-up

The historical D-72 entry describes the initial fix for `--noflush` declarations.
The subsequent snapshot-race fix refines it: **changed owned overrides are always
declared**, even when present in the preceding `iptables-save`, because firmware
can remove them before restore. Their full contents are supplied by the override.
Existing patched or deleted chains are not redeclared, so foreign contents are
not implicitly flushed. Unchanged overrides remain no-ops. All chain deletions
follow the required removals/flushes. Ownership comes from registration metadata,
not the `MT_` prefix. The executable regression is `test_iptables_restore_race.c`;
this clarification does not revert that fix or attribute historical device failures
to a single proven cause.

## API

Authenticated API routes (same protection as the existing settings endpoints):

- `GET /api/v1/profiles` → `{ "profiles": [...] }`, including per-profile usage counts.
- `PUT /api/v1/profiles` with `{ "profiles": [...] }` replaces the collection and saves.
  `?save=false` applies without persistence for the same administrative workflows as
  existing endpoints. IDs supplied by the client must be unique; absent IDs are
  generated by the server. Group/subscription endpoints accept an optional `profile`.
- An in-use deletion/missing reference is rejected. A persistence failure after apply
  returns HTTP 500 with `code: "PERSISTENCE_FAILED"`, `applied: true` and the canonical
  profiles, following the existing retry contract.

## Validation

```sh
cd src/backend-c
make -j2 CFLAGS_EXTRA=-Werror
make test
make sanitize
# Only in a disposable namespace; the test refuses the host network namespace.
sudo unshare --net env MT_TEST_PROFILE_NETNS=1 build/host/tests/test_profile_routes

cd ../frontend
npm run check
npm run test:unit
npm run build
npx playwright test profiles.spec.ts profiles-usage.spec.ts
```

The kernel test uses real Linux links, addresses, IPv4/IPv6 fwmark rules and routing
tables. Its iptables and ipset transports are in-memory; it is not an end-to-end
router forwarding/NAT or Entware hardware test. It covers multi-backup failover,
failback, address-family differences, terminal blackhole, link recreation,
gateway removal, route repair and live reordering without losing ipset contents.
It also injects a conflicting terminal route in each family while the other loses
its primary, verifies independent failover and recovery, and counts actual
`RTM_NEWROUTE`/`RTM_DELROUTE` sends to prove repeated stable passes make no writes.

`test_rtnl_responses.c` injects wire-level replies at send/recv: successful/error
DONE, truncated status, interruption, stale sequence, early ACK, extack and receive
loss. It checks the public reader's error and that no partial snapshot escapes.
`test_route_recovery.c` connects the production watcher to the daemon callbacks
with a controlled clock, receive failures, reconnect failures and app results.
It covers overflow/malformed notifications, event coalescing, `MT_ERR_AGAIN`
(simulated committer contention), apply errors, bounded backoff, resynchronization
after reconnect, timer-allocation failure, teardown and no idle polling.
These deterministic injections do not emulate a real overflowing router socket
or prove a hardware timing bound. Both tests run in the existing unit/sanitizer
suites; no additional workflow, polling loop or production test hook is needed.

Before a router rollout, retain a config backup and an independent management path.
On a test group, verify primary → first backup → second backup → blackhole and
recovery in the reverse direction. Confirm that the saved `interface` remains the
configured primary throughout. Repeat after a daemon restart, and check IPv4 and
IPv6 separately. Do not take down the interface carrying the only management session.
