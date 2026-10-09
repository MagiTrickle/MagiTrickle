#include "greatest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "magitrickle/yamlio.h"

static mt_err_t load_str(mt_config_t *cfg, const char *doc)
{
    return mt_config_load_buffer(cfg, doc, strlen(doc));
}

TEST version_gate(void)
{
    mt_config_t cfg;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_ERR_STATE, load_str(&cfg, "configVersion: 1.0.0\n"));
    mt_config_clear(&cfg);

    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_ERR_STATE, load_str(&cfg, "app: {}\n"));
    mt_config_clear(&cfg);

    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_OK, load_str(&cfg, "configVersion: 0.7.0\n"));
    mt_config_clear(&cfg);
    PASS();
}

TEST overlay_and_defaults(void)
{
    mt_config_t cfg;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    const char *doc =
        "configVersion: 0.7.0\n"
        "app:\n"
        "  dnsProxy:\n"
        "    host:\n"
        "      port: 5353\n"
        "  logLevel: debug\n";
    ASSERT_EQ(MT_OK, load_str(&cfg, doc));
    ASSERT_EQ(5353, cfg.app.dns_proxy.host.port);
    /* untouched defaults stay */
    ASSERT_STR_EQ("[::]", cfg.app.dns_proxy.host.address);
    ASSERT_STR_EQ("127.0.0.1", cfg.app.dns_proxy.upstream.address);
    ASSERT_EQ(53, cfg.app.dns_proxy.upstream.port);
    ASSERT_STR_EQ("debug", cfg.app.log_level);
    ASSERT_EQ(5000 * MT_DURATION_MS, cfg.app.dns_proxy.timeout);
    mt_config_clear(&cfg);
    PASS();
}

TEST legacy_duration_normalization(void)
{
    mt_config_t cfg;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    const char *doc =
        "configVersion: 0.7.0\n"
        "app:\n"
        "  dnsProxy:\n"
        "    timeout: 5000\n" /* bare int ns -> < 1ms -> treated as ms */
        "  netfilter:\n"
        "    ipset:\n"
        "      additionalTTL: 3600\n"; /* < 1s -> treated as s */
    ASSERT_EQ(MT_OK, load_str(&cfg, doc));
    ASSERT_EQ(5000 * MT_DURATION_MS, cfg.app.dns_proxy.timeout);
    ASSERT_EQ(3600 * MT_DURATION_SEC, cfg.app.netfilter.ipset.additional_ttl);
    mt_config_clear(&cfg);
    PASS();
}

TEST absent_enable_is_false_and_color_normalized(void)
{
    mt_config_t cfg;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    const char *doc =
        "configVersion: 0.7.0\n"
        "groups:\n"
        "  - id: d663876a\n"
        "    name: G\n"
        "    color: '#AABBCC'\n"
        "    rules:\n"
        "      - id: 6f34ee91\n"
        "        type: domain\n"
        "        rule: example.com\n";
    ASSERT_EQ(MT_OK, load_str(&cfg, doc));
    ASSERT_EQ(1u, (unsigned)cfg.n_groups);
    ASSERT_FALSE(cfg.groups[0]->enable);          /* absent -> false */
    ASSERT_STR_EQ("#aabbcc", cfg.groups[0]->color); /* lowercased */
    ASSERT_FALSE(cfg.groups[0]->rules[0]->enable);
    mt_config_clear(&cfg);

    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    const char *doc2 =
        "configVersion: 0.7.0\n"
        "groups:\n"
        "  - id: d663876a\n"
        "    color: banana\n";
    ASSERT_EQ(MT_OK, load_str(&cfg, doc2));
    ASSERT_STR_EQ("#ffffff", cfg.groups[0]->color);
    mt_config_clear(&cfg);
    PASS();
}

TEST duplicate_ids_fail(void)
{
    mt_config_t cfg;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    const char *doc =
        "configVersion: 0.7.0\n"
        "groups:\n"
        "  - id: d663876a\n"
        "  - id: d663876a\n";
    ASSERT_EQ(MT_ERR_EXIST, load_str(&cfg, doc));
    mt_config_clear(&cfg);
    PASS();
}

TEST type_mismatch_fails(void)
{
    mt_config_t cfg;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_ERR_INVAL,
              load_str(&cfg, "configVersion: 0.7.0\napp:\n  httpWeb:\n"
                             "    enabled: 1\n"));
    mt_config_clear(&cfg);

    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_ERR_INVAL,
              load_str(&cfg, "configVersion: 0.7.0\napp:\n  httpWeb:\n"
                             "    host:\n      port: \"8080\"\n"));
    mt_config_clear(&cfg);

    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_ERR_INVAL,
              load_str(&cfg, "configVersion: 0.7.0\napp:\n  httpWeb:\n"
                             "    host:\n      port: 70000\n"));
    mt_config_clear(&cfg);
    PASS();
}

TEST corrupt_yaml_fails(void)
{
    mt_config_t cfg;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_ERR_PROTO, load_str(&cfg, "a: [unclosed\n  b: }{"));
    mt_config_clear(&cfg);
    PASS();
}

