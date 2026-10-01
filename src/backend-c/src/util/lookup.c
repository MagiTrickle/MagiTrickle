#include "magitrickle/lookup.h"
#include <stdlib.h>
#include <string.h>

static uint64_t hash_bytes(const void *key, size_t len) {
    const unsigned char *p = key;
    uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    /* Mix high bits into the bucket mask (especially for four-byte IDs). */
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    return h;
}

static size_t find_slot(const mt_lookup_t *map, const void *key, size_t len, uint64_t hash) {
    size_t i = (size_t)hash & (map->cap - 1);
    while (map->entries[i].key) {
        const mt_lookup_entry_t *e = &map->entries[i];
        if (e->hash == hash && e->key_len == len && (!len || memcmp(e->key, key, len) == 0)) { break; }
        i = (i + 1) & (map->cap - 1);
    }
    return i;
}

bool mt_lookup_get(const mt_lookup_t *map, const void *key, size_t len, size_t *value) {
    if (!map->cap) { return false; }
    const mt_lookup_entry_t *e = &map->entries[find_slot(map, key, len, hash_bytes(key, len))];
    if (!e->key) { return false; }
    if (value) { *value = e->value; }
    return true;
}

static mt_err_t grow(mt_lookup_t *map) {
    if (map->cap > SIZE_MAX / 2 / sizeof(mt_lookup_entry_t)) { return MT_ERR_NOMEM; }
    size_t cap = map->cap ? map->cap * 2 : 32;
    mt_lookup_entry_t *entries = calloc(cap, sizeof(*entries));
    if (!entries) { return MT_ERR_NOMEM; }
    mt_lookup_t next = {.entries = entries, .cap = cap, .len = map->len};
    for (size_t i = 0; i < map->cap; i++) {
        mt_lookup_entry_t *e = &map->entries[i];
        if (e->key) { next.entries[find_slot(&next, e->key, e->key_len, e->hash)] = *e; }
    }
    free(map->entries);
    *map = next;
    return MT_OK;
}

mt_err_t mt_lookup_put(mt_lookup_t *map, const void *key, size_t len, size_t value,
                       bool *inserted) {
    if (inserted) { *inserted = false; }
    uint64_t hash = hash_bytes(key, len);
    if (map->cap && map->entries[find_slot(map, key, len, hash)].key) { return MT_OK; }
    if (map->len >= map->cap / 2) {
        mt_err_t err = grow(map);
        if (err != MT_OK) { return err; }
    }
    unsigned char *copy = malloc(len ? len : 1);
    if (!copy) { return MT_ERR_NOMEM; }
    if (len) { memcpy(copy, key, len); }
    size_t slot = find_slot(map, key, len, hash);
    /* Transfer key ownership explicitly; mt_lookup_clear frees each key. */
    mt_lookup_entry_t *entry = &map->entries[slot];
    entry->key = copy;
    entry->key_len = len;
    entry->value = value;
    entry->hash = hash;
    map->len++;
    if (inserted) { *inserted = true; }
    return MT_OK;
}

void mt_lookup_clear(mt_lookup_t *map) {
    for (size_t i = 0; i < map->cap; i++) { free(map->entries[i].key); }
    free(map->entries);
    memset(map, 0, sizeof(*map));
}
