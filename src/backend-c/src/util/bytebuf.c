#include "magitrickle/bytebuf.h"

#include <stdlib.h>
#include <string.h>

void mt_bytebuf_init(mt_bytebuf_t *b) {
    mt_bytebuf_init_bounded(b, 0);
}

void mt_bytebuf_init_bounded(mt_bytebuf_t *b, size_t max_cap) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->max_cap = max_cap ? max_cap : MT_BYTEBUF_MAX_DEFAULT;
}

void mt_bytebuf_free(mt_bytebuf_t *b) {
    if (!b) { return; }
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

mt_err_t mt_bytebuf_append(mt_bytebuf_t *b, const void *data, size_t len) {
    if (len == 0) { return MT_OK; }

    if (len > b->max_cap || b->len > b->max_cap - len) { return MT_ERR_LIMIT; }

    if (b->len + len > b->cap) {
        size_t newcap = b->cap == 0 ? 4096 : b->cap;
        while (newcap < b->len + len) {
            if (newcap > b->max_cap / 2) {
                newcap = b->max_cap;
                break;
            }
            newcap *= 2;
        }
        if (newcap > b->max_cap) { newcap = b->max_cap; }
        uint8_t *tmp = realloc(b->data, newcap);
        if (!tmp) { return MT_ERR_NOMEM; }
        b->data = tmp;
        b->cap = newcap;
    }

    memcpy(b->data + b->len, data, len);
    b->len += len;
    return MT_OK;
}

mt_err_t mt_bytebuf_append_str(mt_bytebuf_t *b, const char *s) {
    return mt_bytebuf_append(b, s, strlen(s));
}

mt_err_t mt_bytebuf_append_byte(mt_bytebuf_t *b, uint8_t byte) {
    return mt_bytebuf_append(b, &byte, 1);
}
