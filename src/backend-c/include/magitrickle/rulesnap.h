/* Immutable rule-set snapshot — the DNS hot path's view of "which groups
 * would this domain route into" (spec §14, decisions.md D-12/D-17).
 *
 * Built once from a loaded mt_config_t and swapped in atomically; matching
 * never touches the config tree or takes a lock (compatibility-contract.md
 * §6-7, migration-plan.md Phase 4 exit criterion "no global lock in match
 * path"). Only *enabled* groups are included — a disabled group can never
 * produce an observable action (Go's RuleSet.AddIPv4Subnet no-ops when the
 * runtime/model enable flag is off), so there is nothing to gain from
 * indexing it; see decisions.md for the full argument.
 *
 * Per-group matching is aggregated into one mt_matcher_t (OR over the
 * group's enabled rules) rather than replaying Go's per-rule break/continue
 * loop, because the two are observably equivalent here: every action Go's
 * loop can produce for a given (group, DNS record) pair uses the same
 * inputs regardless of *which* rule matched (same IP/TTL for A/AAAA, same
 * cached address set for CNAME) — see decisions.md D-18 for the full
 * argument and the one deliberate side effect this changes (Go can invoke
 * the ipset call multiple times for one CNAME event when several rules in
 * a group match; this collapses it to once).
 *
 * Subscription-derived entries participate too (Phase 7): one synthesized
 * mt_group_t per enabled subscription (mt_sub_runtime_group), matched
 * exactly like a real config group; see snapshot.c.
 *
 * Concurrency: single-threaded today (everything runs on the loop thread,
 * decisions.md D-17), so "atomic swap" here is just a pointer store from
 * that same thread — no refcounting/epoch reclamation needed yet. The
 * moment a writer thread exists (e.g. Phase 7 subscription fetch on a
 * worker thread), the *build* can happen off-thread but the *swap* must be
 * marshalled onto the loop thread via mt_loop_post before publishing.
 */
#ifndef MAGITRICKLE_RULESNAP_H
#define MAGITRICKLE_RULESNAP_H

#include <stddef.h>

#include "magitrickle/id.h"
#include "magitrickle/match.h"
#include "magitrickle/models.h"

typedef struct mt_group_snapshot {
    mt_id_t id;
    char *name; /* owned copy, for logging only */
    mt_matcher_t *matcher;
} mt_group_snapshot_t;

typedef struct mt_ruleset_snapshot {
    mt_group_snapshot_t **groups;
    size_t n_groups;
} mt_ruleset_snapshot_t;

/* Builds a fresh snapshot from the config's user groups. Never returns
 * NULL except on OOM. */
mt_ruleset_snapshot_t *mt_ruleset_snapshot_build(const mt_config_t *cfg);
void mt_ruleset_snapshot_free(mt_ruleset_snapshot_t *snap);

#endif /* MAGITRICKLE_RULESNAP_H */
