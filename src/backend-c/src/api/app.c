/* See app.h. Port of the group/interface/config-save slice of app.go. */
#include "magitrickle/app.h"
#include "magitrickle/profiles.h"

#include <errno.h>
#include <ifaddrs.h>
#include <linux/if.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "magitrickle/keenetic_rci.h"
#include "magitrickle/log.h"
#include "magitrickle/lookup.h"
#include "magitrickle/netfilter_cleaner.h"
#include "magitrickle/nfcommit.h"
#include "magitrickle/rulesnap.h"
#include "magitrickle/sub_fetch.h"
#include "magitrickle/sub_runtime.h"
#include "magitrickle/subparse.h"

struct mt_app {
    mt_config_t *cfg;
    /* Immutable startup helper names: rulesets borrow these, not the
     * reloadable configuration strings. */
    char *ipset_prefix;
    char *chain_prefix;
    mt_cache_t *cache;
    mt_ipt_t *ipt4;
    mt_ipt_t *ipt6;
    mt_rtnl_t *rtnl;
    mt_dns_pipeline_t *pipeline;
    uint32_t start_idx;
    bool running;
    uint64_t next_sub_revision;
    size_t next_due_index;

    mt_ruleset_t **rulesets;
    size_t n_rulesets;
    size_t cap_rulesets;

    /* Subscription rulesets: a separate array (mirrors Go's
     * a.subscriptionRuleSets being distinct from a.userRuleSets), since
     * they're rebuilt wholesale on every subscription change rather than
     * edited in place. sub_synth_groups[i] is the mt_group_t backing
     * sub_rulesets[i] -- mt_ruleset_t only *borrows* its group pointer
     * (ruleset.h), so unlike cfg->groups (long-lived, owned by the
     * config), this synthesized group must be kept alive here for
     * exactly as long as its ruleset exists, and freed alongside it. */
    mt_ruleset_t **sub_rulesets;
    mt_group_t **sub_synth_groups;
    size_t n_sub_rulesets;
    size_t cap_sub_rulesets;

    mt_port_remap_t *port_remap; /* borrowed, nullable (main() owns it) */

    /* Netfilter committer (nfcommit.h) and the mutex serializing its
     * rebuilds against this thread's own netfilter mutation. Both are
     * absent unless the committer was started, in which case nf_mu is
     * uncontended and every lock/unlock below is a no-op pair.
     *
     * This does not walk back decisions.md D-19's "netfilter mutation is
     * single-threaded": the committer thread only ever *reads* the
     * ruleset registry (to stage each group's chains), and the loop
     * thread remains the only writer. That leaves the DNS hot path, the
     * other reader, lock-free as D-17 requires -- readers never conflict
     * with each other, and the lock exists solely so the registry cannot
     * be rewritten, or an iptables engine driven, while a rebuild is
     * mid-flight.
     *
     * Recursive on purpose: none of the entry points below nest today,
     * but they are the kind that grows into each other (a subscription
     * replace acquiring a sync, say), and a plain mutex would turn that
     * edit into a deadlocked router rather than a failing test. */
    pthread_mutex_t nf_mu;
    bool nf_mu_ready;
    mt_nfcommit_t *committer;
};

/* Taken by every entry point that mutates the ruleset registry or drives
 * an iptables engine. Interrupting a pass in flight first means this
 * thread waits for an abort rather than for a whole rebuild; the pass
 * reschedules itself and picks up the change we are about to make. */
static void app_nf_enter(mt_app_t *app) {
    if (!app->nf_mu_ready) { return; }
    mt_nfcommit_interrupt(app->committer);
    pthread_mutex_lock(&app->nf_mu);
}

static void app_nf_leave(mt_app_t *app) {
    if (!app->nf_mu_ready) { return; }
    pthread_mutex_unlock(&app->nf_mu);
}

static mt_ruleset_deps_t ruleset_deps(mt_app_t *app) {
    mt_ruleset_deps_t deps = {
        .config = app->cfg,
        .ipt4 = app->ipt4,
        .ipt6 = app->ipt6,
        .rtnl = app->rtnl,
        .ipset_prefix = app->ipset_prefix,
        .chain_prefix = app->chain_prefix,
        .start_idx = app->start_idx,
    };
    return deps;
}

static mt_err_t rulesets_push(mt_app_t *app, mt_ruleset_t *rs) {
    if (app->n_rulesets == app->cap_rulesets) {
        size_t new_cap = app->cap_rulesets ? app->cap_rulesets * 2 : 8;
        mt_ruleset_t **na = realloc(app->rulesets, new_cap * sizeof(*na));
        if (!na) { return MT_ERR_NOMEM; }
        app->rulesets = na;
        app->cap_rulesets = new_cap;
    }
    app->rulesets[app->n_rulesets++] = rs;
    return MT_OK;
}

static void rulesets_remove_at(mt_app_t *app, size_t idx) {
    for (size_t i = idx; i + 1 < app->n_rulesets; i++) {
        app->rulesets[i] = app->rulesets[i + 1];
    }
    app->n_rulesets--;
}

static mt_err_t sub_rulesets_push(mt_app_t *app, mt_ruleset_t *rs, mt_group_t *synth) {
    if (app->n_sub_rulesets == app->cap_sub_rulesets) {
        size_t new_cap = app->cap_sub_rulesets ? app->cap_sub_rulesets * 2 : 8;
        mt_ruleset_t **nr = realloc(app->sub_rulesets, new_cap * sizeof(*nr));
        if (!nr) { return MT_ERR_NOMEM; }
        app->sub_rulesets = nr;
        mt_group_t **ng = realloc(app->sub_synth_groups, new_cap * sizeof(*ng));
        /* cap_sub_rulesets is only bumped once BOTH arrays have grown --
         * if ng fails, sub_rulesets is left secretly larger than
         * cap_sub_rulesets tracks, which is harmless (the next push just
         * redoes an equivalent-or-larger realloc; it never under-allocates
         * sub_synth_groups against a cap that outgrew it). */
        if (!ng) { return MT_ERR_NOMEM; }
        app->sub_synth_groups = ng;
        app->cap_sub_rulesets = new_cap;
    }
    app->sub_rulesets[app->n_sub_rulesets] = rs;
    app->sub_synth_groups[app->n_sub_rulesets] = synth;
    app->n_sub_rulesets++;
    return MT_OK;
}

static void free_subscription_rulesets(mt_app_t *app) {
    for (size_t i = 0; i < app->n_sub_rulesets; i++) {
        mt_ruleset_disable(app->sub_rulesets[i]);
        mt_ruleset_free(app->sub_rulesets[i]);
        mt_group_free(app->sub_synth_groups[i]);
    }
    app->n_sub_rulesets = 0;
}

/* Disables+frees the whole current subscription-ruleset array and
 * rebuilds it from app->cfg->subscriptions -- matches Go's
 * syncSubscriptionRuleSetsLocked/buildSubscriptionRuleSetsLocked exactly:
 * always a full disable-then-rebuild, never an in-place edit. If
 * app->running, each new ruleset is enabled+synced; on failure, only
 * what THIS rebuild attempt already built is torn down (matching Go's
 * buildSubscriptionRuleSetsLocked, which disables what it just built and
 * returns the error without trying to restore any older state -- that's
 * the caller's job, see mt_app_add_subscription/etc.'s rollback). */
static mt_err_t rebuild_subscription_rulesets(mt_app_t *app) {
    free_subscription_rulesets(app);

    mt_ruleset_deps_t rdeps = ruleset_deps(app);
    for (size_t i = 0; i < app->cfg->n_subscriptions; i++) {
        mt_group_t *synth = mt_sub_runtime_group(app->cfg->subscriptions[i]);
        if (!synth) {
            free_subscription_rulesets(app);
            return MT_ERR_NOMEM;
        }
        mt_ruleset_t *rs = mt_ruleset_new(synth, &rdeps);
        if (!rs) {
            mt_group_free(synth);
            free_subscription_rulesets(app);
            return MT_ERR_NOMEM;
        }
        if (sub_rulesets_push(app, rs, synth) != MT_OK) {
            mt_ruleset_free(rs);
            mt_group_free(synth);
            free_subscription_rulesets(app);
            return MT_ERR_NOMEM;
        }

        if (app->running) {
            mt_err_t err = mt_ruleset_enable(rs);
            if (err == MT_OK) { err = mt_ruleset_sync(rs, app->cache, (int64_t)time(NULL)); }
            if (err != MT_OK) {
                free_subscription_rulesets(app);
                return err;
            }
        }
    }
    return MT_OK;
}

static void republish_or_log(mt_app_t *app) {
    mt_err_t err = mt_app_republish_dns_snapshot(app);
    if (err != MT_OK) { MT_ERROR("failed to republish DNS-matching snapshot: %s", mt_err_str(err)); }
}

