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
    "groups: [{id: aabbccdd, name: Group, profile: old, enable: false}]\n"
    "subscriptions: [{id: 11223344, name: Sub, profile: old, enable: false}]\n";

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
    ASSERT_STR_EQ("Renamed", cfg.profiles[0]->name);
    ASSERT_EQ(MT_ERR_NOENT, reload(app, "configVersion: 0.8\nprofiles: []\n"));
    ASSERT_EQ((size_t)1, cfg.n_profiles); ASSERT_STR_EQ("tun1", cfg.groups[0]->iface);
    ASSERT_EQ(MT_OK, reload(app, "configVersion: 0.8\napp: {logLevel: debug}\n"));
    ASSERT_EQ((size_t)1, cfg.n_profiles); ASSERT_EQ((size_t)1, cfg.n_groups);
    mt_app_destroy(app); mt_config_clear(&cfg); PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(failed_apply_restores_definitions_and_all_consumers);
    RUN_TEST(reload_overlay_normalizes_preserved_groups_and_clears_absent_subscriptions);
    GREATEST_MAIN_END();
}
