/* The production runtime converter and ordering key feed the real iptables
 * compiler. Fake save/restore checks both families without kernel privileges. */
#include "greatest.h"
#include "fake_iptables.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "magitrickle/models.h"
#include "magitrickle/sub_runtime.h"

static struct {
    mt_fake_ipt_t *fake[2];
    mt_ipt_t *ipt[2];
    mt_group_t *group;
    mt_group_t *subscription;
} fixture;

static const char *const group_jump[] = {"-j", "TST_00000001"};
static const char *const subscription_jump[] = {"-j", "TST_ffffffff"};

static void fixture_setup(void *unused) {
    (void)unused;
    memset(&fixture, 0, sizeof(fixture));
    fixture.group = mt_group_new();
    assert(fixture.group);
    assert(mt_id_parse("00000001", &fixture.group->id) == MT_OK);
    assert(mt_strset(&fixture.group->name, "Same name") == MT_OK);

    mt_subscription_t sub = {
        .name = "Same name", .iface = "vpn_subscription", .profile = "shared",
        .priority = MT_GROUP_DEFAULT_PRIORITY, .enable = true,
    };
    assert(mt_id_parse("ffffffff", &sub.id) == MT_OK);
    fixture.subscription = mt_sub_runtime_group(&sub);
    assert(fixture.subscription);
    for (unsigned family = 0; family < 2; family++) {
        fixture.fake[family] = mt_fake_ipt_new(family == 0 ? MT_IPT_PROTO_IPV4 : MT_IPT_PROTO_IPV6);
        assert(fixture.fake[family]);
        fixture.ipt[family] = mt_ipt_new(mt_fake_ipt_as_executable(fixture.fake[family]));
        assert(fixture.ipt[family]);
        assert(mt_ipt_register_chain_patch(fixture.ipt[family], "mangle", "PREROUTING") == MT_OK);
    }
}

static void fixture_teardown(void *unused) {
    (void)unused;
    mt_group_free(fixture.group);
    mt_group_free(fixture.subscription);
    for (unsigned family = 0; family < 2; family++) { mt_ipt_free(fixture.ipt[family]); }
}

static mt_err_t stage(unsigned family, const mt_group_t *group) {
    char id[MT_ID_STR_LEN];
    char chain[4 + MT_ID_STR_LEN];
    mt_id_format(group->id, id);
    snprintf(chain, sizeof(chain), "TST_%s", id);
    const char *parts[] = {"-j", chain};
    return mt_ipt_append_ordered(fixture.ipt[family], "mangle", "PREROUTING",
                                 mt_group_routing_order(group), parts, 2);
}

static bool has_order(unsigned family, const char *first, const char *second) {
    mt_ipt_rule_t *const *rules = NULL;
    size_t n = 0;
    if (!mt_fake_ipt_get_rules(fixture.fake[family], "mangle", "PREROUTING", &rules, &n) ||
        n != (second ? 2u : 1u)) { return false; }
    return rules[0]->n_parts == 2 && strcmp(rules[0]->parts[1], first) == 0 &&
           (!second || (rules[1]->n_parts == 2 && strcmp(rules[1]->parts[1], second) == 0));
}

static mt_err_t reset_firewall(unsigned family) {
    mt_fake_ipt_reset(fixture.fake[family]);
    mt_ipt_reset_staged(fixture.ipt[family]);
    return mt_ipt_register_chain_patch(fixture.ipt[family], "mangle", "PREROUTING");
}

TEST runtime_source_does_not_change_configured_priority(void) {
    ASSERT_FALSE(fixture.group->from_subscription);
    ASSERT(fixture.subscription->from_subscription);
    ASSERT_EQ(MT_GROUP_DEFAULT_PRIORITY, fixture.group->priority);
    ASSERT_EQ(MT_GROUP_DEFAULT_PRIORITY, fixture.subscription->priority);
    ASSERT_STR_EQ(fixture.group->name, fixture.subscription->name);
    ASSERT_STR_EQ("shared", fixture.subscription->profile);
    ASSERT(mt_group_routing_order(fixture.group) > mt_group_routing_order(fixture.subscription));
    ASSERT_EQ(MT_GROUP_DEFAULT_PRIORITY, fixture.group->priority);
    ASSERT_EQ(MT_GROUP_DEFAULT_PRIORITY, fixture.subscription->priority);
    PASS();
}

TEST numeric_priority_always_precedes_source_at_every_boundary(void) {
    for (unsigned p = MT_PRIORITY_MIN; p <= MT_PRIORITY_MAX; p++) {
        fixture.group->priority = (uint16_t)p;
        fixture.subscription->priority = (uint16_t)p;
        ASSERT(mt_group_routing_order(fixture.group) > mt_group_routing_order(fixture.subscription));
        if (p < MT_PRIORITY_MAX) {
            fixture.subscription->priority = (uint16_t)(p + 1u);
            ASSERT(mt_group_routing_order(fixture.subscription) > mt_group_routing_order(fixture.group));
        }
        ASSERT_EQ(p, fixture.group->priority);
    }
    PASS();
}