mt_err_t mt_app_republish_dns_snapshot(mt_app_t *app) {
    if (!app->pipeline) { return MT_OK; }
    mt_ruleset_snapshot_t *snap = mt_ruleset_snapshot_build(app->cfg);
    if (!snap) { return MT_ERR_NOMEM; }
    mt_dns_pipeline_set_snapshot(app->pipeline, snap);
    return MT_OK;
}

mt_app_t *mt_app_create(const mt_app_deps_t *deps) {
    mt_app_t *app = calloc(1, sizeof(*app));
    if (!app) { return NULL; }
    app->cfg = deps->cfg;
    if (mt_profiles_normalize(app->cfg) != MT_OK) { mt_app_destroy(app); return NULL; }
    if (mt_strset(&app->ipset_prefix, deps->cfg->app.netfilter.ipset.table_prefix) != MT_OK ||
        mt_strset(&app->chain_prefix, deps->cfg->app.netfilter.iptables.chain_prefix) != MT_OK) {
        mt_app_destroy(app);
        return NULL;
    }
    app->cache = deps->cache;
    app->ipt4 = deps->ipt4;
    app->ipt6 = deps->ipt6;
    app->rtnl = deps->rtnl;
    app->pipeline = deps->pipeline;
    app->start_idx = deps->start_idx;

    mt_ruleset_deps_t rdeps = ruleset_deps(app);
    for (size_t i = 0; i < app->cfg->n_groups; i++) {
        mt_ruleset_t *rs = mt_ruleset_new(app->cfg->groups[i], &rdeps);
        if (!rs || rulesets_push(app, rs) != MT_OK) {
            mt_ruleset_free(rs);
            mt_app_destroy(app);
            return NULL;
        }
    }
    for (size_t i = 0; i < app->cfg->n_subscriptions; i++) {
        app->cfg->subscriptions[i]->revision = ++app->next_sub_revision;
        app->cfg->subscriptions[i]->sync_pending = false;
    }
    /* Mirrors Go's LoadConfig calling syncSubscriptionRuleSetsLocked()
     * unconditionally at the end -- running is always false here (set
     * later via mt_app_set_running), so this only builds raw rulesets,
     * matching addGroupLocked's own enable/sync skip while !a.enabled. */
    if (rebuild_subscription_rulesets(app) != MT_OK) {
        mt_app_destroy(app);
        return NULL;
    }
    return app;
}

void mt_app_destroy(mt_app_t *app) {
    if (!app) { return; }
    /* Nothing below is safe while a rebuild can still be reading the
     * ruleset registry. */
    mt_app_stop_netfilter_committer(app);
    for (size_t i = 0; i < app->n_rulesets; i++) {
        mt_ruleset_disable(app->rulesets[i]);
        mt_ruleset_free(app->rulesets[i]);
    }
    free(app->rulesets);
    free_subscription_rulesets(app);
    free(app->sub_rulesets);
    free(app->sub_synth_groups);
    free(app->ipset_prefix);
    free(app->chain_prefix);
    free(app);
}

void mt_app_set_running(mt_app_t *app, bool running) {
    app->running = running;
}

size_t mt_app_user_group_count(const mt_app_t *app) {
    return app->n_rulesets;
}

mt_ruleset_t *mt_app_user_group_at(const mt_app_t *app, size_t idx) {
    return idx < app->n_rulesets ? app->rulesets[idx] : NULL;
}

mt_ruleset_t *mt_app_find_group_by_id(const mt_app_t *app, mt_id_t id) {
    for (size_t i = 0; i < app->n_rulesets; i++) {
        if (mt_id_equal(mt_ruleset_group(app->rulesets[i])->id, id)) { return app->rulesets[i]; }
    }
    return NULL;
}

static mt_err_t validate_group_rule_ids(const mt_group_t *group) {
    mt_lookup_t ids = {0}; mt_err_t err = MT_OK;
    for (size_t i = 0; i < group->n_rules; i++) {
        bool inserted;
        err = mt_lookup_put(&ids, group->rules[i]->id.b, sizeof(group->rules[i]->id.b), i, &inserted);
        if (err != MT_OK || !inserted) { if (err == MT_OK) { err = MT_ERR_INVAL; } break; }
    }
    mt_lookup_clear(&ids); return err;
}

static bool same_text(const char *a, const char *b) { return strcmp(a ? a : "", b ? b : "") == 0; }

static bool same_group(const mt_group_t *a, const mt_group_t *b) {
    if (!mt_id_equal(a->id, b->id) || a->enable != b->enable || a->priority != b->priority ||
        a->n_rules != b->n_rules || !same_text(a->profile, b->profile) ||
        !same_text(a->name, b->name) || !same_text(a->iface, b->iface) || !same_text(a->color, b->color)) { return false; }
    for (size_t i = 0; i < a->n_rules; i++) {
        const mt_rule_t *x = a->rules[i], *y = b->rules[i];
        if (!mt_id_equal(x->id, y->id) || x->enable != y->enable || !same_text(x->name, y->name) ||
            !same_text(x->type, y->type) || !same_text(x->rule, y->rule)) { return false; }
    }
    return true;
}

/* Validate/stage the whole request before altering live state. A single
 * snapshot publication replaces the old clear + N independently-publishing
 * adds. If order is unchanged, identical groups keep their live rulesets and
 * ipsets; reorder deliberately rebuilds all, preserving ordering semantics.
 * Netfilter side effects cannot be transactional, so rollback is best effort
 * and any rollback failure is logged; live config stays the old one on error. */
mt_err_t mt_app_replace_groups(mt_app_t *app, mt_group_t **groups, size_t n) {
    app_nf_enter(app);
    mt_err_t err = MT_OK;
    mt_config_t replacement = {0};
    mt_lookup_t ids = {0};
    mt_ruleset_t **next = n ? calloc(n, sizeof(*next)) : NULL;
    bool *reused = n ? calloc(n, sizeof(*reused)) : NULL;
    size_t old_n = app->n_rulesets;
    bool *was_enabled = old_n ? calloc(old_n, sizeof(*was_enabled)) : NULL;
    mt_ruleset_snapshot_t *snapshot = NULL;
    bool touched = false;
    if ((n && (!next || !reused)) || (old_n && !was_enabled)) { err = MT_ERR_NOMEM; goto done; }
    bool same_order = n == old_n;
    for (size_t i = 0; same_order && i < n; i++) {
        same_order = mt_id_equal(groups[i]->id, app->cfg->groups[i]->id);
    }
    for (size_t i = 0; i < n; i++) {
        bool inserted;
        err = mt_lookup_put(&ids, groups[i]->id.b, sizeof(groups[i]->id.b), i, &inserted);
        if (err == MT_OK && !inserted) { err = MT_ERR_EXIST; }
        if (err == MT_OK) { err = validate_group_rule_ids(groups[i]); }
        if (err == MT_OK) { err = mt_route_normalize(app->cfg, groups[i]->profile, &groups[i]->iface); }
        if (err != MT_OK) { goto done; }
        err = mt_config_add_group(&replacement, groups[i]);
        if (err != MT_OK) { goto done; }
        groups[i] = NULL; /* ownership transferred; geometric model capacity */
        if (same_order && same_group(replacement.groups[i], app->cfg->groups[i])) {
            mt_group_free(replacement.groups[i]);
            replacement.groups[i] = app->cfg->groups[i];
            next[i] = app->rulesets[i]; reused[i] = true;
        } else {
            mt_ruleset_deps_t deps = ruleset_deps(app);
            next[i] = mt_ruleset_new(replacement.groups[i], &deps);
            if (!next[i]) { err = MT_ERR_NOMEM; goto done; }
        }
    }
    if (app->pipeline) {
        mt_config_t view = *app->cfg;
        view.groups = replacement.groups; view.n_groups = n;
        snapshot = mt_ruleset_snapshot_build(&view);
        if (!snapshot) { err = MT_ERR_NOMEM; goto done; }
    }
    for (size_t i = 0; i < old_n; i++) { was_enabled[i] = mt_ruleset_runtime_enabled(app->rulesets[i]); }
    touched = true;
    for (size_t i = 0; i < old_n; i++) {
        if (same_order && reused[i]) { continue; }
        err = mt_ruleset_disable(app->rulesets[i]);
        if (err != MT_OK) { goto done; }
    }
    if (app->running) {
        for (size_t i = 0; i < n; i++) {
            if (reused[i]) { continue; }
            err = mt_ruleset_enable(next[i]);
            if (err == MT_OK) { err = mt_ruleset_sync(next[i], app->cache, (int64_t)time(NULL)); }
            if (err != MT_OK) { goto done; }
        }
    }
    for (size_t i = 0; i < old_n; i++) {
        if (same_order && reused[i]) { continue; }
        mt_ruleset_free(app->rulesets[i]); mt_group_free(app->cfg->groups[i]);
    }
    free(app->rulesets); free(app->cfg->groups);
    app->rulesets = next; next = NULL; app->n_rulesets = n; app->cap_rulesets = n;
    app->cfg->groups = replacement.groups; app->cfg->n_groups = n;
    replacement.groups = NULL; replacement.n_groups = 0;
    if (snapshot) { mt_dns_pipeline_set_snapshot(app->pipeline, snapshot); snapshot = NULL; }

done:
    if (next) {
        for (size_t i = 0; i < n; i++) {
            if (reused && reused[i]) { continue; }
            if (touched && next[i]) { mt_ruleset_disable(next[i]); }
            mt_ruleset_free(next[i]);
        }
        free(next);
    }
    if (err != MT_OK && touched) {
        for (size_t i = 0; i < old_n; i++) {
            if (!was_enabled[i]) { continue; }
            mt_err_t rollback = mt_ruleset_enable(app->rulesets[i]);
            if (rollback == MT_OK) { rollback = mt_ruleset_sync(app->rulesets[i], app->cache, (int64_t)time(NULL)); }
            if (rollback != MT_OK) { MT_ERROR("failed to restore group after bulk apply: %s", mt_err_str(rollback)); }
        }
    }
    for (size_t i = 0; i < replacement.n_groups; i++) {
        if (!(reused && reused[i])) { mt_group_free(replacement.groups[i]); }
    }
    free(replacement.groups);
    for (size_t i = 0; i < n; i++) { mt_group_free(groups[i]); }
    free(groups); free(reused); free(was_enabled);
    mt_lookup_clear(&ids); mt_ruleset_snapshot_free(snapshot);
    app_nf_leave(app); return err;
}

