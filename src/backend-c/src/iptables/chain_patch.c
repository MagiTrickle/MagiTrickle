/* "patch" chain — port of utils/iptables/chain-patch.go.
 *
 * Rules merge with whatever is already on the chain; for a given exact
 * rule (by content) only the *last* recorded operation survives, and it
 * moves to the end of the pending list (Go: addRule drops any existing
 * entry with the same Rule content, then appends the new one). Insert
 * behaves like InsertUnique (position is only honoured when the rule
 * isn't already present), not a raw iptables -I.
 */
#include "magitrickle/iptables.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct patch_entry {
    mt_ipt_option_t option;
    int rule_num;
    mt_ipt_rule_t *rule; /* owned */
    bool ordered;
    int priority;
} patch_entry_t;

typedef struct chain_patch {
    mt_ipt_chain_t base;
    patch_entry_t *entries;
    size_t n, cap;
} chain_patch_t;

static mt_err_t add_rule(chain_patch_t *c, mt_ipt_option_t option, int rule_num,
                         const mt_ipt_rule_t *rule, bool ordered, int priority) {
    mt_ipt_rule_t *owned = mt_ipt_rule_clone(rule);
    if (!owned) { return MT_ERR_NOMEM; }

    size_t shifted = 0;
    for (size_t i = 0; i < c->n; i++) {
        if (mt_ipt_rule_equal(c->entries[i].rule, owned)) {
            mt_ipt_rule_free(c->entries[i].rule);
            continue;
        }
        if (shifted != i) { c->entries[shifted] = c->entries[i]; }
        shifted++;
    }
    c->n = shifted;

    if (c->n + 1 > c->cap) {
        size_t newcap = c->cap == 0 ? 8 : c->cap * 2;
        patch_entry_t *tmp = realloc(c->entries, newcap * sizeof(*tmp));
        if (!tmp) {
            mt_ipt_rule_free(owned);
            return MT_ERR_NOMEM;
        }
        c->entries = tmp;
        c->cap = newcap;
    }
    c->entries[c->n].option = option;
    c->entries[c->n].rule_num = rule_num;
    c->entries[c->n].rule = owned;
    c->entries[c->n].ordered = ordered;
    c->entries[c->n].priority = priority;
    c->n++;
    return MT_OK;
}

/* ---- tiny rule-string -> count map, local to one Compile() call ------- */

typedef struct str_count {
    char *key;
    uint32_t count;
} str_count_t;

static str_count_t *count_lookup(str_count_t *map, size_t map_n, const char *key) {
    for (size_t i = 0; i < map_n; i++) {
        if (strcmp(map[i].key, key) == 0) { return &map[i]; }
    }
    return NULL;
}

static mt_err_t count_bump(str_count_t **map, size_t *map_n, size_t *map_cap,
                           const char *key) {
    str_count_t *e = count_lookup(*map, *map_n, key);
    if (e) {
        e->count++;
        return MT_OK;
    }
    if (*map_n + 1 > *map_cap) {
        size_t newcap = *map_cap == 0 ? 8 : *map_cap * 2;
        str_count_t *tmp = realloc(*map, newcap * sizeof(**map));
        if (!tmp) { return MT_ERR_NOMEM; }
        *map = tmp;
        *map_cap = newcap;
    }
    (*map)[*map_n].key = strdup(key);
    if (!(*map)[*map_n].key) { return MT_ERR_NOMEM; }
    (*map)[*map_n].count = 1;
    (*map_n)++;
    return MT_OK;
}

static void count_map_free(str_count_t *map, size_t n) {
    if (!map) { return; }
    for (size_t i = 0; i < n; i++) { free(map[i].key); }
    free(map);
}

static mt_err_t cmds_push(mt_ipt_command_t **cmds, size_t *n, size_t *cap,
                          mt_ipt_option_t option, const char *chain_name,
                          int rule_num, const mt_ipt_rule_t *rule) {
    if (*n + 1 > *cap) {
        size_t newcap = *cap == 0 ? 8 : *cap * 2;
        mt_ipt_command_t *tmp = realloc(*cmds, newcap * sizeof(**cmds));
        if (!tmp) { return MT_ERR_NOMEM; }
        *cmds = tmp;
        *cap = newcap;
    }
    mt_ipt_command_t *c = &(*cmds)[*n];
    c->option = option;
    c->chain = strdup(chain_name);
    c->rule_num = rule_num;
    c->rule = rule ? mt_ipt_rule_clone(rule) : NULL;
    if (!c->chain || (rule && !c->rule)) {
        free(c->chain);
        mt_ipt_rule_free(c->rule);
        return MT_ERR_NOMEM;
    }
    (*n)++;
    return MT_OK;
}

