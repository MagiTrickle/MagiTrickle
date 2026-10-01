/* DNS response processing pipeline — port of the observable-state slice
 * of src/backend/dns.go's handleMessage/processARecord/processAAAARecord/
 * processCNameRecord (compatibility-contract.md §4-7).
 *
 * Owns no cache or snapshot; both are injected so the daemon controls
 * their lifetime (the cache persists across config reloads, the snapshot
 * is replaced on each reload — decisions.md D-17).
 *
 * Only successful responses (Rcode == 0) are processed, matching Go; A/AAAA
 * with the wrong rdata length are skipped (Go logs and returns); Answer-
 * section records only (Authority/Additional ignored). Each matching
 * (group, record) pair is reported once via the sink callback instead of
 * replaying Go's per-rule loop — see decisions.md D-18 for why this is
 * state-equivalent. The sink is a stub in Phase 4 (main.c logs it); Phase 5
 * replaces its body with a real ipset add.
 */
#ifndef MAGITRICKLE_DNSPIPELINE_H
#define MAGITRICKLE_DNSPIPELINE_H

#include <stdint.h>

#include "magitrickle/dns_cache.h"
#include "magitrickle/dnswire.h"
#include "magitrickle/rulesnap.h"

typedef struct mt_match_action {
    mt_id_t group_id;
    const char *group_name;
    const uint8_t *addr;
    uint8_t addr_len; /* 4 or 16 */
    uint32_t ttl_seconds;
    const char *record_domain; /* domain the DNS answer was about */
    const char *matched_name;  /* alias that satisfied a rule, for logs */
    const char *record_kind;   /* "A" | "AAAA" | "CNAME" */
} mt_match_action_t;

typedef void (*mt_match_sink_cb)(const mt_match_action_t *action, void *ud);

typedef struct mt_dns_pipeline mt_dns_pipeline_t;

/* Does not take ownership of cache. */
mt_dns_pipeline_t *mt_dns_pipeline_create(mt_cache_t *cache,
                                          uint32_t additional_ttl_seconds,
                                          mt_match_sink_cb sink,
                                          void *sink_ud);
void mt_dns_pipeline_destroy(mt_dns_pipeline_t *p);

/* Takes ownership of snap; frees the previously active one. Call from the
 * loop thread (decisions.md D-17). */
void mt_dns_pipeline_set_snapshot(mt_dns_pipeline_t *p,
                                  mt_ruleset_snapshot_t *snap);

void mt_dns_pipeline_handle_message(mt_dns_pipeline_t *p,
                                    const mt_dns_msg_t *msg, int64_t now);

/* Updates additional_ttl_seconds -- Go's dns.go reads
 * a.config.Netfilter.IPSet.AdditionalTTL fresh on every matched record,
 * unlike the cache/snapshot pointers above (injected once). Used by the
 * SIGHUP config reload (main.c) so this one setting takes live effect
 * exactly like it does in Go, without recreating the pipeline. */
void mt_dns_pipeline_set_additional_ttl(mt_dns_pipeline_t *p,
                                        uint32_t additional_ttl_seconds);

#endif /* MAGITRICKLE_DNSPIPELINE_H */