static mt_err_t mt_app_add_group_unlocked(mt_app_t *app, mt_group_t *group) {
    for (size_t i = 0; i < app->cfg->n_groups; i++) {
        if (mt_id_equal(app->cfg->groups[i]->id, group->id)) {
            mt_group_free(group);
            return MT_ERR_EXIST;
        }
    }
    mt_err_t unique_err = mt_route_normalize(app->cfg, group->profile, &group->iface);
    if (unique_err == MT_OK) { unique_err = validate_group_rule_ids(group); }
    if (unique_err != MT_OK) { mt_group_free(group); return unique_err; }

    mt_err_t err = mt_config_add_group(app->cfg, group);
    if (err != MT_OK) {
        mt_group_free(group);
        return err;
    }
    size_t group_idx = app->cfg->n_groups - 1;

    mt_ruleset_deps_t rdeps = ruleset_deps(app);
    mt_ruleset_t *rs = mt_ruleset_new(app->cfg->groups[group_idx], &rdeps);
    if (!rs) {
        mt_config_remove_group_by_index(app->cfg, group_idx);
        return MT_ERR_NOMEM;
    }
    err = rulesets_push(app, rs);
    if (err != MT_OK) {
        mt_ruleset_free(rs);
        mt_config_remove_group_by_index(app->cfg, group_idx);
        return err;
    }

    if (app->running) {
        err = mt_ruleset_enable(rs);
        if (err == MT_OK) { err = mt_ruleset_sync(rs, app->cache, (int64_t)time(NULL)); }
        if (err != MT_OK) {
            mt_ruleset_disable(rs);
            rulesets_remove_at(app, app->n_rulesets - 1);
            mt_ruleset_free(rs);
            mt_config_remove_group_by_index(app->cfg, group_idx);
            return err;
        }
    }
    republish_or_log(app);
    return MT_OK;
}

mt_err_t mt_app_add_group(mt_app_t *app, mt_group_t *group) {
    app_nf_enter(app);
    mt_err_t r = mt_app_add_group_unlocked(app, group);
    app_nf_leave(app);
    return r;
}

static mt_err_t mt_app_sync_group_unlocked(mt_app_t *app, mt_ruleset_t *rs) {
    return mt_ruleset_sync(rs, app->cache, (int64_t)time(NULL));
}

mt_err_t mt_app_sync_group(mt_app_t *app, mt_ruleset_t *rs) {
    app_nf_enter(app);
    mt_err_t r = mt_app_sync_group_unlocked(app, rs);
    app_nf_leave(app);
    return r;
}

static void mt_app_clear_groups_unlocked(mt_app_t *app) {
    for (size_t i = 0; i < app->n_rulesets; i++) {
        mt_ruleset_disable(app->rulesets[i]);
        mt_ruleset_free(app->rulesets[i]);
    }
    app->n_rulesets = 0;
    mt_config_clear_groups(app->cfg);
    republish_or_log(app);
}

void mt_app_clear_groups(mt_app_t *app) {
    app_nf_enter(app);
    mt_app_clear_groups_unlocked(app);
    app_nf_leave(app);
}

static void mt_app_remove_group_by_index_unlocked(mt_app_t *app, size_t idx) {
    if (idx >= app->n_rulesets) { return; }
    mt_ruleset_free(app->rulesets[idx]);
    rulesets_remove_at(app, idx);
    mt_config_remove_group_by_index(app->cfg, idx);
    republish_or_log(app);
}

void mt_app_remove_group_by_index(mt_app_t *app, size_t idx) {
    app_nf_enter(app);
    mt_app_remove_group_by_index_unlocked(app, idx);
    app_nf_leave(app);
}

static bool mt_app_remove_group_by_id_unlocked(mt_app_t *app, mt_id_t id) {
    for (size_t i = 0; i < app->n_rulesets; i++) {
        if (mt_id_equal(mt_ruleset_group(app->rulesets[i])->id, id)) {
            mt_app_remove_group_by_index(app, i);
            return true;
        }
    }
    return false;
}

bool mt_app_remove_group_by_id(mt_app_t *app, mt_id_t id) {
    app_nf_enter(app);
    bool r = mt_app_remove_group_by_id_unlocked(app, id);
    app_nf_leave(app);
    return r;
}

/* ---- subscriptions ------------------------------------------------------------ */

size_t mt_app_subscription_count(const mt_app_t *app) {
    return app->cfg->n_subscriptions;
}

const mt_subscription_t *mt_app_subscription_at(const mt_app_t *app, size_t idx) {
    return idx < app->cfg->n_subscriptions ? app->cfg->subscriptions[idx] : NULL;
}

const mt_subscription_t *mt_app_find_subscription_by_id(const mt_app_t *app, mt_id_t id) {
    for (size_t i = 0; i < app->cfg->n_subscriptions; i++) {
        if (mt_id_equal(app->cfg->subscriptions[i]->id, id)) { return app->cfg->subscriptions[i]; }
    }
    return NULL;
}

size_t mt_app_subscription_ruleset_count(const mt_app_t *app) {
    return app->n_sub_rulesets;
}

mt_ruleset_t *mt_app_subscription_ruleset_at(const mt_app_t *app, size_t idx) {
    return idx < app->n_sub_rulesets ? app->sub_rulesets[idx] : NULL;
}

mt_ruleset_t *mt_app_find_subscription_ruleset_by_id(const mt_app_t *app, mt_id_t id) {
    for (size_t i = 0; i < app->n_sub_rulesets; i++) {
        if (mt_id_equal(mt_ruleset_group(app->sub_rulesets[i])->id, id)) { return app->sub_rulesets[i]; }
    }
    return NULL;
}

static mt_err_t mt_app_add_subscription_unlocked(mt_app_t *app, mt_subscription_t *sub) {
    mt_err_t route_err = mt_route_normalize(app->cfg, sub->profile, &sub->iface);
    if (route_err != MT_OK) { mt_subscription_free(sub); return route_err; }
    for (size_t i = 0; i < app->cfg->n_subscriptions; i++) {
        if (mt_id_equal(app->cfg->subscriptions[i]->id, sub->id)) {
            mt_subscription_free(sub);
            return MT_ERR_EXIST;
        }
    }
    sub->revision = ++app->next_sub_revision;
    sub->sync_pending = false;
    mt_err_t err = mt_config_add_subscription(app->cfg, sub);
    if (err != MT_OK) {
        mt_subscription_free(sub);
        return err;
    }
    size_t idx = app->cfg->n_subscriptions - 1;

    err = rebuild_subscription_rulesets(app);
    if (err != MT_OK) {
        mt_config_remove_subscription_by_index(app->cfg, idx);
        mt_err_t rollback_err = rebuild_subscription_rulesets(app);
        if (rollback_err != MT_OK) {
            MT_ERROR("failed to rollback subscription rulesets: %s", mt_err_str(rollback_err));
        }
        return err;
    }
    republish_or_log(app);
    return MT_OK;
}

mt_err_t mt_app_add_subscription(mt_app_t *app, mt_subscription_t *sub) {
    app_nf_enter(app);
    mt_err_t r = mt_app_add_subscription_unlocked(app, sub);
    app_nf_leave(app);
    return r;
}