static int ordered_compare(const void *lhs, const void *rhs) {
    const patch_entry_t *a = *(const patch_entry_t *const *)lhs;
    const patch_entry_t *b = *(const patch_entry_t *const *)rhs;
    if (a->priority != b->priority) { return a->priority < b->priority ? -1 : 1; }
    size_t common = a->rule->n_parts < b->rule->n_parts ? a->rule->n_parts : b->rule->n_parts;
    for (size_t i = 0; i < common; i++) {
        int cmp = strcmp(a->rule->parts[i], b->rule->parts[i]);
        if (cmp != 0) { return cmp; }
    }
    return (a->rule->n_parts > b->rule->n_parts) - (a->rule->n_parts < b->rule->n_parts);
}

static size_t ordered_rule_index(const patch_entry_t *const *ordered, size_t n,
                                 const mt_ipt_rule_t *rule) {
    for (size_t i = 0; i < n; i++) {
        if (mt_ipt_rule_equal(ordered[i]->rule, rule)) { return i; }
    }
    return n;
}

/* AppendUnique is insufficient for routing precedence: changing a
 * priority must move an existing jump, not merely recognize it. Compile
 * the ordered subset before ordinary operations (whose rule contents
 * are disjoint). Reuse its existing slots to avoid moving it wholesale
 * past unrelated firewall rules. No prefix-based ownership assumptions. */
static mt_err_t compile_ordered(chain_patch_t *c, const char *chain_name,
                                mt_ipt_rule_t *const *existing, size_t n_existing,
                                mt_ipt_command_t **cmds, size_t *n_cmds, size_t *cap_cmds) {
    size_t n = 0;
    for (size_t i = 0; i < c->n; i++) { if (c->entries[i].ordered) { n++; } }
    if (n == 0) { return MT_OK; }
    const patch_entry_t **ordered = calloc(n, sizeof(*ordered));
    size_t *first_position = calloc(n, sizeof(*first_position));
    if (!ordered || !first_position) { free(ordered); free(first_position); return MT_ERR_NOMEM; }
    size_t at = 0;
    for (size_t i = 0; i < c->n; i++) {
        if (c->entries[i].ordered) { ordered[at++] = &c->entries[i]; }
    }
    qsort(ordered, n, sizeof(*ordered), ordered_compare);

    size_t present = 0, distinct = 0;
    bool same = true;
    for (size_t i = 0; i < n_existing; i++) {
        size_t idx = ordered_rule_index(ordered, n, existing[i]);
        if (idx == n) { continue; }
        if (first_position[idx] == 0) { first_position[idx] = i + 1; distinct++; }
        if (present >= n || !mt_ipt_rule_equal(existing[i], ordered[present]->rule)) { same = false; }
        present++;
    }
    if (same && present == n) { free(ordered); free(first_position); return MT_OK; }

    mt_err_t err = MT_OK;
    for (size_t i = 0; i < n_existing && err == MT_OK; i++) {
        if (ordered_rule_index(ordered, n, existing[i]) < n) {
            err = cmds_push(cmds, n_cmds, cap_cmds, MT_IPT_OP_DELETE, chain_name, 0, existing[i]);
        }
    }
    size_t foreign = 0, seen = 0, inserted = 0;
    for (size_t i = 0; i < n_existing && err == MT_OK; i++) {
        size_t idx = ordered_rule_index(ordered, n, existing[i]);
        if (idx == n) { foreign++; continue; }
        /* Duplicate jumps are removed, not used as extra insertion
         * slots that could shift the original firewall boundaries. */
        if (first_position[idx] != i + 1) { continue; }
        seen++;
        size_t end = seen == distinct ? n : (inserted < n ? inserted + 1 : n);
        while (inserted < end && err == MT_OK) {
            if (foreign >= INT_MAX || inserted >= (size_t)INT_MAX - foreign) {
                err = MT_ERR_INVAL;
                break;
            }
            err = cmds_push(cmds, n_cmds, cap_cmds, MT_IPT_OP_INSERT, chain_name,
                            (int)(foreign + inserted + 1), ordered[inserted]->rule);
            inserted++;
        }
    }
    /* No managed slot exists yet (initial install or external flush). */
    for (; inserted < n && err == MT_OK; inserted++) {
        err = cmds_push(cmds, n_cmds, cap_cmds, MT_IPT_OP_APPEND, chain_name, 0, ordered[inserted]->rule);
    }
    free(ordered);
    free(first_position);
    return err;
}

