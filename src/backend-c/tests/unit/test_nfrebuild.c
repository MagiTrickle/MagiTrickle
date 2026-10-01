/* Full-table rebuild (mt_app_rebuild_netfilter): what has to hold after
 * the Keenetic firmware replaces a table underneath us. */
#include "greatest.h"

#include <stdlib.h>
#include <string.h>

#include "fake_iptables.h"

#include "magitrickle/app.h"
#include "magitrickle/dns_cache.h"
#include "magitrickle/netfilter_cleaner.h"
#include "magitrickle/port_remap.h"

typedef struct {
    mt_config_t cfg;
    mt_cache_t *cache;
    mt_fake_ipt_t *fake;
    mt_ipt_t *ipt;
    mt_app_t *app;
    mt_port_remap_t *remap;
} fixture_t;

/* Rules belonging to somebody else: a rebuild must leave them alone. */
static void seed_foreign_rules(mt_fake_ipt_t *f) {
    static const char *fwd[] = {"-i", "eth0", "-j", "ACCEPT"};
    static const char *pre[] = {"-i", "eth0", "-j", "ACCEPT"};
    static const char *post[] = {"-o", "eth0", "-j", "MASQUERADE"};
    static const char *mangle[] = {"-i", "eth0", "-j", "MARK", "--set-mark", "7"};

    const char *const *rules[1];
    size_t lens[1];

    rules[0] = fwd;
    lens[0] = 4;
    mt_fake_ipt_set_initial_rules(f, "filter", "FORWARD", rules, lens, 1);
    rules[0] = mangle;
    lens[0] = 6;
    mt_fake_ipt_set_initial_rules(f, "mangle", "PREROUTING", rules, lens, 1);
    rules[0] = pre;
    lens[0] = 4;
    mt_fake_ipt_set_initial_rules(f, "nat", "PREROUTING", rules, lens, 1);
    rules[0] = post;
    lens[0] = 4;
    mt_fake_ipt_set_initial_rules(f, "nat", "POSTROUTING", rules, lens, 1);
}

static void fixture_up(fixture_t *fx) {
    memset(fx, 0, sizeof(*fx));
    mt_config_init_defaults(&fx->cfg);
    fx->cache = mt_cache_create(0);

    fx->fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    seed_foreign_rules(fx->fake);
    fx->ipt = mt_ipt_new(mt_fake_ipt_as_executable(fx->fake));
    mt_netfilter_register_base_chains(fx->ipt, NULL);

    mt_app_deps_t deps = {.cfg = &fx->cfg, .cache = fx->cache, .ipt4 = fx->ipt};
    fx->app = mt_app_create(&deps);

    fx->remap = mt_port_remap_new(fx->cfg.app.netfilter.iptables.chain_prefix, 53, 3553, NULL, 0,
                                  fx->ipt, NULL);
    mt_port_remap_enable(fx->remap);
    mt_app_set_port_remap(fx->app, fx->remap);
}

static void fixture_down(fixture_t *fx) {
    mt_port_remap_free(fx->remap);
    mt_app_destroy(fx->app);
    mt_ipt_free(fx->ipt);
    mt_cache_destroy(fx->cache);
    mt_config_clear(&fx->cfg);
}

/* Stands in for a leftover chain from a previous run of the daemon. */
static void seed_stale_chain(mt_fake_ipt_t *f) {
    static const char *rule[] = {"-m", "set", "--match-set", "mt_old_4", "dst", "-j", "MASQUERADE"};
    static const char *jump[] = {"-j", "MT_old"};
    const char *const *rules[1] = {rule};
    size_t lens[1] = {7};
    mt_fake_ipt_set_initial_rules(f, "nat", "MT_old", rules, lens, 1);

    /* ... plus the jump into it, which must go with it. */
    const char *const *keep[2];
    size_t keep_lens[2];
    static const char *foreign[] = {"-o", "eth0", "-j", "MASQUERADE"};
    keep[0] = foreign;
    keep_lens[0] = 4;
    keep[1] = jump;
    keep_lens[1] = 2;
    mt_fake_ipt_set_initial_rules(f, "nat", "POSTROUTING", keep, keep_lens, 2);
}

static bool chain_has_rule(mt_fake_ipt_t *f, const char *table, const char *chain,
                          const char *const *parts, size_t n_parts) {
    mt_ipt_rule_t *const *rules = NULL;
    size_t n = 0;
    if (!mt_fake_ipt_get_rules(f, table, chain, &rules, &n)) { return false; }
    for (size_t i = 0; i < n; i++) {
        if (rules[i]->n_parts != n_parts) { continue; }
        bool same = true;
        for (size_t j = 0; j < n_parts; j++) {
            if (strcmp(rules[i]->parts[j], parts[j]) != 0) {
                same = false;
                break;
            }
        }
        if (same) { return true; }
    }
    return false;
}

static size_t count_rule(mt_fake_ipt_t *f, const char *table, const char *chain,
                        const char *const *parts, size_t n_parts) {
    mt_ipt_rule_t *const *rules = NULL;
    size_t n = 0, found = 0;
    if (!mt_fake_ipt_get_rules(f, table, chain, &rules, &n)) { return 0; }
    for (size_t i = 0; i < n; i++) {
        if (rules[i]->n_parts != n_parts) { continue; }
        bool same = true;
        for (size_t j = 0; j < n_parts; j++) {
            if (strcmp(rules[i]->parts[j], parts[j]) != 0) {
                same = false;
                break;
            }
        }
        if (same) { found++; }
    }
    return found;
}

static const char *const k_remap_jump[] = {"-j", "MT_DNSOR"};
static const char *const k_foreign_post[] = {"-o", "eth0", "-j", "MASQUERADE"};

