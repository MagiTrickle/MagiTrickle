#include "greatest.h"
#include <stdlib.h>
#include <string.h>
#include "magitrickle/models.h"
#include "magitrickle/yamlio.h"
#ifdef MT_ENTWARE_KN
#include "magitrickle/keenetic_policy_rci.h"
#include "magitrickle/port_remap.h"
#include "magitrickle/netfilter_cleaner.h"
#include "fake_iptables.h"
#endif

TEST policy_config_is_platform_specific(void)
{
    const char *yaml = "configVersion: 0.8\napp: {keeneticPolicy: {enabled: true, policyId: Policy0, mode: exclude}}\n";
    mt_config_t c; ASSERT_EQ(MT_OK, mt_config_init_defaults(&c));
#ifdef MT_ENTWARE_KN
    ASSERT_FALSE(c.app.keenetic_policy.enabled);
    ASSERT_EQ(MT_KN_POLICY_INCLUDE, c.app.keenetic_policy.mode);
#endif
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&c, yaml, strlen(yaml)));
    char *out = NULL; size_t len;
    ASSERT_EQ(MT_OK, mt_config_save_buffer(&c, "0.8", &out, &len));
#ifdef MT_ENTWARE_KN
    ASSERT(c.app.keenetic_policy.enabled);
    ASSERT_STR_EQ("Policy0", c.app.keenetic_policy.policy_id);
    ASSERT_EQ(MT_KN_POLICY_EXCLUDE, c.app.keenetic_policy.mode);
    ASSERT(strstr(out, "keeneticPolicy"));
    ASSERT_FALSE(strstr(out, "mark:"));
    mt_config_t copy; ASSERT_EQ(MT_OK, mt_config_init_defaults(&copy));
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&copy, out, len));
    ASSERT(mt_kn_policy_config_equal(&c.app.keenetic_policy, &copy.app.keenetic_policy));
    mt_config_clear(&copy);
#else
    ASSERT_FALSE(strstr(out, "keeneticPolicy"));
    ASSERT_FALSE(strstr(out, "Policy0"));
