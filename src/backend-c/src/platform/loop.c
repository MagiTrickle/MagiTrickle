/* Linux event loop: epoll + timerfd + signalfd + eventfd post queue.
 * Platform layer — uses Linux-specific APIs by design (documented GNU/Linux
 * extension surface, see AGENTS.md / decisions.md D-14).
 */
#define _GNU_SOURCE /* NOLINT(bugprone-reserved-identifier) */

#include "magitrickle/loop.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "magitrickle/log.h"
#include "magitrickle/queue.h"

#define MT_LOOP_MAX_EVENTS 64
#define MT_LOOP_POST_CAP 1024

typedef enum watch_kind {
    WATCH_FD = 0,
    WATCH_TIMER,
    WATCH_SIGNAL,
    WATCH_WAKEUP,
} watch_kind_t;

typedef struct watch {
    watch_kind_t kind;
    bool removed;
    bool one_shot;
    int fd;
    int id; /* timer id */
    mt_fd_cb fd_cb;
    mt_timer_cb timer_cb;
    mt_signal_cb signal_cb;
    void *ud;
    struct watch *next;
} watch_t;

typedef struct post_item {
    mt_post_cb cb;
    void *ud;
} post_item_t;

struct mt_loop {
    int epfd;
    int wakeup_fd;         /* eventfd for post + stop */
    _Atomic bool stopping;
    watch_t *watches;      /* singly-linked; loop-thread only */
    watch_t *retired;      /* removed watches in the current epoll batch */
    bool dispatching;
    int next_timer_id;
    mt_queue_t *posts;     /* bounded cross-thread post queue */
    watch_t wakeup_watch;
    int sigfd;             /* -1 until signals subscribed */
};

static mt_err_t watch_register(mt_loop_t *loop, watch_t *w, uint32_t events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.ptr = w;
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, w->fd, &ev) != 0) {
        return mt_err_from_errno(errno);
    }
    return MT_OK;
}

static void watch_link(mt_loop_t *loop, watch_t *w)
{
    w->next = loop->watches;
    loop->watches = w;
}

static void watch_unlink(mt_loop_t *loop, watch_t *w)
{
    watch_t **p = &loop->watches;
    while (*p != NULL) {
        if (*p == w) {
            *p = w->next;
            return;
        }
        p = &(*p)->next;
    }
}

/* epoll_wait has already copied pointers for the entire ready batch.
 * EPOLL_CTL_DEL cannot revoke those pointers. Invalidate now, free only
 * after every event in that batch has been considered. */
static void watch_retire(mt_loop_t *loop, watch_t *w)
{
    watch_unlink(loop, w);
    w->removed = true;
    if (loop->dispatching) {
        w->next = loop->retired;
        loop->retired = w;
    } else {
        free(w);
    }
}

static void collect_retired(mt_loop_t *loop)
{
    while (loop->retired) {
        watch_t *w = loop->retired;
        loop->retired = w->next;
        free(w);
    }
}

mt_err_t mt_loop_create(mt_loop_t **out)
{
    mt_loop_t *loop = calloc(1, sizeof(*loop));
    if (loop == NULL) {
        return MT_ERR_NOMEM;
    }
    loop->epfd = -1;
    loop->wakeup_fd = -1;
    loop->sigfd = -1;
    loop->next_timer_id = 1;

    loop->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epfd < 0) {
        goto fail_errno;
    }
    loop->wakeup_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (loop->wakeup_fd < 0) {
        goto fail_errno;
    }
    loop->posts = mt_queue_create(MT_LOOP_POST_CAP, MT_QUEUE_REJECT);
    if (loop->posts == NULL) {
        errno = ENOMEM;
        goto fail_errno;
    }

    loop->wakeup_watch.kind = WATCH_WAKEUP;
    loop->wakeup_watch.fd = loop->wakeup_fd;
    mt_err_t err = watch_register(loop, &loop->wakeup_watch, EPOLLIN);
    if (err != MT_OK) {
        mt_loop_destroy(loop);
        return err;
    }

    *out = loop;
    return MT_OK;

fail_errno: {
        mt_err_t e = mt_err_from_errno(errno);
        mt_loop_destroy(loop);
        return e;
    }
}

