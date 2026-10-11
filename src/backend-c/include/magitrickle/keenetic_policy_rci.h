#ifndef MAGITRICKLE_KEENETIC_POLICY_RCI_H
#define MAGITRICKLE_KEENETIC_POLICY_RCI_H
#ifdef MT_ENTWARE_KN
#include <stdatomic.h>
#include <stddef.h>
#include "magitrickle/keenetic_policy.h"
#define MT_KN_POLICY_BODY_LIMIT (1024u * 1024u)
#define MT_KN_POLICY_LIST_LIMIT 256u
typedef struct {
    char id[MT_KN_POLICY_ID_SIZE];
    char description[512];
    bool available;
    uint32_t mark;
} mt_kn_policy_entry_t;
/* Caller owns *entries on success. Strict bounded JSON, no CLI/grep/shell. */
mt_err_t mt_kn_policy_parse(const char *json, size_t len, mt_kn_policy_entry_t **entries, size_t *count);
mt_err_t mt_kn_policy_fetch(mt_kn_policy_entry_t **entries, size_t *count, const atomic_bool *stop);
#endif
#endif
