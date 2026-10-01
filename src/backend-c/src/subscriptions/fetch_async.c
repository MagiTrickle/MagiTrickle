/* Bounded fetch/parse workers with loop-owned completions. Workers own
 * detached rule arrays, never live models, HTTP objects or event-loop watches. */
#include "magitrickle/sub_fetch.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <time.h>
#include "magitrickle/log.h"
#include "magitrickle/subparse.h"

#define FETCH_WORKERS 2
#define FETCH_LIMIT 32

typedef struct fetch_job {
    struct fetch_job *next;
    char *url;
    char *body;
    size_t len;
    mt_err_t err;
    mt_sub_fetch_done_fn done;
    mt_sub_rules_done_fn rules_done;
    mt_sub_rule_t **rules;
    size_t n_rules;
    void *ud;
} fetch_job_t;

struct mt_sub_fetcher {
    mt_loop_t *loop;
    int notify[2];
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t workers[FETCH_WORKERS];
    size_t n_workers;
    size_t pending;
    size_t rules_pending;
    atomic_bool stopping;
    fetch_job_t *queue_head, *queue_tail;
    fetch_job_t *done_head, *done_tail;
};

static void append_job(fetch_job_t **head, fetch_job_t **tail, fetch_job_t *j) {
    j->next = NULL;
    if (*tail) { (*tail)->next = j; } else { *head = j; }
    *tail = j;
}

static fetch_job_t *pop_job(fetch_job_t **head, fetch_job_t **tail) {
    fetch_job_t *j = *head;
    if (j) {
        *head = j->next;
        if (!*head) { *tail = NULL; }
        j->next = NULL;
    }
    return j;
}

static void finish_job(fetch_job_t *j, bool is_canceled) {
    mt_err_t err = is_canceled ? MT_ERR_CANCELED : j->err;
    if (j->rules_done) {
        if (err != MT_OK) { mt_sub_rules_free(j->rules, j->n_rules); j->rules = NULL; j->n_rules = 0; }
        j->rules_done(j->ud, err, j->rules, j->n_rules);
    } else { j->done(j->ud, err, j->body, j->len); }
    free(j->body); free(j->url); free(j);
}

static double monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1000000.0;
}

static void *fetch_worker(void *ud) {
    mt_sub_fetcher_t *f = ud;
    for (;;) {
        pthread_mutex_lock(&f->mu);
        while (!f->queue_head && !atomic_load(&f->stopping)) {
            pthread_cond_wait(&f->cv, &f->mu);
        }
        if (atomic_load(&f->stopping)) { pthread_mutex_unlock(&f->mu); break; }
        fetch_job_t *j = pop_job(&f->queue_head, &f->queue_tail);
        pthread_mutex_unlock(&f->mu);
        double start = monotonic_ms();
        j->err = mt_sub_fetch_list_cancel(j->url, &j->body, &j->len, &f->stopping);
        double fetched = monotonic_ms();
        if (j->rules_done) {
            if (j->err == MT_OK) {
                j->err = mt_sub_parse_rules_cancel(j->body, &j->rules, &j->n_rules, &f->stopping);
            } else if (j->err != MT_ERR_CANCELED && j->err != MT_ERR_LIMIT) {
                j->err = MT_ERR_UPSTREAM;
            }
            MT_DEBUG("subscription fetch/parse: bytes=%zu rules=%zu fetch_ms=%.1f parse_ms=%.1f result=%s",
                     j->len, j->n_rules, fetched - start, monotonic_ms() - fetched, mt_err_str(j->err));
            free(j->body); j->body = NULL;
        }
        pthread_mutex_lock(&f->mu);
        append_job(&f->done_head, &f->done_tail, j);
        pthread_mutex_unlock(&f->mu);
        ssize_t n;
        do { n = write(f->notify[1], "x", 1); } while (n < 0 && errno == EINTR);
        /* A full pipe is already readable; its callback drains all jobs. */
    }
    return NULL;
}

static void fetch_ready(mt_loop_t *loop, int fd, uint32_t events, void *ud) {
    (void)loop; (void)events;
    mt_sub_fetcher_t *f = ud;
    char bytes[64];
    for (;;) {
        ssize_t n = read(fd, bytes, sizeof(bytes));
        if (n > 0 || (n < 0 && errno == EINTR)) { continue; }
        break;
    }
    for (;;) {
        pthread_mutex_lock(&f->mu);
        fetch_job_t *j = pop_job(&f->done_head, &f->done_tail);
        if (j) { f->pending--; if (j->rules_done) { f->rules_pending--; } }
        pthread_mutex_unlock(&f->mu);
        if (!j) { break; }
        finish_job(j, false);
    }
}

