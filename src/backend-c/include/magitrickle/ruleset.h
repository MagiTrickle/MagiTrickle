/* Per-group netfilter runtime state — port of rule_set.go's RuleSet type
 * (Enable/Disable/Sync/AddIPv4Subnet/AddIPv6Subnet/LinkUpHook/AddrChangeHook),
 * built on the Phase 5 ipset/ipset_to_link/rtnl modules.
 *
 * Unlike mt_ruleset_snapshot_t (rulesnap.h — DNS-hot-path-only: aggregates
 * enabled rules into one matcher per enabled group, skips disabled groups
 * entirely, and has no notion of subnet/subnet6 rules or the group's
 * interface), this module is netfilter-facing and mirrors Go's
 * app.userRuleSets: one mt_ruleset_t exists per *configured* group
 * regardless of that group's `enable` flag, so a group that gets disabled
 * still has its leftover iptables/ipset/rule/route state torn down via
 * mt_ruleset_disable() (matching Go's ClearIfDisabled/disable() paths).
 *
 * Runtime-enabled vs configured-enabled mirrors Go exactly: mt_ruleset_enable()
 * always marks the object runtime-enabled (idempotent, matches the CAS in
 * RuleSet.enable()) but only actually creates the ipset/ipset-to-link/chain
 * state when the group's `enable` field is true; mt_ruleset_add_ipv4/6,
 * mt_ruleset_sync, mt_ruleset_on_link_up/on_addr_change all no-op unless both
 * flags are set, exactly like Go's AddIPv4Subnet/Sync/LinkUpHook/AddrChangeHook.
 *
 * Concurrency: no internal locking, matching decisions.md D-17/D-19 (single
 * loop thread owns all netfilter mutation in the C backend, unlike Go's
 * atomic.Bool + sync.Mutex guards needed because multiple goroutines share
 * one *RuleSet there).
 */
#ifndef MAGITRICKLE_RULESET_H
#define MAGITRICKLE_RULESET_H

#include <stdbool.h>
#include <stdint.h>

#include "magitrickle/dns_cache.h"
#include "magitrickle/err.h"
#include "magitrickle/ipset.h"
#include "magitrickle/ipset_to_link.h"
#include "magitrickle/iptables.h"
#include "magitrickle/models.h"
#include "magitrickle/rtnl.h"

typedef struct mt_ruleset mt_ruleset_t;

/* All pointers borrowed; must outlive every mt_ruleset_t built with them.
 * ipset_prefix/chain_prefix are borrowed too (typically the
 * cfg.app.netfilter.ipset.table_prefix / cfg.app.netfilter.iptables.chain_prefix
 * strings), matching Go's Helper.IpsetPrefix/ChainPrefix. */
typedef struct mt_ruleset_deps {
    mt_ipt_t *ipt4;
    mt_ipt_t *ipt6;
    mt_rtnl_t *rtnl;
    const char *ipset_prefix;
    const char *chain_prefix;
    uint32_t start_idx;
} mt_ruleset_deps_t;

/* group is borrowed (must outlive the ruleset -- callers keep the owning
 * mt_config_t alive for the ruleset's lifetime, exactly like Go's RuleSet
 * holding spec.Model). deps is copied by value (the pointers it holds
 * remain borrowed). */
mt_ruleset_t *mt_ruleset_new(const mt_group_t *group, const mt_ruleset_deps_t *deps);
void mt_ruleset_free(mt_ruleset_t *rs);

const mt_group_t *mt_ruleset_group(const mt_ruleset_t *rs);
/* Mutable accessor to the same borrowed group pointer mt_ruleset_group
 * returns -- the group is genuinely mutable (owned by cfg->groups, see
 * app.h's header comment), the ruleset just stores it as const to keep
 * ruleset.c itself read-only. Used by the HTTP group/rule handlers
 * (Phase 6) to mutate a group's fields in place, mirroring Go's
 * RuleSet.Model() returning a *models.Group the caller mutates directly. */
mt_group_t *mt_ruleset_group_mut(mt_ruleset_t *rs);
bool mt_ruleset_runtime_enabled(const mt_ruleset_t *rs);

mt_err_t mt_ruleset_enable(mt_ruleset_t *rs);
mt_err_t mt_ruleset_disable(mt_ruleset_t *rs);

/* Re-stages this group's iptables chains for a full table rebuild without
 * writing them (the rebuild commits the whole table at once) and without
 * touching the group's ipset, ip rule or routes -- those survive a table
 * rewrite by the firmware and must not be torn down and recreated, which
 * would drop every address the DNS path has cached into the set. No-op
 * unless both runtime- and configured-enabled, like every other mutator
 * here. */
mt_err_t mt_ruleset_prepare_iptables(mt_ruleset_t *rs);

/* Rebuilds ipset contents from the group's subnet/subnet6 rules (static
 * CIDRs, including the "0.0.0.0/0"/"::/0" two-halves split -- see
 * decisions.md) plus domain-type rules matched against the cache's known
 * domains, diffed against the ipset's current contents -- port of
 * rule_set.go's sync(). No-op unless both runtime- and configured-enabled. */
mt_err_t mt_ruleset_sync(mt_ruleset_t *rs, mt_cache_t *cache, int64_t now);

/* Direct single-address add -- port of RuleSet.AddIPv4Subnet/AddIPv6Subnet,
 * called from the DNS hot path for a freshly-resolved A/AAAA/CNAME address.
 * ttl may be NULL (permanent entry). */
mt_err_t mt_ruleset_add_ipv4(mt_ruleset_t *rs, mt_ipv4_subnet_t subnet, const uint32_t *ttl);
mt_err_t mt_ruleset_add_ipv6(mt_ruleset_t *rs, mt_ipv6_subnet_t subnet, const uint32_t *ttl);

/* Netlink-watcher-driven hooks -- port of LinkUpHook/AddrChangeHook. Callers
 * dispatch only to rulesets whose group interface matches the event's
 * interface name, exactly as netlink.go's handleLink/handleAddr iterate the
 * rule-set snapshot. */
mt_err_t mt_ruleset_on_link_up(mt_ruleset_t *rs);
mt_err_t mt_ruleset_on_addr_change(mt_ruleset_t *rs);

#endif /* MAGITRICKLE_RULESET_H */
