/* Netlink link/addr watcher — port of netlink.go's subscribeLinkUpdates/
 * subscribeAddrUpdates + handleLink/handleAddr dispatch (the "netlink
 * watcher must at least keep: link add/remove, link state, address
 * add/remove; no route watcher unless required" constraint from the
 * master spec).
 *
 * Scope matches Go exactly: RTM_NEWLINK only dispatches when the link's
 * IFF_UP flag is set (Go: `if linkAttrs.Flags&net.FlagUp == 0 { break }`);
 * RTM_DELLINK is log-only (no hook call) in Go and stays that way here;
 * address events only dispatch for RTM_NEWADDR (Go: `if !event.NewAddr {
 * return }`), never RTM_DELADDR. Callbacks receive the interface *name*
 * (resolved from ifindex for address events) so the caller can match it
 * against each group's configured RouteInterface(), exactly like
 * handleLink/handleAddr iterating the rule-set snapshot in Go.
 */
#ifndef MAGITRICKLE_NETLINK_WATCHER_H
#define MAGITRICKLE_NETLINK_WATCHER_H

#include <stdbool.h>

#include "magitrickle/err.h"
#include "magitrickle/loop.h"

typedef struct mt_nl_watcher mt_nl_watcher_t;

typedef void (*mt_nl_link_cb)(const char *iface_name, bool up, void *ud);
typedef void (*mt_nl_addr_cb)(const char *iface_name, void *ud);

/* Registers an fd with `loop` (borrowed; watcher must be destroyed before
 * the loop). Either callback may be NULL to ignore that event class. */
mt_err_t mt_nl_watcher_create(mt_loop_t *loop, mt_nl_link_cb link_cb, void *link_ud,
                              mt_nl_addr_cb addr_cb, void *addr_ud, mt_nl_watcher_t **out);
void mt_nl_watcher_destroy(mt_nl_watcher_t *w);

#endif /* MAGITRICKLE_NETLINK_WATCHER_H */
