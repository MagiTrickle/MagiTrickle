/* Rtnetlink watcher for interface, address, and route changes.
 * Owns a NETLINK_ROUTE multicast socket borrowed by the event loop.
 * On receive loss or socket failure, asks for full state resynchronization;
 * reconnect attempts are one-shot (backoff), never periodic polling.
 */
#ifndef MAGITRICKLE_NETLINK_WATCHER_H
#define MAGITRICKLE_NETLINK_WATCHER_H

#include <stdbool.h>
#include "magitrickle/err.h"
#include "magitrickle/loop.h"

typedef struct mt_nl_watcher mt_nl_watcher_t;
typedef void (*mt_nl_link_cb)(const char *iface_name, bool up, void *ud);
typedef void (*mt_nl_addr_cb)(const char *iface_name, void *ud);
typedef void (*mt_nl_change_cb)(void *ud);

/* Backward-compatible link/address subscriber. */
mt_err_t mt_nl_watcher_create(mt_loop_t *loop, mt_nl_link_cb link_cb, void *link_ud,
                              mt_nl_addr_cb addr_cb, void *addr_ud, mt_nl_watcher_t **out);

/* Extended watcher: route_cb is notified on every IPv4/IPv6 route change.
 * resync_cb handles overflow, malformed messages, socket loss and reconnect.
 * All callbacks run on the event-loop thread. */
mt_err_t mt_nl_watcher_create_with_routes(mt_loop_t *loop, mt_nl_link_cb link_cb,
                                          void *link_ud, mt_nl_addr_cb addr_cb,
                                          void *addr_ud, mt_nl_change_cb route_cb,
                                          void *route_ud, mt_nl_change_cb resync_cb,
                                          void *resync_ud, mt_nl_watcher_t **out);
void mt_nl_watcher_destroy(mt_nl_watcher_t *w);
#endif
