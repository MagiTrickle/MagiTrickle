/* Port of utils/netfilterTools/ipset-to-link.go's IPSetToLink: links one
 * group's ipset to a route interface via iptables chain rules (filter
 * FORWARD accept, mangle PREROUTING mark+connmark-save, nat POSTROUTING
 * masquerade) plus an ip-rule (fwmark -> table) and ip-route pair
 * (blackhole fallback + interface/gateway route) in a dedicated table.
 *
 * "blackhole" as the interface name (MT_IPSET_TO_LINK_BLACKHOLE) skips
 * the filter-FORWARD accept rule and the interface route, leaving only
 * the always-drop blackhole route -- matches Go's `Blackhole` constant.
 */
#ifndef MAGITRICKLE_IPSET_TO_LINK_H
#define MAGITRICKLE_IPSET_TO_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "magitrickle/err.h"
#include "magitrickle/ipset.h"
#include "magitrickle/iptables.h"
#include "magitrickle/rtnl.h"

#define MT_IPSET_TO_LINK_BLACKHOLE "blackhole"

typedef struct mt_ipset_to_link mt_ipset_to_link_t;

/* All pointer arguments are borrowed (must outlive this object); ipt4/ipt6
 * may each be NULL (matches Go's nh.IPTables4/6 being nil when a family is
 * disabled) but at least one must be non-NULL and must already have
 * "filter"/FORWARD, "mangle"/PREROUTING and "nat"/POSTROUTING registered
 * as patch chains by the caller (matches start.go's one-time
 * RegisterChainPatch calls before any group is enabled). */
mt_ipset_to_link_t *mt_ipset_to_link_new(const char *chain_name, const char *iface_name,
                                         mt_ipset_t *ipset, mt_ipt_t *ipt4, mt_ipt_t *ipt6,
                                         mt_rtnl_t *rtnl, uint32_t start_idx);
void mt_ipset_to_link_free(mt_ipset_to_link_t *l);

mt_err_t mt_ipset_to_link_enable(mt_ipset_to_link_t *l);
mt_err_t mt_ipset_to_link_disable(mt_ipset_to_link_t *l);
/* No-op unless currently disabled (matches Go's ClearIfDisabled: tears
 * down any leftover iptables/rule/route state from a previous run without
 * touching a group that's actively enabled). */
mt_err_t mt_ipset_to_link_clear_if_disabled(mt_ipset_to_link_t *l);

/* Re-stages this group's chains, rules and jumps for a full table
 * rebuild, leaving the write to the caller's single commit. No-op unless
 * the link is enabled. */
mt_err_t mt_ipset_to_link_prepare_iptables(mt_ipset_to_link_t *l);

/* Netlink-watcher-driven hooks (matches LinkUpHook/AddrChangeHook).
 * changed_iface_name/changed_ifindex identify which interface the
 * triggering event was about; callers should invoke these for every
 * mt_ipset_to_link_t whose configured RouteInterface() matches, exactly
 * as netlink.go's handleLink/handleAddr iterate the rule-set snapshot. */
mt_err_t mt_ipset_to_link_on_link_up(mt_ipset_to_link_t *l);
mt_err_t mt_ipset_to_link_on_addr_change(mt_ipset_to_link_t *l);

#endif /* MAGITRICKLE_IPSET_TO_LINK_H */
