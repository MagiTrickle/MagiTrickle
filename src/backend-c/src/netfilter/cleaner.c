/* See netfilter_cleaner.h. Port of utils/netfilterTools/iptables-cleaner.go. */
#include "magitrickle/netfilter_cleaner.h"

#include <stdio.h>
#include <string.h>

static mt_err_t clean_one(mt_ipt_t *ipt, const char *chain_prefix) {
    if (!ipt) { return MT_OK; }

    char jump[128];
    snprintf(jump, sizeof(jump), "-j %s", chain_prefix);
    size_t prefix_len = strlen(chain_prefix);

    mt_ipt_rules_snapshot_t *snap;
    mt_err_t err = mt_ipt_get_current_rules(ipt, &snap);
    if (err != MT_OK) { return err; }

    for (size_t ti = 0; ti < snap->n_tables && err == MT_OK; ti++) {
        const mt_ipt_table_rules_t *t = &snap->tables[ti];
        for (size_t ci = 0; ci < t->n_chains && err == MT_OK; ci++) {
            const mt_ipt_chain_rules_t *c = &t->chains[ci];

            if (strncmp(c->chain_name, chain_prefix, prefix_len) == 0) {
                err = mt_ipt_register_chain_delete(ipt, t->table_name, c->chain_name);
                continue;
            }

            for (size_t ri = 0; ri < c->n_rules && err == MT_OK; ri++) {
                mt_ipt_rule_t *r = c->rules[ri];
                if (!mt_ipt_rule_contains(r, jump)) { continue; }

                err = mt_ipt_delete(ipt, t->table_name, c->chain_name,
                                    (const char *const *)r->parts, r->n_parts);
                if (mt_ipt_err_is_chain_not_initialized(err)) {
                    err = mt_ipt_register_chain_patch(ipt, t->table_name, c->chain_name);
                    if (err == MT_OK) {
                        err = mt_ipt_delete(ipt, t->table_name, c->chain_name,
                                           (const char *const *)r->parts, r->n_parts);
                    }
                }
            }
        }
    }

    mt_ipt_rules_snapshot_free(snap);
    if (err != MT_OK) { return err; }
    return mt_ipt_commit(ipt);
}

mt_err_t mt_netfilter_clean_iptables(mt_ipt_t *ipt4, mt_ipt_t *ipt6, const char *chain_prefix) {
    mt_err_t e1 = clean_one(ipt4, chain_prefix);
    mt_err_t e2 = clean_one(ipt6, chain_prefix);
    return e1 != MT_OK ? e1 : e2;
}

mt_err_t mt_netfilter_register_base_chains(mt_ipt_t *ipt4, mt_ipt_t *ipt6) {
    static const struct {
        const char *table;
        const char *chain;
    } base[] = {
        {"filter", "FORWARD"},
        {"mangle", "PREROUTING"},
        {"nat", "PREROUTING"},
        {"nat", "POSTROUTING"},
    };

    mt_ipt_t *ipts[2] = {ipt4, ipt6};
    for (size_t i = 0; i < 2; i++) {
        if (!ipts[i]) { continue; }
        for (size_t b = 0; b < sizeof(base) / sizeof(base[0]); b++) {
            mt_err_t err = mt_ipt_register_chain_patch(ipts[i], base[b].table, base[b].chain);
            if (err != MT_OK) { return err; }
        }
    }
    return MT_OK;
}
