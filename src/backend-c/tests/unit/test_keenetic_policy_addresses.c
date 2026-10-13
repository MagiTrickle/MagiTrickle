/* Regression for Keenetic 1.4.21 rejecting addrtype at mangle COMMIT.
 * Exercise the real app/remap lifecycle and policy compiler. Only the
 * getifaddrs boundary and xtables execution are replaced, not policy logic. */
#include "greatest.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <ifaddrs.h>
#include <stdlib.h>
#include <string.h>
#include "magitrickle/app.h"
#include "magitrickle/netfilter_cleaner.h"
#include "fake_iptables.h"

static struct {
    struct ifaddrs entries[8];
    struct sockaddr_in lan4, lo4;
    struct sockaddr_in6 lan6, lo6;
    struct sockaddr other;
    unsigned reads, frees;
    int error;
} addresses;

int __wrap_getifaddrs(struct ifaddrs **out);
void __wrap_freeifaddrs(struct ifaddrs *list);
int __wrap_getifaddrs(struct ifaddrs **out)
{
    addresses.reads++;
    *out = NULL;
    if (addresses.error) { errno = addresses.error; return -1; }
    *out = addresses.entries;
    return 0;
}
void __wrap_freeifaddrs(struct ifaddrs *list)
{
    assert(list == addresses.entries);
    addresses.frees++;
}
static void address_fixture(void)
{
    memset(&addresses, 0, sizeof(addresses));
    addresses.lan4.sin_family = addresses.lo4.sin_family = AF_INET;
    addresses.lan6.sin6_family = addresses.lo6.sin6_family = AF_INET6;
    addresses.other.sa_family = AF_PACKET;
    assert(inet_pton(AF_INET, "192.0.2.1", &addresses.lan4.sin_addr) == 1);
    assert(inet_pton(AF_INET, "127.0.0.1", &addresses.lo4.sin_addr) == 1);
    assert(inet_pton(AF_INET6, "fd00::1", &addresses.lan6.sin6_addr) == 1);
    assert(inet_pton(AF_INET6, "::1", &addresses.lo6.sin6_addr) == 1);
    struct sockaddr *items[] = {
        (struct sockaddr *)&addresses.lan4, (struct sockaddr *)&addresses.lan4,
        (struct sockaddr *)&addresses.lan6, (struct sockaddr *)&addresses.lan6,
        (struct sockaddr *)&addresses.lo4, (struct sockaddr *)&addresses.lo6,
        NULL, &addresses.other,
    };
    for (size_t i = 0; i < 8; i++) {
        addresses.entries[i].ifa_addr = items[i];
        addresses.entries[i].ifa_next = i + 1 < 8 ? &addresses.entries[i + 1] : NULL;
    }
}

typedef struct {
    mt_ipt_executable_t base;
    mt_ipt_executable_t *inner;
} legacy_t;
static bool contains(const uint8_t *data, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; n <= len && i <= len - n; i++) {
        if (memcmp(data + i, needle, n) == 0) { return true; }
    }
    return false;
}
static mt_err_t legacy_save(mt_ipt_executable_t *exe, uint8_t **data, size_t *len)
{
    legacy_t *self = (legacy_t *)exe;
    return self->inner->ops->save(self->inner, data, len);
}
static mt_err_t legacy_restore(mt_ipt_executable_t *exe, const uint8_t *data, size_t len)
{
    legacy_t *self = (legacy_t *)exe;
    /* Match the user's unsupported extension, even in an unreachable rule. */
    if (contains(data, len, "addrtype") || contains(data, len, "--dst-type")) { return MT_ERR_IO; }
    return self->inner->ops->restore(self->inner, data, len);
}
static mt_ipt_proto_t legacy_proto(mt_ipt_executable_t *exe)
{
    legacy_t *self = (legacy_t *)exe;
    return self->inner->ops->proto(self->inner);
}
static void legacy_destroy(mt_ipt_executable_t *exe)
{
    legacy_t *self = (legacy_t *)exe;
    mt_ipt_executable_free(self->inner);
    free(self);
}
static const mt_ipt_executable_ops_t legacy_ops = {
    .save = legacy_save, .restore = legacy_restore, .proto = legacy_proto, .destroy = legacy_destroy,
};
static mt_ipt_t *legacy_new(unsigned family, mt_fake_ipt_t **fake)
{
    *fake = mt_fake_ipt_new(family ? MT_IPT_PROTO_IPV6 : MT_IPT_PROTO_IPV4);
    assert(*fake);
    legacy_t *exe = calloc(1, sizeof(*exe)); assert(exe);
    exe->base.ops = &legacy_ops;
    exe->inner = mt_fake_ipt_as_executable(*fake);
    mt_ipt_t *ipt = mt_ipt_new(&exe->base); assert(ipt);
    return ipt;
}
static bool has_destination(mt_fake_ipt_t *fake, const char *destination, size_t *number)
{
    mt_ipt_rule_t *const *rules = NULL; size_t n = 0;
    *number = 0;
    if (!mt_fake_ipt_get_rules(fake, "mangle", "MT_POLICY", &rules, &n)) { return false; }
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        const mt_ipt_rule_t *r = rules[i];
        if (r->n_parts != 4 || strcmp(r->parts[0], "-d") != 0) { continue; }
        (*number)++;
        assert(strcmp(r->parts[2], "-j") == 0 && strcmp(r->parts[3], "RETURN") == 0);
        found |= strcmp(r->parts[1], destination) == 0;
    }
    return found;
}

