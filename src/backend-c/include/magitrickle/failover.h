/* Ordered failover selection; no config mutation and no network policy hidden
 * in the selector. Called by the netfilter owner for each IP family. */
#ifndef MAGITRICKLE_FAILOVER_H
#define MAGITRICKLE_FAILOVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "magitrickle/err.h"

typedef struct mt_route_candidate {
    int ifindex;
    uint8_t gateway[16];
    uint8_t gateway_len; /* zero means device-only */
} mt_route_candidate_t;

typedef struct mt_failover_ops {
    mt_err_t (*probe)(void *ctx, const char *name, int family,
                      mt_route_candidate_t *candidate, bool *available);
    /* Atomically replace the selected unicast route, leaving terminal
     * blackhole installed. unavailable=true is a link/gateway race. */
    mt_err_t (*replace)(void *ctx, int family, const mt_route_candidate_t *candidate,
                        bool *unavailable);
    /* Remove only the active unicast route; keep blackhole, mark and rule. */
    mt_err_t (*block)(void *ctx, int family);
} mt_failover_ops_t;

/* selected is SIZE_MAX for blackhole, otherwise an index into names.
 * Every pass verifies the installed route; unchanged routes are not written.
 * Lost external routes are repaired by the next event/recovery pass. */
mt_err_t mt_failover_reconcile(const char *const *names, size_t count, int family,
                              const mt_failover_ops_t *ops, void *ctx, size_t *selected);

#endif