TEST group_beats_larger_subscription_id_in_either_arrival_order(void) {
    for (unsigned family = 0; family < 2; family++) {
        for (unsigned reverse = 0; reverse < 2; reverse++) {
            ASSERT_EQ(MT_OK, reset_firewall(family));
            ASSERT_EQ(MT_OK, stage(family, reverse ? fixture.subscription : fixture.group));
            ASSERT_EQ(MT_OK, mt_ipt_commit(fixture.ipt[family]));
            ASSERT_EQ(MT_OK, stage(family, reverse ? fixture.group : fixture.subscription));
            ASSERT_EQ(MT_OK, mt_ipt_commit(fixture.ipt[family]));
            /* The later MARK wins; the manual group has the SMALLER ID. */
            ASSERT(has_order(family, subscription_jump[1], group_jump[1]));
        }
    }
    PASS();
}

TEST live_numeric_override_and_return_to_equal_priority(void) {
    for (unsigned family = 0; family < 2; family++) {
        fixture.subscription->priority = fixture.group->priority;
        ASSERT_EQ(MT_OK, stage(family, fixture.group));
        ASSERT_EQ(MT_OK, stage(family, fixture.subscription));
        ASSERT_EQ(MT_OK, mt_ipt_commit(fixture.ipt[family]));
        ASSERT(has_order(family, subscription_jump[1], group_jump[1]));

        fixture.subscription->priority++;
        ASSERT_EQ(MT_OK, stage(family, fixture.subscription));
        ASSERT_EQ(MT_OK, mt_ipt_commit(fixture.ipt[family]));
        ASSERT(has_order(family, group_jump[1], subscription_jump[1]));

        fixture.subscription->priority = fixture.group->priority;
        ASSERT_EQ(MT_OK, stage(family, fixture.subscription));
        ASSERT_EQ(MT_OK, mt_ipt_commit(fixture.ipt[family]));
        ASSERT(has_order(family, subscription_jump[1], group_jump[1]));
    }
    PASS();
}

TEST source_tie_survives_disable_reenable_and_firewall_rebuild(void) {
    for (unsigned family = 0; family < 2; family++) {
        ASSERT_EQ(MT_OK, stage(family, fixture.group));
        ASSERT_EQ(MT_OK, stage(family, fixture.subscription));
        ASSERT_EQ(MT_OK, mt_ipt_commit(fixture.ipt[family]));
        ASSERT(has_order(family, subscription_jump[1], group_jump[1]));
        ASSERT_EQ(MT_OK, mt_ipt_delete(fixture.ipt[family], "mangle", "PREROUTING", group_jump, 2));
        ASSERT_EQ(MT_OK, mt_ipt_commit(fixture.ipt[family]));
        ASSERT(has_order(family, subscription_jump[1], NULL));
        ASSERT_EQ(MT_OK, stage(family, fixture.group));
        ASSERT_EQ(MT_OK, mt_ipt_commit(fixture.ipt[family]));
        ASSERT(has_order(family, subscription_jump[1], group_jump[1]));

        ASSERT_EQ(MT_OK, reset_firewall(family));
        ASSERT_EQ(MT_OK, stage(family, fixture.subscription));
        ASSERT_EQ(MT_OK, stage(family, fixture.group));
        ASSERT_EQ(MT_OK, mt_ipt_commit(fixture.ipt[family]));
        ASSERT(has_order(family, subscription_jump[1], group_jump[1]));
    }
    PASS();
}

TEST equal_priorities_within_one_source_keep_the_id_tie_break(void) {
    for (unsigned family = 0; family < 2; family++) {
        for (unsigned subscription = 0; subscription < 2; subscription++) {
            fixture.group->from_subscription = subscription != 0;
            fixture.subscription->from_subscription = subscription != 0;
            ASSERT_EQ(MT_OK, reset_firewall(family));
            ASSERT_EQ(MT_OK, stage(family, fixture.subscription));
            ASSERT_EQ(MT_OK, stage(family, fixture.group));
            ASSERT_EQ(MT_OK, mt_ipt_commit(fixture.ipt[family]));
            ASSERT(has_order(family, group_jump[1], subscription_jump[1]));
        }
    }
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    SET_SETUP(fixture_setup, NULL);
    SET_TEARDOWN(fixture_teardown, NULL);
    RUN_TEST(runtime_source_does_not_change_configured_priority);
    RUN_TEST(numeric_priority_always_precedes_source_at_every_boundary);
    RUN_TEST(group_beats_larger_subscription_id_in_either_arrival_order);
    RUN_TEST(live_numeric_override_and_return_to_equal_priority);
    RUN_TEST(source_tie_survives_disable_reenable_and_firewall_rebuild);
    RUN_TEST(equal_priorities_within_one_source_keep_the_id_tie_break);
    GREATEST_MAIN_END();
}