static mt_err_t mt_app_replace_subscriptions_unlocked(mt_app_t *app, mt_subscription_t **subs, size_t n) {
    /* Use the model allocator: an exactly-n allocation is incompatible
     * with the geometric capacity assumed by mt_config_add_subscription. */
    mt_config_t replacement = {0};
    for (size_t i = 0; i < n; i++) {
        mt_err_t reserve_err = mt_route_normalize(app->cfg, subs[i]->profile, &subs[i]->iface);
        if (reserve_err == MT_OK) { reserve_err = mt_config_add_subscription(&replacement, subs[i]); }
        if (reserve_err != MT_OK) {
            for (size_t j = i; j < n; j++) { mt_subscription_free(subs[j]); }
            free(subs);
            mt_config_clear_subscriptions(&replacement);
            return reserve_err;
        }
    }
    mt_subscription_t **new_arr = replacement.subscriptions;
    free(subs); /* array shell only; elements moved into new_arr */

    mt_subscription_t **old_arr = app->cfg->subscriptions;
    size_t old_n = app->cfg->n_subscriptions;
    app->cfg->subscriptions = new_arr;
    app->cfg->n_subscriptions = n;
    for (size_t i = 0; i < n; i++) {
        new_arr[i]->revision = ++app->next_sub_revision;
        new_arr[i]->sync_pending = false;
    }

    mt_err_t err = rebuild_subscription_rulesets(app);
    if (err != MT_OK) {
        for (size_t i = 0; i < n; i++) { mt_subscription_free(new_arr[i]); }
        free(new_arr);
        app->cfg->subscriptions = old_arr;
        app->cfg->n_subscriptions = old_n;
        mt_err_t rollback_err = rebuild_subscription_rulesets(app);
        if (rollback_err != MT_OK) {
            MT_ERROR("failed to rollback subscription rulesets: %s", mt_err_str(rollback_err));
        }
        return err;
    }

    for (size_t i = 0; i < old_n; i++) { mt_subscription_free(old_arr[i]); }
    free(old_arr);
    republish_or_log(app);
    return MT_OK;
}

mt_err_t mt_app_replace_subscriptions(mt_app_t *app, mt_subscription_t **subs, size_t n) {
    app_nf_enter(app);
    mt_err_t r = mt_app_replace_subscriptions_unlocked(app, subs, n);
    app_nf_leave(app);
    return r;
}

static mt_err_t mt_app_remove_subscription_by_id_unlocked(mt_app_t *app, mt_id_t id, bool *out_found) {
    size_t idx = app->cfg->n_subscriptions;
    for (size_t i = 0; i < app->cfg->n_subscriptions; i++) {
        if (mt_id_equal(app->cfg->subscriptions[i]->id, id)) {
            idx = i;
            break;
        }
    }
    if (idx == app->cfg->n_subscriptions) {
        *out_found = false;
        return MT_OK;
    }
    *out_found = true;

    mt_subscription_t *removed = app->cfg->subscriptions[idx];
    for (size_t i = idx; i + 1 < app->cfg->n_subscriptions; i++) {
        app->cfg->subscriptions[i] = app->cfg->subscriptions[i + 1];
    }
    app->cfg->n_subscriptions--;

    mt_err_t err = rebuild_subscription_rulesets(app);
    if (err != MT_OK) {
        /* Restore `removed` at its original index -- append then rotate
         * into place, matching Go's positional splice-back exactly. */
        mt_err_t insert_err = mt_config_add_subscription(app->cfg, removed);
        if (insert_err == MT_OK) {
            size_t last = app->cfg->n_subscriptions - 1;
            for (size_t i = last; i > idx; i--) {
                app->cfg->subscriptions[i] = app->cfg->subscriptions[i - 1];
            }
            app->cfg->subscriptions[idx] = removed;
        } else {
            MT_ERROR("failed to restore subscription after rollback: %s", mt_err_str(insert_err));
            mt_subscription_free(removed);
        }
        mt_err_t rollback_err = rebuild_subscription_rulesets(app);
        if (rollback_err != MT_OK) {
            MT_ERROR("failed to rollback subscription rulesets: %s", mt_err_str(rollback_err));
        }
        return err;
    }

    mt_subscription_free(removed);
    republish_or_log(app);
    return MT_OK;
}

mt_err_t mt_app_remove_subscription_by_id(mt_app_t *app, mt_id_t id, bool *out_found) {
    app_nf_enter(app);
    mt_err_t r = mt_app_remove_subscription_by_id_unlocked(app, id, out_found);
    app_nf_leave(app);
    return r;
}

/* ---- subscription sync (fetch-backed) ----------------------------------------- */

static mt_subscription_t *find_subscription_mut(mt_app_t *app, mt_id_t id) {
    for (size_t i = 0; i < app->cfg->n_subscriptions; i++) {
        if (mt_id_equal(app->cfg->subscriptions[i]->id, id)) { return app->cfg->subscriptions[i]; }
    }
    return NULL;
}

static void free_sub_rule_array(mt_sub_rule_t **rules, size_t n) {
    for (size_t i = 0; i < n; i++) { mt_sub_rule_free(rules[i]); }
    free(rules);
}

static mt_err_t apply_subscription_rules(mt_app_t *app, mt_id_t id,
                                         int64_t now_unix, const char *fetch_url,
                                         mt_sub_rule_t **refreshed, size_t n_refreshed,
                                         bool *out_changed) {
    *out_changed = false;
    mt_subscription_t *sub = find_subscription_mut(app, id);
    if (!sub) { free_sub_rule_array(refreshed, n_refreshed); return MT_ERR_NOENT; }
    mt_err_t rerr = mt_sub_reconcile_rules(refreshed, n_refreshed, sub->rules, sub->n_rules);
    if (rerr != MT_OK) { free_sub_rule_array(refreshed, n_refreshed); return rerr; }
    bool rules_changed = !mt_sub_same_rules(sub->rules, sub->n_rules, refreshed, n_refreshed);

    bool url_changed = strcmp(sub->url ? sub->url : "", fetch_url) != 0;
    char *new_url = NULL;
    if (mt_strset(&new_url, fetch_url) != MT_OK) {
        free_sub_rule_array(refreshed, n_refreshed);
        return MT_ERR_NOMEM;
    }
    char *prev_url = sub->url;
    uint32_t prev_last_check = sub->last_check;
    uint32_t prev_last_update = sub->last_update;
    mt_sub_rule_t **prev_rules = sub->rules;
    size_t prev_n_rules = sub->n_rules;

    sub->url = new_url;
    sub->last_check = (uint32_t)now_unix;

    if (rules_changed) {
        sub->rules = refreshed;
        sub->n_rules = n_refreshed;
        sub->last_update = (uint32_t)now_unix;

        mt_err_t err = rebuild_subscription_rulesets(app);
        if (err != MT_OK) {
            free_sub_rule_array(sub->rules, sub->n_rules);
            free(sub->url);
            sub->url = prev_url;
            sub->last_check = prev_last_check;
            sub->last_update = prev_last_update;
            sub->rules = prev_rules;
            sub->n_rules = prev_n_rules;
            mt_err_t rollback_err = rebuild_subscription_rulesets(app);
            if (rollback_err != MT_OK) {
                MT_ERROR("failed to rollback subscription rulesets: %s", mt_err_str(rollback_err));
            }
            *out_changed = true;
            return err;
        }
        free_sub_rule_array(prev_rules, prev_n_rules);
        republish_or_log(app);
    } else {
        free_sub_rule_array(refreshed, n_refreshed);
    }
    free(prev_url);

    sub->revision = ++app->next_sub_revision;
    *out_changed = url_changed || rules_changed;
    return MT_OK;
}

/* Blocking compatibility path for embeddings; daemon callbacks use detached
 * worker results instead. apply_subscription_rules always takes ownership. */
static mt_err_t apply_subscription_body(mt_app_t *app, mt_id_t id, int64_t now,
                                        const char *url, const char *body, bool *changed) {
    mt_sub_rule_t **rules = NULL; size_t n = 0;
    mt_err_t err = mt_sub_parse_rules(body, &rules, &n);
    if (err != MT_OK) { return err; }
    return apply_subscription_rules(app, id, now, url, rules, n, changed);
}

static mt_err_t mt_app_sync_subscription_by_id_unlocked(mt_app_t *app, mt_id_t id,
                                                        int64_t now_unix,
                                                        const char *url_override,
                                                        bool *out_changed) {
    *out_changed = false;
    mt_subscription_t *sub = find_subscription_mut(app, id);
    if (!sub) { return MT_ERR_NOENT; }
    if (sub->sync_pending) { return MT_ERR_STATE; }
    const char *url = (url_override && url_override[0]) ? url_override : sub->url;
    if (!url || !url[0]) { return MT_ERR_INVAL; }
    char *body = NULL;
    size_t len = 0;
    mt_err_t err = mt_sub_fetch_list(url, &body, &len);
    if (err != MT_OK) { return MT_ERR_UPSTREAM; }
    err = apply_subscription_body(app, id, now_unix, url, body, out_changed);
    free(body);
    return err;
}

