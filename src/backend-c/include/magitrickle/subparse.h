/* Subscription list parsing/refresh — port of src/backend/subscriptions
 * (parse.go, validate.go, auto_update.go).
 *
 * Behaviour contract:
 * - tokenized on '\n', '\r' and ',', trimmed, empty and '#'-prefixed
 *   entries skipped;
 * - duplicates removed by (detected type + '|' + text);
 * - type auto-detection order: subnet6, subnet, namespace, domain, regex,
 *   wildcard, "" (note: namespace and domain use the same validator, so
 *   plain domains always detect as "namespace" — Go quirk kept);
 * - refresh keeps ID/Enable/Type of entries whose exact text survived,
 *   assigns fresh unique random IDs to the rest;
 * - is_due mirrors subscriptions.IsDue.
 */
#ifndef MAGITRICKLE_SUBPARSE_H
#define MAGITRICKLE_SUBPARSE_H

#include <stdbool.h>
#include <stdint.h>

#include "magitrickle/err.h"
#include "magitrickle/models.h"

/* Detected type for a single trimmed pattern; returns one of the
 * MT_RULE_* constants or "" (never NULL). */
const char *mt_sub_detect_type(const char *pattern);

/* Parses a raw list into an array of mt_sub_rule_t (caller owns).
 * Every parsed rule: random unique ID, Enable=true. */
mt_err_t mt_sub_parse_rules(const char *list, mt_sub_rule_t ***out_rules,
                            size_t *out_n);

/* RefreshRules port: parse `list`, then preserve ID/Enable/Type from
 * `existing` for rules with identical text. Returns a fresh array. */
mt_err_t mt_sub_refresh_rules(const char *list, mt_sub_rule_t **existing,
                              size_t n_existing, mt_sub_rule_t ***out_rules,
                              size_t *out_n);

/* sameRules port (order-insensitive multiset compare incl. ID/Enable). */
bool mt_sub_same_rules(mt_sub_rule_t **left, size_t n_left,
                       mt_sub_rule_t **right, size_t n_right);

/* IsDue port. */
bool mt_sub_is_due(const mt_subscription_t *sub, int64_t now_unix);

#endif /* MAGITRICKLE_SUBPARSE_H */
