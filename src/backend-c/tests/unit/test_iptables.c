/* Port of utils/iptables/iptables_test.go: every case replayed against
 * the fake in-memory Executable backend (fake_iptables.c/.h), asserting
 * byte-for-byte-equivalent resulting rule tables to the Go expected
 * values in that file.
 */
#include "greatest.h"

#include "fake_iptables.h"
#include "magitrickle/iptables.h"

#include <string.h>

static bool rule_eq_strs(const mt_ipt_rule_t *r, const char *const *expected, size_t n_expected) {
    if (r->n_parts != n_expected) return false;
    for (size_t i = 0; i < n_expected; i++) {
        if (strcmp(r->parts[i], expected[i]) != 0) return false;
    }
    return true;
}

static bool rules_seq_eq(mt_ipt_rule_t *const *got, size_t n_got,
                         const char *const *const *expected, const size_t *expected_lens,
                         size_t n_expected) {
    if (n_got != n_expected) return false;
    for (size_t i = 0; i < n_expected; i++) {
        if (!rule_eq_strs(got[i], expected[i], expected_lens[i])) return false;
    }
    return true;
}

/* TestChainPatch: rules merge with what's already on the chain. */
TEST chain_patch_appends_after_existing(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    const char *r0[] = {"-i", "eth0", "-j", "ACCEPT"};
    const char *r1[] = {"-i", "eth1", "-j", "DROP"};
    const char *const *initial[] = {r0, r1};
    size_t initial_lens[] = {4, 4};
    ASSERT_EQ(MT_OK,
             mt_fake_ipt_set_initial_rules(fake, "filter", "FORWARD", initial, initial_lens, 2));

    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_patch(ipt, "filter", "FORWARD"));
    const char *newr[] = {"-i", "eth2", "-j", "ACCEPT"};
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "filter", "FORWARD", newr, 4));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    mt_ipt_rule_t *const *got;
    size_t n_got;
    ASSERT(mt_fake_ipt_get_rules(fake, "filter", "FORWARD", &got, &n_got));
    const char *const *expected[] = {r0, r1, newr};
    size_t expected_lens[] = {4, 4, 4};
    ASSERT(rules_seq_eq(got, n_got, expected, expected_lens, 3));

    mt_ipt_free(ipt);
    PASS();
}

/* TestChainPatchDelete */
TEST chain_patch_deletes_existing(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    const char *r22[] = {"-p", "tcp", "--dport", "22", "-j", "ACCEPT"};
    const char *r80[] = {"-p", "tcp", "--dport", "80", "-j", "ACCEPT"};
    const char *r443[] = {"-p", "tcp", "--dport", "443", "-j", "ACCEPT"};
    const char *const *initial[] = {r22, r80, r443};
    size_t initial_lens[] = {6, 6, 6};
    ASSERT_EQ(MT_OK,
             mt_fake_ipt_set_initial_rules(fake, "filter", "INPUT", initial, initial_lens, 3));

    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_patch(ipt, "filter", "INPUT"));
    ASSERT_EQ(MT_OK, mt_ipt_delete(ipt, "filter", "INPUT", r80, 6));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    mt_ipt_rule_t *const *got;
    size_t n_got;
    ASSERT(mt_fake_ipt_get_rules(fake, "filter", "INPUT", &got, &n_got));
    const char *const *expected[] = {r22, r443};
    size_t expected_lens[] = {6, 6};
    ASSERT(rules_seq_eq(got, n_got, expected, expected_lens, 2));

    mt_ipt_free(ipt);
    PASS();
}

/* TestChainPatchInsert */
TEST chain_patch_inserts_at_front(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    const char *r10[] = {"-d", "10.0.0.0/8", "-j", "ACCEPT"};
    const char *const *initial[] = {r10};
    size_t initial_lens[] = {4};
    ASSERT_EQ(MT_OK,
             mt_fake_ipt_set_initial_rules(fake, "filter", "OUTPUT", initial, initial_lens, 1));

    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_patch(ipt, "filter", "OUTPUT"));
    const char *r192[] = {"-d", "192.168.0.0/16", "-j", "ACCEPT"};
    ASSERT_EQ(MT_OK, mt_ipt_insert(ipt, "filter", "OUTPUT", 1, r192, 4));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    mt_ipt_rule_t *const *got;
    size_t n_got;
    ASSERT(mt_fake_ipt_get_rules(fake, "filter", "OUTPUT", &got, &n_got));
    const char *const *expected[] = {r192, r10};
    size_t expected_lens[] = {4, 4};
    ASSERT(rules_seq_eq(got, n_got, expected, expected_lens, 2));

    mt_ipt_free(ipt);
    PASS();
}

