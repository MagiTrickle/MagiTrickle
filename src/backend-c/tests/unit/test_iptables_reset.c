/* Full-rebuild reset is an in-memory operation, not a kernel flush. */
#include "greatest.h"

#include "fake_iptables.h"
#include "magitrickle/iptables.h"

#include <assert.h>
#include <string.h>

static mt_ipt_t *ipt;
static mt_fake_ipt_t *fake;
static mt_cancel_t *cancel;

static void setup(void *unused) {
    (void)unused;
    fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV6);
    assert(fake);
    ipt = mt_ipt_new(mt_fake_ipt_as_executable(fake));
    assert(ipt);
    cancel = mt_cancel_new();
    assert(cancel);
}

static void teardown(void *unused) {
    (void)unused;
    mt_ipt_free(ipt);
    mt_cancel_free(cancel);
}

TEST reset_discards_all_registration_kinds_without_kernel_changes(void) {
    const char *rule[] = {"-j", "RETURN"};
    const char *const *initial[] = {rule};
    const size_t lengths[] = {2};
    ASSERT_EQ(MT_OK, mt_fake_ipt_set_initial_rules(fake, "nat", "KEEP", initial, lengths, 1));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_delete(ipt, "nat", "KEEP"));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_patch(ipt, "filter", "FORWARD"));
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "filter", "FORWARD", rule, 2));
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_override(ipt, "nat", "MT_NEW"));
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "nat", "MT_NEW", rule, 2));

    mt_ipt_reset_staged(NULL);
    mt_ipt_reset_staged(ipt);
    mt_ipt_reset_staged(ipt);
    ASSERT_EQ(MT_ERR_STATE, mt_ipt_append(ipt, "nat", "KEEP", rule, 2));
    ASSERT_EQ(MT_ERR_STATE, mt_ipt_append(ipt, "nat", "MT_NEW", rule, 2));
    ASSERT_EQ(MT_ERR_STATE, mt_ipt_append(ipt, "filter", "FORWARD", rule, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    ASSERT(mt_fake_ipt_chain_exists(fake, "nat", "KEEP"));
    ASSERT(!mt_fake_ipt_chain_exists(fake, "nat", "MT_NEW"));
    ASSERT(!mt_fake_ipt_chain_exists(fake, "filter", "FORWARD"));
    mt_ipt_rule_t *const *rules = NULL;
    size_t count = 0;
    ASSERT(mt_fake_ipt_get_rules(fake, "nat", "KEEP", &rules, &count));
    ASSERT_EQ(1u, count);
    ASSERT_EQ(2u, rules[0]->n_parts);
    ASSERT_STR_EQ("RETURN", rules[0]->parts[1]);
    PASS();
}

TEST reset_keeps_transport_and_cancellation_token(void) {
    mt_ipt_set_cancel(ipt, cancel);
    mt_cancel_raise(cancel);
    mt_ipt_reset_staged(ipt);
    ASSERT_EQ(MT_IPT_PROTO_IPV6, mt_ipt_proto(ipt));
    ASSERT_EQ(MT_ERR_CANCELED, mt_ipt_commit(ipt));
    mt_cancel_clear(cancel);
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_override(ipt, "nat", "MT_AFTER_RESET"));
    const char *rule[] = {"-j", "RETURN"};
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "nat", "MT_AFTER_RESET", rule, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    ASSERT(mt_fake_ipt_chain_exists(fake, "nat", "MT_AFTER_RESET"));
    mt_ipt_set_cancel(ipt, NULL);
    PASS();
}

TEST ordinary_commit_still_retains_desired_state(void) {
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_override(ipt, "nat", "MT_RETAIN"));
    const char *rule[] = {"-j", "RETURN"};
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "nat", "MT_RETAIN", rule, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    mt_fake_ipt_reset(fake);
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    ASSERT(mt_fake_ipt_chain_exists(fake, "nat", "MT_RETAIN"));
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    SET_SETUP(setup, NULL);
    SET_TEARDOWN(teardown, NULL);
    RUN_TEST(reset_discards_all_registration_kinds_without_kernel_changes);
    RUN_TEST(reset_keeps_transport_and_cancellation_token);
    RUN_TEST(ordinary_commit_still_retains_desired_state);
    GREATEST_MAIN_END();
}
