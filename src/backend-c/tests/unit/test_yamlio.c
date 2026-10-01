#include "greatest.h"

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
    /* the byte-exact Go fixture from the Phase 1 yaml spike */
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
    GREATEST_MAIN_END();
}