void mt_loop_destroy(mt_loop_t *loop)
{
    if (loop == NULL) {
        return;
    }
    watch_t *w = loop->watches;
    while (w != NULL) {
        watch_t *next = w->next;
        /* timer/signal fds are owned by the loop; plain fds are owned by
         * their registrant. */
        if (w->kind == WATCH_TIMER || w->kind == WATCH_SIGNAL) {
            close(w->fd);
        }
        free(w);
        w = next;
    }
    collect_retired(loop);
    if (loop->posts != NULL) {
        /* drain unexecuted posts: post items are loop-owned wrappers */
        void *item;
        while (mt_queue_try_pop(loop->posts, &item) == MT_OK) {
            free(item);
        }
        mt_queue_destroy(loop->posts);
    }
    if (loop->wakeup_fd >= 0) {
        close(loop->wakeup_fd);
    }
    if (loop->epfd >= 0) {
        close(loop->epfd);
    }
    free(loop);
}

mt_err_t mt_loop_add_fd(mt_loop_t *loop, int fd, uint32_t events, mt_fd_cb cb,
                        void *ud)
{
    watch_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return MT_ERR_NOMEM;
    }
    w->kind = WATCH_FD;
    w->fd = fd;
    w->fd_cb = cb;
    w->ud = ud;
    mt_err_t err = watch_register(loop, w, events);
    if (err != MT_OK) {
        free(w);
        return err;
    }
    watch_link(loop, w);
    return MT_OK;
}

static watch_t *find_fd_watch(mt_loop_t *loop, int fd)
{
    for (watch_t *w = loop->watches; w != NULL; w = w->next) {
        if (w->kind == WATCH_FD && w->fd == fd) {
            return w;
        }
    }
    return NULL;
}

mt_err_t mt_loop_mod_fd(mt_loop_t *loop, int fd, uint32_t events)
{
    watch_t *w = find_fd_watch(loop, fd);
    if (w == NULL) {
        return MT_ERR_NOENT;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.ptr = w;
    if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, fd, &ev) != 0) {
        return mt_err_from_errno(errno);
    }
    return MT_OK;
}

mt_err_t mt_loop_del_fd(mt_loop_t *loop, int fd)
{
    watch_t *w = find_fd_watch(loop, fd);
    if (w == NULL) {
        return MT_ERR_NOENT;
    }
    if (epoll_ctl(loop->epfd, EPOLL_CTL_DEL, fd, NULL) != 0) {
        return mt_err_from_errno(errno);
    }
    watch_retire(loop, w);
    return MT_OK;
}

mt_err_t mt_loop_add_timer(mt_loop_t *loop, uint64_t initial_ms,
                           uint64_t interval_ms, mt_timer_cb cb, void *ud,
                           int *out_id)
{
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tfd < 0) {
        return mt_err_from_errno(errno);
    }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = (time_t)(initial_ms / 1000);
    its.it_value.tv_nsec = (long)(initial_ms % 1000) * 1000000L;
    if (initial_ms == 0) {
        /* fire "immediately": timerfd disarms on all-zero, so use 1 ns */
        its.it_value.tv_nsec = 1;
    }
    its.it_interval.tv_sec = (time_t)(interval_ms / 1000);
    its.it_interval.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
    if (timerfd_settime(tfd, 0, &its, NULL) != 0) {
        mt_err_t e = mt_err_from_errno(errno);
        close(tfd);
        return e;
    }

    watch_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        close(tfd);
        return MT_ERR_NOMEM;
    }
    w->kind = WATCH_TIMER;
    w->one_shot = interval_ms == 0;
    w->fd = tfd;
    w->id = loop->next_timer_id++;
    w->timer_cb = cb;
    w->ud = ud;
    mt_err_t err = watch_register(loop, w, EPOLLIN);
    if (err != MT_OK) {
        close(tfd);
        free(w);
        return err;
    }
    watch_link(loop, w);
    if (out_id != NULL) {
        *out_id = w->id;
    }
    return MT_OK;
}

mt_err_t mt_loop_del_timer(mt_loop_t *loop, int timer_id)
{
    for (watch_t *w = loop->watches; w != NULL; w = w->next) {
        if (w->kind == WATCH_TIMER && w->id == timer_id) {
            (void)epoll_ctl(loop->epfd, EPOLL_CTL_DEL, w->fd, NULL);
            close(w->fd);
            watch_retire(loop, w);
            return MT_OK;
        }
    }
    return MT_ERR_NOENT;
}