mt_err_t mt_app_sync_subscription_by_id(mt_app_t *app, mt_id_t id, int64_t now_unix,
                                        const char *url_override, bool *out_changed) {
    app_nf_enter(app);
    mt_err_t r = mt_app_sync_subscription_by_id_unlocked(app, id, now_unix, url_override,
                                                         out_changed);
    app_nf_leave(app);
    return r;
}

static mt_err_t mt_app_sync_due_subscriptions_unlocked(mt_app_t *app, int64_t now_unix, bool *out_any_changed) {
    *out_any_changed = false;

    mt_id_t *due_ids = NULL;
    size_t n_due = 0, cap_due = 0;
    for (size_t i = 0; i < app->cfg->n_subscriptions; i++) {
        if (app->cfg->subscriptions[i]->sync_pending ||
            !mt_sub_is_due(app->cfg->subscriptions[i], now_unix)) { continue; }
        if (n_due == cap_due) {
            size_t new_cap = cap_due ? cap_due * 2 : 8;
            mt_id_t *na = realloc(due_ids, new_cap * sizeof(*na));
            if (!na) {
                free(due_ids);
                return MT_ERR_NOMEM;
            }
            due_ids = na;
            cap_due = new_cap;
        }
        due_ids[n_due++] = app->cfg->subscriptions[i]->id;
    }
    if (n_due == 0) {
        free(due_ids);
        return MT_OK;
    }

    typedef struct sub_plan {
        mt_id_t id;
        mt_sub_rule_t **refreshed;
        size_t n_refreshed;
        bool changed;
    } sub_plan_t;
    sub_plan_t *plans = calloc(n_due, sizeof(*plans));
    if (!plans) {
        free(due_ids);
        return MT_ERR_NOMEM;
    }
    size_t n_plans = 0;

    for (size_t i = 0; i < n_due; i++) {
        const mt_subscription_t *sub = find_subscription_mut(app, due_ids[i]);
        if (!sub || !sub->url || sub->url[0] == '\0') { continue; }

        char *body = NULL;
        size_t body_len = 0;
        mt_err_t ferr = mt_sub_fetch_list(sub->url, &body, &body_len);
        if (ferr != MT_OK) {
            char idbuf[MT_ID_STR_LEN];
            mt_id_format(sub->id, idbuf);
            MT_ERROR("failed to fetch subscription %s: %s", idbuf, mt_err_str(ferr));
            continue;
        }

        mt_sub_rule_t **refreshed = NULL;
        size_t n_refreshed = 0;
        mt_err_t rerr = mt_sub_refresh_rules(body, sub->rules, sub->n_rules, &refreshed, &n_refreshed);
        free(body);
        if (rerr != MT_OK) { continue; }

        plans[n_plans].id = due_ids[i];
        plans[n_plans].refreshed = refreshed;
        plans[n_plans].n_refreshed = n_refreshed;
        plans[n_plans].changed = !mt_sub_same_rules(sub->rules, sub->n_rules, refreshed, n_refreshed);
        n_plans++;
    }
    free(due_ids);

    if (n_plans == 0) {
        free(plans);
        return MT_OK;
    }

    typedef struct rollback_entry {
        mt_subscription_t *sub;
        mt_sub_rule_t **rules;
        size_t n_rules;
        uint32_t last_check;
        uint32_t last_update;
        bool rules_replaced;
    } rollback_entry_t;
    rollback_entry_t *rollback = calloc(n_plans, sizeof(*rollback));
    if (!rollback) {
        for (size_t i = 0; i < n_plans; i++) { free_sub_rule_array(plans[i].refreshed, plans[i].n_refreshed); }
        free(plans);
        return MT_ERR_NOMEM;
    }
    size_t n_rollback = 0;
    bool any_changed = false;

    for (size_t i = 0; i < n_plans; i++) {
        mt_subscription_t *sub = find_subscription_mut(app, plans[i].id);
        if (!sub) {
            free_sub_rule_array(plans[i].refreshed, plans[i].n_refreshed);
            continue;
        }
        rollback[n_rollback].sub = sub;
        rollback[n_rollback].rules = sub->rules;
        rollback[n_rollback].n_rules = sub->n_rules;
        rollback[n_rollback].last_check = sub->last_check;
        rollback[n_rollback].last_update = sub->last_update;
        rollback[n_rollback].rules_replaced = plans[i].changed;
        n_rollback++;

        sub->last_check = (uint32_t)now_unix;
        if (plans[i].changed) {
            sub->rules = plans[i].refreshed;
            sub->n_rules = plans[i].n_refreshed;
            sub->last_update = (uint32_t)now_unix;
            any_changed = true;
        } else {
            free_sub_rule_array(plans[i].refreshed, plans[i].n_refreshed);
        }
    }
    free(plans);

    if (!any_changed) {
        free(rollback);
        return MT_OK;
    }

    mt_err_t err = rebuild_subscription_rulesets(app);
    if (err != MT_OK) {
        for (size_t i = 0; i < n_rollback; i++) {
            mt_subscription_t *sub = rollback[i].sub;
            if (rollback[i].rules_replaced) { free_sub_rule_array(sub->rules, sub->n_rules); }
            sub->rules = rollback[i].rules;
            sub->n_rules = rollback[i].n_rules;
            sub->last_check = rollback[i].last_check;
            sub->last_update = rollback[i].last_update;
        }
        free(rollback);
        mt_err_t rollback_err = rebuild_subscription_rulesets(app);
        if (rollback_err != MT_OK) {
            MT_ERROR("failed to rollback subscription rulesets: %s", mt_err_str(rollback_err));
        }
        *out_any_changed = true;
        return err;
    }

    /* Success: rollback[i].rules is each subscription's PRE-sync rules
     * array, already superseded by plans[i].refreshed for every entry
     * with rules_replaced == true (see the loop above). Must be freed
     * here -- unlike the rollback-on-failure branch, which frees the
     * NEW rules and restores these same old ones, this success path was
     * previously leaking them (found by the Phase 7 fault-injection
     * soak under ASan/LSan: repeated real syncs across a SIGHUP reload
     * leaked the exact size of a superseded rule array -- see
     * decisions.md D-36). */
    for (size_t i = 0; i < n_rollback; i++) {
        if (rollback[i].rules_replaced) {
            free_sub_rule_array(rollback[i].rules, rollback[i].n_rules);
        }
    }
    free(rollback);
    republish_or_log(app);
    *out_any_changed = true;
    return MT_OK;
}

mt_err_t mt_app_sync_due_subscriptions(mt_app_t *app, int64_t now_unix, bool *out_any_changed) {
    app_nf_enter(app);
    mt_err_t r = mt_app_sync_due_subscriptions_unlocked(app, now_unix, out_any_changed);
    app_nf_leave(app);
    return r;
}

typedef struct async_sync {
    mt_app_t *app;
    mt_id_t id;
    uint64_t revision;
    int64_t now;
    char *url;
    mt_app_sync_done_fn done;
    void *ud;
} async_sync_t;

static void subscription_fetched(void *ud, mt_err_t err, mt_sub_rule_t **rules, size_t n) {
    async_sync_t *job = ud;
    mt_subscription_t *sub = find_subscription_mut(job->app, job->id);
    bool changed = false;
    if (!sub) {
        err = MT_ERR_NOENT;
    } else if (sub->revision != job->revision) {
        err = MT_ERR_STATE; /* removed/replaced/edited while I/O was pending */
    } else {
        sub->sync_pending = false;
        if (err == MT_OK) {
            app_nf_enter(job->app);
            err = apply_subscription_rules(job->app, job->id, job->now,
                                            job->url, rules, n, &changed);
            rules = NULL; n = 0;
            app_nf_leave(job->app);
        }
    }
    mt_sub_rules_free(rules, n);
    job->done(job->ud, job->id, err, changed);
    free(job->url);
    free(job);
}

