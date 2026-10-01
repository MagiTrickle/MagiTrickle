/* Unit tests for the high-level mt_ipset_t wrapper (src/netfilter/ipset.c)
 * against the fake in-memory transport (fake_ipset_nl.c). Exercises the
 * Enable/Disable/Add/Del/List gating logic ported from
 * utils/netfilterTools/ipset.go. The real socket path cannot be exercised
 * here because this sandbox has no `ip_set` kernel module; its wire parser
 * is covered separately by test_ipset_nl_parser.c.
 */
#include "greatest.h"

#include "fake_ipset_nl.h"
#include "magitrickle/ipset.h"

#include <string.h>

TEST enable_creates_both_families(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new();
    mt_ipset_t *r = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "mt_g1");

    ASSERT_EQ(MT_OK, mt_ipset_enable(r));
    ASSERT(mt_ipset_enabled(r));
    ASSERT(mt_fake_ipset_nl_set_exists(fake, "mt_g1_4"));
    ASSERT(mt_fake_ipset_nl_set_exists(fake, "mt_g1_6"));

    size_t n;
    const fake_ipset_call_t *calls = mt_fake_ipset_nl_calls(fake, &n);
    /* enable() destroys-then-creates both families (matches Go's
     * enable(): ipsetDestroy() then ipsetCreate()). */
    ASSERT_EQ(4u, (unsigned)n);
    ASSERT_EQ(FAKE_IPSET_CALL_DESTROY, calls[0].kind);
    ASSERT_STR_EQ("mt_g1_4", calls[0].name);
    ASSERT_EQ(FAKE_IPSET_CALL_DESTROY, calls[1].kind);
    ASSERT_STR_EQ("mt_g1_6", calls[1].name);
    ASSERT_EQ(FAKE_IPSET_CALL_CREATE, calls[2].kind);
    ASSERT_STR_EQ("mt_g1_4", calls[2].name);
    ASSERT_EQ(FAKE_IPSET_CALL_CREATE, calls[3].kind);
    ASSERT_STR_EQ("mt_g1_6", calls[3].name);

    mt_ipset_free(r);
    PASS();
}

TEST enable_is_noop_when_already_enabled(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new();
    mt_ipset_t *r = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "mt_g1");

    ASSERT_EQ(MT_OK, mt_ipset_enable(r));
    size_t n1;
    mt_fake_ipset_nl_calls(fake, &n1);
    ASSERT_EQ(MT_OK, mt_ipset_enable(r)); /* Go: CompareAndSwap(false,true) fails -> nil */
    size_t n2;
    mt_fake_ipset_nl_calls(fake, &n2);
    ASSERT_EQ(n1, n2);

    mt_ipset_free(r);
    PASS();
}

TEST disable_destroys_both_and_resets_enabled(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new();
    mt_ipset_t *r = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "mt_g1");
    mt_ipset_enable(r);

    ASSERT_EQ(MT_OK, mt_ipset_disable(r));
    ASSERT_FALSE(mt_ipset_enabled(r));
    ASSERT_FALSE(mt_fake_ipset_nl_set_exists(fake, "mt_g1_4"));
    ASSERT_FALSE(mt_fake_ipset_nl_set_exists(fake, "mt_g1_6"));

    mt_ipset_free(r);
    PASS();
}

TEST add_noop_while_disabled(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new();
    mt_ipset_t *r = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "mt_g1");

    mt_ipv4_subnet_t subnet = {.addr = {192, 168, 1, 0}, .cidr = 24};
    ASSERT_EQ(MT_OK, mt_ipset_add4(r, subnet, NULL));

    size_t n;
    mt_fake_ipset_nl_calls(fake, &n);
    ASSERT_EQ(0u, (unsigned)n); /* never reaches the transport while disabled */

    mt_ipset_free(r);
    PASS();
}

TEST add4_nil_timeout_sends_zero(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new();
    mt_ipset_t *r = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "mt_g1");
    mt_ipset_enable(r);

    mt_ipv4_subnet_t subnet = {.addr = {10, 0, 0, 0}, .cidr = 8};
    ASSERT_EQ(MT_OK, mt_ipset_add4(r, subnet, NULL));

    size_t n;
    const fake_ipset_call_t *calls = mt_fake_ipset_nl_calls(fake, &n);
    const fake_ipset_call_t *add = &calls[n - 1];
    ASSERT_EQ(FAKE_IPSET_CALL_ADD, add->kind);
    ASSERT_STR_EQ("mt_g1_4", add->name);
    ASSERT(add->has_timeout); /* Go: nil -> zeroTimeout, always sent explicitly */
    ASSERT_EQ(0u, (unsigned)add->timeout);
    ASSERT(add->replace); /* Go: AddIPv4Subnet always passes Replace: true */
    ASSERT_EQ(8, add->cidr);
    ASSERT_EQ(0, memcmp(add->ip, subnet.addr, 4));

    mt_ipset_free(r);
    PASS();
}

