#include "greatest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "magitrickle/app.h"
#include "magitrickle/ipset.h"
#include "magitrickle/profiles.h"

/* Deterministic allocation failure before any privileged side effects. */
mt_ipset_nl_t *__wrap_mt_ipset_nl_real_new(void);
mt_ipset_nl_t *__wrap_mt_ipset_nl_real_new(void) { return NULL; }

static const char *original =
    "configVersion: 0.8\nprofiles: [{id: old, name: Old, interfaces: [tun0, tun1]}]\n"
    "groups: [{id: aabbccdd, name: Group, profile: old, enable: false, priority: 731}]\n"
    "subscriptions: [{id: 11223344, name: Sub, profile: old, enable: false, priority: 219}]\n";

static mt_err_t reload(mt_app_t *app, const char *text) {
    char path[] = "/tmp/mt-profile-reload-XXXXXX";
    int fd = mkstemp(path); if (fd < 0) { return MT_ERR_IO; }
    size_t size = strlen(text);
    if (write(fd, text, size) != (ssize_t)size) { close(fd); unlink(path); return MT_ERR_IO; }
    close(fd); mt_err_t err = mt_app_reload_config(app, path); unlink(path); return err;
}

TEST failed_apply_restores_definitions_and_all_consumers(void) {
    mt_config_t cfg = {0}; ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&cfg, original, strlen(original)));
    mt_app_deps_t deps = {.cfg = &cfg}; mt_app_t *app = mt_app_create(&deps); ASSERT(app);
    mt_group_t *old_group = cfg.groups[0]; mt_subscription_t *old_sub = cfg.subscriptions[0];
    mt_app_set_running(app, true);
    const char *next = "configVersion: 0.8\nprofiles: [{id: new, name: New, interfaces: [tun2]}]\n"
        "groups: [{id: aabbccdd, profile: new, enable: true}]\nsubscriptions: []\n";
    ASSERT_EQ(MT_ERR_NOMEM, reload(app, next));
    ASSERT(cfg.groups[0] == old_group); ASSERT(cfg.subscriptions[0] == old_sub);
    ASSERT_EQ(MT_OK, mt_profiles_validate(&cfg, NULL, 0));
    ASSERT_STR_EQ("old", cfg.profiles[0]->id); ASSERT_STR_EQ("tun0", cfg.groups[0]->iface);
    ASSERT_STR_EQ("old", mt_ruleset_group(mt_app_subscription_ruleset_at(app, 0))->profile);
    ASSERT_EQ(731, cfg.groups[0]->priority); ASSERT_EQ(219, cfg.subscriptions[0]->priority);
    ASSERT_EQ(219, mt_ruleset_group(mt_app_subscription_ruleset_at(app, 0))->priority);
    char *out = NULL; size_t n;
    ASSERT_EQ(MT_OK, mt_config_save_buffer(&cfg, "0.8", &out, &n)); free(out);
    mt_app_destroy(app); mt_config_clear(&cfg); PASS();
}

TEST reload_overlay_normalizes_preserved_groups_and_clears_absent_subscriptions(void) {
    mt_config_t cfg = {0}; ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&cfg, original, strlen(original)));
    mt_app_deps_t deps = {.cfg = &cfg}; mt_app_t *app = mt_app_create(&deps); ASSERT(app);
    ASSERT_EQ(MT_OK, reload(app, "configVersion: 0.8\nprofiles: [{id: old, name: Renamed, interfaces: [tun1, tun0]}]\n"));
    ASSERT_EQ((size_t)1, cfg.n_groups); ASSERT_EQ((size_t)0, cfg.n_subscriptions);
    ASSERT_STR_EQ("tun1", cfg.groups[0]->iface); ASSERT_STR_EQ("old", cfg.groups[0]->profile);
    ASSERT_EQ(731, cfg.groups[0]->priority);
    ASSERT_EQ(731, mt_ruleset_group(mt_app_user_group_at(app, 0))->priority);
    ASSERT_STR_EQ("Renamed", cfg.profiles[0]->name);
    ASSERT_EQ(MT_ERR_NOENT, reload(app, "configVersion: 0.8\nprofiles: []\n"));
    ASSERT_EQ((size_t)1, cfg.n_profiles); ASSERT_STR_EQ("tun1", cfg.groups[0]->iface);
    ASSERT_EQ(731, cfg.groups[0]->priority);
    ASSERT_EQ(MT_OK, reload(app, "configVersion: 0.8\napp: {logLevel: debug}\n"));
    ASSERT_EQ((size_t)1, cfg.n_profiles); ASSERT_EQ((size_t)1, cfg.n_groups);
    ASSERT_EQ(731, cfg.groups[0]->priority);
    ASSERT_EQ(731, mt_ruleset_group(mt_app_user_group_at(app, 0))->priority);
    char *saved = NULL; size_t saved_size = 0;
    ASSERT_EQ(MT_OK, mt_config_save_buffer(&cfg, "0.8", &saved, &saved_size));
    mt_config_t loaded; ASSERT_EQ(MT_OK, mt_config_init_defaults(&loaded));
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&loaded, saved, saved_size));
    ASSERT_EQ(731, loaded.groups[0]->priority);
    ASSERT_STR_EQ("old", loaded.groups[0]->profile);
    ASSERT_STR_EQ("tun1", loaded.groups[0]->iface);
    free(saved); mt_config_clear(&loaded);
    mt_app_destroy(app); mt_config_clear(&cfg); PASS();
}

