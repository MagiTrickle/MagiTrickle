#include "magitrickle/failover.h"

mt_err_t mt_failover_reconcile(const char *const *names, size_t count, int family,
                              const mt_failover_ops_t *ops, void *ctx, size_t *selected)
{
    if (!ops || !ops->probe || !ops->replace || !ops->block || !selected ||
        (count && !names)) { return MT_ERR_INVAL; }
    *selected = SIZE_MAX;
    mt_err_t first_error = MT_OK;
    for (size_t i = 0; i < count; i++) {
        mt_route_candidate_t candidate = {0};
        bool available = false;
        mt_err_t err = ops->probe(ctx, names[i], family, &candidate, &available);
        if (err != MT_OK) { first_error = err; break; }
        if (!available) { continue; }
        bool unavailable = false;
        err = ops->replace(ctx, family, &candidate, &unavailable);
        if (err != MT_OK) { first_error = err; break; }
        if (unavailable) { continue; }
        *selected = i;
        return MT_OK;
    }
    mt_err_t err = ops->block(ctx, family);
    return first_error != MT_OK ? first_error : err;
}
