#include "greatest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "magitrickle/profiles.h"
#include "magitrickle/yamlio.h"
#include "magitrickle/sub_runtime.h"

static const char *fixture =
    "configVersion: 0.8.2.2\n"
    "profiles:\n"
    "- id: vpn\n  name: VPN\n  interfaces: [tun0, tun1, tun2]\n  on_unavailable: blackhole\n"
    "groups:\n- id: aabbccdd\n  name: GitHub\n  interface: stale0\n  profile: vpn\n  enable: true\n"
    "subscriptions:\n- id: 11223344\n  name: Networks\n  profile: vpn\n  enable: true\n  url: https://example.test/list\n";

static mt_err_t load(mt_config_t *c, const char *text) {
    return mt_config_load_buffer(c, text, strlen(text));
}

TEST roundtrip_and_primary_shadow(void) {
    mt_config_t c = {0}, d = {0};
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&c));
    ASSERT_EQ(MT_OK, load(&c, fixture));
    ASSERT_EQ((size_t)1, c.n_profiles);
    ASSERT_EQ((size_t)3, c.profiles[0]->n_interfaces);
    ASSERT_STR_EQ("tun0", c.groups[0]->iface);
    ASSERT_STR_EQ("tun0", c.subscriptions[0]->iface);
    mt_group_t *synth = mt_sub_runtime_group(c.subscriptions[0]);
    ASSERT(synth != NULL); ASSERT(synth->enable); ASSERT_STR_EQ("vpn", synth->profile);
    mt_group_free(synth);
    /* Runtime failover must not overwrite configured values. Even a stale
     * compatibility shadow written by a hand editor serializes canonically. */
    ASSERT_EQ(MT_OK, mt_strset(&c.groups[0]->iface, "tun2"));
    char *out = NULL; size_t size = 0;
    ASSERT_EQ(MT_OK, mt_config_save_buffer(&c, "0.8.2.2", &out, &size));
    ASSERT_STR_EQ("tun2", c.groups[0]->iface); /* serializer is read-only */
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&d));
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&d, out, size));
    ASSERT_STR_EQ("tun0", d.groups[0]->iface);
    ASSERT_STR_EQ("vpn", d.groups[0]->profile);
    ASSERT_STR_EQ("vpn", d.subscriptions[0]->profile);
    ASSERT(strstr(out, "on_unavailable: blackhole") != NULL);
    free(out); mt_config_clear(&c); mt_config_clear(&d); PASS();
}

TEST old_config_remains_valid_and_does_not_gain_fields(void) {
    mt_config_t c = {0};
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&c));
    ASSERT_EQ(MT_OK, load(&c, "configVersion: 0.8\ngroups:\n- id: 11223344\n  interface: blackhole\n"));
    ASSERT_EQ((size_t)0, c.n_profiles);
    ASSERT_STR_EQ("blackhole", c.groups[0]->iface);
    char *out = NULL; size_t n = 0;
    ASSERT_EQ(MT_OK, mt_config_save_buffer(&c, "0.8", &out, &n));
    ASSERT(strstr(out, "profiles:") == NULL);
    ASSERT(strstr(out, "profile:") == NULL);
    free(out); mt_config_clear(&c); PASS();
}

TEST validation_rejects_bad_profiles_and_references(void) {
    const char *invalid[] = {
        "profiles: [{id: a, name: A, interfaces: []}]",
        "profiles: [{id: a, name: A, interfaces: [tun0, tun0]}]",
        "profiles: [{id: a, name: A, interfaces: [blackhole]}]",
        "profiles: [{id: a, name: A, interfaces: [eth+]}]",
        "profiles: [{id: a, name: A, interfaces: ['bad name']}]",
        "profiles: [{id: a, name: A, interfaces: [abcdefghijklmnop]}]",
        "profiles: [{id: a, name: '', interfaces: [tun0]}]",
        "profiles: [{id: a, name: A, interfaces: [tun0], on_unavailable: direct}]",
        "profiles: [{id: a, name: A, interfaces: [tun0]}, {id: a, name: B, interfaces: [tun1]}]",
        "groups: [{id: 11223344, profile: missing, interface: tun0}]",
        "subscriptions: [{id: 11223344, profile: missing, interface: tun0}]",
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        mt_config_t c = {0}; char buf[1024];
        ASSERT_EQ(MT_OK, mt_config_init_defaults(&c));
        snprintf(buf, sizeof(buf), "configVersion: 0.8\n%s\n", invalid[i]);
        ASSERT(load(&c, buf) != MT_OK);
        mt_config_clear(&c);
    }
    PASS();
}

TEST absent_profiles_overlay_and_explicit_clear(void) {
    mt_config_t c = {0}; ASSERT_EQ(MT_OK, mt_config_init_defaults(&c));
    ASSERT_EQ(MT_OK, load(&c, fixture));
    ASSERT_EQ(MT_OK, load(&c, "configVersion: 0.8\napp: {logLevel: debug}\n"));
    ASSERT_EQ((size_t)1, c.n_profiles);
    ASSERT_STR_EQ("tun0", c.groups[0]->iface);
    /* Removing a referenced profile does not reinterpret interface as a
     * route. Whole-document validation rejects it. */
    ASSERT_EQ(MT_ERR_NOENT, load(&c, "configVersion: 0.8\nprofiles: []\n"));
    mt_config_clear(&c);
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&c));
    ASSERT_EQ(MT_OK, load(&c, fixture));
    ASSERT_EQ(MT_OK, load(&c, "configVersion: 0.8\nprofiles: []\ngroups: []\nsubscriptions: []\n"));
    ASSERT_EQ((size_t)0, c.n_profiles); mt_config_clear(&c); PASS();
}

TEST many_profiles_and_candidates_have_no_fixed_count_limit(void) {
    mt_config_t c = {0}, d = {0}; ASSERT_EQ(MT_OK, mt_config_init_defaults(&c));
    for (size_t i = 0; i < 512; i++) {
        mt_profile_t *p = mt_profile_new(); ASSERT(p != NULL);
        char id[32]; snprintf(id, sizeof(id), "p_%zu", i);
        ASSERT_EQ(MT_OK, mt_strset(&p->id, id));
        ASSERT_EQ(MT_OK, mt_strset(&p->name, id));
        for (size_t j = 0; j < (i ? 1u : 128u); j++) {
            char name[16]; snprintf(name, sizeof(name), "tun%u", (unsigned)j);
            ASSERT_EQ(MT_OK, mt_profile_add_interface(p, name));
        }
        ASSERT_EQ(MT_OK, mt_config_add_profile(&c, p));
    }
    ASSERT_EQ(MT_OK, mt_profiles_validate(&c, NULL, 0));
    ASSERT_EQ(MT_OK, mt_config_clone_profiles(&d, &c));
    ASSERT_EQ((size_t)512, d.n_profiles);
    ASSERT_EQ((size_t)128, d.profiles[0]->n_interfaces);
    ASSERT_EQ(MT_OK, mt_profile_add_interface(d.profiles[0], "another0"));
    ASSERT_EQ((size_t)128, c.profiles[0]->n_interfaces);
    ASSERT_STR_EQ("tun127", d.profiles[0]->interfaces[127]);
    mt_config_clear(&c); mt_config_clear(&d); PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(roundtrip_and_primary_shadow);
    RUN_TEST(old_config_remains_valid_and_does_not_gain_fields);
    RUN_TEST(validation_rejects_bad_profiles_and_references);
    RUN_TEST(absent_profiles_overlay_and_explicit_clear);
    RUN_TEST(many_profiles_and_candidates_have_no_fixed_count_limit);
    GREATEST_MAIN_END();
}