TEST save_shape_matches_committed_fixture(void)
{
    /* The Phase 1 Go fixture plus the intentionally added priority field. */
    mt_config_t cfg;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    const char *doc =
        "configVersion: 0.7.0\n"
        "groups:\n"
        "  - id: d663876a\n"
        "    name: Example\n"
        "    color: '#ffffff'\n"
        "    interface: nwg0\n"
        "    enable: false\n"
        "    rules:\n"
        "      - id: 6f34ee91\n"
        "        name: Wildcard Example\n"
        "        type: wildcard\n"
        "        rule: '*wildcard.example.com'\n"
        "        enable: true\n"
        "subscriptions: []\n";
    ASSERT_EQ(MT_OK, load_str(&cfg, doc));

    char *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(MT_OK, mt_config_save_buffer(&cfg, "0.99.0", &out, &out_len));

    const char *want =
        "configVersion: 0.99.0\n"
        "app:\n"
        "  httpWeb:\n"
        "    enabled: true\n"
        "    auth:\n"
        "      enabled: false\n"
        "    host:\n"
        "      address: '[::]'\n"
        "      port: 8080\n"
        "    skin: default\n"
        "  dnsProxy:\n"
        "    host:\n"
        "      address: '[::]'\n"
        "      port: 3553\n"
        "    upstream:\n"
        "      address: 127.0.0.1\n"
        "      port: 53\n"
        "    disableRemap53: false\n"
        "    disableFakePTR: false\n"
        "    disableDropAAAA: false\n"
        "    maxIdleConns: 10\n"
        "    maxConcurrent: 100\n"
        "    timeout: 5s\n"
        "  netfilter:\n"
        "    iptables:\n"
        "      chainPrefix: MT_\n"
        "    ipset:\n"
        "      tablePrefix: mt_\n"
        "      additionalTTL: 1h0m0s\n"
        "    disableIPv4: false\n"
        "    disableIPv6: false\n"
        "    startMarkTableIndex: 1298229097\n"
        "  link:\n"
        "  - br0\n"
        "  showAllInterfaces: false\n"
        "  logLevel: info\n"
        "groups:\n"
        "- id: d663876a\n"
        "  name: Example\n"
        "  color: '#ffffff'\n"
        "  interface: nwg0\n"
        "  enable: false\n"
        "  priority: 300\n"
        "  rules:\n"
        "  - id: 6f34ee91\n"
        "    name: Wildcard Example\n"
        "    type: wildcard\n"
        "    rule: '*wildcard.example.com'\n"
        "    enable: true\n"
        "subscriptions: []\n";
    ASSERT_STR_EQ(want, out);
    free(out);
    mt_config_clear(&cfg);
    PASS();
}

TEST quoted_id_shapes(void)
{
    /* digit-only and digits+e IDs must stay strings on save */
    mt_config_t cfg;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    const char *doc =
        "configVersion: 0.7.0\n"
        "groups:\n"
        "  - id: \"12345678\"\n"
        "  - id: 666e0000\n";
    ASSERT_EQ(MT_OK, load_str(&cfg, doc));
    char *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(MT_OK, mt_config_save_buffer(&cfg, "0.99.0", &out, &out_len));
    ASSERT(strstr(out, "- id: \"12345678\"\n") != NULL);
    ASSERT(strstr(out, "- id: \"666e0000\"\n") != NULL);
    free(out);
    mt_config_clear(&cfg);
    PASS();
}

TEST priority_defaults_boundaries_and_save_reload(void)
{
    mt_group_t *g = mt_group_new();
    mt_subscription_t *s = mt_subscription_new();
    ASSERT(g != NULL);
    ASSERT(s != NULL);
    ASSERT_EQ(MT_GROUP_DEFAULT_PRIORITY, g->priority);
    ASSERT_EQ(MT_SUBSCRIPTION_DEFAULT_PRIORITY, s->priority);
    mt_group_free(g);
    mt_subscription_free(s);

    mt_config_t cfg;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    const char *doc =
        "configVersion: 0.7.0\n"
        "groups:\n"
        "- {id: aaaa0001, name: legacy}\n"
        "- {id: aaaa0002, priority: 1}\n"
        "- {id: aaaa0003, priority: 1000}\n"
        "subscriptions:\n"
        "- {id: bbbb0001, name: legacy}\n"
        "- {id: bbbb0002, priority: 1}\n"
        "- {id: bbbb0003, priority: 1000}\n";
    ASSERT_EQ(MT_OK, load_str(&cfg, doc));
    ASSERT_EQ(3, cfg.n_groups);
    ASSERT_EQ(3, cfg.n_subscriptions);
    ASSERT_EQ(300, cfg.groups[0]->priority);
    ASSERT_EQ(1, cfg.groups[1]->priority);
    ASSERT_EQ(1000, cfg.groups[2]->priority);
    ASSERT_EQ(100, cfg.subscriptions[0]->priority);
    ASSERT_EQ(1, cfg.subscriptions[1]->priority);
    ASSERT_EQ(1000, cfg.subscriptions[2]->priority);

    char *saved = NULL;
    size_t size = 0;
    ASSERT_EQ(MT_OK, mt_config_save_buffer(&cfg, "0.99.0", &saved, &size));
    ASSERT(strstr(saved, "priority: 300\n") != NULL);
    ASSERT(strstr(saved, "priority: 100\n") != NULL);
    mt_config_t loaded;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&loaded));
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&loaded, saved, size));
    ASSERT_EQ(cfg.n_groups, loaded.n_groups);
    ASSERT_EQ(cfg.n_subscriptions, loaded.n_subscriptions);
    for (size_t i = 0; i < cfg.n_groups; i++) {
        ASSERT_EQ(cfg.groups[i]->priority, loaded.groups[i]->priority);
    }
    for (size_t i = 0; i < cfg.n_subscriptions; i++) {
        ASSERT_EQ(cfg.subscriptions[i]->priority, loaded.subscriptions[i]->priority);
    }
    free(saved);
    mt_config_clear(&loaded);
    mt_config_clear(&cfg);
    PASS();
}

