/* Growable byte buffer with an explicit hard cap (spec: no unbounded memory
 * growth). Used for iptables-save/restore transcripts and captured
 * subprocess stdout/stderr — all locally generated, trusted-but-still-
 * bounded content, not attacker-controlled network input.
 */
#ifndef MAGITRICKLE_BYTEBUF_H
#define MAGITRICKLE_BYTEBUF_H

#include <stddef.h>
#include <stdint.h>

#include "magitrickle/err.h"

#define MT_BYTEBUF_MAX_DEFAULT ((size_t)64 * 1024 * 1024) /* 64 MiB */

typedef struct mt_bytebuf {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t max_cap; /* hard limit; 0 means MT_BYTEBUF_MAX_DEFAULT */
} mt_bytebuf_t;

void mt_bytebuf_init(mt_bytebuf_t *b);
void mt_bytebuf_init_bounded(mt_bytebuf_t *b, size_t max_cap);
void mt_bytebuf_free(mt_bytebuf_t *b);

/* MT_ERR_LIMIT if growing would exceed max_cap; MT_ERR_NOMEM on allocation
 * failure; overflow-checked. */
mt_err_t mt_bytebuf_append(mt_bytebuf_t *b, const void *data, size_t len);
mt_err_t mt_bytebuf_append_str(mt_bytebuf_t *b, const char *s);
mt_err_t mt_bytebuf_append_byte(mt_bytebuf_t *b, uint8_t byte);

#endif /* MAGITRICKLE_BYTEBUF_H */
