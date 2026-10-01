#include "greatest.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "magitrickle/nfcommit.h"

/* Shared state driven by the fake rebuild callback. Everything is touched
 * from both the committer thread and the test thread, so it all lives
 * under one mutex. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;

    int started;    /* rebuilds entered */
    int finished;   /* rebuilds that ran to completion */
    int interrupted; /* rebuilds that saw the cancel token raised */

    bool block;          /* park in the rebuild until released */
    bool release;
    int fail_times;      /* fail the next N rebuilds ... */
    mt_err_t fail_with;  /* ... with this */
} probe_t;

static void probe_init(probe_t *p) {
    *p = (probe_t){0};
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
}

static void probe_destroy(probe_t *p) {
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->mu);
}

static mt_err_t probe_rebuild(void *ud, mt_cancel_t *cancel) {
    probe_t *p = ud;

    pthread_mutex_lock(&p->mu);
    p->started++;
    bool block = p->block;
    mt_err_t fail = MT_OK;
    if (p->fail_times > 0) {
        p->fail_times--;
        fail = p->fail_with;
    }
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);

    if (block) {
        /* Stand in for a long iptables write: return as soon as the
         * committer asks us to, or when the test releases us. */
        for (;;) {
            if (mt_cancel_raised(cancel)) {
                pthread_mutex_lock(&p->mu);
                p->interrupted++;
                pthread_cond_broadcast(&p->cv);
                pthread_mutex_unlock(&p->mu);
                return MT_ERR_CANCELED;
            }
            pthread_mutex_lock(&p->mu);
            bool released = p->release;
            pthread_mutex_unlock(&p->mu);
            if (released) { break; }

            struct timespec ts = {.tv_sec = 0, .tv_nsec = 200000L};
            nanosleep(&ts, NULL);
        }
    }

    pthread_mutex_lock(&p->mu);
    if (fail == MT_OK) { p->finished++; }
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return fail;
}

/* Spins until cond(p) holds or ~2s pass. */
#define AWAIT(p, cond)                                                                      \
    do {                                                                                    \
        bool ok_ = false;                                                                   \
        for (int i_ = 0; i_ < 4000; i_++) {                                                 \
            pthread_mutex_lock(&(p)->mu);                                                   \
            ok_ = (cond);                                                                   \
            pthread_mutex_unlock(&(p)->mu);                                                 \
            if (ok_) { break; }                                                             \
            struct timespec ts_ = {.tv_sec = 0, .tv_nsec = 500000L};                        \
            nanosleep(&ts_, NULL);                                                          \
        }                                                                                   \
        ASSERT(ok_);                                                                        \
    } while (0)

static void sleep_ms(unsigned ms) {
    struct timespec ts = {.tv_sec = ms / 1000u, .tv_nsec = (long)(ms % 1000u) * 1000000L};
    nanosleep(&ts, NULL);
}

static mt_nfcommit_t *start_committer(probe_t *p) {
    mt_nfcommit_t *c = mt_nfcommit_new(probe_rebuild, p);
    mt_nfcommit_set_delays_for_test(c, 1, 4);
    mt_nfcommit_start(c);
    return c;
}

TEST request_triggers_a_rebuild(void) {
    probe_t p;
    probe_init(&p);
    mt_nfcommit_t *c = start_committer(&p);

    mt_nfcommit_request(c);
    AWAIT(&p, p.finished >= 1);

    mt_nfcommit_free(c);
    probe_destroy(&p);
    PASS();
}

/* Nothing owed to the caller: the hook must return while the write is
 * still going. */
TEST request_does_not_wait_for_the_write(void) {
    probe_t p;
    probe_init(&p);
    p.block = true;
    mt_nfcommit_t *c = start_committer(&p);

    mt_nfcommit_request(c);
    AWAIT(&p, p.started >= 1);

    /* A second request lands while the first rebuild is parked. If it
     * blocked, this test would hang rather than fail. */
    mt_nfcommit_request(c);

    AWAIT(&p, p.interrupted >= 1);

    pthread_mutex_lock(&p.mu);
    p.release = true;
    pthread_mutex_unlock(&p.mu);

    mt_nfcommit_free(c);
    probe_destroy(&p);
    PASS();
}

/* The whole point: a request arriving mid-write aborts it and a fresh
 * pass runs afterwards. */
TEST request_interrupts_and_restarts(void) {
    probe_t p;
    probe_init(&p);
    p.block = true;
    mt_nfcommit_t *c = start_committer(&p);

    mt_nfcommit_request(c);
    AWAIT(&p, p.started >= 1);

    mt_nfcommit_request(c);
    AWAIT(&p, p.interrupted >= 1);

    /* Let the restarted pass through; it must actually happen. */
    pthread_mutex_lock(&p.mu);
    p.block = false;
    pthread_mutex_unlock(&p.mu);

    AWAIT(&p, p.finished >= 1);
    ASSERT(p.started >= 2);

    mt_nfcommit_free(c);
    probe_destroy(&p);
    PASS();
}

/* One firmware rewrite fires a netfilter.d event per table; they must
 * collapse into a pass or two, not one pass each. */