/* TestChainPatchNoDuplicates */
TEST chain_patch_no_duplicates(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    const char *r0[] = {"-i", "eth0", "-j", "ACCEPT"};
    const char *const *initial[] = {r0};
    size_t initial_lens[] = {4};
    ASSERT_EQ(MT_OK,
             mt_fake_ipt_set_initial_rules(fake, "filter", "FORWARD", initial, initial_lens, 1));

    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_patch(ipt, "filter", "FORWARD"));
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "filter", "FORWARD", r0, 4));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    mt_ipt_rule_t *const *got;
    size_t n_got;
    ASSERT(mt_fake_ipt_get_rules(fake, "filter", "FORWARD", &got, &n_got));
    const char *const *expected[] = {r0};
    size_t expected_lens[] = {4};
    ASSERT(rules_seq_eq(got, n_got, expected, expected_lens, 1));

    mt_ipt_free(ipt);
    PASS();
}

/* TestChainOverride */
TEST chain_override_replaces_all(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    const char *r80[] = {"-p", "tcp", "--dport", "80", "-j", "REDIRECT", "--to-port", "8080"};
    const char *r443[] = {"-p", "tcp", "--dport", "443", "-j", "REDIRECT", "--to-port", "8443"};
    const char *const *initial[] = {r80, r443};
    size_t initial_lens[] = {8, 8};
    ASSERT_EQ(MT_OK,
             mt_fake_ipt_set_initial_rules(fake, "nat", "PREROUTING", initial, initial_lens, 2));

    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_override(ipt, "nat", "PREROUTING"));
    const char *rudp[] = {"-p", "udp", "--dport", "53", "-j", "REDIRECT", "--to-port", "5353"};
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "nat", "PREROUTING", rudp, 8));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    mt_ipt_rule_t *const *got;
    size_t n_got;
    ASSERT(mt_fake_ipt_get_rules(fake, "nat", "PREROUTING", &got, &n_got));
    const char *const *expected[] = {rudp};
    size_t expected_lens[] = {8};
    ASSERT(rules_seq_eq(got, n_got, expected, expected_lens, 1));

    mt_ipt_free(ipt);
    PASS();
}

/* TestChainOverrideMultipleRules */
TEST chain_override_multiple_rules(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    const char *rold[] = {"-j", "OLD_CHAIN"};
    const char *const *initial[] = {rold};
    size_t initial_lens[] = {2};
    ASSERT_EQ(MT_OK,
             mt_fake_ipt_set_initial_rules(fake, "mangle", "PREROUTING", initial, initial_lens, 1));

    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_override(ipt, "mangle", "PREROUTING"));
    const char *m1[] = {"-j", "MARK", "--set-mark", "1"};
    const char *m2[] = {"-j", "MARK", "--set-mark", "2"};
    const char *m3[] = {"-j", "CONNMARK", "--save-mark"};
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "mangle", "PREROUTING", m1, 4));
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "mangle", "PREROUTING", m2, 4));
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "mangle", "PREROUTING", m3, 3));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    mt_ipt_rule_t *const *got;
    size_t n_got;
    ASSERT(mt_fake_ipt_get_rules(fake, "mangle", "PREROUTING", &got, &n_got));
    const char *const *expected[] = {m1, m2, m3};
    size_t expected_lens[] = {4, 4, 3};
    ASSERT(rules_seq_eq(got, n_got, expected, expected_lens, 3));

    mt_ipt_free(ipt);
    PASS();
}

/* TestChainOverrideNoChangeIfSame */
TEST chain_override_noop_if_unchanged(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    const char *racc[] = {"-j", "ACCEPT"};
    const char *const *initial[] = {racc};
    size_t initial_lens[] = {2};
    ASSERT_EQ(MT_OK,
             mt_fake_ipt_set_initial_rules(fake, "filter", "TEST", initial, initial_lens, 1));

    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_override(ipt, "filter", "TEST"));
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "filter", "TEST", racc, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    mt_ipt_rule_t *const *got;
    size_t n_got;
    ASSERT(mt_fake_ipt_get_rules(fake, "filter", "TEST", &got, &n_got));
    const char *const *expected[] = {racc};
    size_t expected_lens[] = {2};
    ASSERT(rules_seq_eq(got, n_got, expected, expected_lens, 1));

    mt_ipt_free(ipt);
    PASS();
}

