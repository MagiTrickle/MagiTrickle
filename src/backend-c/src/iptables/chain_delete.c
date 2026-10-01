/* "delete" chain — port of utils/iptables/chain-delete.go: schedules a
 * chain for removal (FLUSH + -X) if it currently exists; a no-op if it
 * doesn't. Append/Insert/Delete are no-ops (there is nothing meaningful
 * to stage for a chain being torn down).
 */
#include "magitrickle/iptables.h"

#include <stdlib.h>
#include <string.h>

typedef struct chain_delete {
    mt_ipt_chain_t base;
} chain_delete_t;

static mt_err_t delete_compile(mt_ipt_chain_t *self, const char *chain_name,
                               mt_ipt_rule_t *const *existing, size_t n_existing,
                               mt_ipt_command_t **out_cmds, size_t *out_n,
                               int8_t *out_priority) {
    (void)self;
    (void)n_existing;
    *out_priority = 127;
    *out_cmds = NULL;
    *out_n = 0;

    if (existing == NULL) { return MT_OK; /* chain doesn't exist: nothing to do */ }

    mt_ipt_command_t *cmds = calloc(2, sizeof(*cmds));
    if (!cmds) { return MT_ERR_NOMEM; }

    cmds[0].option = MT_IPT_OP_FLUSH;
    cmds[0].chain = strdup(chain_name);
    cmds[1].option = MT_IPT_OP_DELETE_CHAIN;
    cmds[1].chain = strdup(chain_name);
    if (!cmds[0].chain || !cmds[1].chain) {
        mt_ipt_command_list_free(cmds, 2);
        return MT_ERR_NOMEM;
    }

    *out_cmds = cmds;
    *out_n = 2;
    return MT_OK;
}

static mt_err_t delete_noop_append(mt_ipt_chain_t *self, const mt_ipt_rule_t *rule) {
    (void)self;
    (void)rule;
    return MT_OK;
}

static mt_err_t delete_noop_insert(mt_ipt_chain_t *self, int rule_num, const mt_ipt_rule_t *rule) {
    (void)self;
    (void)rule_num;
    (void)rule;
    return MT_OK;
}

static mt_err_t delete_noop_remove(mt_ipt_chain_t *self, const mt_ipt_rule_t *rule) {
    (void)self;
    (void)rule;
    return MT_OK;
}

static void delete_destroy(mt_ipt_chain_t *self) {
    free(self);
}

static const mt_ipt_chain_ops_t k_delete_ops = {
    .compile = delete_compile,
    .append = delete_noop_append,
    .insert = delete_noop_insert,
    .remove = delete_noop_remove,
    .destroy = delete_destroy,
};

mt_ipt_chain_t *mt_ipt_chain_delete_new(void) {
    chain_delete_t *c = calloc(1, sizeof(*c));
    if (!c) { return NULL; }
    c->base.ops = &k_delete_ops;
    return &c->base;
}
