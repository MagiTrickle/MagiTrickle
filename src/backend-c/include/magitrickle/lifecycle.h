/* Partial-initialization unwinding.
 *
 * Init functions that construct several sub-objects register a destructor
 * for each successfully created one; on a later failure mt_lc_fail() tears
 * them down in reverse order. On success mt_lc_commit() transfers ownership
 * to the enclosing object (which must run the same destructors in its own
 * destroy path). Keeps init/destroy symmetric without goto ladders.
 */
#ifndef MAGITRICKLE_LIFECYCLE_H
#define MAGITRICKLE_LIFECYCLE_H

#include <stddef.h>

#include "magitrickle/err.h"

typedef void (*mt_lc_destroy_fn)(void *ctx);

typedef struct mt_lc_entry {
    mt_lc_destroy_fn destroy;
    void *ctx;
    const char *name; /* for debug logging */
} mt_lc_entry_t;

typedef struct mt_lifecycle {
    mt_lc_entry_t *entries;
    size_t len;
    size_t cap;
} mt_lifecycle_t;

void mt_lc_init(mt_lifecycle_t *lc);

/* Register a destructor for an object that has just been created. */
mt_err_t mt_lc_push(mt_lifecycle_t *lc, mt_lc_destroy_fn destroy, void *ctx,
                    const char *name);

/* Failure path: run destructors in reverse registration order, free the
 * tracking storage. */
void mt_lc_fail(mt_lifecycle_t *lc);

/* Success path: drop tracking without running destructors. */
void mt_lc_commit(mt_lifecycle_t *lc);

#endif /* MAGITRICKLE_LIFECYCLE_H */
