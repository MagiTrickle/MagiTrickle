/* Compile the production translation unit into this test, excluding its .o
 * in the Makefile. This exercises private reconciliation helpers without
 * exporting test-only APIs or needing a router's netfilter kernel modules. */
#include "greatest.h"
#include "fake_ipset_nl.h"
#include "../../src/netfilter/ruleset.c"

static void lists_clear(new4_list_t *a, new6_list_t *b) {
    free(a->items); free(b->items); mt_lookup_clear(&a->index); mt_lookup_clear(&b->index);
}
TEST fifty_thousand_subnet_entries_are_indexed_and_ordered(void) {
    new4_list_t a = {0}; new6_list_t b = {0};
    for (uint32_t i = 0; i < 50000; i++) {
        mt_ipv4_subnet_t key = {.addr = {10, (uint8_t)(i >> 8), (uint8_t)i, 0}, .cidr = 24};
        ASSERT_EQ(MT_OK, new4_upsert(&a, key, true, 60, false));
        ASSERT_EQ(MT_OK, new4_upsert(&a, key, true, 30, true));
        ASSERT_EQ(MT_OK, new4_upsert(&a, key, true, 90, true));
    }
    ASSERT_EQ(50000u, a.n); ASSERT_EQ(50000u, a.index.len);
    for (size_t i = 0; i < a.n; i++) {
        ASSERT_EQ((uint8_t)(i >> 8), a.items[i].subnet.addr[1]);
        ASSERT_EQ(90u, a.items[i].ttl);
    }
    ASSERT_EQ(MT_OK, new4_upsert(&a, a.items[0].subnet, false, 0, false));
    ASSERT_EQ(MT_OK, new4_upsert(&a, a.items[0].subnet, true, 999, true));
    ASSERT(!a.items[0].has_ttl);
    mt_ipv6_subnet_t six; ASSERT(parse_ipv6_rule("2001:db8::ffff/64", &six));
    ASSERT_EQ(MT_OK, new6_upsert(&b, six, true, 60, false));
    ASSERT_EQ(MT_OK, new6_upsert(&b, six, true, 120, true)); ASSERT_EQ(1u, b.n);
    ASSERT_EQ(120u, b.items[0].ttl); ASSERT_EQ(0u, b.items[0].subnet.addr[15]);
    lists_clear(&a, &b); PASS();
}
TEST indexed_diff_preserves_permanent_entries_and_updates_ttls(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new(); ASSERT(fake);
    mt_ipset_t *ipset = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "test"); ASSERT(ipset);
    ASSERT_EQ(MT_OK, mt_ipset_enable(ipset));
    mt_group_t group = {.name = "test", .enable = true};
    mt_ruleset_t rs = {.group = &group, .ipset = ipset, .enabled = true};
    mt_ipv4_subnet_t permanent = {.addr = {10, 0, 0, 0}, .cidr = 24};
    mt_ipv4_subnet_t timed = {.addr = {10, 1, 0, 0}, .cidr = 24};
    mt_ipv4_subnet_t obsolete = {.addr = {10, 2, 0, 0}, .cidr = 24};
    uint32_t ttl = 60;
    /* Simulate the kernel LIST representation of a permanent entry:
     * no timeout attribute, rather than the fake's literal ADD timeout=0. */
    mt_ipset_nl_t *transport = mt_fake_ipset_nl_as_transport(fake);
    ASSERT_EQ(MT_OK, transport->ops->add(transport, mt_ipset_name4(ipset), permanent.addr,
                                        4, permanent.cidr, false, 0, true));
    ASSERT_EQ(MT_OK, mt_ipset_add4(ipset, timed, &ttl));
    ASSERT_EQ(MT_OK, mt_ipset_add4(ipset, obsolete, NULL));
    size_t before; (void)mt_fake_ipset_nl_calls(fake, &before);
    new4_list_t a = {0}; new6_list_t b = {0};
    ASSERT_EQ(MT_OK, new4_upsert(&a, permanent, true, 120, false));
    ASSERT_EQ(MT_OK, new4_upsert(&a, timed, true, 90, false));
    mt_err_t err = MT_OK; sync_diff_v4(&rs, &a, &err); ASSERT_EQ(MT_OK, err);
    size_t count; const fake_ipset_call_t *calls = mt_fake_ipset_nl_calls(fake, &count);
    unsigned adds = 0, deletes = 0;
    for (size_t i = before; i < count; i++) {
        if (calls[i].kind == FAKE_IPSET_CALL_ADD) {
            adds++; ASSERT_EQ(1u, calls[i].ip[1]); ASSERT_EQ(90u, calls[i].timeout);
        }
        if (calls[i].kind == FAKE_IPSET_CALL_DEL) { deletes++; ASSERT_EQ(2u, calls[i].ip[1]); }
    }
    ASSERT_EQ(1u, adds); ASSERT_EQ(1u, deletes);
    /* A lower requested TTL does not shorten an existing entry. */
    a.items[1].ttl = 10;
    (void)mt_fake_ipset_nl_calls(fake, &before);
    sync_diff_v4(&rs, &a, &err); ASSERT_EQ(MT_OK, err);
    calls = mt_fake_ipset_nl_calls(fake, &count);
    for (size_t i = before; i < count; i++) { ASSERT_EQ(FAKE_IPSET_CALL_LIST, calls[i].kind); }
    lists_clear(&a, &b); mt_ipset_free(ipset); PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(fifty_thousand_subnet_entries_are_indexed_and_ordered);
    RUN_TEST(indexed_diff_preserves_permanent_entries_and_updates_ttls);
    GREATEST_MAIN_END();
}