mt_err_t mt_app_sync_subscription_async(mt_app_t *app, mt_sub_fetcher_t *fetcher,
                                         mt_id_t id, int64_t now,
                                         const char *url_override,
                                         mt_app_sync_done_fn done, void *ud) {
    if (!done) { return MT_ERR_INVAL; }
    mt_subscription_t *sub = find_subscription_mut(app, id);
    if (!sub) { return MT_ERR_NOENT; }
    if (sub->sync_pending) { return MT_ERR_STATE; }
    const char *url = url_override && url_override[0] ? url_override : sub->url;
    if (!url || !url[0]) { return MT_ERR_INVAL; }
    async_sync_t *job = calloc(1, sizeof(*job));
    if (!job) { return MT_ERR_NOMEM; }
    job->url = strdup(url);
    if (!job->url) { free(job); return MT_ERR_NOMEM; }
    job->app = app; job->id = id; job->now = now;
    job->revision = sub->revision; job->done = done; job->ud = ud;
    mt_err_t err = mt_sub_fetcher_submit_rules(fetcher, url, subscription_fetched, job);
    if (err != MT_OK) { free(job->url); free(job); return err; }
    sub->sync_pending = true;
    return MT_OK;
}

mt_err_t mt_app_sync_due_subscriptions_async(mt_app_t *app, mt_sub_fetcher_t *fetcher,
                                             int64_t now, mt_app_sync_done_fn done,
                                             void *ud) {
    mt_err_t result = MT_OK;
    size_t n = app->cfg->n_subscriptions;
    if (n == 0) { return MT_OK; }
    size_t start = app->next_due_index % n;
    for (size_t offset = 0; offset < n; offset++) {
        size_t i = (start + offset) % n;
        mt_subscription_t *sub = app->cfg->subscriptions[i];
        if (sub->sync_pending || !mt_sub_is_due(sub, now)) { continue; }
        mt_err_t err = mt_app_sync_subscription_async(app, fetcher, sub->id,
                                                      now, NULL, done, ud);
        if (err != MT_OK) { result = err; }
        /* Resume with the rejected entry next tick. Failed early URLs
         * must not starve later subscriptions when the queue fills. */
        app->next_due_index = err == MT_ERR_LIMIT ? i : (i + 1) % n;
        if (err == MT_ERR_LIMIT) { break; }
    }
    return result;
}

/* ---- interfaces -------------------------------------------------------------- */

/* Mirrors Go's constant.IgnoredInterfaces: empty on the default/OpenWrt
 * platform, a fixed Keenetic virtual-interface list under entware_kn (see
 * src/backend/constant/iface-ignore_entware_kn.go) -- only applied when
 * !show_all_interfaces, exactly like Go's filterManaged. This was
 * scaffolded but never wired in Phase 6 (app.h's mt_app_list_interfaces
 * comment deferred it to "Phase 8", which never circled back); found and
 * fixed during Phase 9's parity checklist review. */
#ifdef MT_ENTWARE_KN
static const char *const MT_IGNORED_INTERFACES[] = {
    "ezcfg0",
    "ra0", "ra1", "ra2", "ra3", "ra4", "ra5", "ra6", "ra7",
    "ra8", "ra9", "ra10", "ra11", "ra12", "ra13", "ra14", "ra15",
};
#define MT_IGNORED_INTERFACES_N (sizeof(MT_IGNORED_INTERFACES) / sizeof(MT_IGNORED_INTERFACES[0]))

static bool iface_is_ignored(const char *name) {
    for (size_t i = 0; i < MT_IGNORED_INTERFACES_N; i++) {
        if (strcmp(MT_IGNORED_INTERFACES[i], name) == 0) { return true; }
    }
    return false;
}
#else
static bool iface_is_ignored(const char *name) {
    (void)name;
    return false;
}
#endif

bool mt_iface_is_ignored_for_test(const char *name) { return iface_is_ignored(name); }

mt_err_t mt_app_list_interfaces(const mt_app_t *app, mt_iface_info_t **out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;

    struct ifaddrs *ifap;
    if (getifaddrs(&ifap) != 0) { return mt_err_from_errno(errno); }

    bool show_all = app->cfg->app.show_all_interfaces;
    size_t cap = 0;
    size_t n = 0;
    mt_iface_info_t *arr = NULL;

    for (struct ifaddrs *p = ifap; p != NULL; p = p->ifa_next) {
        bool already = false;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(arr[i].id, p->ifa_name) == 0) {
                already = true;
                break;
            }
        }
        if (already) { continue; }

        if (!show_all && ((p->ifa_flags & IFF_POINTOPOINT) == 0 || iface_is_ignored(p->ifa_name))) { continue; }

        if (n == cap) {
            size_t new_cap = cap ? cap * 2 : 8;
            mt_iface_info_t *na = realloc(arr, new_cap * sizeof(*na));
            if (!na) {
                free(arr);
                freeifaddrs(ifap);
                return MT_ERR_NOMEM;
            }
            arr = na;
            cap = new_cap;
        }
        memset(&arr[n], 0, sizeof(arr[n]));
        snprintf(arr[n].id, sizeof(arr[n].id), "%s", p->ifa_name);
        n++;
    }
    freeifaddrs(ifap);

    /* Friendly names, when the platform can supply them (Keenetic RCI;
     * a no-op empty set elsewhere). Go's interfaces.List logged the
     * error at debug level and carried on with unnamed interfaces --
     * an unreachable RCI must never fail the interface list itself. */
    mt_kn_aliases_t aliases = {0};
    mt_err_t alias_err = mt_kn_get_iface_aliases(&aliases);
    if (alias_err != MT_OK) {
        MT_DEBUG("failed to load interface aliases: %s", mt_err_str(alias_err));
    } else {
        for (size_t i = 0; i < n; i++) {
            const char *alias = mt_kn_aliases_lookup(&aliases, arr[i].id);
            if (alias) { snprintf(arr[i].name, sizeof(arr[i].name), "%s", alias); }
        }
    }
    mt_kn_aliases_free(&aliases);

    *out = arr;
    *out_n = n;
    return MT_OK;
}

/* ---- config save / iptables commit -------------------------------------------- */

/* A reload must publish definitions and their consumers together: retaining
 * an old subscription while discarding its profile on a failed kernel update
 * would poison the next save. Stage all models/rulesets/snapshot first, keep
 * the previous registry alive, and restore it on any apply failure. */
static mt_err_t clone_reload_group(mt_config_t *dst, const mt_group_t *source)
{
    mt_group_t *g = mt_group_new();
    if (!g) { return MT_ERR_NOMEM; }
    g->id = source->id; g->enable = source->enable;
    g->priority = source->priority;
    mt_err_t err = mt_strset(&g->name, source->name);
    if (err == MT_OK) { err = mt_strset(&g->color, source->color); }
    if (err == MT_OK) { err = mt_strset(&g->iface, source->iface); }
    if (err == MT_OK) { err = mt_strset(&g->profile, source->profile); }
    for (size_t i = 0; err == MT_OK && i < source->n_rules; i++) {
        const mt_rule_t *r = source->rules[i];
        mt_rule_t *copy = mt_rule_new();
        if (!copy) { err = MT_ERR_NOMEM; break; }
        copy->id = r->id; copy->enable = r->enable;
        err = mt_strset(&copy->name, r->name);
        if (err == MT_OK) { err = mt_strset(&copy->type, r->type); }
        if (err == MT_OK) { err = mt_strset(&copy->rule, r->rule); }
        if (err == MT_OK) { err = mt_group_add_rule(g, copy); }
        if (err != MT_OK) { mt_rule_free(copy); }
    }
    if (err == MT_OK) { err = mt_config_add_group(dst, g); }
    if (err != MT_OK) { mt_group_free(g); }
    return err;
}