/* TestChainDelete */
TEST chain_delete_removes_chain(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    const char *racc[] = {"-j", "ACCEPT"};
    const char *rdrop[] = {"-j", "DROP"};
    const char *const *initial[] = {racc, rdrop};
    size_t initial_lens[] = {2, 2};
    ASSERT_EQ(MT_OK,
             mt_fake_ipt_set_initial_rules(fake, "filter", "MY_CHAIN", initial, initial_lens, 2));

    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_delete(ipt, "filter", "MY_CHAIN"));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    ASSERT_FALSE(mt_fake_ipt_chain_exists(fake, "filter", "MY_CHAIN"));

    mt_ipt_free(ipt);
    PASS();
}

/* TestChainDeleteEmpty */
TEST chain_delete_removes_empty_chain(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    ASSERT_EQ(MT_OK, mt_fake_ipt_set_initial_rules(fake, "filter", "EMPTY_CHAIN", NULL, NULL, 0));

    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_delete(ipt, "filter", "EMPTY_CHAIN"));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    ASSERT_FALSE(mt_fake_ipt_chain_exists(fake, "filter", "EMPTY_CHAIN"));

    mt_ipt_free(ipt);
    PASS();
}

/* TestChainDeleteNonExistent */
TEST chain_delete_nonexistent_is_noop(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_delete(ipt, "filter", "NON_EXISTENT"));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    ASSERT_FALSE(mt_fake_ipt_chain_exists(fake, "filter", "NON_EXISTENT"));
    mt_ipt_free(ipt);
    PASS();
}

/* TestChainDeleteIgnoresAppend */
TEST chain_delete_ignores_mutations(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    const char *racc[] = {"-j", "ACCEPT"};
    const char *const *initial[] = {racc};
    size_t initial_lens[] = {2};
    ASSERT_EQ(MT_OK,
             mt_fake_ipt_set_initial_rules(fake, "filter", "TO_DELETE", initial, initial_lens, 1));

    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_delete(ipt, "filter", "TO_DELETE"));

    const char *rdrop[] = {"-j", "DROP"};
    const char *rlog[] = {"-j", "LOG"};
    (void)mt_ipt_append(ipt, "filter", "TO_DELETE", rdrop, 2);
    (void)mt_ipt_insert(ipt, "filter", "TO_DELETE", 1, rlog, 2);
    (void)mt_ipt_delete(ipt, "filter", "TO_DELETE", racc, 2);

    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    ASSERT_FALSE(mt_fake_ipt_chain_exists(fake, "filter", "TO_DELETE"));

    mt_ipt_free(ipt);
    PASS();
}

/* TestMixedChainTypes */
TEST mixed_chain_types_in_one_table(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    const char *racc[] = {"-j", "ACCEPT"};
    const char *rold[] = {"-j", "OLD_RULE"};
    const char *rsomething[] = {"-j", "SOMETHING"};
    {
        const char *const *initial[] = {racc};
        size_t lens[] = {2};
        ASSERT_EQ(MT_OK, mt_fake_ipt_set_initial_rules(fake, "filter", "INPUT", initial, lens, 1));
    }
    {
        const char *const *initial[] = {rold};
        size_t lens[] = {2};
        ASSERT_EQ(MT_OK,
                 mt_fake_ipt_set_initial_rules(fake, "filter", "FORWARD", initial, lens, 1));
    }
    {
        const char *const *initial[] = {rsomething};
        size_t lens[] = {2};
        ASSERT_EQ(MT_OK,
                 mt_fake_ipt_set_initial_rules(fake, "filter", "TO_DELETE", initial, lens, 1));
    }

    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));

    ASSERT_EQ(MT_OK, mt_ipt_register_chain_patch(ipt, "filter", "INPUT"));
    const char *rdrop[] = {"-j", "DROP"};
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "filter", "INPUT", rdrop, 2));

    ASSERT_EQ(MT_OK, mt_ipt_register_chain_override(ipt, "filter", "FORWARD"));
    const char *rnew[] = {"-j", "NEW_RULE"};
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "filter", "FORWARD", rnew, 2));

    ASSERT_EQ(MT_OK, mt_ipt_register_chain_delete(ipt, "filter", "TO_DELETE"));

    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    mt_ipt_rule_t *const *got;
    size_t n_got;
    ASSERT(mt_fake_ipt_get_rules(fake, "filter", "INPUT", &got, &n_got));
    {
        const char *const *expected[] = {racc, rdrop};
        size_t lens[] = {2, 2};
        ASSERT(rules_seq_eq(got, n_got, expected, lens, 2));
    }

    ASSERT(mt_fake_ipt_get_rules(fake, "filter", "FORWARD", &got, &n_got));
    {
        const char *const *expected[] = {rnew};
        size_t lens[] = {2};
        ASSERT(rules_seq_eq(got, n_got, expected, lens, 1));
    }

    ASSERT_FALSE(mt_fake_ipt_chain_exists(fake, "filter", "TO_DELETE"));

    mt_ipt_free(ipt);
    PASS();
}