TEST startup_and_both_modes_need_no_addrtype(void)
{
    address_fixture(); addresses.error = ENOMEM;
    mt_config_t cfg; ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    cfg.app.keenetic_policy = (mt_kn_policy_config_t){.enabled = true, .policy_id = "Policy0"};
    mt_fake_ipt_t *fake[2];
    mt_ipt_t *ipt[2] = {legacy_new(0, &fake[0]), legacy_new(1, &fake[1])};
    ASSERT_EQ(MT_OK, mt_netfilter_register_base_chains(ipt[0], ipt[1]));
    mt_cache_t *cache = mt_cache_create(0); ASSERT(cache);
    mt_app_deps_t deps = {.cfg = &cfg, .cache = cache, .ipt4 = ipt[0], .ipt6 = ipt[1]};
    mt_app_t *app = mt_app_create(&deps); ASSERT(app);
    const mt_remap_addr_t local[] = {
        {.family = AF_INET, .ip = {192,0,2,1}, .iplen = 4},
        {.family = AF_INET6, .ip = {0xfd,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}, .iplen = 16},
    };
    mt_port_remap_t *remap = mt_port_remap_new("MT_", 53, 3553, local, 2, ipt[0], ipt[1]); ASSERT(remap);
    mt_app_set_port_remap(app, remap);
    /* Exact failing startup path from the router: RCI is not ready yet. */
    ASSERT_EQ(MT_OK, mt_port_remap_enable(remap));
    ASSERT_EQ(0u, addresses.reads);
    addresses.error = 0;
    for (unsigned mode = 0; mode < 2; mode++) {
        mt_kn_policy_config_t policy = cfg.app.keenetic_policy;
        policy.mode = mode ? MT_KN_POLICY_EXCLUDE : MT_KN_POLICY_INCLUDE;
        ASSERT_EQ(MT_OK, mt_app_policy_set(app, &policy, true, 0xffffd00));
        for (unsigned family = 0; family < 2; family++) {
            size_t n = 0;
            ASSERT(has_destination(fake[family], family ? "fd00::1/128" : "192.0.2.1/32", &n));
            ASSERT_EQ(2u, n);
            mt_ipt_rule_t *const *rules = NULL;
            ASSERT(mt_fake_ipt_get_rules(fake[family], "nat", "MT_DNSOR", &rules, &n));
            ASSERT_EQ(3u, n);
            ASSERT_STR_EQ("mark", rules[0]->parts[1]);
            ASSERT_STR_EQ("RETURN", rules[0]->parts[rules[0]->n_parts - 1]);
        }
    }
    /* RCI loss closes scope again, even if enumeration now fails. */
    unsigned reads = addresses.reads; addresses.error = ENOMEM;
    mt_kn_policy_config_t policy = cfg.app.keenetic_policy;
    ASSERT_EQ(MT_OK, mt_app_policy_set(app, &policy, false, 0));
    ASSERT_EQ(reads, addresses.reads);
    policy.enabled = false;
    ASSERT_EQ(MT_OK, mt_app_policy_set(app, &policy, false, 0));
    ASSERT_EQ(reads, addresses.reads);
    ASSERT_EQ(MT_OK, mt_port_remap_disable(remap));
    mt_app_set_port_remap(app, NULL);
    mt_port_remap_free(remap); mt_app_destroy(app);
    mt_cache_destroy(cache); mt_ipt_free(ipt[0]); mt_ipt_free(ipt[1]); mt_config_clear(&cfg);
    PASS();
}

TEST exact_addresses_are_deduplicated_and_refreshed(void)
{
    address_fixture();
    mt_kn_policy_config_t config = {.enabled = true, .policy_id = "Policy0"};
    mt_kn_policy_scope_t scope;
    ASSERT_EQ(MT_OK, mt_kn_policy_scope_init(&scope, &config, "MT_"));
    scope.ready = true; scope.mark = 0xffffd00;
    for (unsigned family = 0; family < 2; family++) {
        mt_fake_ipt_t *fake;
        mt_ipt_t *ipt = legacy_new(family, &fake);
        ASSERT_EQ(MT_OK, mt_netfilter_register_base_chains(family ? NULL : ipt, family ? ipt : NULL));
        ASSERT_EQ(MT_OK, mt_kn_policy_prepare(&scope, ipt));
        ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
        size_t number;
        ASSERT(has_destination(fake, family ? "::1/128" : "127.0.0.1/32", &number));
        ASSERT_EQ(2u, number); /* duplicates, NULL and AF_PACKET are omitted */
        ASSERT(has_destination(fake, family ? "fd00::1/128" : "192.0.2.1/32", &number));
        if (family) { ASSERT_EQ(1, inet_pton(AF_INET6, "fd00::99", &addresses.lan6.sin6_addr)); }
        else { ASSERT_EQ(1, inet_pton(AF_INET, "192.0.2.99", &addresses.lan4.sin_addr)); }
        mt_ipt_reset_staged(ipt);
        ASSERT_EQ(MT_OK, mt_netfilter_register_base_chains(family ? NULL : ipt, family ? ipt : NULL));
        ASSERT_EQ(MT_OK, mt_kn_policy_prepare(&scope, ipt));
        ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
        ASSERT(has_destination(fake, family ? "fd00::99/128" : "192.0.2.99/32", &number));
        ASSERT_FALSE(has_destination(fake, family ? "fd00::1/128" : "192.0.2.1/32", &number));
        ASSERT_EQ(2u, number);
        /* Failure must not be mistaken for an empty successful inventory. */
        addresses.error = ENOMEM;
        ASSERT_EQ(MT_ERR_NOMEM, mt_kn_policy_prepare(&scope, ipt));
        addresses.error = 0;
        mt_ipt_free(ipt);
    }
    ASSERT_EQ(addresses.reads - 2u, addresses.frees);
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(startup_and_both_modes_need_no_addrtype);
    RUN_TEST(exact_addresses_are_deduplicated_and_refreshed);
    GREATEST_MAIN_END();
}
