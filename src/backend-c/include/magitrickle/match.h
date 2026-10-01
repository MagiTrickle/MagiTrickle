/* Rule matching — semantics must be identical to Go models.Rule.IsMatch
 * (compatibility-contract.md §6):
 *   domain     exact, case-sensitive
 *   namespace  exact or dot-boundary suffix (".example.com" also matches)
 *   wildcard   IGLOU-EU/go-wildcard v2 Match port: '*' any run, '?' exactly
 *              one byte, '.' IN THE PATTERN MATCHES ANY SINGLE BYTE (quirk
 *              of the library the Go backend uses) — case-sensitive
 *   regex      PCRE2, CASELESS|UTF|UCP, unanchored, match/depth limits;
 *              a pattern that fails to compile never matches (like Go's
 *              invalid-regex behaviour) and reports the error once
 *   subnet/subnet6  never match a domain name
 *
 * mt_matcher_t is the per-group index (decisions D-12 consumer arrives in
 * Phase 4): exact domains in a hash table, namespaces in a reversed-label
 * trie, wildcard/regex pre-compiled and scanned linearly. Group-level
 * matching is an OR over enabled rules, so index order is free.
 */
#ifndef MAGITRICKLE_MATCH_H
#define MAGITRICKLE_MATCH_H

#include <stdbool.h>

#include "magitrickle/err.h"
#include "magitrickle/models.h"

/* --- single-rule semantics (reference implementation) --- */

typedef struct mt_rule_matcher mt_rule_matcher_t;

/* Compiles one rule (regex is compiled eagerly here, unlike Go's lazy
 * sync.Once — observable behaviour is the same). Never fails for
 * non-regex types; invalid regex yields a matcher that never matches
 * (with compile_ok=false observable for diagnostics). */
mt_rule_matcher_t *mt_rule_matcher_new(const char *type, const char *rule);
void mt_rule_matcher_free(mt_rule_matcher_t *m);
bool mt_rule_matcher_ok(const mt_rule_matcher_t *m); /* regex compiled? */
bool mt_rule_matcher_match(mt_rule_matcher_t *m, const char *domain);

/* Standalone wildcard match (exact IGLOU go-wildcard v2 port). */
bool mt_wildcard_match(const char *pattern, const char *s);

/* --- per-group index --- */

typedef struct mt_matcher mt_matcher_t;

mt_matcher_t *mt_matcher_new(void);
void mt_matcher_free(mt_matcher_t *m);

/* Adds an enabled rule to the index. Skips disabled rules at the caller. */
mt_err_t mt_matcher_add(mt_matcher_t *m, const char *type, const char *rule);

/* True when any indexed rule matches the domain. */
bool mt_matcher_match(mt_matcher_t *m, const char *domain);

#endif /* MAGITRICKLE_MATCH_H */
