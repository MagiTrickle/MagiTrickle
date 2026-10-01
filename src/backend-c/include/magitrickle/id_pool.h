/* Per-operation random ID batches; no shared PRNG or persistent descriptor. */
#ifndef MAGITRICKLE_ID_POOL_H
#define MAGITRICKLE_ID_POOL_H
#include "magitrickle/id.h"
#include "magitrickle/lookup.h"
#include "magitrickle/rand.h"
typedef struct mt_id_pool { mt_id_t ids[256]; size_t next, available; } mt_id_pool_t;
static inline mt_err_t mt_id_pool_unique(mt_lookup_t *used, mt_id_pool_t *pool, mt_id_t *out) {
    for (unsigned attempt = 0; attempt < 128; attempt++) {
        if (pool->next == pool->available) {
            mt_err_t err = mt_random_bytes((uint8_t *)pool->ids, sizeof(pool->ids));
            if (err != MT_OK) { return err; }
            pool->next = 0;
            pool->available = sizeof(pool->ids) / sizeof(pool->ids[0]);
        }
        mt_id_t id = pool->ids[pool->next++];
        if (mt_id_is_zero(id) || mt_lookup_get(used, id.b, sizeof(id.b), NULL)) { continue; }
        mt_err_t err = mt_lookup_put(used, id.b, sizeof(id.b), 0, NULL);
        if (err == MT_OK) { *out = id; }
        return err;
    }
    return MT_ERR_SYS;
}
#endif