TEST requests_coalesce(void) {
    probe_t p;
    probe_init(&p);
    mt_nfcommit_t *c = mt_nfcommit_new(probe_rebuild, &p);
    mt_nfcommit_set_delays_for_test(c, 30, 60);
    mt_nfcommit_start(c);

    for (int i = 0; i < 200; i++) { mt_nfcommit_request(c); }

    AWAIT(&p, p.finished >= 1);
    sleep_ms(200);

    uint64_t passes = mt_nfcommit_passes(c);
    ASSERT_FALSE(passes > 4);

    mt_nfcommit_free(c);
    probe_destroy(&p);
    PASS();
}

/* Losing a race with the firmware is not a failure: keep starting over
 * until a pass gets through. */
TEST retries_until_the_table_holds_still(void) {
    probe_t p;
    probe_init(&p);
    p.fail_times = 5;
    p.fail_with = MT_ERR_AGAIN;
    mt_nfcommit_t *c = start_committer(&p);

    mt_nfcommit_request(c);
    AWAIT(&p, p.finished >= 1);
    ASSERT(p.started >= 6);

    mt_nfcommit_free(c);
    probe_destroy(&p);
    PASS();
}

/* Errors we cannot attribute to a race are retried too -- just more
 * slowly. Nothing is ever reported outward. */
TEST retries_unclassified_failures(void) {
    probe_t p;
    probe_init(&p);
    p.fail_times = 3;
    p.fail_with = MT_ERR_IO;
    mt_nfcommit_t *c = start_committer(&p);

    mt_nfcommit_request(c);
    AWAIT(&p, p.finished >= 1);

    mt_nfcommit_free(c);
    probe_destroy(&p);
    PASS();
}

/* A request made before the thread exists is remembered, so a hook that
 * fires during startup is not lost. */
TEST request_before_start_is_served(void) {
    probe_t p;
    probe_init(&p);
    mt_nfcommit_t *c = mt_nfcommit_new(probe_rebuild, &p);
    mt_nfcommit_set_delays_for_test(c, 1, 4);

    mt_nfcommit_request(c);
    ASSERT_EQ(0u, mt_nfcommit_passes(c));

    ASSERT_EQ(MT_OK, mt_nfcommit_start(c));
    AWAIT(&p, p.finished >= 1);

    mt_nfcommit_free(c);
    probe_destroy(&p);
    PASS();
}

/* Shutdown must not wait out a write in flight. */
TEST stop_aborts_a_running_rebuild(void) {
    probe_t p;
    probe_init(&p);
    p.block = true;
    mt_nfcommit_t *c = start_committer(&p);

    mt_nfcommit_request(c);
    AWAIT(&p, p.started >= 1);

    mt_nfcommit_stop(c); /* hangs here if the abort does not reach it */
    ASSERT(p.interrupted >= 1);

    mt_nfcommit_free(c);
    probe_destroy(&p);
    PASS();
}

/* An edit on the loop thread only wants the netfilter state back; it must
 * not turn into a rebuild of its own. The interrupted pass still
 * reschedules itself, so nothing is lost either. */
TEST interrupt_yields_without_asking_for_more_work(void) {
    probe_t p;
    probe_init(&p);
    mt_nfcommit_t *c = start_committer(&p);

    /* Nothing running: interrupting must not schedule anything. */
    mt_nfcommit_interrupt(c);
    sleep_ms(30);
    ASSERT_EQ(0u, mt_nfcommit_passes(c));

    /* A pass in flight is aborted, and comes back on its own. */
    pthread_mutex_lock(&p.mu);
    p.block = true;
    pthread_mutex_unlock(&p.mu);

    mt_nfcommit_request(c);
    AWAIT(&p, p.started >= 1);

    mt_nfcommit_interrupt(c);
    AWAIT(&p, p.interrupted >= 1);

    pthread_mutex_lock(&p.mu);
    p.block = false;
    pthread_mutex_unlock(&p.mu);
    AWAIT(&p, p.finished >= 1);

    mt_nfcommit_free(c);
    probe_destroy(&p);
    PASS();
}

TEST idle_committer_does_nothing(void) {
    probe_t p;
    probe_init(&p);
    mt_nfcommit_t *c = start_committer(&p);

    sleep_ms(50);
    ASSERT_EQ(0u, mt_nfcommit_passes(c));

    mt_nfcommit_free(c);
    probe_destroy(&p);
    PASS();
}

TEST null_and_double_stop_are_safe(void) {
    mt_nfcommit_request(NULL);
    mt_nfcommit_stop(NULL);
    mt_nfcommit_free(NULL);
    ASSERT_EQ(0u, mt_nfcommit_passes(NULL));
    ASSERT(mt_nfcommit_cancel(NULL) == NULL);
    ASSERT(mt_nfcommit_new(NULL, NULL) == NULL);

    probe_t p;
    probe_init(&p);
    mt_nfcommit_t *c = start_committer(&p);
    mt_nfcommit_stop(c);
    mt_nfcommit_stop(c);
    mt_nfcommit_free(c);
    probe_destroy(&p);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(request_triggers_a_rebuild);
    RUN_TEST(request_does_not_wait_for_the_write);
    RUN_TEST(request_interrupts_and_restarts);
    RUN_TEST(requests_coalesce);
    RUN_TEST(retries_until_the_table_holds_still);
    RUN_TEST(retries_unclassified_failures);
    RUN_TEST(request_before_start_is_served);
    RUN_TEST(stop_aborts_a_running_rebuild);
    RUN_TEST(interrupt_yields_without_asking_for_more_work);
    RUN_TEST(idle_committer_does_nothing);
    RUN_TEST(null_and_double_stop_are_safe);
    GREATEST_MAIN_END();
}
