/* Small owning byte-key -> array-index table. No deletion; clear as a unit.
 * Zero initialization is sufficient. Rehashing preserves first-inserted values.
 * Callers retain their own objects: only the copied keys belong to this table. */
#ifndef MAGITRICKLE_LOOKUP_H
#define MAGITRICKLE_LOOKUP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "magitrickle/err.h"
typedef struct mt_lookup_entry {
    unsigned char *key;
    size_t key_len;
    size_t value;
    uint64_t hash;
} mt_lookup_entry_t;
typedef struct mt_lookup {
    mt_lookup_entry_t *entries;
    size_t len, cap;
} mt_lookup_t;
bool mt_lookup_get(const mt_lookup_t *map, const void *key, size_t len, size_t *value);
/* Existing key is kept unchanged (first occurrence wins); inserted may be NULL. */
mt_err_t mt_lookup_put(mt_lookup_t *map, const void *key, size_t len, size_t value,
                       bool *inserted);
void mt_lookup_clear(mt_lookup_t *map);
#endif