mt_err_t mt_loop_add_signals(mt_loop_t *loop, const sigset_t *set,
                             mt_signal_cb cb, void *ud)
{
    if (loop->sigfd >= 0) {
        return MT_ERR_EXIST;
    }
    if (pthread_sigmask(SIG_BLOCK, set, NULL) != 0) {
        return mt_err_from_errno(errno);
    }
    int sfd = signalfd(-1, set, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sfd < 0) {
        return mt_err_from_errno(errno);
    }
    watch_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        close(sfd);
        return MT_ERR_NOMEM;
    }
    w->kind = WATCH_SIGNAL;
    w->fd = sfd;
    w->signal_cb = cb;
    w->ud = ud;
    mt_err_t err = watch_register(loop, w, EPOLLIN);
    if (err != MT_OK) {
        close(sfd);
        free(w);
        return err;
    }
    watch_link(loop, w);
    loop->sigfd = sfd;
    return MT_OK;
}

static void wakeup(mt_loop_t *loop)
{
    uint64_t one = 1;
    ssize_t rc = write(loop->wakeup_fd, &one, sizeof(one));
    (void)rc; /* EAGAIN means a wakeup is already pending — fine */
}

mt_err_t mt_loop_post(mt_loop_t *loop, mt_post_cb cb, void *ud)
{
    post_item_t *item = malloc(sizeof(*item));
    if (item == NULL) {
        return MT_ERR_NOMEM;
    }
    item->cb = cb;
    item->ud = ud;
    mt_err_t err = mt_queue_push(loop->posts, item, NULL);
    if (err != MT_OK) {
        free(item);
        return err;
    }
    wakeup(loop);
    return MT_OK;
}

void mt_loop_stop(mt_loop_t *loop)
{
    atomic_store(&loop->stopping, true);
    wakeup(loop);
}

static void handle_wakeup(mt_loop_t *loop)
{
    uint64_t counter;
    while (read(loop->wakeup_fd, &counter, sizeof(counter)) > 0) {
    }
    void *raw;
    while (mt_queue_try_pop(loop->posts, &raw) == MT_OK) {
        post_item_t *item = raw;
        item->cb(loop, item->ud);
        free(item);
    }
}

static void handle_timer(mt_loop_t *loop, watch_t *w)
{
    uint64_t expirations;
    if (read(w->fd, &expirations, sizeof(expirations)) !=
        (ssize_t)sizeof(expirations)) {
        return;
    }
    w->timer_cb(loop, w->ud);
    /* Callbacks may remove themselves; retired watches remain readable
     * until this batch finishes. One-shot timerfds otherwise never close. */
    if (w->one_shot && !w->removed) {
        (void)mt_loop_del_timer(loop, w->id);
    }
}

static void handle_signal(mt_loop_t *loop, watch_t *w)
{
    struct signalfd_siginfo si;
    while (read(w->fd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
        w->signal_cb(loop, (int)si.ssi_signo, w->ud);
    }
}

mt_err_t mt_loop_run(mt_loop_t *loop)
{
    struct epoll_event events[MT_LOOP_MAX_EVENTS];

    while (!atomic_load(&loop->stopping)) {
        int n = epoll_wait(loop->epfd, events, MT_LOOP_MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return mt_err_from_errno(errno);
        }
        loop->dispatching = true;
        for (int i = 0; i < n; i++) {
            if (atomic_load(&loop->stopping)) {
                break;
            }
            watch_t *w = events[i].data.ptr;
            if (w->removed) { continue; }
            switch (w->kind) {
            case WATCH_WAKEUP:
                handle_wakeup(loop);
                break;
            case WATCH_TIMER:
                handle_timer(loop, w);
                break;
            case WATCH_SIGNAL:
                handle_signal(loop, w);
                break;
            case WATCH_FD:
                w->fd_cb(loop, w->fd, events[i].events, w->ud);
                break;
            }
        }
        loop->dispatching = false;
        collect_retired(loop);
    }
    return MT_OK;
}