mt_err_t mt_app_reload_config(mt_app_t *app, const char *path) {
    mt_config_t next = {0};
    mt_err_t err = mt_app_config_clone(&next.app, &app->cfg->app);
    if (err == MT_OK) { err = mt_config_clone_profiles(&next, app->cfg); }
    if (err == MT_OK) { err = mt_config_load_file(&next, path); }
    if (err != MT_OK) { mt_config_clear(&next); return err; }
    if (!next.groups_present) {
        for (size_t i = 0; err == MT_OK && i < app->cfg->n_groups; i++) {
            err = clone_reload_group(&next, app->cfg->groups[i]);
        }
    }
    /* Absent subscriptions still CLEAR, as before; absent profiles overlay. */
    if (err == MT_OK) { err = mt_profiles_normalize(&next); }
    if (err != MT_OK) { mt_config_clear(&next); return err; }

    size_t ng = next.n_groups, ns = next.n_subscriptions;
    mt_ruleset_t **groups = ng ? calloc(ng, sizeof(*groups)) : NULL;
    mt_ruleset_t **subs = ns ? calloc(ns, sizeof(*subs)) : NULL;
    mt_group_t **synth = ns ? calloc(ns, sizeof(*synth)) : NULL;
    bool *old_groups_enabled = app->n_rulesets ? calloc(app->n_rulesets, sizeof(bool)) : NULL;
    bool *old_subs_enabled = app->n_sub_rulesets ? calloc(app->n_sub_rulesets, sizeof(bool)) : NULL;
    mt_ruleset_snapshot_t *snapshot = NULL;
    bool touched = false, swapped = false;
    mt_config_t old = {0};
    app_nf_enter(app);
    if ((ng && !groups) || (ns && (!subs || !synth)) ||
        (app->n_rulesets && !old_groups_enabled) || (app->n_sub_rulesets && !old_subs_enabled)) {
        err = MT_ERR_NOMEM; goto done;
    }
    mt_ruleset_deps_t deps = ruleset_deps(app); /* stable cfg address */
    for (size_t i = 0; i < ng; i++) {
        groups[i] = mt_ruleset_new(next.groups[i], &deps);
        if (!groups[i]) { err = MT_ERR_NOMEM; goto done; }
    }
    for (size_t i = 0; i < ns; i++) {
        synth[i] = mt_sub_runtime_group(next.subscriptions[i]);
        if (synth[i]) { subs[i] = mt_ruleset_new(synth[i], &deps); }
        if (!subs[i]) { err = MT_ERR_NOMEM; goto done; }
    }
    if (app->pipeline) {
        snapshot = mt_ruleset_snapshot_build(&next);
        if (!snapshot) { err = MT_ERR_NOMEM; goto done; }
    }
    for (size_t i = 0; i < app->n_rulesets; i++) {
        old_groups_enabled[i] = mt_ruleset_runtime_enabled(app->rulesets[i]);
    }
    for (size_t i = 0; i < app->n_sub_rulesets; i++) {
        old_subs_enabled[i] = mt_ruleset_runtime_enabled(app->sub_rulesets[i]);
    }
    touched = true;
    for (size_t i = 0; err == MT_OK && i < app->n_rulesets; i++) {
        err = mt_ruleset_disable(app->rulesets[i]);
    }
    for (size_t i = 0; err == MT_OK && i < app->n_sub_rulesets; i++) {
        err = mt_ruleset_disable(app->sub_rulesets[i]);
    }
    if (err != MT_OK) { goto done; }
    old = *app->cfg; *app->cfg = next; memset(&next, 0, sizeof(next)); swapped = true;
    for (size_t family = 0; app->running && err == MT_OK && family < 2; family++) {
        mt_ruleset_t **sets = family ? subs : groups;
        size_t count = family ? ns : ng;
        for (size_t i = 0; err == MT_OK && i < count; i++) {
            err = mt_ruleset_enable(sets[i]);
            if (err == MT_OK) { err = mt_ruleset_sync(sets[i], app->cache, (int64_t)time(NULL)); }
        }
    }
    if (err != MT_OK) { goto done; }
    for (size_t i = 0; i < app->n_rulesets; i++) { mt_ruleset_free(app->rulesets[i]); }
    for (size_t i = 0; i < app->n_sub_rulesets; i++) {
        mt_ruleset_free(app->sub_rulesets[i]); mt_group_free(app->sub_synth_groups[i]);
    }
    free(app->rulesets); free(app->sub_rulesets); free(app->sub_synth_groups);
    app->rulesets = groups; app->n_rulesets = ng; app->cap_rulesets = ng; groups = NULL;
    app->sub_rulesets = subs; app->n_sub_rulesets = ns; app->cap_sub_rulesets = ns; subs = NULL;
    app->sub_synth_groups = synth; synth = NULL;
    for (size_t i = 0; i < ns; i++) {
        app->cfg->subscriptions[i]->revision = ++app->next_sub_revision;
        app->cfg->subscriptions[i]->sync_pending = false;
    }
    if (app->pipeline) {
        uint32_t ttl = (uint32_t)(app->cfg->app.netfilter.ipset.additional_ttl / MT_DURATION_SEC);
        mt_dns_pipeline_set_additional_ttl(app->pipeline, ttl);
        mt_dns_pipeline_set_snapshot(app->pipeline, snapshot); snapshot = NULL;
    }
    mt_config_clear(&old);
    swapped = false; touched = false;
done:
    for (size_t i = 0; groups && i < ng; i++) {
        if (groups[i]) { mt_ruleset_disable(groups[i]); mt_ruleset_free(groups[i]); }
    }
    for (size_t i = 0; i < ns; i++) {
        if (subs && subs[i]) { mt_ruleset_disable(subs[i]); mt_ruleset_free(subs[i]); }
        if (synth) { mt_group_free(synth[i]); }
    }
    if (swapped) { next = *app->cfg; *app->cfg = old; }
    if (err != MT_OK && touched) {
        for (size_t family = 0; family < 2; family++) {
            mt_ruleset_t **sets = family ? app->sub_rulesets : app->rulesets;
            bool *enabled = family ? old_subs_enabled : old_groups_enabled;
            size_t count = family ? app->n_sub_rulesets : app->n_rulesets;
            for (size_t i = 0; i < count; i++) {
                if (!enabled[i]) { continue; }
                mt_err_t restore = mt_ruleset_enable(sets[i]);
                if (restore == MT_OK) { restore = mt_ruleset_sync(sets[i], app->cache, (int64_t)time(NULL)); }
                if (restore != MT_OK) { MT_ERROR("failed to restore routes after reload: %s", mt_err_str(restore)); }
            }
        }
    }
    free(groups); free(subs); free(synth); free(old_groups_enabled); free(old_subs_enabled);
    mt_ruleset_snapshot_free(snapshot); mt_config_clear(&next);
    app_nf_leave(app);
    return err;
}

mt_err_t mt_app_save_config(mt_app_t *app, const char *path, const char *version) {
    return mt_config_save_file(app->cfg, version, path);
}

void mt_app_set_port_remap(mt_app_t *app, mt_port_remap_t *remap) {
    app->port_remap = remap;
}

/* One pass of the rebuild, with the netfilter lock already held. */
static mt_err_t rebuild_netfilter_locked(mt_app_t *app, mt_cancel_t *cancel) {
    /* Take everything of ours out of the kernel before putting anything
     * back. What is left in the tables after an aborted write, or after
     * the firmware replaced them, is not something worth reasoning about
     * -- starting from "none of it is there" makes the result depend only
     * on the current group set. */
    if (mt_cancel_raised(cancel)) { return MT_ERR_CANCELED; }

    /* Cleanup must compile ONLY removals discovered in the current kernel
     * snapshot. Retained desired jumps/overrides from a previous attempt can
     * otherwise recreate a jump while its target is being deleted (MT_DNSOR),
     * poisoning every subsequent retry. Reset both families under nf_mu. */
    mt_ipt_reset_staged(app->ipt4);
    mt_ipt_reset_staged(app->ipt6);
    mt_err_t err = mt_netfilter_clean_iptables(app->ipt4, app->ipt6,
                                               app->chain_prefix);

    /* Delete registrations are temporary too, including after failed or
     * canceled cleanup. Keep the engines/transports/cancel token themselves:
     * groups and the DNS remap still borrow those exact objects. */
    mt_ipt_reset_staged(app->ipt4);
    mt_ipt_reset_staged(app->ipt6);
    if (err != MT_OK) { return err; }
    if (mt_cancel_raised(cancel)) { return MT_ERR_CANCELED; }

    err = mt_netfilter_register_base_chains(app->ipt4, app->ipt6);
    if (err != MT_OK) { return err; }

    err = mt_port_remap_prepare_iptables(app->port_remap);
    if (err != MT_OK) { return err; }

    for (size_t i = 0; i < app->n_rulesets; i++) {
        err = mt_ruleset_prepare_iptables(app->rulesets[i]);
        if (err != MT_OK) { return err; }
    }
    for (size_t i = 0; i < app->n_sub_rulesets; i++) {
        err = mt_ruleset_prepare_iptables(app->sub_rulesets[i]);
        if (err != MT_OK) { return err; }
    }

    if (mt_cancel_raised(cancel)) { return MT_ERR_CANCELED; }

    if (app->ipt4) {
        err = mt_ipt_commit(app->ipt4);
        if (err != MT_OK) { return err; }
    }
    if (app->ipt6) {
        err = mt_ipt_commit(app->ipt6);
        if (err != MT_OK) { return err; }
    }
    return MT_OK;
}

mt_err_t mt_app_rebuild_netfilter(mt_app_t *app, mt_cancel_t *cancel) {
    if (app->nf_mu_ready) { pthread_mutex_lock(&app->nf_mu); }

    /* Attached for exactly this pass. Leaving it on the engines would
     * also abort commits made by the API on the loop thread -- those are
     * synchronous, report their result to a caller who is waiting for it,
     * and have no business being cancelled by a netfilter.d event. Since
     * both threads only drive an engine while holding nf_mu, the token is
     * attached precisely while the committer owns them. */
    if (app->ipt4) { mt_ipt_set_cancel(app->ipt4, cancel); }
    if (app->ipt6) { mt_ipt_set_cancel(app->ipt6, cancel); }

    mt_err_t err = rebuild_netfilter_locked(app, cancel);

    if (app->ipt4) { mt_ipt_set_cancel(app->ipt4, NULL); }
    if (app->ipt6) { mt_ipt_set_cancel(app->ipt6, NULL); }

    if (app->nf_mu_ready) { pthread_mutex_unlock(&app->nf_mu); }
    return err;
}