/* Exercise the zero/nonzero boundaries independently for both old registries.
 * Disabled definitions still have runtime enable flags, so these tests cover
 * snapshot/rollback without requiring iptables, ipset or root privileges. */
static mt_err_t cardinality_config(char *text, size_t capacity, size_t ng,
                                    size_t ns, int fail_family) {
    int written = snprintf(text, capacity,
        "configVersion: 0.8\nprofiles: [{id: test, name: Test, interfaces: [lo]}]\n");
    if (written < 0 || (size_t)written >= capacity) { return MT_ERR_LIMIT; }
    size_t used = (size_t)written;
    for (int family = 0; family < 2; family++) {
        size_t count = family ? ns : ng;
        written = snprintf(text + used, capacity - used, "%s:%s\n",
                           family ? "subscriptions" : "groups", count ? "" : " []");
        if (written < 0 || (size_t)written >= capacity - used) { return MT_ERR_LIMIT; }
        used += (size_t)written;
        for (size_t i = 0; i < count; i++) {
            written = snprintf(text + used, capacity - used,
                "  - {id: %u%07zu, name: Item%zu, profile: test, enable: %s, priority: %zu}\n",
                family ? 2u : 1u, i + 1, i,
                family == fail_family && i + 1 == count ? "true" : "false", 700 + i);
            if (written < 0 || (size_t)written >= capacity - used) { return MT_ERR_LIMIT; }
            used += (size_t)written;
        }
    }
    return MT_OK;
}

TEST reload_cardinality_matrix_keeps_config_and_runtime_consistent(void) {
    for (size_t old_ng = 0; old_ng < 3; old_ng++) {
        for (size_t old_ns = 0; old_ns < 3; old_ns++) {
            for (size_t new_ng = 0; new_ng < 3; new_ng++) {
                for (size_t new_ns = 0; new_ns < 3; new_ns++) {
                    char text[1024];
                    ASSERT_EQ(MT_OK, cardinality_config(text, sizeof(text), old_ng, old_ns, -1));
                    mt_config_t cfg = {0}; ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
                    ASSERT_EQ(MT_OK, mt_config_load_buffer(&cfg, text, strlen(text)));
                    mt_app_deps_t deps = {.cfg = &cfg};
                    mt_app_t *app = mt_app_create(&deps); ASSERT(app);
                    mt_app_set_running(app, true);
                    ASSERT_EQ(MT_OK, cardinality_config(text, sizeof(text), new_ng, new_ns, -1));
                    ASSERT_EQ(MT_OK, reload(app, text));
                    ASSERT_EQ(new_ng, cfg.n_groups); ASSERT_EQ(new_ns, cfg.n_subscriptions);
                    for (size_t i = 0; i < new_ng; i++) {
                        mt_ruleset_t *rs = mt_app_user_group_at(app, i); ASSERT(rs);
                        ASSERT(mt_ruleset_group(rs) == cfg.groups[i]);
                        ASSERT(mt_ruleset_runtime_enabled(rs));
                        ASSERT_EQ((int)(700 + i), mt_ruleset_group(rs)->priority);
                    }
                    for (size_t i = 0; i < new_ns; i++) {
                        mt_ruleset_t *rs = mt_app_subscription_ruleset_at(app, i); ASSERT(rs);
                        ASSERT(mt_ruleset_runtime_enabled(rs));
                        ASSERT_EQ((int)(700 + i), mt_ruleset_group(rs)->priority);
                        ASSERT_STR_EQ(cfg.subscriptions[i]->profile, mt_ruleset_group(rs)->profile);
                    }
                    ASSERT(mt_app_user_group_at(app, new_ng) == NULL);
                    ASSERT(mt_app_subscription_ruleset_at(app, new_ns) == NULL);
                    ASSERT_EQ(MT_OK, mt_profiles_validate(&cfg, NULL, 0));
                    mt_app_destroy(app); mt_config_clear(&cfg);
                }
            }
        }
    }
    PASS();
}

