/* "override" chain — port of utils/iptables/chain-override.go.
 *
 * Full chain replacement: Compile() emits FLUSH + append-all whenever the
 * desired rule list differs from what's on the system (or the chain does
 * not exist yet), otherwise zero commands (no-op commit). `existing == NULL`
 * means "this chain does not exist on the system at all" (Go: a nil slice
 * from the current-rules map, as opposed to a declared-but-empty chain,
 * which is a non-NULL zero-length slice) — see mt_ipt_table_rules_find_chain.
 */
#include "magitrickle/iptables.h"

#include <stdlib.h>
#include <string.h>

typedef struct chain_override {
    mt_ipt_chain_t base;
    mt_ipt_rule_t **rules; /* owned */
    size_t n, cap;
} chain_override_t;

static mt_err_t override_compile(mt_ipt_chain_t *self, const char *chain_name,
                                 mt_ipt_rule_t *const *existing, size_t n_existing,
                                 mt_ipt_command_t **out_cmds, size_t *out_n,
                                 int8_t *out_priority) {
    chain_override_t *c = (chain_override_t *)self;
    *out_cmds = NULL;
    *out_n = 0;
    *out_priority = -128;

    if (existing != NULL && n_existing == c->n) {
        bool match = true;
        for (size_t i = 0; i < c->n; i++) {
            if (!mt_ipt_rule_equal(c->rules[i], existing[i])) {
                match = false;
                break;
            }
        }
        if (match) { return MT_OK; /* already in the desired state */ }
    }

    mt_ipt_command_t *cmds = calloc(c->n + 1, sizeof(*cmds));
    if (!cmds) { return MT_ERR_NOMEM; }

    cmds[0].option = MT_IPT_OP_FLUSH;
    cmds[0].chain = strdup(chain_name);
    cmds[0].rule = NULL;
    if (!cmds[0].chain) {
        free(cmds);
        return MT_ERR_NOMEM;
    }

    for (size_t i = 0; i < c->n; i++) {
        cmds[i + 1].option = MT_IPT_OP_APPEND;
        cmds[i + 1].chain = strdup(chain_name);
        cmds[i + 1].rule = mt_ipt_rule_clone(c->rules[i]);
        if (!cmds[i + 1].chain || !cmds[i + 1].rule) {
            mt_ipt_command_list_free(cmds, i + 2);
            return MT_ERR_NOMEM;
        }
    }

    *out_cmds = cmds;
    *out_n = c->n + 1;
    return MT_OK;
}

static mt_err_t override_append(mt_ipt_chain_t *self, const mt_ipt_rule_t *rule) {
    chain_override_t *c = (chain_override_t *)self;
    if (c->n + 1 > c->cap) {
        size_t newcap = c->cap == 0 ? 8 : c->cap * 2;
        mt_ipt_rule_t **tmp = realloc(c->rules, newcap * sizeof(*tmp));
        if (!tmp) { return MT_ERR_NOMEM; }
        c->rules = tmp;
        c->cap = newcap;
    }
    mt_ipt_rule_t *owned = mt_ipt_rule_clone(rule);
    if (!owned) { return MT_ERR_NOMEM; }
    c->rules[c->n++] = owned;
    return MT_OK;
}

static mt_err_t override_insert(mt_ipt_chain_t *self, int rule_num, const mt_ipt_rule_t *rule) {
    chain_override_t *c = (chain_override_t *)self;
    if (rule_num < 1 || (size_t)rule_num > c->n + 1) { return MT_OK; /* silently ignored, matches Go */ }

    if (c->n + 1 > c->cap) {
        size_t newcap = c->cap == 0 ? 8 : c->cap * 2;
        mt_ipt_rule_t **tmp = realloc(c->rules, newcap * sizeof(*tmp));
        if (!tmp) { return MT_ERR_NOMEM; }
        c->rules = tmp;
        c->cap = newcap;
    }
    mt_ipt_rule_t *owned = mt_ipt_rule_clone(rule);
    if (!owned) { return MT_ERR_NOMEM; }

    size_t insert_idx = (size_t)rule_num - 1;
    for (size_t i = c->n; i > insert_idx; i--) { c->rules[i] = c->rules[i - 1]; }
    c->rules[insert_idx] = owned;
    c->n++;
    return MT_OK;
}

static mt_err_t override_remove(mt_ipt_chain_t *self, const mt_ipt_rule_t *rule) {
    chain_override_t *c = (chain_override_t *)self;
    for (size_t i = 0; i < c->n; i++) {
        if (!mt_ipt_rule_equal(c->rules[i], rule)) { continue; }
        mt_ipt_rule_free(c->rules[i]);
        for (size_t j = i; j + 1 < c->n; j++) { c->rules[j] = c->rules[j + 1]; }
        c->n--;
        return MT_OK;
    }
    return MT_OK;
}

static void override_destroy(mt_ipt_chain_t *self) {
    chain_override_t *c = (chain_override_t *)self;
    if (!c) { return; }
    for (size_t i = 0; i < c->n; i++) { mt_ipt_rule_free(c->rules[i]); }
    free(c->rules);
    free(c);
}

static const mt_ipt_chain_ops_t k_override_ops = {
    .compile = override_compile,
    .append = override_append,
    .insert = override_insert,
    .remove = override_remove,
    .destroy = override_destroy,
};

mt_ipt_chain_t *mt_ipt_chain_override_new(void) {
    chain_override_t *c = calloc(1, sizeof(*c));
    if (!c) { return NULL; }
    c->base.ops = &k_override_ops;
    return &c->base;
}