#ifdef MT_ENTWARE_KN
static mt_err_t rebuild_netfilter_cb(void *ud, mt_cancel_t *cancel) {
    return mt_app_rebuild_netfilter(ud, cancel);
}
#endif

mt_err_t mt_app_start_netfilter_committer(mt_app_t *app) {
#ifndef MT_ENTWARE_KN
    (void)app;
    return MT_OK;
#else
    if (app->committer) { return MT_ERR_STATE; }

    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) { return MT_ERR_SYS; }
    bool mu_ok = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0 &&
                 pthread_mutex_init(&app->nf_mu, &attr) == 0;
    pthread_mutexattr_destroy(&attr);
    if (!mu_ok) { return MT_ERR_SYS; }

    app->committer = mt_nfcommit_new(rebuild_netfilter_cb, app);
    if (!app->committer) {
        pthread_mutex_destroy(&app->nf_mu);
        return MT_ERR_NOMEM;
    }

    /* Published before the thread starts so the first request coming in
     * over the webhook already takes the lock. */
    app->nf_mu_ready = true;

    mt_err_t err = mt_nfcommit_start(app->committer);
    if (err != MT_OK) {
        app->nf_mu_ready = false;
        mt_nfcommit_free(app->committer);
        app->committer = NULL;
        pthread_mutex_destroy(&app->nf_mu);
        return err;
    }
    MT_INFO("netfilter table committer started");
    return MT_OK;
#endif
}

void mt_app_stop_netfilter_committer(mt_app_t *app) {
    if (!app || !app->committer) { return; }

    /* Joins the thread, so no pass can still have the token attached to
     * an engine by the time this returns. */
    mt_nfcommit_free(app->committer);
    app->committer = NULL;

    app->nf_mu_ready = false;
    pthread_mutex_destroy(&app->nf_mu);
}

mt_err_t mt_app_force_commit_iptables(mt_app_t *app) {
    if (app->committer) {
        /* Nothing to wait for and nothing to report: the committer aborts
         * whatever it is writing and rebuilds the table from scratch. */
        mt_nfcommit_request(app->committer);
        return MT_OK;
    }

    app_nf_enter(app);
    mt_err_t err = MT_OK;
    if (app->ipt4) { err = mt_ipt_commit(app->ipt4); }
    if (err == MT_OK && app->ipt6) { err = mt_ipt_commit(app->ipt6); }
    app_nf_leave(app);
    return err;
}

/* ---- profiles ---- */

size_t mt_app_profile_count(const mt_app_t *app) { return app->cfg->n_profiles; }

const mt_profile_t *mt_app_profile_at(const mt_app_t *app, size_t index)
{
    return index < app->cfg->n_profiles ? app->cfg->profiles[index] : NULL;
}

mt_err_t mt_app_normalize_route(const mt_app_t *app, const char *profile, char **iface)
{
    return mt_route_normalize(app->cfg, profile, iface);
}

static mt_err_t reconfigure_profiles(mt_app_t *app)
{
    mt_err_t first = MT_OK;
    for (size_t i = 0; i < app->n_rulesets; i++) {
        mt_err_t err = mt_ruleset_reconfigure_profile(app->rulesets[i]);
        if (first == MT_OK) { first = err; }
    }
    for (size_t i = 0; i < app->n_sub_rulesets; i++) {
        mt_err_t err = mt_ruleset_reconfigure_profile(app->sub_rulesets[i]);
        if (first == MT_OK) { first = err; }
    }
    return first;
}

typedef struct primary_shadow {
    char **field;
    char *next;
} primary_shadow_t;

static mt_err_t stage_shadow(primary_shadow_t *shadows, size_t capacity, size_t *count,
                              const mt_config_t *view, const char *profile, char **iface)
{
    if (!profile || !*profile) { return MT_OK; }
    if (!shadows || *count >= capacity) { return MT_ERR_NOMEM; }
    const char *primary;
    mt_err_t err = mt_route_primary(view, profile, *iface, &primary);
    if (err != MT_OK) { return err; }
    char *next = strdup(primary);
    if (!next) { return MT_ERR_NOMEM; }
    shadows[*count].field = iface;
    shadows[*count].next = next;
    (*count)++;
    return MT_OK;
}

mt_err_t mt_app_replace_profiles(mt_app_t *app, mt_config_t *incoming,
                                char *message, size_t message_size)
{
    if (!app || !incoming || incoming == app->cfg) { return MT_ERR_INVAL; }
    app_nf_enter(app);
    mt_config_t view = *app->cfg;
    view.profiles = incoming->profiles; view.n_profiles = incoming->n_profiles;
    mt_err_t err = mt_profiles_validate(&view, message, message_size);
    if (err != MT_OK) { app_nf_leave(app); return err; }
    if (view.n_groups > SIZE_MAX - view.n_subscriptions ||
        view.n_groups + view.n_subscriptions > SIZE_MAX - app->n_sub_rulesets) {
        app_nf_leave(app);
        return MT_ERR_NOMEM;
    }
    size_t capacity = view.n_groups + view.n_subscriptions + app->n_sub_rulesets;
    primary_shadow_t *shadows = capacity ? calloc(capacity, sizeof(*shadows)) : NULL;
    if (capacity && !shadows) { app_nf_leave(app); return MT_ERR_NOMEM; }
    size_t count = 0;
    for (size_t i = 0; err == MT_OK && i < view.n_groups; i++) {
        err = stage_shadow(shadows, capacity, &count, &view, view.groups[i]->profile, &view.groups[i]->iface);
    }
    for (size_t i = 0; err == MT_OK && i < view.n_subscriptions; i++) {
        err = stage_shadow(shadows, capacity, &count, &view, view.subscriptions[i]->profile, &view.subscriptions[i]->iface);
    }
    for (size_t i = 0; err == MT_OK && i < app->n_sub_rulesets; i++) {
        mt_group_t *g = app->sub_synth_groups[i];
        err = stage_shadow(shadows, capacity, &count, &view, g->profile, &g->iface);
    }
    if (err == MT_OK) {
        mt_config_t old = {.profiles = app->cfg->profiles, .n_profiles = app->cfg->n_profiles};
        app->cfg->profiles = incoming->profiles; app->cfg->n_profiles = incoming->n_profiles;
        err = reconfigure_profiles(app);
        if (err != MT_OK) {
            app->cfg->profiles = old.profiles; app->cfg->n_profiles = old.n_profiles;
            mt_err_t rollback = reconfigure_profiles(app);
            if (rollback != MT_OK) { MT_ERROR("failed to restore profile routes: %s", mt_err_str(rollback)); }
        } else {
            incoming->profiles = NULL; incoming->n_profiles = 0;
            for (size_t i = 0; i < count; i++) {
                free(*shadows[i].field);
                *shadows[i].field = shadows[i].next;
                shadows[i].next = NULL;
            }
            mt_config_clear_profiles(&old);
        }
    }
    for (size_t i = 0; i < count; i++) { free(shadows[i].next); }
    free(shadows);
    app_nf_leave(app);
    return err;
}

mt_err_t mt_app_reconcile_routes(mt_app_t *app, const char *interface_name)
{
    if (!app) { return MT_ERR_INVAL; }
    /* Never lose an event while a committer rebuild owns the mutex.
     * Return AGAIN to the event-loop retry queue; do not interrupt the
     * background netfilter rebuild on each route notification. */
    if (app->nf_mu_ready) {
        int rc = pthread_mutex_trylock(&app->nf_mu);
        if (rc != 0) { return rc == EBUSY ? MT_ERR_AGAIN : MT_ERR_SYS; }
    }
    mt_err_t result = MT_OK;
    for (size_t family = 0; family < 2; family++) {
        mt_ruleset_t **sets = family ? app->sub_rulesets : app->rulesets;
        size_t count = family ? app->n_sub_rulesets : app->n_rulesets;
        for (size_t i = 0; i < count; i++) {
            if (interface_name && !mt_ruleset_uses_interface(sets[i], interface_name)) {
                continue;
            }
            mt_err_t err = mt_ruleset_on_link_up(sets[i]);
            if (err != MT_OK) {
                const mt_group_t *g = mt_ruleset_group(sets[i]);
                MT_ERROR("failed to reconcile route: group=%s err=%s", g->name, mt_err_str(err));
                if (result == MT_OK) { result = err; }
            }
        }
    }
    if (app->nf_mu_ready) { pthread_mutex_unlock(&app->nf_mu); }
    return result;
}
