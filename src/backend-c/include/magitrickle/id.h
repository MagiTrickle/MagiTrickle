/* 4-byte identifiers (groups, rules, subscriptions), text form = 8 lowercase
 * hex chars. Mirrors Go's utils/intID (UnmarshalText requires exactly 8
 * chars; hex.DecodeString accepts both cases). */
#ifndef MAGITRICKLE_ID_H
#define MAGITRICKLE_ID_H

#include <stdbool.h>
#include <stdint.h>

#include "magitrickle/err.h"

typedef struct mt_id {
    uint8_t b[4];
} mt_id_t;

#define MT_ID_STR_LEN 9 /* 8 hex chars + NUL */

/* Requires exactly 8 hex chars (any case), like Go ParseID. */
mt_err_t mt_id_parse(const char *s, mt_id_t *out);

/* Lowercase hex, like Go ID.String(). buf >= MT_ID_STR_LEN. */
void mt_id_format(mt_id_t id, char *buf);

bool mt_id_is_zero(mt_id_t id);
bool mt_id_equal(mt_id_t a, mt_id_t b);

/* Cryptographically random ID (getrandom, /dev/urandom fallback). */
mt_id_t mt_id_random(void);

#endif /* MAGITRICKLE_ID_H */
