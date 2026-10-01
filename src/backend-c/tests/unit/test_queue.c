#include "greatest.h"

#include <pthread.h>

#include "magitrickle/queue.h"

TEST reject_policy_drops_and_counts(void)
{
    mt_queue_t *q = mt_queue_create(2, MT_QUEUE_REJECT);
    ASSERT(q != NULL);

    int a = 1, b = 2, c = 3;
    ASSERT_EQ(MT_OK, mt_queue_push(q, &a, NULL));
    ASSERT_EQ(MT_OK, mt_queue_push(q, &b, NULL));
    ASSERT_EQ(MT_ERR_LIMIT, mt_queue_push(q, &c, NULL));
    ASSERT_EQ(1u, (unsigned)mt_queue_dropped(q));
    ASSERT_EQ(2u, (unsigned)mt_queue_len(q));

    void *item = NULL;
    ASSERT_EQ(MT_OK, mt_queue_try_pop(q, &item));
    ASSERT_EQ(&a, item);
    ASSERT_EQ(MT_OK, mt_queue_try_pop(q, &item));
    ASSERT_EQ(&b, item);
    ASSERT_EQ(MT_ERR_AGAIN, mt_queue_try_pop(q, &item));

    mt_queue_destroy(q);
    PASS();
}

TEST drop_oldest_policy_evicts_head(void)
{
    mt_queue_t *q = mt_queue_create(2, MT_QUEUE_DROP_OLDEST);
    ASSERT(q != NULL);

    int a = 1, b = 2, c = 3;
    ASSERT_EQ(MT_OK, mt_queue_push(q, &a, NULL));
    ASSERT_EQ(MT_OK, mt_queue_push(q, &b, NULL));

    void *evicted = NULL;
    ASSERT_EQ(MT_OK, mt_queue_push(q, &c, &evicted));
    ASSERT_EQ(&a, evicted);
    ASSERT_EQ(1u, (unsigned)mt_queue_dropped(q));

    void *item = NULL;
    ASSERT_EQ(MT_OK, mt_queue_try_pop(q, &item));
    ASSERT_EQ(&b, item);
    ASSERT_EQ(MT_OK, mt_queue_try_pop(q, &item));
    ASSERT_EQ(&c, item);

    mt_queue_destroy(q);
    PASS();
}

TEST pop_timeout_expires(void)
{
    mt_queue_t *q = mt_queue_create(1, MT_QUEUE_REJECT);
    ASSERT(q != NULL);
    void *item = NULL;
    ASSERT_EQ(MT_ERR_TIMEOUT, mt_queue_pop(q, &item, 20));
    mt_queue_destroy(q);
    PASS();
}

TEST close_wakes_and_drains(void)
{
    mt_queue_t *q = mt_queue_create(4, MT_QUEUE_REJECT);
    ASSERT(q != NULL);
    int a = 1;
    ASSERT_EQ(MT_OK, mt_queue_push(q, &a, NULL));
    mt_queue_close(q);

    ASSERT_EQ(MT_ERR_CLOSED, mt_queue_push(q, &a, NULL));

    void *item = NULL;
    ASSERT_EQ(MT_OK, mt_queue_pop(q, &item, -1)); /* drains remaining */
    ASSERT_EQ(&a, item);
    ASSERT_EQ(MT_ERR_CLOSED, mt_queue_pop(q, &item, -1));

    mt_queue_destroy(q);
    PASS();
}

#define PRODUCER_ITEMS 1000

static void *producer_thread(void *arg)
{
    mt_queue_t *q = arg;
    static int values[PRODUCER_ITEMS];
    for (int i = 0; i < PRODUCER_ITEMS; i++) {
        values[i] = i;
        while (mt_queue_push(q, &values[i], NULL) == MT_ERR_LIMIT) {
            sched_yield();
        }
    }
    return NULL;
}

TEST concurrent_producer_consumer(void)
{
    mt_queue_t *q = mt_queue_create(16, MT_QUEUE_REJECT);
    ASSERT(q != NULL);

    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, producer_thread, q));

    int received = 0;
    while (received < PRODUCER_ITEMS) {
        void *item = NULL;
        mt_err_t err = mt_queue_pop(q, &item, 1000);
        ASSERT_EQ(MT_OK, err);
        ASSERT_EQ(received, *(int *)item);
        received++;
    }
    pthread_join(th, NULL);
    mt_queue_destroy(q);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(reject_policy_drops_and_counts);
    RUN_TEST(drop_oldest_policy_evicts_head);
    RUN_TEST(pop_timeout_expires);
    RUN_TEST(close_wakes_and_drains);
    RUN_TEST(concurrent_producer_consumer);
    GREATEST_MAIN_END();
}