static mt_err_t patch_compile(mt_ipt_chain_t *self, const char *chain_name,
                              mt_ipt_rule_t *const *existing, size_t n_existing,
                              mt_ipt_command_t **out_cmds, size_t *out_n,
                              int8_t *out_priority) {
    chain_patch_t *c = (chain_patch_t *)self;
    *out_cmds = NULL;
    *out_n = 0;
    *out_priority = 0;

    str_count_t *map = NULL;
    size_t map_n = 0, map_cap = 0;
    mt_err_t err = MT_OK;

    for (size_t i = 0; i < n_existing && err == MT_OK; i++) {
        char *key = mt_ipt_rule_string(existing[i]);
        if (!key) {
            err = MT_ERR_NOMEM;
            break;
        }
        err = count_bump(&map, &map_n, &map_cap, key);
        free(key);
    }
    if (err != MT_OK) {
        count_map_free(map, map_n);
        return err;
    }

    mt_ipt_command_t *cmds = NULL;
    size_t n = 0, cap = 0;
    err = compile_ordered(c, chain_name, existing, n_existing, &cmds, &n, &cap);

    for (size_t i = 0; i < c->n && err == MT_OK; i++) {
        patch_entry_t *e = &c->entries[i];
        if (e->ordered) { continue; }
        char *key = mt_ipt_rule_string(e->rule);
        if (!key) {
            err = MT_ERR_NOMEM;
            break;
        }
        str_count_t *found = count_lookup(map, map_n, key);
        free(key);
        uint32_t count = found ? found->count : 0;

        while (count > 1 && err == MT_OK) {
            err = cmds_push(&cmds, &n, &cap, MT_IPT_OP_DELETE, chain_name, 0, e->rule);
            count--;
        }
        if (err != MT_OK) { break; }

        switch (e->option) {
        case MT_IPT_OP_APPEND:
            if (count > 0) { continue; }
            err = cmds_push(&cmds, &n, &cap, MT_IPT_OP_APPEND, chain_name, 0, e->rule);
            break;
        case MT_IPT_OP_INSERT:
            if (count > 0) { continue; }
            err = cmds_push(&cmds, &n, &cap, MT_IPT_OP_INSERT, chain_name, e->rule_num, e->rule);
            break;
        case MT_IPT_OP_DELETE:
            if (count == 0) { continue; }
            err = cmds_push(&cmds, &n, &cap, MT_IPT_OP_DELETE, chain_name, 0, e->rule);
            break;
        default:
            break;
        }
    }

    count_map_free(map, map_n);

    if (err != MT_OK) {
        mt_ipt_command_list_free(cmds, n);
        return err;
    }

    *out_cmds = cmds;
    *out_n = n;
    return MT_OK;
}

static mt_err_t patch_append(mt_ipt_chain_t *self, const mt_ipt_rule_t *rule) {
    return add_rule((chain_patch_t *)self, MT_IPT_OP_APPEND, 0, rule, false, 0);
}

static mt_err_t patch_append_ordered(mt_ipt_chain_t *self, const mt_ipt_rule_t *rule, int priority) {
    return add_rule((chain_patch_t *)self, MT_IPT_OP_APPEND, 0, rule, true, priority);
}

static mt_err_t patch_insert(mt_ipt_chain_t *self, int rule_num, const mt_ipt_rule_t *rule) {
    return add_rule((chain_patch_t *)self, MT_IPT_OP_INSERT, rule_num, rule, false, 0);
}

static mt_err_t patch_remove(mt_ipt_chain_t *self, const mt_ipt_rule_t *rule) {
    return add_rule((chain_patch_t *)self, MT_IPT_OP_DELETE, 0, rule, false, 0);
}

static void patch_destroy(mt_ipt_chain_t *self) {
    chain_patch_t *c = (chain_patch_t *)self;
    if (!c) { return; }
    for (size_t i = 0; i < c->n; i++) { mt_ipt_rule_free(c->entries[i].rule); }
    free(c->entries);
    free(c);
}

static const mt_ipt_chain_ops_t k_patch_ops = {
    .compile = patch_compile,
    .append = patch_append,
    .insert = patch_insert,
    .remove = patch_remove,
    .destroy = patch_destroy,
    .append_ordered = patch_append_ordered,
};

mt_ipt_chain_t *mt_ipt_chain_patch_new(void) {
    chain_patch_t *c = calloc(1, sizeof(*c));
    if (!c) { return NULL; }
    c->base.ops = &k_patch_ops;
    return &c->base;
}