/* TestMultipleCommits */
TEST multiple_commits_accumulate(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));

    ASSERT_EQ(MT_OK, mt_ipt_register_chain_override(ipt, "filter", "MY_CHAIN"));
    const char *racc[] = {"-j", "ACCEPT"};
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "filter", "MY_CHAIN", racc, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    mt_ipt_rule_t *const *got;
    size_t n_got;
    ASSERT(mt_fake_ipt_get_rules(fake, "filter", "MY_CHAIN", &got, &n_got));
    ASSERT_EQ(1u, (unsigned)n_got);
    ASSERT(rule_eq_strs(got[0], (const char *const[]){"-j", "ACCEPT"}, 2));

    const char *rdrop[] = {"-j", "DROP"};
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "filter", "MY_CHAIN", rdrop, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    ASSERT(mt_fake_ipt_get_rules(fake, "filter", "MY_CHAIN", &got, &n_got));
    const char *const *expected[] = {racc, rdrop};
    size_t lens[] = {2, 2};
    ASSERT(rules_seq_eq(got, n_got, expected, lens, 2));

    mt_ipt_free(ipt);
    PASS();
}

/* TestIPv6 */
TEST ipv6_rules(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV6);
    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    ASSERT_EQ(MT_IPT_PROTO_IPV6, mt_ipt_proto(ipt));

    ASSERT_EQ(MT_OK, mt_ipt_register_chain_override(ipt, "filter", "INPUT"));
    const char *r[] = {"-s", "::1", "-j", "ACCEPT"};
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "filter", "INPUT", r, 4));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    mt_ipt_rule_t *const *got;
    size_t n_got;
    ASSERT(mt_fake_ipt_get_rules(fake, "filter", "INPUT", &got, &n_got));
    const char *const *expected[] = {r};
    size_t lens[] = {4};
    ASSERT(rules_seq_eq(got, n_got, expected, lens, 1));

    mt_ipt_free(ipt);
    PASS();
}

/* TestErrorOnUninitializedChain */
TEST error_on_uninitialized_chain(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));

    const char *r[] = {"-j", "ACCEPT"};
    mt_err_t err = mt_ipt_append(ipt, "filter", "NONEXISTENT", r, 2);
    ASSERT(mt_ipt_err_is_chain_not_initialized(err));

    err = mt_ipt_insert(ipt, "filter", "NONEXISTENT", 1, r, 2);
    ASSERT(mt_ipt_err_is_chain_not_initialized(err));

    err = mt_ipt_delete(ipt, "filter", "NONEXISTENT", r, 2);
    ASSERT(mt_ipt_err_is_chain_not_initialized(err));

    mt_ipt_free(ipt);
    PASS();
}

/* TestPatchRemovesDuplicates */
TEST patch_removes_duplicates(void) {
    mt_fake_ipt_t *fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    const char *racc[] = {"-j", "ACCEPT"};
    const char *rdrop[] = {"-j", "DROP"};
    const char *const *initial[] = {racc, racc, rdrop};
    size_t lens[] = {2, 2, 2};
    ASSERT_EQ(MT_OK,
             mt_fake_ipt_set_initial_rules(fake, "filter", "FORWARD", initial, lens, 3));

    mt_ipt_t *ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_patch(ipt, "filter", "FORWARD"));
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "filter", "FORWARD", racc, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));

    mt_ipt_rule_t *const *got;
    size_t n_got;
    ASSERT(mt_fake_ipt_get_rules(fake, "filter", "FORWARD", &got, &n_got));
    const char *const *expected[] = {racc, rdrop};
    size_t explens[] = {2, 2};
    ASSERT(rules_seq_eq(got, n_got, expected, explens, 2));

    mt_ipt_free(ipt);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(chain_patch_appends_after_existing);
    RUN_TEST(chain_patch_deletes_existing);
    RUN_TEST(chain_patch_inserts_at_front);
    RUN_TEST(chain_patch_no_duplicates);
    RUN_TEST(chain_override_replaces_all);
    RUN_TEST(chain_override_multiple_rules);
    RUN_TEST(chain_override_noop_if_unchanged);
    RUN_TEST(chain_delete_removes_chain);
    RUN_TEST(chain_delete_removes_empty_chain);
    RUN_TEST(chain_delete_nonexistent_is_noop);
    RUN_TEST(chain_delete_ignores_mutations);
    RUN_TEST(mixed_chain_types_in_one_table);
    RUN_TEST(multiple_commits_accumulate);
    RUN_TEST(ipv6_rules);
    RUN_TEST(error_on_uninitialized_chain);
    RUN_TEST(patch_removes_duplicates);
    GREATEST_MAIN_END();
}
