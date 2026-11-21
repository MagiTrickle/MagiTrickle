# DNS journal: automatic updates and system device labels

DNS records arrive on the existing SSE connection. Pause closes the subscription;
Resume reconnects and receives a current snapshot. Retry reconnects SSE after a
failure. The toolbar has no manual Refresh/GET path.

## Addresses and names

`::ffff:192.168.1.42` is presented as `192.168.1.42`; actual IPv6 addresses are
retained. The ring's binary address representation is unchanged. Optional
`client_name` values are presentation metadata. Filtering and grouping retain
normalized IP identity, so distinct addresses named "Phone" are not merged.

Names describe the router's current inventory, not the device that necessarily
owned an address when an older query was recorded. Missing, invalid or ambiguous
names fall back to the IP. Conflicting known owners or interface scopes suppress
a label even when aliases are equal or one owner has no name. MACs and interface
information are used only during discovery and are not sent to the browser.

The bounded `clients_begin/chunk/end` stream updates labels for already displayed
queries without replaying DNS history. Changes during a metadata transfer restart
staging, so viewers never combine portions of different snapshots. A first completed
inventory notifies viewers even when empty; after that only changes notify viewers.
When a tab resumes, its last displayed labels remain provisional until this fresh
inventory arrives. The backend does not send a misleading empty map while the first
lookup is pending. A genuinely empty or failed lookup clears stale labels, including
after a device has changed ownership of its IP. No periodic browser request is added.

## Keenetic `_kn`

The shared metadata worker reads two local RCI resources with the existing
libcurl/cJSON:

- `GET http://127.0.0.1:79/rci/show/ip/hotspot`: prefer the Client Lists `name`,
  then the DHCP `hostname`; ignore explicitly inactive hosts.
- `GET http://127.0.0.1:79/rci/show/ip/neighbour`: use the firmware's IPv4/IPv6
  neighbour records and interface IDs. Ignore expired neighbours and IPv6
  addresses without an active status.

Explicit hotspot IP/name pairs work independently of neighbour discovery.
Additional addresses join by the exact observed client MAC and compatible
interface ID. The `via` MAC identifies a different relationship and is not used
for naming. This supports a device whose hotspot entry only has IPv4 while its
DNS request arrives from a link-local, ULA or other IPv6 address. It also supports
IPv6-only clients when hotspot supplies a valid MAC and name.

An unrecognised optional hotspot interface value keeps the explicit IP/name
pair but is not used to name additional addresses through a MAC join. An absent
interface field remains supported; known neighbour scopes still participate in
conflict detection.

RCI neighbour responses normally contain an object indexed by neighbour ID;
array and single-record forms with the same fields are accepted. IPv6 addresses
come from `addresses.address`. The `expired` flag accepts booleans and exact
`"yes"`/`"no"` strings; other present values are rejected. Input validation is
conservative: an unsupported, malformed or oversized neighbour response drops the entire optional join and
retains only fresh explicit hotspot labels. Hotspot failure clears all labels.
No previous response or partial neighbour join survives an error.
Encoded NUL characters are rejected before cJSON can truncate a decoded name,
address, MAC or field name; literal text such as `\\u0000` remains valid.

The installed `/opt/etc/ndm/neighbour.d/100-magitrickle` hook uses Keenetic's
documented `hook` invocation. It reuses the existing socat delivery helper to
send a constant POST to `/api/v1/system/hooks/ndm-neighbour` over
`/opt/var/run/magitrickle.sock`. This route is registered only on the Unix server
in `_kn` builds. It acknowledges immediately and invalidates the whole snapshot;
it neither performs RCI in the hook handler nor stores event fields as a second
inventory. Unlike interface hooks, neighbour hooks do not require `system_name`.

The custom netlink neighbour dump/watch, hotspot cache, unknown-MAC cache,
cooldown and deferred cache retry have been removed. `libndm` is not required.
No client probing, EUI-64 inference, separate PTR request or router write is used.

## OpenWrt

Use the already packaged `luci-rpc.getHostHints` through native libubus/libubox.
This API delegates inventory aggregation to the platform: neighbours, configured
dnsmasq/odhcpd leases, static host definitions, `/etc/ethers` and local interface
data. It provides both IPv4 and IPv6 host hints, including addresses learned
outside DHCP. mt-c does not read lease files, implement the ubus protocol or
spawn a ubus command.

Each MAC-keyed record can contain `name`, `ipaddrs` and `ip6addrs`. Unnamed
records still participate in ambiguity checks. `getHostHints` includes static
or inactive hints and does not expose reachability, expiry, provenance or
interface scope. Labels therefore carry the system's display-hint semantics;
they are not a verified list of online devices. The system may obtain a name
from its optional reverse-DNS provider. mt-c does not add a reverse-DNS request
or require `rpcd-mod-rrdns` or the full LuCI interface.

One worker owns all libubus calls and runs native `uloop`. The control pipe is a
`uloop_fd`; inventory is an asynchronous ubus request with a two-second response
deadline. Native `ubus_auto_conn` and automatic object subscriptions handle
broker and object recovery. No extra polling loop, exponential reconnect
implementation or lease-expiry scheduler is maintained in mt-c.
During shutdown, stop socket I/O and let libubus dispatch and release deferred
messages before shutting down the context. Such messages can be queued while
the library performs a synchronous lookup; cancelling their timer alone does
not release them.

Subscribe to dnsmasq instances, `dhcp`, `service` and the inventory provider's
lifecycle. DHCP notifications invalidate the snapshot. Configuration changes
arrive as **notifications on the `service` object**, with method `config.change`
and payload `{"config":"dhcp"}`,
not as a global `config.change` event. Unrelated notifications are ignored.

