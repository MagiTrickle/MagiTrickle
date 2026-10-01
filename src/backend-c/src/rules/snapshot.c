#include "magitrickle/rulesnap.h"

#include <stdlib.h>
#include <string.h>

#include "magitrickle/sub_runtime.h"

static void group_snapshot_free(mt_group_snapshot_t *g)
{
    if (g == NULL) {
        return;
    }
    free(g->name);
    mt_matcher_free(g->matcher);
    free(g);
}

static mt_err_t append_snapshot_entry(mt_ruleset_snapshot_t *snap, mt_id_t id,
                                      const char *name, mt_rule_t **rules,
                                      size_t n_rules)
{
    mt_group_snapshot_t *gs = calloc(1, sizeof(*gs));
    if (gs == NULL) {
        return MT_ERR_NOMEM;
    }
    gs->id = id;
    gs->name = strdup(name != NULL ? name : "");
    gs->matcher = mt_matcher_new();
    if (gs->name == NULL || gs->matcher == NULL) {
        group_snapshot_free(gs);
        return MT_ERR_NOMEM;
    }
    for (size_t j = 0; j < n_rules; j++) {
        const mt_rule_t *r = rules[j];
        if (!r->enable) {
            continue;
        }
        if (mt_matcher_add(gs->matcher, r->type, r->rule) != MT_OK) {
            group_snapshot_free(gs);
            return MT_ERR_NOMEM;
        }
    }
    snap->groups[snap->n_groups++] = gs;
    return MT_OK;
}

mt_ruleset_snapshot_t *mt_ruleset_snapshot_build(const mt_config_t *cfg)
{
    mt_ruleset_snapshot_t *snap = calloc(1, sizeof(*snap));
    if (snap == NULL) {
        return NULL;
    }
    size_t cap = cfg->n_groups + cfg->n_subscriptions;
    if (cap == 0) {
        return snap;
    }
    snap->groups = calloc(cap, sizeof(mt_group_snapshot_t *));
    if (snap->groups == NULL) {
        free(snap);
        return NULL;
    }

    for (size_t i = 0; i < cfg->n_groups; i++) {
        const mt_group_t *g = cfg->groups[i];
        if (!g->enable) {
            continue; /* disabled groups can never emit an action */
        }
        if (append_snapshot_entry(snap, g->id, g->name, g->rules, g->n_rules) !=
            MT_OK) {
            mt_ruleset_snapshot_free(snap);
            return NULL;
        }
    }

    /* Subscription-derived entries: synthesize a runtime mt_group_t per
     * subscription (mt_sub_runtime_group already applies the same
     * enable-gating -- sub.Enable && interface non-empty -- as Go's
     * subscriptionAsRuntimeRuleSet), feed it through the same
     * append_snapshot_entry path as a real config group, then discard the
     * synthesized shell -- only its id/name/matcher survive into the
     * snapshot, exactly as for a real group. */
    for (size_t i = 0; i < cfg->n_subscriptions; i++) {
        mt_group_t *synth = mt_sub_runtime_group(cfg->subscriptions[i]);
        if (synth == NULL) {
            mt_ruleset_snapshot_free(snap);
            return NULL;
        }
        mt_err_t err = MT_OK;
        if (synth->enable) {
            err = append_snapshot_entry(snap, synth->id, synth->name,
                                        synth->rules, synth->n_rules);
        }
        mt_group_free(synth);
        if (err != MT_OK) {
            mt_ruleset_snapshot_free(snap);
            return NULL;
        }
    }
    return snap;
}

void mt_ruleset_snapshot_free(mt_ruleset_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }
    for (size_t i = 0; i < snap->n_groups; i++) {
        group_snapshot_free(snap->groups[i]);
    }
    free(snap->groups);
    free(snap);
}
