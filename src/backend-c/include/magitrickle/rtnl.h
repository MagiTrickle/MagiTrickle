/* rtnetlink (NETLINK_ROUTE) helper — port of the ip-rule/ip-route/link
 * pieces of utils/netfilterTools/ipset-to-link.go (insertIPRule,
 * insertIPRoute/updateIfaceRoute/deleteIPRoute, getUnusedMarkAndTable,
 * getGwFromIface) via libmnl, replacing github.com/vishvananda/netlink.
 *
 * Unlike ipset.h/iptables.h, this module's wire correctness IS verified
 * empirically in this sandbox: unlike ip_set, plain rtnetlink (ip rule/
 * route/link) works here (confirmed via `ip rule`/`ip route`/`ip link`),
 * so rather than byte-mirroring every attribute vishvananda/netlink
 * happens to send (including some that look like incidental zero-values,
 * e.g. an always-present RTA_OIF=0 on non-interface routes), this port
 * builds idiomatic, standard rtnetlink requests and the manual/functional
 * test in phase-5-report.md confirms the resulting kernel state (rule/
 * route tables, via `ip rule`/`ip route show table N`) matches what the
 * Go code is documented to produce. See decisions.md for this choice.
 */
#ifndef MAGITRICKLE_RTNL_H
#define MAGITRICKLE_RTNL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "magitrickle/err.h"

typedef struct mt_rtnl mt_rtnl_t;

mt_rtnl_t *mt_rtnl_open(void);
void mt_rtnl_close(mt_rtnl_t *r);

/* "ip rule add from all fwmark <mark> lookup <table>" / del. MT_OK if the
 * rule to delete doesn't exist (matches Go's unix.ENOENT swallow). */
mt_err_t mt_rtnl_rule_add(mt_rtnl_t *r, int family, uint32_t mark, uint32_t table);
mt_err_t mt_rtnl_rule_del(mt_rtnl_t *r, int family, uint32_t mark, uint32_t table);

/* Default route (dst 0/0) in `table`, type blackhole, given priority.
 * MT_OK if "already exists" (add) / "no such process" (del) -- matches
 * Go's EEXIST/ESRCH swallows in insertIPRoute/deleteIPRoute. */
mt_err_t mt_rtnl_route_add_blackhole(mt_rtnl_t *r, int family, uint32_t table, uint32_t priority);
mt_err_t mt_rtnl_route_del_blackhole(mt_rtnl_t *r, int family, uint32_t table, uint32_t priority);

/* Default route (dst 0/0) in `table` via oif (+ optional gateway),
 * priority. gw may be NULL/gw_len 0 for a device-only (point-to-point
 * style) route. Same EEXIST/ESRCH/ENODEV swallows as Go's
 * updateIfaceRoute/deleteIPRoute. ENODEV ("interface not ready for this
 * family") is reported back via *enodev so the caller can replicate Go's
 * "skip this route, log a warning" branch. */
mt_err_t mt_rtnl_route_add_iface(mt_rtnl_t *r, int family, uint32_t table, uint32_t priority,
                                 int oif, const uint8_t *gw, uint8_t gw_len, bool *enodev);
mt_err_t mt_rtnl_route_del_iface(mt_rtnl_t *r, int family, uint32_t table, uint32_t priority,
                                 int oif, const uint8_t *gw, uint8_t gw_len);

typedef struct mt_link_info {
    int ifindex;
    bool up;
    bool point_to_point;
} mt_link_info_t;

/* *found=false + MT_OK when the interface doesn't exist yet (matches Go's
 * LinkNotFoundError swallow at the ipset-to-link call site: "it can be
 * caught later" via the link-up watcher hook). */
mt_err_t mt_rtnl_link_by_name(mt_rtnl_t *r, const char *name, mt_link_info_t *out, bool *found);

/* First gateway found among routes via ifindex (matches getGwFromIface).
 * *found=false + MT_OK when none exists (not an error condition there). */
mt_err_t mt_rtnl_gateway_for_iface(mt_rtnl_t *r, int family, int ifindex, bool *found, uint8_t *gw,
                                   uint8_t *gw_len);

/* Scans existing rules (all families) + routes (all families) for
 * mark/table usage starting at start_idx, matching getUnusedMarkAndTable
 * (tables 0/253/254/255 are always considered used). */
mt_err_t mt_rtnl_alloc_mark_table(mt_rtnl_t *r, uint32_t start_idx, uint32_t *out_idx);

#endif /* MAGITRICKLE_RTNL_H */
