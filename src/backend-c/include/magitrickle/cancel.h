/* Cancellation token: a one-bit "abort what you are doing" flag that is
 * both cheap to poll from the working thread and pollable as an fd, so a
 * thread blocked in poll() waiting on a child process can be woken by it.
 *
 * Raising is thread-safe and may be done from any thread (the netfilter.d
 * hook handler raises the committer thread's token from the loop thread);
 * clearing belongs to the thread that owns the work being cancelled, and
 * happens once at the start of each attempt.
 *
 * The fd is a self-pipe rather than an eventfd: eventfd is a Linux/glibc
 * extension outside the project's _POSIX_C_SOURCE surface, and this token
 * is used from src/iptables/, which is otherwise dependency-free POSIX.
 * The pipe holds at most one byte -- raise() only writes on the
 * false->true transition -- so it can never fill and block.
 */
#ifndef MAGITRICKLE_CANCEL_H
#define MAGITRICKLE_CANCEL_H

#include <stdbool.h>

#include "magitrickle/err.h"

typedef struct mt_cancel mt_cancel_t;

/* NULL on failure (OOM or pipe exhaustion). */
mt_cancel_t *mt_cancel_new(void);
void mt_cancel_free(mt_cancel_t *c);

/* Thread-safe; idempotent while already raised. NULL-safe. */
void mt_cancel_raise(mt_cancel_t *c);

/* Lowers the flag and drains the pipe. Call from the working thread
 * before starting a fresh attempt, never while an attempt is in flight.
 * NULL-safe. */
void mt_cancel_clear(mt_cancel_t *c);

/* NULL-safe: an absent token is never raised. */
bool mt_cancel_raised(const mt_cancel_t *c);

/* Readable exactly while raised, for adding to a poll() set. -1 when c is
 * NULL, so callers can pass it straight into a pollfd guard. */
int mt_cancel_fd(const mt_cancel_t *c);

#endif /* MAGITRICKLE_CANCEL_H */