mt_err_t mt_sub_fetcher_create(mt_loop_t *loop, mt_sub_fetcher_t **out) {
    *out = NULL;
    mt_sub_fetcher_t *f = calloc(1, sizeof(*f));
    if (!f) { return MT_ERR_NOMEM; }
    f->loop = loop;
    atomic_init(&f->stopping, false);
    int rc = pthread_mutex_init(&f->mu, NULL);
    if (rc != 0) { free(f); return mt_err_from_errno(rc); }
    rc = pthread_cond_init(&f->cv, NULL);
    if (rc != 0) { pthread_mutex_destroy(&f->mu); free(f); return mt_err_from_errno(rc); }
    mt_err_t err = MT_ERR_SYS;
    if (pipe(f->notify) != 0) { err = mt_err_from_errno(errno); goto fail; }
    for (size_t i = 0; i < 2; i++) {
        if (fcntl(f->notify[i], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(f->notify[i], F_SETFL, O_NONBLOCK) < 0) {
            err = mt_err_from_errno(errno);
            goto fail_pipe;
        }
    }
    err = mt_loop_add_fd(loop, f->notify[0], EPOLLIN, fetch_ready, f);
    if (err != MT_OK) { goto fail_pipe; }
    for (size_t i = 0; i < FETCH_WORKERS; i++) {
        rc = pthread_create(&f->workers[i], NULL, fetch_worker, f);
        if (rc != 0) { mt_sub_fetcher_destroy(f); return mt_err_from_errno(rc); }
        f->n_workers++;
    }
    *out = f;
    return MT_OK;
fail_pipe:
    close(f->notify[0]); close(f->notify[1]);
fail:
    pthread_cond_destroy(&f->cv);
    pthread_mutex_destroy(&f->mu);
    free(f);
    return err;
}

static mt_err_t submit(mt_sub_fetcher_t *f, const char *url,
                        mt_sub_fetch_done_fn done, mt_sub_rules_done_fn rules_done, void *ud) {
    if (!f || !url || (!done && !rules_done)) { return MT_ERR_INVAL; }
    fetch_job_t *j = calloc(1, sizeof(*j));
    if (!j) { return MT_ERR_NOMEM; }
    j->url = strdup(url);
    if (!j->url) { free(j); return MT_ERR_NOMEM; }
    j->done = done; j->rules_done = rules_done; j->ud = ud;
    pthread_mutex_lock(&f->mu);
    mt_err_t err = atomic_load(&f->stopping) ? MT_ERR_STATE :
        ((f->pending >= FETCH_LIMIT || (rules_done && f->rules_pending >= MT_SUB_FETCH_RULE_JOBS))
            ? MT_ERR_LIMIT : MT_OK);
    if (err == MT_OK) {
        f->pending++;
        if (rules_done) { f->rules_pending++; }
        append_job(&f->queue_head, &f->queue_tail, j);
        pthread_cond_signal(&f->cv);
    }
    pthread_mutex_unlock(&f->mu);
    if (err != MT_OK) { free(j->url); free(j); }
    return err;
}

mt_err_t mt_sub_fetcher_submit(mt_sub_fetcher_t *f, const char *url,
                               mt_sub_fetch_done_fn done, void *ud) {
    return submit(f, url, done, NULL, ud);
}

mt_err_t mt_sub_fetcher_submit_rules(mt_sub_fetcher_t *f, const char *url,
                                     mt_sub_rules_done_fn done, void *ud) {
    return submit(f, url, NULL, done, ud);
}

void mt_sub_fetcher_destroy(mt_sub_fetcher_t *f) {
    if (!f) { return; }
    atomic_store(&f->stopping, true);
    pthread_mutex_lock(&f->mu);
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->mu);
    for (size_t i = 0; i < f->n_workers; i++) { pthread_join(f->workers[i], NULL); }
    (void)mt_loop_del_fd(f->loop, f->notify[0]);
    close(f->notify[0]); close(f->notify[1]);
    fetch_job_t *j;
    while ((j = pop_job(&f->queue_head, &f->queue_tail))) { finish_job(j, true); }
    while ((j = pop_job(&f->done_head, &f->done_tail))) { finish_job(j, true); }
    pthread_cond_destroy(&f->cv);
    pthread_mutex_destroy(&f->mu);
    free(f);
}
