/* Bounded blocking-I/O workers with loop-owned completions. No model,
 * ruleset, HTTP connection or event-loop watch is touched by a worker. */
#include "magitrickle/sub_fetch.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define FETCH_WORKERS 2
#define FETCH_LIMIT 32

typedef struct fetch_job {
    struct fetch_job *next;
    char *url;
    char *body;
    size_t len;
    mt_err_t err;
    mt_sub_fetch_done_fn done;
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

static void finish_job(fetch_job_t *j, bool canceled) {
    j->done(j->ud, canceled ? MT_ERR_CANCELED : j->err, j->body, j->len);
    free(j->body);
    free(j->url);
    free(j);
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
        j->err = mt_sub_fetch_list_cancel(j->url, &j->body, &j->len, &f->stopping);
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
        if (j) { f->pending--; }
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

mt_err_t mt_sub_fetcher_submit(mt_sub_fetcher_t *f, const char *url,
                               mt_sub_fetch_done_fn done, void *ud) {
    if (!f || !url || !done) { return MT_ERR_INVAL; }
    fetch_job_t *j = calloc(1, sizeof(*j));
    if (!j) { return MT_ERR_NOMEM; }
    j->url = strdup(url);
    if (!j->url) { free(j); return MT_ERR_NOMEM; }
    j->done = done; j->ud = ud;
    pthread_mutex_lock(&f->mu);
    mt_err_t err = atomic_load(&f->stopping) ? MT_ERR_STATE :
                   (f->pending >= FETCH_LIMIT ? MT_ERR_LIMIT : MT_OK);
    if (err == MT_OK) {
        f->pending++;
        append_job(&f->queue_head, &f->queue_tail, j);
        pthread_cond_signal(&f->cv);
    }
    pthread_mutex_unlock(&f->mu);
    if (err != MT_OK) { free(j->url); free(j); }
    return err;
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