TEST invalid_priorities_preserve_config_before_any_overlay(void)
{
    const char *invalid[] = {
        "0", "-1", "1001", "65536", "1.5", "300.0", ".nan", ".inf",
        "18446744073709551616", "null", "~", "true", "'300'", "[]", "{}"
    };
    mt_config_t cfg;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_OK, load_str(&cfg,
        "configVersion: 0.7.0\n"
        "groups: [{id: aaaa0001, name: original-group, priority: 777}]\n"
        "subscriptions: [{id: bbbb0001, name: original-sub, priority: 222}]\n"));
    mt_group_t *original_group = cfg.groups[0];
    mt_subscription_t *original_sub = cfg.subscriptions[0];
    const char *collections[] = {"groups", "subscriptions"};
    for (size_t collection = 0; collection < 2; collection++) {
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            char doc[512];
            snprintf(doc, sizeof(doc),
                "configVersion: 0.7.0\napp: {logLevel: debug}\n"
                "%s:\n- {id: cccc0001, priority: 1}\n"
                "- {id: cccc0002, priority: %s}\n", collections[collection], invalid[i]);
            ASSERT_EQ(MT_ERR_INVAL, load_str(&cfg, doc));
            ASSERT_STR_EQ("info", cfg.app.log_level);
            ASSERT_EQ(1, cfg.n_groups);
            ASSERT_EQ(1, cfg.n_subscriptions);
            ASSERT(cfg.groups[0] == original_group);
            ASSERT(cfg.subscriptions[0] == original_sub);
            ASSERT_EQ(777, cfg.groups[0]->priority);
            ASSERT_EQ(222, cfg.subscriptions[0]->priority);
            ASSERT_STR_EQ("original-group", cfg.groups[0]->name);
            ASSERT_STR_EQ("original-sub", cfg.subscriptions[0]->name);
        }
    }
    ASSERT_EQ(MT_ERR_INVAL, load_str(&cfg,
        "configVersion: 0.7.0\ngroups:\n"
        "- {id: aaaa0001, priority: 100, priority: 300}\n"));
    ASSERT(cfg.groups[0] == original_group);
    ASSERT(cfg.subscriptions[0] == original_sub);
    mt_config_clear(&cfg);
    PASS();
}

TEST priority_prevalidation_preserves_legacy_malformed_overlay_timing(void)
{
    const char *malformed[] = {
        "groups: 1\n", "groups: [1]\n",
        "subscriptions: 1\n", "subscriptions: [1]\n"
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        mt_config_t cfg;
        ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
        char doc[128];
        snprintf(doc, sizeof(doc), "configVersion: 0.7.0\napp: {logLevel: debug}\n%s", malformed[i]);
        ASSERT_EQ(MT_ERR_INVAL, load_str(&cfg, doc));
        /* Without an explicit priority error, the historical loader applies
         * the app overlay before reporting a malformed collection or item. */
        ASSERT_STR_EQ("debug", cfg.app.log_level);
        mt_config_clear(&cfg);
    }
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(version_gate);
    RUN_TEST(overlay_and_defaults);
    RUN_TEST(legacy_duration_normalization);
    RUN_TEST(absent_enable_is_false_and_color_normalized);
    RUN_TEST(duplicate_ids_fail);
    RUN_TEST(type_mismatch_fails);
    RUN_TEST(corrupt_yaml_fails);
    RUN_TEST(save_shape_matches_committed_fixture);
    RUN_TEST(quoted_id_shapes);
    RUN_TEST(priority_defaults_boundaries_and_save_reload);
    RUN_TEST(invalid_priorities_preserve_config_before_any_overlay);
    RUN_TEST(priority_prevalidation_preserves_legacy_malformed_overlay_timing);
    GREATEST_MAIN_END();
}
