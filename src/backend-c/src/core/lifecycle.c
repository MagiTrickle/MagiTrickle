#include "magitrickle/lifecycle.h"

#include <stdlib.h>

#include "magitrickle/log.h"

void mt_lc_init(mt_lifecycle_t *lc)
{
    lc->entries = NULL;
    lc->len = 0;
    lc->cap = 0;
}

mt_err_t mt_lc_push(mt_lifecycle_t *lc, mt_lc_destroy_fn destroy, void *ctx,
                    const char *name)
{
    if (lc->len == lc->cap) {
        size_t cap = lc->cap == 0 ? 8 : lc->cap * 2;
        mt_lc_entry_t *entries =
            realloc(lc->entries, cap * sizeof(mt_lc_entry_t));
        if (entries == NULL) {
            return MT_ERR_NOMEM;
        }
        lc->entries = entries;
        lc->cap = cap;
    }
    lc->entries[lc->len].destroy = destroy;
    lc->entries[lc->len].ctx = ctx;
    lc->entries[lc->len].name = name;
    lc->len++;
    return MT_OK;
}

void mt_lc_fail(mt_lifecycle_t *lc)
{
    while (lc->len > 0) {
        lc->len--;
        mt_lc_entry_t *e = &lc->entries[lc->len];
        MT_DEBUG("lifecycle: unwinding %s", e->name ? e->name : "?");
        e->destroy(e->ctx);
    }
    free(lc->entries);
    mt_lc_init(lc);
}

void mt_lc_commit(mt_lifecycle_t *lc)
{
    free(lc->entries);
    mt_lc_init(lc);
}
