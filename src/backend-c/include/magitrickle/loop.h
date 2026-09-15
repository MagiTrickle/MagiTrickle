/* Event loop: epoll + timerfd + signalfd (Linux platform layer).
 *
 * Single-threaded by design: all callbacks run on the thread that calls
 * mt_loop_run(). Cross-thread work is handed to the loop via bounded
 * queues + an eventfd wakeup (mt_loop_post). This is the concurrency model
 * chosen for the C backend (docs/c-rewrite/decisions.md D-02): the DNS hot
 * path stays non-blocking on this loop; blocking work (fork/exec iptables,
 * libcurl, crypt) lives on worker threads that post completions back.
 *
 * fd callbacks receive the epoll event mask. Timers use timerfd; one-shot
 * and periodic. Signals use signalfd — callers must block the signals they
 * subscribe to before starting the loop (mt_loop_add_signals does this for
 * the calling thread; spawn worker threads after, so they inherit the mask).
 */
#ifndef MAGITRICKLE_LOOP_H
#define MAGITRICKLE_LOOP_H

#include <signal.h>
#include <stdint.h>

#include "magitrickle/err.h"

typedef struct mt_loop mt_loop_t;

typedef void (*mt_fd_cb)(mt_loop_t *loop, int fd, uint32_t events, void *ud);
typedef void (*mt_timer_cb)(mt_loop_t *loop, void *ud);
typedef void (*mt_signal_cb)(mt_loop_t *loop, int signo, void *ud);
typedef void (*mt_post_cb)(mt_loop_t *loop, void *ud);

mt_err_t mt_loop_create(mt_loop_t **out);
void mt_loop_destroy(mt_loop_t *loop);

/* events: EPOLLIN/EPOLLOUT/... (edge-trigger is NOT used; level-triggered). */
mt_err_t mt_loop_add_fd(mt_loop_t *loop, int fd, uint32_t events, mt_fd_cb cb,
                        void *ud);
mt_err_t mt_loop_mod_fd(mt_loop_t *loop, int fd, uint32_t events);
mt_err_t mt_loop_del_fd(mt_loop_t *loop, int fd);

/* Returns a timer id (>0) via *out_id. interval_ms==0 -> one-shot after
 * initial_ms; otherwise fires every interval_ms after initial_ms. A one-shot
 * is removed automatically after its callback; explicit deletion in that
 * callback remains supported. All registration/deletion is loop-thread-only. */
mt_err_t mt_loop_add_timer(mt_loop_t *loop, uint64_t initial_ms,
                           uint64_t interval_ms, mt_timer_cb cb, void *ud,
                           int *out_id);
mt_err_t mt_loop_del_timer(mt_loop_t *loop, int timer_id);

/* Subscribe to signals (also blocks them for the calling thread). */
mt_err_t mt_loop_add_signals(mt_loop_t *loop, const sigset_t *set,
                             mt_signal_cb cb, void *ud);

/* Thread-safe: schedule cb(ud) on the loop thread. Bounded internally;
 * MT_ERR_LIMIT when the post queue is full. */
mt_err_t mt_loop_post(mt_loop_t *loop, mt_post_cb cb, void *ud);

/* Run until mt_loop_stop() is called. Returns MT_OK on clean stop. */
mt_err_t mt_loop_run(mt_loop_t *loop);

/* Thread-safe and signal-safe. */
void mt_loop_stop(mt_loop_t *loop);

#endif /* MAGITRICKLE_LOOP_H */