TEST rebuild_restores_the_table_after_a_wipe(void) {
    fixture_t fx;
    fixture_up(&fx);

    ASSERT(mt_fake_ipt_chain_exists(fx.fake, "nat", "MT_DNSOR"));
    ASSERT(chain_has_rule(fx.fake, "nat", "PREROUTING", k_remap_jump, 2));

    /* The firmware replaces every table, taking all of ours with it. */
    mt_fake_ipt_reset(fx.fake);
    seed_foreign_rules(fx.fake);
    ASSERT_FALSE(mt_fake_ipt_chain_exists(fx.fake, "nat", "MT_DNSOR"));

    ASSERT_EQ(MT_OK, mt_app_rebuild_netfilter(fx.app, NULL));

    ASSERT(mt_fake_ipt_chain_exists(fx.fake, "nat", "MT_DNSOR"));
    ASSERT(chain_has_rule(fx.fake, "nat", "PREROUTING", k_remap_jump, 2));

    fixture_down(&fx);
    PASS();
}

TEST rebuild_does_not_duplicate_jumps(void) {
    fixture_t fx;
    fixture_up(&fx);

    for (int i = 0; i < 4; i++) { ASSERT_EQ(MT_OK, mt_app_rebuild_netfilter(fx.app, NULL)); }

    ASSERT_EQ(1u, count_rule(fx.fake, "nat", "PREROUTING", k_remap_jump, 2));

    fixture_down(&fx);
    PASS();
}

TEST rebuild_drops_chains_left_by_a_previous_run(void) {
    fixture_t fx;
    fixture_up(&fx);
    seed_stale_chain(fx.fake);
    ASSERT(mt_fake_ipt_chain_exists(fx.fake, "nat", "MT_old"));

    ASSERT_EQ(MT_OK, mt_app_rebuild_netfilter(fx.app, NULL));

    ASSERT_FALSE(mt_fake_ipt_chain_exists(fx.fake, "nat", "MT_old"));
    static const char *const stale_jump[] = {"-j", "MT_old"};
    ASSERT_EQ(0u, count_rule(fx.fake, "nat", "POSTROUTING", stale_jump, 2));
    /* ... while what we do want is (re)built in the same pass. */
    ASSERT(mt_fake_ipt_chain_exists(fx.fake, "nat", "MT_DNSOR"));

    fixture_down(&fx);
    PASS();
}

TEST rebuild_keeps_other_writers_rules(void) {
    fixture_t fx;
    fixture_up(&fx);

    ASSERT_EQ(MT_OK, mt_app_rebuild_netfilter(fx.app, NULL));

    ASSERT(chain_has_rule(fx.fake, "nat", "POSTROUTING", k_foreign_post, 4));
    static const char *const foreign_fwd[] = {"-i", "eth0", "-j", "ACCEPT"};
    ASSERT(chain_has_rule(fx.fake, "filter", "FORWARD", foreign_fwd, 4));

    fixture_down(&fx);
    PASS();
}

/* An already-raised token means the pass is pointless: it must give up
 * without writing, and say so rather than reporting a failure. */
TEST rebuild_aborts_on_a_raised_cancel(void) {
    fixture_t fx;
    fixture_up(&fx);

    mt_fake_ipt_reset(fx.fake);
    seed_foreign_rules(fx.fake);

    mt_cancel_t *cancel = mt_cancel_new();
    mt_cancel_raise(cancel);

    ASSERT_EQ(MT_ERR_CANCELED, mt_app_rebuild_netfilter(fx.app, cancel));
    ASSERT_FALSE(mt_fake_ipt_chain_exists(fx.fake, "nat", "MT_DNSOR"));

    /* Lowering it and going again must converge -- an aborted pass leaves
     * nothing behind that a later one cannot fix. */
    mt_cancel_clear(cancel);
    ASSERT_EQ(MT_OK, mt_app_rebuild_netfilter(fx.app, cancel));
    ASSERT(mt_fake_ipt_chain_exists(fx.fake, "nat", "MT_DNSOR"));

    mt_cancel_free(cancel);
    fixture_down(&fx);
    PASS();
}

/* Without a committer the webhook keeps its old meaning: commit here and
 * now, and report what happened. */
TEST force_commit_without_a_committer_commits_in_place(void) {
    fixture_t fx;
    fixture_up(&fx);

    ASSERT_EQ(MT_OK, mt_app_force_commit_iptables(fx.app));

    fixture_down(&fx);
    PASS();
}

TEST port_remap_uses_the_configured_chain_prefix(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    mt_netfilter_register_base_chains(ipt, NULL);

    mt_port_remap_t *remap = mt_port_remap_new("XX_", 53, 3553, NULL, 0, ipt, NULL);
    ASSERT(remap != NULL);
    ASSERT_EQ(MT_OK, mt_port_remap_enable(remap));
    ASSERT(mt_fake_ipt_chain_exists(fake, "nat", "XX_DNSOR"));
    static const char *const jump[] = {"-j", "XX_DNSOR"};
    ASSERT(chain_has_rule(fake, "nat", "PREROUTING", jump, 2));

    ASSERT_EQ(MT_OK, mt_port_remap_disable(remap));
    mt_port_remap_free(remap);
    mt_ipt_free(ipt);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(rebuild_restores_the_table_after_a_wipe);
    RUN_TEST(rebuild_does_not_duplicate_jumps);
    RUN_TEST(rebuild_drops_chains_left_by_a_previous_run);
    RUN_TEST(rebuild_keeps_other_writers_rules);
    RUN_TEST(rebuild_aborts_on_a_raised_cancel);
    RUN_TEST(force_commit_without_a_committer_commits_in_place);
    RUN_TEST(port_remap_uses_the_configured_chain_prefix);
    GREATEST_MAIN_END();
}
