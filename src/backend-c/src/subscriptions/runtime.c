/* See sub_runtime.h. Port of subscriptions/runtime_rule_sets.go's
 * subscriptionAsRuntimeRuleSet. */
#include "magitrickle/sub_runtime.h"

#include <stdio.h>
#include <stdlib.h>

#include "magitrickle/id.h"

mt_group_t *mt_sub_runtime_group(const mt_subscription_t *sub) {
    mt_group_t *g = mt_group_new();
    if (!g) { return NULL; }

    g->id = sub->id;

    const char *name = sub->name;
    char fallback[16 + MT_ID_STR_LEN];
    if (!name || name[0] == '\0') {
        char id_buf[MT_ID_STR_LEN];
        mt_id_format(sub->id, id_buf);
        snprintf(fallback, sizeof(fallback), "subscription:%s", id_buf);
        name = fallback;
    }
    if (mt_strset(&g->name, name) != MT_OK) {
        mt_group_free(g);
        return NULL;
    }
    if (mt_strset(&g->iface, sub->iface) != MT_OK) {
        mt_group_free(g);
        return NULL;
    }
    g->enable = sub->enable && sub->iface != NULL && sub->iface[0] != '\0';

    for (size_t i = 0; i < sub->n_rules; i++) {
        const mt_sub_rule_t *sr = sub->rules[i];
        mt_rule_t *r = mt_rule_new();
        if (!r) {
            mt_group_free(g);
            return NULL;
        }
        r->id = sr->id;
        mt_err_t err = mt_strset(&r->name, "");
        if (err == MT_OK) { err = mt_strset(&r->type, sr->type); }
        if (err == MT_OK) { err = mt_strset(&r->rule, sr->rule); }
        r->enable = sr->enable;
        if (err != MT_OK || mt_group_add_rule(g, r) != MT_OK) {
            mt_rule_free(r);
            mt_group_free(g);
            return NULL;
        }
    }
    return g;
}
