#include "greatest.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "magitrickle/app.h"
#include "magitrickle/dns_cache.h"
#include "magitrickle/iptables.h"

static mt_group_t *make_group(const char *name, bool enable) {
    mt_group_t *g = mt_group_new();
    g->id = mt_id_random();
    mt_strset(&g->name, name);
    mt_strset(&g->color, "#ffffff");
    mt_strset(&g->iface, "eth0");
    g->enable = enable;
    return g;
}

static mt_rule_t *make_rule(void) {
    mt_rule_t *r = mt_rule_new();
    r->id = mt_id_random();
    mt_strset(&r->type, "domain");
    mt_strset(&r->rule, "example.com");
    r->enable = true;
    return r;
}

TEST create_wraps_preexisting_groups(void) {
    mt_config_t cfg;
    mt_config_init_defaults(&cfg);
    mt_config_add_group(&cfg, make_group("g1", false));
    mt_config_add_group(&cfg, make_group("g2", false));

    mt_cache_t *cache = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &cfg, .cache = cache};
    mt_app_t *app = mt_app_create(&deps);
    ASSERT(app != NULL);

    ASSERT_EQ(2u, mt_app_user_group_count(app));
    ASSERT_STR_EQ("g1", mt_ruleset_group(mt_app_user_group_at(app, 0))->name);
    ASSERT_STR_EQ("g2", mt_ruleset_group(mt_app_user_group_at(app, 1))->name);

    mt_app_destroy(app);
    mt_cache_destroy(cache);
    mt_config_clear(&cfg);
    PASS();
}

TEST add_group_rejects_duplicate_id(void) {
    mt_config_t cfg;
    mt_config_init_defaults(&cfg);
    mt_cache_t *cache = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &cfg, .cache = cache};
    mt_app_t *app = mt_app_create(&deps);

    mt_group_t *g1 = make_group("g1", false);
    ASSERT_EQ(MT_OK, mt_app_add_group(app, g1));

    mt_group_t *dup = make_group("dup", false);
    dup->id = g1->id;
    ASSERT_EQ(MT_ERR_EXIST, mt_app_add_group(app, dup));
    ASSERT_EQ(1u, mt_app_user_group_count(app));
    ASSERT_EQ(1u, cfg.n_groups);

    mt_app_destroy(app);
    mt_cache_destroy(cache);
    mt_config_clear(&cfg);
    PASS();
}

TEST add_group_rejects_duplicate_rule_id(void) {
    mt_config_t cfg;
    mt_config_init_defaults(&cfg);
    mt_cache_t *cache = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &cfg, .cache = cache};
    mt_app_t *app = mt_app_create(&deps);

    mt_group_t *g = make_group("g", false);
    mt_rule_t *r1 = make_rule();
    mt_rule_t *r2 = make_rule();
    r2->id = r1->id; /* force a duplicate */
    mt_group_add_rule(g, r1);
    mt_group_add_rule(g, r2);

    ASSERT_EQ(MT_ERR_INVAL, mt_app_add_group(app, g));
    ASSERT_EQ(0u, mt_app_user_group_count(app));
    ASSERT_EQ(0u, cfg.n_groups);

    mt_app_destroy(app);
    mt_cache_destroy(cache);
    mt_config_clear(&cfg);
    PASS();
}

TEST add_group_while_not_running_does_not_touch_netfilter(void) {
    mt_config_t cfg;
    mt_config_init_defaults(&cfg);
    mt_cache_t *cache = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &cfg, .cache = cache};
    mt_app_t *app = mt_app_create(&deps);
    /* mt_app_set_running is never called -> app starts "not running" */

    mt_group_t *g = make_group("g", true); /* enable=true, but app isn't
                                            * running yet */
    ASSERT_EQ(MT_OK, mt_app_add_group(app, g));
    ASSERT_EQ(1u, mt_app_user_group_count(app));
    ASSERT(!mt_ruleset_runtime_enabled(mt_app_user_group_at(app, 0)));

    mt_app_destroy(app);
    mt_cache_destroy(cache);
    mt_config_clear(&cfg);
    PASS();
}

TEST add_group_while_running_rolls_back_on_failure(void) {
    /* Enabling a group with enable=true while the app is "running"
     * attempts a real ipset create via libmnl, which fails in this
     * sandbox (no ip_set kernel module, confirmed since Phase 0) --
     * exercising the exact rollback path this test checks: the group
     * must NOT remain in either the app's ruleset list or cfg.groups. */
    mt_config_t cfg;
    mt_config_init_defaults(&cfg);
    mt_cache_t *cache = mt_cache_create(0);

    mt_ipt_executable_t *exe4 = mt_ipt_executable_real_new(MT_IPT_PROTO_IPV4);
    mt_ipt_t *ipt4 = mt_ipt_new(exe4);
    mt_rtnl_t *rtnl = mt_rtnl_open();
    ASSERT(ipt4 != NULL && rtnl != NULL);

    mt_app_deps_t deps = {.cfg = &cfg, .cache = cache, .ipt4 = ipt4, .rtnl = rtnl};
    mt_app_t *app = mt_app_create(&deps);
    mt_app_set_running(app, true);

    mt_group_t *g = make_group("g", true);
    mt_err_t err = mt_app_add_group(app, g);
    ASSERT(err != MT_OK);
    ASSERT_EQ(0u, mt_app_user_group_count(app));
    ASSERT_EQ(0u, cfg.n_groups);

    mt_app_destroy(app);
    mt_ipt_free(ipt4);
    mt_rtnl_close(rtnl);
    mt_cache_destroy(cache);
    mt_config_clear(&cfg);
    PASS();
}