TEST add4_explicit_timeout_passed_through(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new();
    mt_ipset_t *r = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "mt_g1");
    mt_ipset_enable(r);

    mt_ipv4_subnet_t subnet = {.addr = {1, 2, 3, 4}, .cidr = 32};
    uint32_t ttl = 3900;
    ASSERT_EQ(MT_OK, mt_ipset_add4(r, subnet, &ttl));

    size_t n;
    const fake_ipset_call_t *calls = mt_fake_ipset_nl_calls(fake, &n);
    const fake_ipset_call_t *add = &calls[n - 1];
    ASSERT(add->has_timeout);
    ASSERT_EQ(3900u, (unsigned)add->timeout);

    ASSERT_EQ(1u, (unsigned)mt_fake_ipset_nl_entry_count(fake, "mt_g1_4"));

    mt_ipset_free(r);
    PASS();
}

TEST add6_targets_the_6_suffixed_set(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new();
    mt_ipset_t *r = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "mt_g1");
    mt_ipset_enable(r);

    mt_ipv6_subnet_t subnet = {.cidr = 128};
    subnet.addr[15] = 1; /* ::1 */
    ASSERT_EQ(MT_OK, mt_ipset_add6(r, subnet, NULL));

    size_t n;
    const fake_ipset_call_t *calls = mt_fake_ipset_nl_calls(fake, &n);
    const fake_ipset_call_t *add = &calls[n - 1];
    ASSERT_STR_EQ("mt_g1_6", add->name);
    ASSERT_EQ(16, add->iplen);

    mt_ipset_free(r);
    PASS();
}

TEST del4_removes_entry(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new();
    mt_ipset_t *r = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "mt_g1");
    mt_ipset_enable(r);

    mt_ipv4_subnet_t subnet = {.addr = {8, 8, 8, 8}, .cidr = 32};
    mt_ipset_add4(r, subnet, NULL);
    ASSERT_EQ(1u, (unsigned)mt_fake_ipset_nl_entry_count(fake, "mt_g1_4"));

    ASSERT_EQ(MT_OK, mt_ipset_del4(r, subnet));
    ASSERT_EQ(0u, (unsigned)mt_fake_ipset_nl_entry_count(fake, "mt_g1_4"));

    mt_ipset_free(r);
    PASS();
}

TEST del4_of_missing_entry_is_ok(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new();
    mt_ipset_t *r = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "mt_g1");
    mt_ipset_enable(r);

    mt_ipv4_subnet_t subnet = {.addr = {9, 9, 9, 9}, .cidr = 32};
    ASSERT_EQ(MT_OK, mt_ipset_del4(r, subnet)); /* never added -- must not error */

    mt_ipset_free(r);
    PASS();
}

TEST list4_reflects_added_entries(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new();
    mt_ipset_t *r = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "mt_g1");
    mt_ipset_enable(r);

    mt_ipv4_subnet_t s1 = {.addr = {1, 1, 1, 1}, .cidr = 32};
    mt_ipv4_subnet_t s2 = {.addr = {2, 2, 2, 0}, .cidr = 24};
    uint32_t ttl = 100;
    mt_ipset_add4(r, s1, NULL);
    mt_ipset_add4(r, s2, &ttl);

    mt_ipset_entry4_t *entries = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_ipset_list4(r, &entries, &n));
    ASSERT_EQ(2u, (unsigned)n);

    bool found1 = false, found2 = false;
    for (size_t i = 0; i < n; i++) {
        if (memcmp(entries[i].subnet.addr, s1.addr, 4) == 0 && entries[i].subnet.cidr == 32) {
            found1 = true;
            ASSERT(entries[i].has_timeout);
            ASSERT_EQ(0u, (unsigned)entries[i].timeout);
        }
        if (memcmp(entries[i].subnet.addr, s2.addr, 4) == 0 && entries[i].subnet.cidr == 24) {
            found2 = true;
            ASSERT_EQ(100u, (unsigned)entries[i].timeout);
        }
    }
    ASSERT(found1);
    ASSERT(found2);

    free(entries);
    mt_ipset_free(r);
    PASS();
}

TEST list4_empty_while_disabled(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new();
    mt_ipset_t *r = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "mt_g1");

    mt_ipset_entry4_t *entries = (void *)0x1; /* sentinel to prove it gets nulled */
    size_t n = 999;
    ASSERT_EQ(MT_OK, mt_ipset_list4(r, &entries, &n));
    ASSERT(entries == NULL);
    ASSERT_EQ(0u, (unsigned)n);

    mt_ipset_free(r);
    PASS();
}

TEST enable_propagates_create_failure(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new();
    mt_ipset_t *r = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "mt_g1");

    mt_fake_ipset_nl_fail_next(fake, FAKE_IPSET_CALL_CREATE, MT_ERR_SYS);
    ASSERT_EQ(MT_ERR_SYS, mt_ipset_enable(r));
    ASSERT_FALSE(mt_ipset_enabled(r)); /* Go: enable() leaves enabled=false on failure */

    mt_ipset_free(r);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(enable_creates_both_families);
    RUN_TEST(enable_is_noop_when_already_enabled);
    RUN_TEST(disable_destroys_both_and_resets_enabled);
    RUN_TEST(add_noop_while_disabled);
    RUN_TEST(add4_nil_timeout_sends_zero);
    RUN_TEST(add4_explicit_timeout_passed_through);
    RUN_TEST(add6_targets_the_6_suffixed_set);
    RUN_TEST(del4_removes_entry);
    RUN_TEST(del4_of_missing_entry_is_ok);
    RUN_TEST(list4_reflects_added_entries);
    RUN_TEST(list4_empty_while_disabled);
    RUN_TEST(enable_propagates_create_failure);
    GREATEST_MAIN_END();
}
