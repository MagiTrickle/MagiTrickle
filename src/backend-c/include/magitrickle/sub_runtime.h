/* Subscription -> runtime group synthesis — port of
 * subscriptions/runtime_rule_sets.go's subscriptionAsRuntimeRuleSet.
 *
 * Go's RuleSet operates over a generic rulesets.Spec (Model *models.Group
 * for user groups, or a synthesized Spec value for subscriptions, built
 * fresh on every rebuild). The C port's mt_ruleset_t is built directly
 * around mt_group_t (Phase 5); rather than generalizing it to a Spec-like
 * abstraction, this module mirrors Go's own approach at the subscription
 * side: synthesize a fresh, independent mt_group_t from a mt_subscription_t
 * on every rebuild, then hand it to the existing, unmodified
 * mt_ruleset_new() exactly as a real config-file group would be. The
 * synthesized group's `color` field is left NULL (ruleset.c never reads
 * it; only the HTTP JSON layer -- which never sees synthesized groups --
 * does).
 */
#ifndef MAGITRICKLE_SUB_RUNTIME_H
#define MAGITRICKLE_SUB_RUNTIME_H

#include "magitrickle/models.h"

/* Builds a new mt_group_t (caller owns, mt_group_free) from `sub`:
 * id/name/interface copied; enable = sub->enable && sub->iface is
 * non-empty (matches Go's `enable := sub.Enable; if sub.Interface == ""
 * { enable = false }`); name falls back to "subscription:<id>" when
 * sub->name is empty (matches Go's fmt.Sprintf fallback); rules are
 * mt_sub_rule_t -> mt_rule_t 1:1 (Name always "", matching Go). Returns
 * NULL on OOM. */
mt_group_t *mt_sub_runtime_group(const mt_subscription_t *sub);

#endif /* MAGITRICKLE_SUB_RUNTIME_H */
