/* Bounded multi-producer/multi-consumer pointer queue.
 *
 * Every queue in the backend is bounded (spec §11/§12). The overflow policy
 * is explicit at creation time:
 *   MT_QUEUE_REJECT — push on a full queue fails with MT_ERR_LIMIT and
 *                     increments the dropped counter (backpressure at the
 *                     producer, DNS-style).
 *   MT_QUEUE_DROP_OLDEST — push evicts the oldest element (its pointer is
 *                     returned via *evicted so the caller can free it) and
 *                     increments the dropped counter.
 *
 * Shutdown: mt_queue_close() wakes all blocked consumers; subsequent pushes
 * fail with MT_ERR_CLOSED; pops drain remaining elements, then return
 * MT_ERR_CLOSED. Destroy does not free element payloads — ownership of
 * queued pointers stays with the producer/consumer contract documented at
 * each usage site.
 */
#ifndef MAGITRICKLE_QUEUE_H
#define MAGITRICKLE_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#include "magitrickle/err.h"

typedef enum mt_queue_policy {
    MT_QUEUE_REJECT = 0,
    MT_QUEUE_DROP_OLDEST,
} mt_queue_policy_t;

typedef struct mt_queue mt_queue_t;

/* capacity must be >= 1. Returns NULL on allocation failure. */
mt_queue_t *mt_queue_create(size_t capacity, mt_queue_policy_t policy);
void mt_queue_destroy(mt_queue_t *q);

/* Non-blocking. *evicted (may be NULL) receives the dropped element when
 * policy is DROP_OLDEST and the queue was full; untouched otherwise. */
mt_err_t mt_queue_push(mt_queue_t *q, void *item, void **evicted);

/* Non-blocking pop: MT_ERR_AGAIN when empty (and open). */
mt_err_t mt_queue_try_pop(mt_queue_t *q, void **item);

/* Blocking pop with timeout; timeout_ms < 0 waits forever, 0 == try_pop.
 * MT_ERR_TIMEOUT on expiry, MT_ERR_CLOSED when closed and drained. */
mt_err_t mt_queue_pop(mt_queue_t *q, void **item, int timeout_ms);

void mt_queue_close(mt_queue_t *q);

size_t mt_queue_len(mt_queue_t *q);
size_t mt_queue_capacity(const mt_queue_t *q);
uint64_t mt_queue_dropped(mt_queue_t *q);

#endif /* MAGITRICKLE_QUEUE_H */
