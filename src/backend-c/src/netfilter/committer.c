/* See nfcommit.h. */
#include "magitrickle/nfcommit.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "magitrickle/log.h"

/* A firmware table rewrite produces one netfilter.d event per table, all
 * within a few milliseconds. Settling briefly before a pass turns that
 * burst into one rebuild instead of one rebuild per table. */
#define MT_NFCOMMIT_DELAY_MS 150u
/* Ceiling for the backoff that only repeated *hard* failures reach --
 * losing a race with the firmware never backs off, since the whole point
 * is to converge as soon as it stops rewriting. */
#define MT_NFCOMMIT_MAX_DELAY_MS 5000u

struct mt_nfcommit {
    mt_nfcommit_rebuild_fn rebuild;
    void *ud;

    mt_cancel_t *cancel;

    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;

    bool started;
    bool stopping;
    bool pending; /* a rebuild is owed; further requests fold into it */
    bool running; /* a rebuild is in flight and can be aborted */
    uint64_t passes;

    unsigned delay_ms;
    unsigned max_delay_ms;
};

mt_nfcommit_t *mt_nfcommit_new(mt_nfcommit_rebuild_fn fn, void *ud) {
    if (!fn) { return NULL; }

    mt_nfcommit_t *c = calloc(1, sizeof(*c));
    if (!c) { return NULL; }

    c->cancel = mt_cancel_new();
    if (!c->cancel) {
        free(c);
        return NULL;
    }
    if (pthread_mutex_init(&c->mu, NULL) != 0) {
        mt_cancel_free(c->cancel);
        free(c);
        return NULL;
    }

    pthread_condattr_t attr;
    int attr_rc = pthread_condattr_init(&attr);
    bool cv_ok = false;
    if (attr_rc == 0) {
        cv_ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 &&
                pthread_cond_init(&c->cv, &attr) == 0;
        pthread_condattr_destroy(&attr);
    }
    if (!cv_ok) {
        pthread_mutex_destroy(&c->mu);
        mt_cancel_free(c->cancel);
        free(c);
        return NULL;
    }

    c->rebuild = fn;
    c->ud = ud;
    c->delay_ms = MT_NFCOMMIT_DELAY_MS;
    c->max_delay_ms = MT_NFCOMMIT_MAX_DELAY_MS;
    return c;
}

void mt_nfcommit_set_delays_for_test(mt_nfcommit_t *c, unsigned delay_ms, unsigned max_delay_ms) {
    if (!c) { return; }
    c->delay_ms = delay_ms;
    c->max_delay_ms = max_delay_ms;
}

mt_cancel_t *mt_nfcommit_cancel(const mt_nfcommit_t *c) {
    return c ? c->cancel : NULL;
}

uint64_t mt_nfcommit_passes(const mt_nfcommit_t *c) {
    if (!c) { return 0; }
    mt_nfcommit_t *m = (mt_nfcommit_t *)c;
    pthread_mutex_lock(&m->mu);
    uint64_t n = m->passes;
    pthread_mutex_unlock(&m->mu);
    return n;
}

void mt_nfcommit_request(mt_nfcommit_t *c) {
    if (!c) { return; }

    pthread_mutex_lock(&c->mu);
    c->pending = true;
    bool abort_current = c->running;
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->mu);

    /* Raised outside the lock: the committer thread never blocks on the
     * token, and raising is already thread-safe. */
    if (abort_current) { mt_cancel_raise(c->cancel); }
}

void mt_nfcommit_interrupt(mt_nfcommit_t *c) {
    if (!c) { return; }

    pthread_mutex_lock(&c->mu);
    bool abort_current = c->running;
    pthread_mutex_unlock(&c->mu);

    /* No pending flag set here on purpose: committer_main re-arms itself
     * for any pass that did not finish, this one included. */
    if (abort_current) { mt_cancel_raise(c->cancel); }
}

/* Waits up to delay_ms, returning false if the committer should exit.
 * Deliberately does not return early on a new request: one is already
 * owed, and the pass about to start will serve it too. */
static bool settle(mt_nfcommit_t *c, unsigned delay_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)(delay_ms / 1000u);
    deadline.tv_nsec += (long)(delay_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&c->mu);
    while (!c->stopping) {
        int rc = pthread_cond_timedwait(&c->cv, &c->mu, &deadline);
        if (rc == ETIMEDOUT) { break; }
    }
    bool go_on = !c->stopping;
    pthread_mutex_unlock(&c->mu);
    return go_on;
}

static void *committer_main(void *arg) {
    mt_nfcommit_t *c = arg;
    unsigned delay_ms = c->delay_ms;

    for (;;) {
        pthread_mutex_lock(&c->mu);
        while (!c->pending && !c->stopping) { pthread_cond_wait(&c->cv, &c->mu); }
        bool stopping = c->stopping;
        pthread_mutex_unlock(&c->mu);
        if (stopping) { break; }

        if (!settle(c, delay_ms)) { break; }

        /* Everything owed so far is about to be served by this pass: it
         * rebuilds the table from the state as it is right now, so a
         * request made before this point has nothing left to ask for. */
        pthread_mutex_lock(&c->mu);
        if (c->stopping) {
            pthread_mutex_unlock(&c->mu);
            break;
        }
        c->pending = false;
        c->running = true;
        c->passes++;
        pthread_mutex_unlock(&c->mu);

        mt_cancel_clear(c->cancel);

        mt_err_t err = c->rebuild(c->ud, c->cancel);

        pthread_mutex_lock(&c->mu);
        c->running = false;
        stopping = c->stopping;
        pthread_mutex_unlock(&c->mu);
        if (stopping) { break; }

        if (err == MT_OK) {
            delay_ms = c->delay_ms;
            continue;
        }

        /* No error ever leaves this thread: an unfinished rebuild is
         * simply started over. Only failures we cannot attribute to the
         * table moving under us are worth a louder log and a backoff. */
        if (err == MT_ERR_CANCELED) {
            MT_DEBUG("netfilter table rebuild interrupted, starting over");
            delay_ms = c->delay_ms;
        } else if (err == MT_ERR_AGAIN) {
            MT_DEBUG("netfilter table changed during rebuild, starting over");
            delay_ms = c->delay_ms;
        } else {
            MT_WARN("failed to rebuild netfilter table (%s), starting over", mt_err_str(err));
            delay_ms = delay_ms * 2 < c->max_delay_ms ? delay_ms * 2 : c->max_delay_ms;
        }

        pthread_mutex_lock(&c->mu);
        c->pending = true;
        pthread_mutex_unlock(&c->mu);
    }

    return NULL;
}

mt_err_t mt_nfcommit_start(mt_nfcommit_t *c) {
    if (!c) { return MT_ERR_INVAL; }
    if (c->started) { return MT_ERR_STATE; }

    if (pthread_create(&c->thread, NULL, committer_main, c) != 0) { return MT_ERR_SYS; }
    c->started = true;
    return MT_OK;
}

void mt_nfcommit_stop(mt_nfcommit_t *c) {
    if (!c || !c->started) { return; }

    pthread_mutex_lock(&c->mu);
    c->stopping = true;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);

    /* Unblocks a rebuild parked in poll() on an iptables child. */
    mt_cancel_raise(c->cancel);

    pthread_join(c->thread, NULL);
    c->started = false;
}

void mt_nfcommit_free(mt_nfcommit_t *c) {
    if (!c) { return; }
    mt_nfcommit_stop(c);
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->mu);
    mt_cancel_free(c->cancel);
    free(c);
}