TEST reload_failure_matrix_restores_exact_old_runtime_flags(void) {
    for (size_t ng = 0; ng < 3; ng++) {
        for (size_t ns = 0; ns < 3; ns++) {
            for (int fail_family = 0; fail_family < 2; fail_family++) {
                char text[1024];
                ASSERT_EQ(MT_OK, cardinality_config(text, sizeof(text), ng, ns, -1));
                mt_config_t cfg = {0}; ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
                ASSERT_EQ(MT_OK, mt_config_load_buffer(&cfg, text, strlen(text)));
                mt_app_deps_t deps = {.cfg = &cfg};
                mt_app_t *app = mt_app_create(&deps); ASSERT(app);
                mt_ruleset_t *old_groups[2] = {0}, *old_subs[2] = {0};
                for (size_t i = 0; i < ng; i++) {
                    old_groups[i] = mt_app_user_group_at(app, i); ASSERT(old_groups[i]);
                    if (i == 0) { ASSERT_EQ(MT_OK, mt_ruleset_enable(old_groups[i])); }
                }
                for (size_t i = 0; i < ns; i++) {
                    old_subs[i] = mt_app_subscription_ruleset_at(app, i); ASSERT(old_subs[i]);
                    if (i == 0) { ASSERT_EQ(MT_OK, mt_ruleset_enable(old_subs[i])); }
                }
                mt_group_t **old_definitions = cfg.groups;
                mt_subscription_t **old_subscriptions = cfg.subscriptions;
                mt_profile_t **old_profiles = cfg.profiles;
                mt_app_set_running(app, true);
                /* Fail after at least one staged ruleset was enabled. */
                ASSERT_EQ(MT_OK, cardinality_config(text, sizeof(text), 2, 2, fail_family));
                ASSERT_EQ(MT_ERR_NOMEM, reload(app, text));
                ASSERT_EQ(ng, cfg.n_groups); ASSERT_EQ(ns, cfg.n_subscriptions);
                ASSERT(cfg.groups == old_definitions);
                ASSERT(cfg.subscriptions == old_subscriptions);
                ASSERT(cfg.profiles == old_profiles);
                for (size_t i = 0; i < ng; i++) {
                    ASSERT(mt_app_user_group_at(app, i) == old_groups[i]);
                    ASSERT_EQ(i == 0, mt_ruleset_runtime_enabled(old_groups[i]));
                    ASSERT(mt_ruleset_group(old_groups[i]) == cfg.groups[i]);
                }
                for (size_t i = 0; i < ns; i++) {
                    ASSERT(mt_app_subscription_ruleset_at(app, i) == old_subs[i]);
                    ASSERT_EQ(i == 0, mt_ruleset_runtime_enabled(old_subs[i]));
                    ASSERT_EQ((int)(700 + i), mt_ruleset_group(old_subs[i])->priority);
                }
                ASSERT(mt_app_user_group_at(app, ng) == NULL);
                ASSERT(mt_app_subscription_ruleset_at(app, ns) == NULL);
                ASSERT_EQ(MT_OK, mt_profiles_validate(&cfg, NULL, 0));
                /* A failed reload must not poison the next reload. */
                ASSERT_EQ(MT_OK, cardinality_config(text, sizeof(text), 0, 0, -1));
                ASSERT_EQ(MT_OK, reload(app, text));
                ASSERT_EQ((size_t)0, cfg.n_groups); ASSERT_EQ((size_t)0, cfg.n_subscriptions);
                mt_app_destroy(app); mt_config_clear(&cfg);
            }
        }
    }
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(failed_apply_restores_definitions_and_all_consumers);
    RUN_TEST(reload_overlay_normalizes_preserved_groups_and_clears_absent_subscriptions);
    RUN_TEST(reload_cardinality_matrix_keeps_config_and_runtime_consistent);
    RUN_TEST(reload_failure_matrix_restores_exact_old_runtime_flags);
    GREATEST_MAIN_END();
}
