#include "magitrickle/queue.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

struct mt_queue {
    pthread_mutex_t mu;
    pthread_cond_t nonempty;
    void **items;
    size_t capacity;
    size_t head; /* next pop position */
    size_t len;
    uint64_t dropped;
    mt_queue_policy_t policy;
    bool closed;
};

mt_queue_t *mt_queue_create(size_t capacity, mt_queue_policy_t policy)
{
    if (capacity == 0) {
        return NULL;
    }
    mt_queue_t *q = calloc(1, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }
    q->items = (void **)calloc(capacity, sizeof(void *));
    if (q->items == NULL) {
        free(q);
        return NULL;
    }
    q->capacity = capacity;
    q->policy = policy;
    if (pthread_mutex_init(&q->mu, NULL) != 0) {
        free((void *)q->items);
        free(q);
        return NULL;
    }
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0 ||
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&q->nonempty, &attr) != 0) {
        pthread_mutex_destroy(&q->mu);
        free((void *)q->items);
        free(q);
        return NULL;
    }
    pthread_condattr_destroy(&attr);
    return q;
}

void mt_queue_destroy(mt_queue_t *q)
{
    if (q == NULL) {
        return;
    }
    pthread_cond_destroy(&q->nonempty);
    pthread_mutex_destroy(&q->mu);
    free((void *)q->items);
    free(q);
}

mt_err_t mt_queue_push(mt_queue_t *q, void *item, void **evicted)
{
    mt_err_t err = MT_OK;
    pthread_mutex_lock(&q->mu);
    if (q->closed) {
        err = MT_ERR_CLOSED;
    } else if (q->len == q->capacity) {
        if (q->policy == MT_QUEUE_REJECT) {
            q->dropped++;
            err = MT_ERR_LIMIT;
        } else {
            /* DROP_OLDEST: evict head, then append. */
            if (evicted != NULL) {
                *evicted = q->items[q->head];
            }
            q->head = (q->head + 1) % q->capacity;
            q->len--;
            q->dropped++;
            q->items[(q->head + q->len) % q->capacity] = item;
            q->len++;
        }
    } else {
        q->items[(q->head + q->len) % q->capacity] = item;
        q->len++;
    }
    if (err == MT_OK) {
        pthread_cond_signal(&q->nonempty);
    }
    pthread_mutex_unlock(&q->mu);
    return err;
}

static mt_err_t pop_locked(mt_queue_t *q, void **item)
{
    if (q->len == 0) {
        return q->closed ? MT_ERR_CLOSED : MT_ERR_AGAIN;
    }
    *item = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->len--;
    return MT_OK;
}

mt_err_t mt_queue_try_pop(mt_queue_t *q, void **item)
{
    pthread_mutex_lock(&q->mu);
    mt_err_t err = pop_locked(q, item);
    pthread_mutex_unlock(&q->mu);
    return err;
}

mt_err_t mt_queue_pop(mt_queue_t *q, void **item, int timeout_ms)
{
    if (timeout_ms == 0) {
        return mt_queue_try_pop(q, item);
    }

    struct timespec deadline;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&q->mu);
    for (;;) {
        mt_err_t err = pop_locked(q, item);
        if (err != MT_ERR_AGAIN) {
            pthread_mutex_unlock(&q->mu);
            return err;
        }
        int rc;
        if (timeout_ms < 0) {
            rc = pthread_cond_wait(&q->nonempty, &q->mu);
        } else {
            rc = pthread_cond_timedwait(&q->nonempty, &q->mu, &deadline);
        }
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&q->mu);
            return MT_ERR_TIMEOUT;
        }
    }
}

void mt_queue_close(mt_queue_t *q)
{
    pthread_mutex_lock(&q->mu);
    q->closed = true;
    pthread_cond_broadcast(&q->nonempty);
    pthread_mutex_unlock(&q->mu);
}

size_t mt_queue_len(mt_queue_t *q)
{
    pthread_mutex_lock(&q->mu);
    size_t n = q->len;
    pthread_mutex_unlock(&q->mu);
    return n;
}

size_t mt_queue_capacity(const mt_queue_t *q)
{
    return q->capacity;
}

uint64_t mt_queue_dropped(mt_queue_t *q)
{
    pthread_mutex_lock(&q->mu);
    uint64_t n = q->dropped;
    pthread_mutex_unlock(&q->mu);
    return n;
}