#endif
    free(out); mt_config_clear(&c); PASS();
}
#ifdef MT_ENTWARE_KN
TEST policy_json_handles_ids_names_and_hex_marks(void)
{
    const char *json = "{\"Policy0\":{\"description\":\"Моя VPN\",\"mark\":\"ffffd00\",\"route4\":{}},"
        "\"Policy1\":{\"description\":\"Моя VPN\",\"mark\":\"0x0FFFFD01\"},\"Zero\":{\"mark\":0},\"Waiting\":{}}";
    mt_kn_policy_entry_t *entries = NULL; size_t count;
    ASSERT_EQ(MT_OK, mt_kn_policy_parse(json, strlen(json), &entries, &count));
    ASSERT_EQ(4u, count);
    ASSERT_STR_EQ("Policy0", entries[0].id); ASSERT_STR_EQ("Моя VPN", entries[0].description);
    ASSERT_EQ(UINT32_C(0xffffd00), entries[0].mark); ASSERT(entries[0].available);
    ASSERT_EQ(UINT32_C(0xffffd01), entries[1].mark);
    ASSERT(entries[2].available); ASSERT_EQ(0u, entries[2].mark);
    ASSERT_FALSE(entries[3].available);
    free(entries);
    ASSERT_EQ(MT_OK, mt_kn_policy_parse("{} \n", 4, &entries, &count));
    ASSERT_EQ(0u, count); ASSERT(entries == NULL); free(entries); PASS();
}
TEST malformed_or_ambiguous_inventory_never_becomes_an_empty_success(void)
{
    const char *bad[] = {
        "[]", "{", "{}garbage", "{\"status\":[{\"status\":\"error\"}]}",
        "{\"P\":{},\"P\":{}}", "{\"P\":42}", "{\"\":{}}",
        "{\"P\":{\"mark\":\"0x\"}}", "{\"P\":{\"mark\":\"-1\"}}",
        "{\"P\":{\"mark\":\"zzzz\"}}", "{\"P\":{\"mark\":\"ffffffff\"}}",
        "{\"P\":{\"mark\":\" 0\"}}", "{\"P\":{\"mark\":1.5}}",
        "{\"P\":{\"mark\":-1}}", "{\"P\":{\"mark\":true}}",
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); i++) {
        mt_kn_policy_entry_t *entries = NULL; size_t count = 42;
        ASSERT(mt_kn_policy_parse(bad[i], strlen(bad[i]), &entries, &count) != MT_OK);
        ASSERT_EQ(0u, count); ASSERT(entries == NULL);
    }
    mt_kn_policy_entry_t *entries = NULL; size_t count;
    ASSERT_EQ(MT_ERR_LIMIT, mt_kn_policy_parse("{}", MT_KN_POLICY_BODY_LIMIT + 1, &entries, &count));
    PASS();
}
TEST invalid_saved_configuration_is_rejected(void)
{
    const char *bad[] = {
        "enabled: true, mode: include", "enabled: yes, policyId: ''",
        "enabled: true, policyId: P, mode: other", "enabled: 1, policyId: P",
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); i++) {
        char yaml[512]; snprintf(yaml, sizeof(yaml), "configVersion: 0.8\napp: {keeneticPolicy: {%s}}\n", bad[i]);
        mt_config_t c; ASSERT_EQ(MT_OK, mt_config_init_defaults(&c));
        ASSERT_EQ(MT_ERR_INVAL, mt_config_load_buffer(&c, yaml, strlen(yaml)));
        mt_config_clear(&c);
    }
    PASS();
}
TEST dns_guards_and_ordered_dispatch_compile_for_both_families(void)
{
    mt_kn_policy_config_t config = {.enabled = true, .policy_id = "Policy0"};
    for (unsigned family = 0; family < 2; family++) {
        for (unsigned mode = 0; mode < 3; mode++) {
            config.mode = mode == 1 ? MT_KN_POLICY_EXCLUDE : MT_KN_POLICY_INCLUDE;
            mt_kn_policy_scope_t scope;
            ASSERT_EQ(MT_OK, mt_kn_policy_scope_init(&scope, &config, "MT_"));
            scope.ready = mode != 2; scope.mark = 0xffffd00;
            mt_fake_ipt_t *f = mt_fake_ipt_new(family ? MT_IPT_PROTO_IPV6 : MT_IPT_PROTO_IPV4); ASSERT(f);
            mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(f)); ASSERT(ipt);
            ASSERT_EQ(MT_OK, mt_netfilter_register_base_chains(family ? NULL : ipt, family ? ipt : NULL));
            ASSERT_EQ(MT_OK, mt_kn_policy_prepare(&scope, ipt));
            const char *a[] = {"-j", "MT_group"}, *b[] = {"-j", "MT_subscription"};
            ASSERT_EQ(MT_OK, mt_ipt_register_chain_override(ipt, "mangle", "MT_group"));
            ASSERT_EQ(MT_OK, mt_ipt_register_chain_override(ipt, "mangle", "MT_subscription"));
            ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", scope.dispatch, 600, a, 2));
            ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", scope.dispatch, 200, b, 2));
            mt_remap_addr_t address = {.ip = {192,168,1,1}, .iplen = 4};
            if (family) { memset(address.ip, 0, sizeof(address.ip)); address.ip[15] = 1; address.iplen = 16; }
            mt_port_remap_t *remap = mt_port_remap_new("MT_", 53, 5353, &address, 1, family ? NULL : ipt, family ? ipt : NULL); ASSERT(remap);
            mt_port_remap_set_policy(remap, &scope);
            ASSERT_EQ(MT_OK, mt_port_remap_enable(remap));
            mt_ipt_rule_t *const *rules = NULL; size_t count;
            ASSERT(mt_fake_ipt_get_rules(f, "mangle", scope.dispatch, &rules, &count));
            ASSERT_EQ(3u, count); ASSERT_STR_EQ("MT_subscription", rules[0]->parts[1]); ASSERT_STR_EQ("MT_group", rules[1]->parts[1]);
            ASSERT(mt_fake_ipt_get_rules(f, "nat", "MT_DNSOR", &rules, &count));
            ASSERT_EQ(3u, count);
            ASSERT_STR_EQ("RETURN", rules[0]->parts[rules[0]->n_parts - 1]);
            ASSERT_EQ(mode == 2 ? 2u : mode == 1 ? 6u : 7u, rules[0]->n_parts);
            if (mode != 2) { ASSERT_STR_EQ("0xffffd00/0x0fffffff", rules[0]->parts[mode == 1 ? 3 : 4]); }
            ASSERT_EQ(MT_OK, mt_port_remap_disable(remap)); mt_port_remap_free(remap); mt_ipt_free(ipt);
        }
    }
    PASS();
}
#endif
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(policy_config_is_platform_specific);
#ifdef MT_ENTWARE_KN
    RUN_TEST(policy_json_handles_ids_names_and_hex_marks);
    RUN_TEST(malformed_or_ambiguous_inventory_never_becomes_an_empty_success);
    RUN_TEST(invalid_saved_configuration_is_rejected);
    RUN_TEST(dns_guards_and_ordered_dispatch_compile_for_both_families);
#endif
    GREATEST_MAIN_END();
}