TEST clear_groups_empties_both_lists(void) {
    mt_config_t cfg;
    mt_config_init_defaults(&cfg);
    mt_cache_t *cache = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &cfg, .cache = cache};
    mt_app_t *app = mt_app_create(&deps);

    mt_app_add_group(app, make_group("a", false));
    mt_app_add_group(app, make_group("b", false));
    ASSERT_EQ(2u, mt_app_user_group_count(app));

    mt_app_clear_groups(app);
    ASSERT_EQ(0u, mt_app_user_group_count(app));
    ASSERT_EQ(0u, cfg.n_groups);

    mt_app_destroy(app);
    mt_cache_destroy(cache);
    mt_config_clear(&cfg);
    PASS();
}

TEST remove_by_id_and_by_index(void) {
    mt_config_t cfg;
    mt_config_init_defaults(&cfg);
    mt_cache_t *cache = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &cfg, .cache = cache};
    mt_app_t *app = mt_app_create(&deps);

    mt_app_add_group(app, make_group("a", false));
    mt_app_add_group(app, make_group("b", false));
    mt_app_add_group(app, make_group("c", false));
    mt_id_t b_id = mt_ruleset_group(mt_app_user_group_at(app, 1))->id;

    ASSERT(mt_app_remove_group_by_id(app, b_id));
    ASSERT_EQ(2u, mt_app_user_group_count(app));
    ASSERT_STR_EQ("a", mt_ruleset_group(mt_app_user_group_at(app, 0))->name);
    ASSERT_STR_EQ("c", mt_ruleset_group(mt_app_user_group_at(app, 1))->name);
    ASSERT_EQ(2u, cfg.n_groups);

    mt_id_t ghost = mt_id_random();
    ASSERT(!mt_app_remove_group_by_id(app, ghost));

    mt_app_remove_group_by_index(app, 0);
    ASSERT_EQ(1u, mt_app_user_group_count(app));
    ASSERT_STR_EQ("c", mt_ruleset_group(mt_app_user_group_at(app, 0))->name);

    mt_app_destroy(app);
    mt_cache_destroy(cache);
    mt_config_clear(&cfg);
    PASS();
}

TEST list_interfaces_show_all_finds_loopback(void) {
    mt_config_t cfg;
    mt_config_init_defaults(&cfg);
    cfg.app.show_all_interfaces = true;
    mt_cache_t *cache = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &cfg, .cache = cache};
    mt_app_t *app = mt_app_create(&deps);

    mt_iface_info_t *ifaces = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_app_list_interfaces(app, &ifaces, &n));
    ASSERT(n > 0);
    bool found_lo = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(ifaces[i].id, "lo") == 0) { found_lo = true; }
    }
    ASSERT(found_lo);
    /* no duplicate names (getifaddrs yields one entry per address
     * family per interface; must be deduped) */
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            ASSERT(strcmp(ifaces[i].id, ifaces[j].id) != 0);
        }
    }

    free(ifaces);
    mt_app_destroy(app);
    mt_cache_destroy(cache);
    mt_config_clear(&cfg);
    PASS();
}

TEST save_config_round_trips(void) {
    mt_config_t cfg;
    mt_config_init_defaults(&cfg);
    mt_cache_t *cache = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &cfg, .cache = cache};
    mt_app_t *app = mt_app_create(&deps);
    mt_app_add_group(app, make_group("saved-group", false));

    char path[] = "/tmp/mt_app_test_config_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    ASSERT_EQ(MT_OK, mt_app_save_config(app, path, "0.1"));

    mt_config_t reloaded;
    mt_config_init_defaults(&reloaded);
    ASSERT_EQ(MT_OK, mt_config_load_file(&reloaded, path));
    ASSERT_EQ(1u, reloaded.n_groups);
    ASSERT_STR_EQ("saved-group", reloaded.groups[0]->name);

    unlink(path);
    mt_config_clear(&reloaded);
    mt_app_destroy(app);
    mt_cache_destroy(cache);
    mt_config_clear(&cfg);
    PASS();
}

TEST force_commit_iptables_noop_without_engines(void) {
    mt_config_t cfg;
    mt_config_init_defaults(&cfg);
    mt_cache_t *cache = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &cfg, .cache = cache};
    mt_app_t *app = mt_app_create(&deps);

    ASSERT_EQ(MT_OK, mt_app_force_commit_iptables(app));

    mt_app_destroy(app);
    mt_cache_destroy(cache);
    mt_config_clear(&cfg);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(create_wraps_preexisting_groups);
    RUN_TEST(add_group_rejects_duplicate_id);
    RUN_TEST(add_group_rejects_duplicate_rule_id);
    RUN_TEST(add_group_while_not_running_does_not_touch_netfilter);
    RUN_TEST(add_group_while_running_rolls_back_on_failure);
    RUN_TEST(clear_groups_empties_both_lists);
    RUN_TEST(remove_by_id_and_by_index);
    RUN_TEST(list_interfaces_show_all_finds_loopback);
    RUN_TEST(save_config_round_trips);
    RUN_TEST(force_commit_iptables_noop_without_engines);
    GREATEST_MAIN_END();
}