Provider or broker failure clears names; recreation reconnects and rediscovers
them. The native library's synchronous connection, registration and lookup calls
are not covered by the inventory response timer. A stalled broker can delay
that worker, including its shutdown; this implementation does not claim a total
shutdown bound for a hung system ubus service. DNS forwarding remains independent
of the metadata worker.

## Shared lifecycle and resource limits

Discovery starts with the first journal viewer. Source notifications are
debounced for one second; a notification received during a fetch remains pending
for a later snapshot. Both platforms reconcile every **30 seconds** while a
viewer is connected. This bounds normal discovery staleness after missed hooks,
alias changes, SLAAC changes or missing DHCP notifications; failed/slow sources
can take longer. No source provides a complete notification contract for all of
these changes. There is no per-query lookup or frontend polling.

RCI connect/total timeouts are 500/2000 ms per request, with cancellation;
redirects and environment proxies are disabled. Responses are capped at 1 MiB
each, observations at 2048 and names at 127 printable UTF-8 bytes. Snapshot
failure clears optional labels rather than retaining names indefinitely.
If the periodic reconciliation timer cannot be created, optional discovery
does not start; the journal continues to deliver DNS records with IP addresses.

Removing the final viewer stops timers, closes OpenWrt discovery and releases
the map. Subsequent viewers start a fresh lookup; retaining provisional names
in the browser is a presentation-only safeguard against a brief IP/name flicker,
not an assertion about historical ownership. A bounded RCI request already in progress may finish, but its result
cannot enter a later viewer generation. HTTP streams are closed before service
destruction. Generic Entware builds instantiate no provider and gain no OpenWrt
dependency. This refactor adds no runtime dependencies to PR #25.

## Verification

- `test_dns_clients.c`: canonical addresses, UTF-8, alias/hostname precedence,
  fresh RCI joins, IPv6-only clients, expiry, interface and owner conflicts,
  encoded NUL rejection, malformed and oversized atomic fallback.
- `test_dns_client_owners.c`: equal aliases on different MACs, unnamed conflicting
  owners, scope conflicts, canonical MACs, bounded observations and compaction.
- `test_dns_client_service.c` and `test_dns_client_events.c`: off-loop discovery,
  refresh coalescing, rename/failure/recovery, bursts, notifications during an
  inflight request and closed/reopened viewer generations.
- `test_dns_stream_bounds.c`: failure to allocate the metadata timer preserves
  snapshot/live DNS streaming and does not consume subscription slots.
- `test_dns_keenetic.c`: native libcurl against an isolated local HTTP fixture.
- `test_dns_keenetic_hook.c`: real Unix/TCP HTTP servers, Unix-only registration,
  immediate hook acknowledgement and coalesced invalidation.
- `test_dns_openwrt.c`: native blobmsg parsing and an isolated real ubusd/provider,
  including host hints, service notifications, provider/broker recreation,
  request timeout/cancellation, viewer lifecycle and deferred-message cleanup
  with a stopped broker.
- `fuzz_dns_clients.c`: mutate both RCI documents under libFuzzer/ASan/UBSan;
  successful output must remain bounded, valid, sorted and unique by IP.
- `tools/tests/package_identity.py`: the installed neighbour hook and SDK package
  inventory, together with existing package checks.

Run backend builds/tests with separate `BUILD` directories for generic,
`ENTWARE_KN=1` and `PLATFORM=openwrt`, using `CFLAGS_EXTRA=-Werror`. For OpenWrt
host tests, `tools/ci/openwrt-host-libs.sh` builds the official pinned native
libraries and broker; set `MT_TEST_UBUSD` and `LD_LIBRARY_PATH` to that prefix.
The broker fixture needs root or fakeroot for isolated ACL ownership checks.
Sanitizer builds cover the same parser and lifecycle code. Target SDK builds
use their own libraries; host results do not establish physical-router firmware
behaviour. Frontend and DNS forwarding code are unchanged by this refactor.

The RCI fixtures combine published hotspot JSON, a working neighbour consumer
and the official CLI's address/status examples. They are not complete captures
from a router. CLI output alone does not establish every JSON field type;
verification on a specific Keenetic firmware still requires its actual hotspot
and neighbour responses. An unsupported neighbour shape falls back to explicit
hotspot labels, so a passing fixture suite alone cannot establish IPv6 naming
on that firmware.

## Sources

- [Keenetic OPKG hook contract](https://support.keenetic.com/peak/kn-2710/en/42407-opkg-component-description.html)
- [Keenetic CLI reference: `show ip neighbour` and RCI](https://storage.googleapis.com/docs.help.keenetic.com/cli/4.1/en/cli_manual_kn-2210.pdf)
- [Published KeeneticOS 4.2.6.3 hotspot response](https://forum.keenetic.ru/topic/20770-4263-%D1%81%D0%BF%D0%B8%D1%81%D0%BE%D0%BA-%D0%BA%D0%BB%D0%B8%D0%B5%D0%BD%D1%82%D0%BE%D0%B2/)
- [Working consumer of the RCI neighbour address fields](https://blog.rvb.name/posts/pbr-keenetic)
- [OpenWrt 24.10 host-hint implementation](https://github.com/openwrt/luci/blob/0e6537981036ea221a73e497920192179e9f74c3/libs/rpcd-mod-luci/src/luci.c)
- [procd service notifications](https://github.com/openwrt/procd/blob/42d3937654508b04da64969f9d764ac2ec411904/service/service.c)
- [Native ubus subscriber API](https://github.com/openwrt/ubus/blob/afa57cce0aff82f4a7a0e509d4387ebc23dd3be7/libubus-sub.c)
