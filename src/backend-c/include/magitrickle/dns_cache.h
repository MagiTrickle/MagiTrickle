/* DNS records cache — port of src/backend/utils/recordsCache (Go).
 *
 * Semantics (compatibility-contract.md §5), ported 1:1 from records.go:
 * - domain -> list of (address, deadline), deduped by exact address bytes;
 *   re-adding the same address refreshes its deadline.
 * - domain -> single alias (CNAME target) + deadline; last write wins;
 *   a self-alias (domain == alias) is ignored.
 * - reverse index alias -> [domains pointing at it], used by
 *   mt_cache_get_aliases (BFS; returns the domain itself plus every domain
 *   that transitively CNAMEs to it). This lookup does NOT filter by TTL —
 *   only mt_cache_cleanup() and mt_cache_get_addresses() consult deadlines
 *   (exact behavioural parity with Go, which has the same asymmetry).
 * - mt_cache_get_addresses walks the alias chain FORWARD (domain -> its own
 *   alias -> ...) with a cycle guard, returning the first hop with any
 *   unexpired address.
 *
 * Concurrency: this module has NO internal locking. It is designed to be
 * touched only from the single event-loop thread (decisions.md D-02/D-17)
 * — the same thread that runs the DNS proxy callbacks and the cleanup
 * timer. If a future phase calls it from another thread, that call must be
 * marshalled onto the loop thread first (mt_loop_post).
 *
 * Bounds (hardening beyond Go, spec §15): the domain count is capped;
 * once at capacity, insertion of a *new* domain is rejected and counted in
 * mt_cache_dropped() rather than growing unbounded. Existing domains can
 * still gain addresses/aliases. Per-domain address list length is left
 * unbounded like Go (naturally small: distinct IPs actually seen).
 */
#ifndef MAGITRICKLE_DNS_CACHE_H
#define MAGITRICKLE_DNS_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "magitrickle/err.h"

#define MT_CACHE_DEFAULT_MAX_DOMAINS 65536

typedef struct mt_cache mt_cache_t;

typedef struct mt_cache_addr {
    uint8_t addr[16];
    uint8_t addr_len; /* 4 or 16 */
    int64_t deadline; /* unix seconds */
} mt_cache_addr_t;

mt_cache_t *mt_cache_create(size_t max_domains); /* 0 -> MT_CACHE_DEFAULT_MAX_DOMAINS */
void mt_cache_destroy(mt_cache_t *c);

/* ttl_seconds is the *total* TTL (record TTL + additionalTTL), matching
 * Go's ttlDuration; deadline is computed as now+ttl_seconds internally. */
void mt_cache_add_address(mt_cache_t *c, const char *domain,
                          const uint8_t *addr, uint8_t addr_len,
                          uint32_t ttl_seconds, int64_t now);

/* Self-alias (domain == alias) is silently ignored, like Go. */
void mt_cache_add_alias(mt_cache_t *c, const char *domain, const char *alias,
                        uint32_t ttl_seconds, int64_t now);

/* Walks the alias chain forward; returns a malloc'd array (caller frees
 * with free()) of valid (non-expired) addresses at the first hop that has
 * any, or NULL (with *out_n==0) if none / cycle / missing. */
mt_err_t mt_cache_get_addresses(mt_cache_t *c, const char *domain,
                                int64_t now, mt_cache_addr_t **out,
                                size_t *out_n);

/* BFS over the reverse-alias graph: domain itself + every domain that
 * transitively CNAMEs to it. Returns a malloc'd array of malloc'd strings
 * (free each string, then the array) via mt_cache_free_strings. Always
 * contains at least the domain itself (mirrors Go, even for unknown
 * domains). */
mt_err_t mt_cache_get_aliases(mt_cache_t *c, const char *domain,
                              char ***out, size_t *out_n);

/* Union of domains present in the address or alias tables. Same
 * ownership/free convention as mt_cache_get_aliases. */
mt_err_t mt_cache_list_known_domains(mt_cache_t *c, char ***out,
                                     size_t *out_n);

void mt_cache_free_strings(char **strs, size_t n);

/* Physically removes expired address entries (compacting each domain's
 * list) and expired aliases (fixing up the reverse index); a domain node
 * with no addresses and no alias left is removed entirely. Call
 * periodically (Go: every 30s) from the loop thread. */
void mt_cache_cleanup(mt_cache_t *c, int64_t now);

size_t mt_cache_domain_count(const mt_cache_t *c);
uint64_t mt_cache_dropped(const mt_cache_t *c);

#endif /* MAGITRICKLE_DNS_CACHE_H */
