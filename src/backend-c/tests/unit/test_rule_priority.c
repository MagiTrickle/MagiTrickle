/* Priority changes must reorder live mangle rules, not just the next rebuild.
 * Include the production link implementation to supply fake enabled links
 * with fixed marks. Route reads and reservation release are isolated from
 * the kernel; staging, disable, ipset membership and iptables compilation
 * are production paths.
 * The small packet evaluator below covers the generated mangle instructions;
 * it is not a replacement for the namespace/kernel integration suite. */
#include "greatest.h"
#include "fake_iptables.h"
#include "fake_ipset_nl.h"

#include <assert.h>
#include <stdint.h>

#include "magitrickle/models.h"
#include "magitrickle/netfilter_cleaner.h"
#include "magitrickle/ipset_to_link.h"

/* Profile reconciliation and teardown now inspect the actual route table.
 * Model an existing terminal blackhole and unavailable profile interfaces
 * so these mangle tests remain independent of privileged netlink access. */
static mt_err_t priority_get_default_route(mt_rtnl_t *rtnl, int family, uint32_t table,
                                           uint32_t metric, mt_rtnl_default_route_t *out);
static mt_err_t priority_link_by_name(mt_rtnl_t *rtnl, const char *name,
                                      mt_link_info_t *out, bool *found);
static void priority_release_table(mt_rtnl_t *rtnl, uint32_t table);
#define mt_rtnl_get_default_route priority_get_default_route
#define mt_rtnl_link_by_name priority_link_by_name
#define mt_rtnl_release_table priority_release_table
#include "../../src/netfilter/ipset_to_link.c"
#undef mt_rtnl_get_default_route
#undef mt_rtnl_link_by_name
#undef mt_rtnl_release_table

static unsigned route_reads;

static mt_err_t priority_get_default_route(mt_rtnl_t *rtnl, int family, uint32_t table,
                                           uint32_t metric, mt_rtnl_default_route_t *out) {
    (void)rtnl; (void)family; (void)table;
    route_reads++;
    memset(out, 0, sizeof(*out));
    if (metric == 20) { out->found = true; out->type = RTN_BLACKHOLE; }
    return MT_OK;
}

static mt_err_t priority_link_by_name(mt_rtnl_t *rtnl, const char *name,
                                      mt_link_info_t *out, bool *found) {
    (void)rtnl; (void)name;
    memset(out, 0, sizeof(*out));
    *found = false;
    return MT_OK;
}

static void priority_release_table(mt_rtnl_t *rtnl, uint32_t table) {
    (void)rtnl; (void)table;
}

typedef struct {
    mt_ipt_executable_t base;
    mt_ipt_executable_t *inner;
    unsigned restores;
    bool fail_restore;
} observed_executable_t;

static mt_err_t observed_save(mt_ipt_executable_t *self, uint8_t **out, size_t *len) {
    observed_executable_t *observed = (observed_executable_t *)self;
    return observed->inner->ops->save(observed->inner, out, len);
}

static mt_err_t observed_restore(mt_ipt_executable_t *self, const uint8_t *data, size_t len) {
    observed_executable_t *observed = (observed_executable_t *)self;
    observed->restores++;
    if (observed->fail_restore) {
        observed->fail_restore = false;
        return MT_ERR_AGAIN;
    }
    return observed->inner->ops->restore(observed->inner, data, len);
}

static mt_ipt_proto_t observed_proto(mt_ipt_executable_t *self) {
    observed_executable_t *observed = (observed_executable_t *)self;
    return observed->inner->ops->proto(observed->inner);
}

static void observed_destroy(mt_ipt_executable_t *self) {
    observed_executable_t *observed = (observed_executable_t *)self;
    mt_ipt_executable_free(observed->inner);
    free(observed);
}

static const mt_ipt_executable_ops_t observed_ops = {
    .save = observed_save, .restore = observed_restore,
    .proto = observed_proto, .destroy = observed_destroy,
};

static struct {
    mt_fake_ipt_t *fake[2];
    mt_ipt_t *ipt[2];
    observed_executable_t *observed[2];
    mt_ipset_t *sets[2];
    mt_ipset_to_link_t *links[2];
} fixture;

static void fixture_setup(void *unused) {
    (void)unused;
    memset(&fixture, 0, sizeof(fixture));
    route_reads = 0;
    for (unsigned i = 0; i < 2; i++) {
        fixture.fake[i] = mt_fake_ipt_new(i == 0 ? MT_IPT_PROTO_IPV4 : MT_IPT_PROTO_IPV6);
        assert(fixture.fake[i]);
        observed_executable_t *observed = calloc(1, sizeof(*observed));
        assert(observed);
        observed->base.ops = &observed_ops;
        observed->inner = mt_fake_ipt_as_executable(fixture.fake[i]);
        fixture.observed[i] = observed;
        fixture.ipt[i] = mt_ipt_new(&observed->base);
        assert(fixture.ipt[i]);
    }
    assert(mt_netfilter_register_base_chains(fixture.ipt[0], fixture.ipt[1]) == MT_OK);
}

static void fixture_teardown(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 2; i++) {
        mt_ipset_to_link_free(fixture.links[i]);
        mt_ipset_free(fixture.sets[i]);
        mt_ipt_free(fixture.ipt[i]);
    }
}

static const char *const jump_a[] = {"-j", "TST_00000001"};
static const char *const jump_b[] = {"-j", "TST_00000002"};
static const char *const jump_c[] = {"-j", "TST_00000003"};
static const char *const foreign_a[] = {"-m", "mark", "--mark", "77", "-j", "RETURN"};
static const char *const foreign_b[] = {"-j", "MARK", "--set-mark", "19"};
static const char *const foreign_c[] = {"-m", "mark", "--mark", "88", "-j", "RETURN"};
static const char *const foreign_tail[] = {"-j", "CONNMARK", "--restore-mark"};

static bool same_rules(unsigned family, const char *const *const *expected,
                       const size_t *lengths, size_t count) {
    mt_ipt_rule_t *const *rules = NULL;
    size_t n = 0;
    if (!mt_fake_ipt_get_rules(fixture.fake[family], "mangle", "PREROUTING", &rules, &n) ||
        n != count) { return false; }
    for (size_t i = 0; i < n; i++) {
        if (rules[i]->n_parts != lengths[i]) { return false; }
        for (size_t j = 0; j < lengths[i]; j++) {
            if (strcmp(rules[i]->parts[j], expected[i][j]) != 0) { return false; }
        }
    }
    return true;
}

TEST live_priority_changes_reorder_existing_jumps(void) {
    for (unsigned family = 0; family < 2; family++) {
        mt_ipt_t *ipt = fixture.ipt[family];
        ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 300, jump_a, 2));
        ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
        /* A lower-priority subscription arrives after the group already exists. */
        ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 100, jump_b, 2));
        ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
        const char *const *group_wins[] = {jump_b, jump_a};
        const size_t lengths[] = {2, 2};
        ASSERT(same_rules(family, group_wins, lengths, 2));

        ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 400, jump_b, 2));
        ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
        const char *const *subscription_wins[] = {jump_a, jump_b};
        ASSERT(same_rules(family, subscription_wins, lengths, 2));

        ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 1, jump_b, 2));
        ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
        ASSERT(same_rules(family, group_wins, lengths, 2));
        unsigned restores = fixture.observed[family]->restores;
        ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
        ASSERT_EQ(restores, fixture.observed[family]->restores);
    }
    PASS();
}

TEST priority_reorder_preserves_foreign_slots_and_normal_append(void) {
    const char *const *initial[] = {foreign_a, jump_a, foreign_b, jump_b, foreign_c};
    const size_t initial_lengths[] = {6, 2, 4, 2, 6};
    ASSERT_EQ(MT_OK, mt_fake_ipt_set_initial_rules(fixture.fake[0], "mangle", "PREROUTING",
                                                  initial, initial_lengths, 5));
    mt_ipt_t *ipt = fixture.ipt[0];
    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 300, jump_a, 2));
    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 100, jump_b, 2));
    ASSERT_EQ(MT_OK, mt_ipt_append(ipt, "mangle", "PREROUTING", foreign_tail, 3));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    const char *const *expected[] = {foreign_a, jump_b, foreign_b, jump_a, foreign_c, foreign_tail};
    const size_t lengths[] = {6, 2, 4, 2, 6, 3};
    ASSERT(same_rules(0, expected, lengths, 6));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    ASSERT(same_rules(0, expected, lengths, 6));

    /* A newly introduced entry fills one more owned slot before the foreign tail. */
    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 1, jump_c, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    const char *const *expanded[] = {foreign_a, jump_c, foreign_b, jump_b, jump_a,
                                   foreign_c, foreign_tail};
    const size_t expanded_lengths[] = {6, 2, 4, 2, 2, 6, 3};
    ASSERT(same_rules(0, expanded, expanded_lengths, 7));
    PASS();
}

TEST duplicate_managed_jumps_are_removed_without_touching_other_rules(void) {
    const char *const *initial[] = {foreign_a, jump_a, jump_a, foreign_b,
                                  jump_b, jump_b, foreign_c};
    const size_t initial_lengths[] = {6, 2, 2, 4, 2, 2, 6};
    ASSERT_EQ(MT_OK, mt_fake_ipt_set_initial_rules(fixture.fake[0], "mangle", "PREROUTING",
                                                  initial, initial_lengths, 7));
    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(fixture.ipt[0], "mangle", "PREROUTING", 300, jump_a, 2));
    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(fixture.ipt[0], "mangle", "PREROUTING", 100, jump_b, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(fixture.ipt[0]));
    const char *const *expected[] = {foreign_a, jump_b, foreign_b, jump_a, foreign_c};
    const size_t lengths[] = {6, 2, 4, 2, 6};
    ASSERT(same_rules(0, expected, lengths, 5));
    PASS();
}

TEST equal_priorities_are_stable_across_disable_and_rebuild(void) {
    mt_ipt_t *ipt = fixture.ipt[0];
    /* Deliberately register in reverse lexical order. */
    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 300, jump_b, 2));
    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 300, jump_a, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    const char *const *expected[] = {jump_a, jump_b};
    const size_t lengths[] = {2, 2};
    ASSERT(same_rules(0, expected, lengths, 2));
    ASSERT_EQ(MT_OK, mt_ipt_delete(ipt, "mangle", "PREROUTING", jump_a, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    const char *const *only_b[] = {jump_b};
    ASSERT(same_rules(0, only_b, lengths, 1));
    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 300, jump_a, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    ASSERT(same_rules(0, expected, lengths, 2));

    mt_fake_ipt_reset(fixture.fake[0]);
    mt_ipt_reset_staged(ipt);
    ASSERT_EQ(MT_OK, mt_ipt_register_chain_patch(ipt, "mangle", "PREROUTING"));
    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 300, jump_b, 2));
    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 300, jump_a, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    ASSERT(same_rules(0, expected, lengths, 2));
    PASS();
}

TEST failed_or_canceled_reorder_can_be_retried(void) {
    mt_ipt_t *ipt = fixture.ipt[0];
    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 300, jump_a, 2));
    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 100, jump_b, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    const char *const *before[] = {jump_b, jump_a};
    const char *const *after[] = {jump_a, jump_b};
    const size_t lengths[] = {2, 2};
    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 400, jump_b, 2));
    fixture.observed[0]->fail_restore = true;
    ASSERT_EQ(MT_ERR_AGAIN, mt_ipt_commit(ipt));
    ASSERT(same_rules(0, before, lengths, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    ASSERT(same_rules(0, after, lengths, 2));

    ASSERT_EQ(MT_OK, mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", 100, jump_b, 2));
    mt_cancel_t *cancel = mt_cancel_new();
    ASSERT(cancel);
    mt_ipt_set_cancel(ipt, cancel);
    mt_cancel_raise(cancel);
    mt_err_t canceled = mt_ipt_commit(ipt);
    mt_cancel_clear(cancel);
    mt_ipt_set_cancel(ipt, NULL);
    mt_cancel_free(cancel);
    ASSERT_EQ(MT_ERR_CANCELED, canceled);
    ASSERT(same_rules(0, after, lengths, 2));
    ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
    ASSERT(same_rules(0, before, lengths, 2));
    PASS();
}

static void make_links(void) {
    for (unsigned i = 0; i < 2; i++) {
        mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new();
        assert(fake);
        fixture.sets[i] = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake),
                                       i == 0 ? "test_host" : "test_cloud");
        assert(fixture.sets[i]);
        assert(mt_ipset_enable(fixture.sets[i]) == MT_OK);
        fixture.links[i] = mt_ipset_to_link_new(i == 0 ? jump_a[1] : jump_b[1],
            i == 0 ? "vpn_group" : "vpn_subscription", fixture.sets[i],
            fixture.ipt[0], fixture.ipt[1], NULL, 0);
        assert(fixture.links[i]);
        fixture.links[i]->enabled = true;
        fixture.links[i]->mark = i == 0 ? 101u : 202u;
        mt_ipset_to_link_set_priority(fixture.links[i], i == 0 ? 300u : 100u);
    }
    /* Host membership models a DNS-learned address. Cloud membership is a
     * permanent broad prefix containing that same address in each family. */
    const mt_ipv4_subnet_t host4 = {.addr = {198, 51, 100, 42}, .cidr = 32};
    const mt_ipv4_subnet_t cloud4 = {.addr = {198, 51, 100, 0}, .cidr = 24};
    const mt_ipv6_subnet_t host6 = {
        .addr = {0x20, 0x01, 0x0d, 0xb8, 0xab, 0xcd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x42},
        .cidr = 128,
    };
    mt_ipv6_subnet_t cloud6 = host6;
    cloud6.addr[15] = 0;
    cloud6.cidr = 64;
    const uint32_t ttl = 60;
    assert(mt_ipset_add4(fixture.sets[0], host4, &ttl) == MT_OK);
    assert(mt_ipset_add4(fixture.sets[1], cloud4, NULL) == MT_OK);
    assert(mt_ipset_add6(fixture.sets[0], host6, &ttl) == MT_OK);
    assert(mt_ipset_add6(fixture.sets[1], cloud6, NULL) == MT_OK);
}

static mt_err_t prepare_links(void) {
    for (unsigned i = 0; i < 2; i++) {
        mt_err_t err = mt_ipset_to_link_prepare_iptables(fixture.links[i]);
        if (err != MT_OK) { return err; }
    }
    for (unsigned i = 0; i < 2; i++) {
        mt_err_t err = mt_ipt_commit(fixture.ipt[i]);
        if (err != MT_OK) { return err; }
    }
    return MT_OK;
}

static bool prefix_contains(const uint8_t *prefix, const uint8_t *address, uint8_t cidr) {
    for (unsigned bit = 0; bit < cidr; bit++) {
        uint8_t mask = (uint8_t)(0x80u >> (bit % 8u));
        if ((prefix[bit / 8u] & mask) != (address[bit / 8u] & mask)) { return false; }
    }
    return true;
}

static bool set_contains(const char *name, unsigned family, const uint8_t *address) {
    for (unsigned i = 0; i < 2; i++) {
        mt_ipset_t *set = fixture.sets[i];
        const char *candidate = family == 0 ? mt_ipset_name4(set) : mt_ipset_name6(set);
        if (strcmp(candidate, name) != 0) { continue; }
        bool found = false;
        size_t count = 0;
        if (family == 0) {
            mt_ipset_entry4_t *entries = NULL;
            assert(mt_ipset_list4(set, &entries, &count) == MT_OK);
            for (size_t j = 0; j < count; j++) {
                found |= prefix_contains(entries[j].subnet.addr, address, entries[j].subnet.cidr);
            }
            free(entries);
        } else {
            mt_ipset_entry6_t *entries = NULL;
            assert(mt_ipset_list6(set, &entries, &count) == MT_OK);
            for (size_t j = 0; j < count; j++) {
                found |= prefix_contains(entries[j].subnet.addr, address, entries[j].subnet.cidr);
            }
            free(entries);
        }
        return found;
    }
    return false;
}

static const char *rule_option(const mt_ipt_rule_t *rule, const char *option) {
    for (size_t i = 0; i + 1 < rule->n_parts; i++) {
        if (strcmp(rule->parts[i], option) == 0) { return rule->parts[i + 1]; }
    }
    return NULL;
}

typedef struct {
    unsigned family;
    uint8_t address[16];
    bool reply;
    uint32_t mark;
    uint32_t connmark;
} packet_t;

static bool evaluate_chain(const char *chain, packet_t *packet, unsigned depth) {
    if (depth > 8) { return false; }
    mt_ipt_rule_t *const *rules = NULL;
    size_t count = 0;
    if (!mt_fake_ipt_get_rules(fixture.fake[packet->family], "mangle", chain, &rules, &count)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        const mt_ipt_rule_t *rule = rules[i];
        const char *direction = rule_option(rule, "--ctdir");
        if (direction && strcmp(direction, "REPLY") == 0 && !packet->reply) { continue; }
        const char *set = rule_option(rule, "--match-set");
        if (set && !set_contains(set, packet->family, packet->address)) { continue; }
        const char *target = rule_option(rule, "-j");
        if (!target) { return false; }
        if (strcmp(target, "RETURN") == 0) { return true; }
        if (strcmp(target, "MARK") == 0) {
            const char *value = rule_option(rule, "--set-mark");
            if (!value) { return false; }
            packet->mark = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(target, "CONNMARK") == 0) {
            bool save = false;
            for (size_t j = 0; j < rule->n_parts; j++) {
                save |= strcmp(rule->parts[j], "--save-mark") == 0;
            }
            if (!save) { return false; }
            packet->connmark = packet->mark;
        } else if (!evaluate_chain(target, packet, depth + 1)) {
            return false;
        }
    }
    return true;
}

static packet_t packet(unsigned family, bool host, bool match, bool reply) {
    packet_t result = {.family = family, .reply = reply, .mark = 17, .connmark = 23};
    if (family == 0) {
        const uint8_t address[] = {198, 51, match ? 100 : 101, host ? 42 : 43};
        memcpy(result.address, address, sizeof(address));
    } else {
        const uint8_t address[] = {0x20, 0x01, 0x0d, 0xb8, 0xab, match ? 0xcd : 0xce,
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, host ? 0x42 : 0x43};
        memcpy(result.address, address, sizeof(address));
    }
    return result;
}

TEST dns_host_group_beats_cloud_subscription_and_saves_final_connmark(void) {
    make_links();
    ASSERT_EQ(MT_OK, prepare_links());
    for (unsigned family = 0; family < 2; family++) {
        packet_t overlap = packet(family, true, true, false);
        ASSERT(evaluate_chain("PREROUTING", &overlap, 0));
        ASSERT_EQ(101u, overlap.mark);
        ASSERT_EQ(101u, overlap.connmark);
        packet_t cloud_only = packet(family, false, true, false);
        ASSERT(evaluate_chain("PREROUTING", &cloud_only, 0));
        ASSERT_EQ(202u, cloud_only.mark);
        ASSERT_EQ(202u, cloud_only.connmark);
        packet_t unrelated = packet(family, true, false, false);
        ASSERT(evaluate_chain("PREROUTING", &unrelated, 0));
        ASSERT_EQ(17u, unrelated.mark);
        ASSERT_EQ(23u, unrelated.connmark);
        packet_t reply = packet(family, true, true, true);
        ASSERT(evaluate_chain("PREROUTING", &reply, 0));
        ASSERT_EQ(17u, reply.mark);
        ASSERT_EQ(23u, reply.connmark);
    }
    PASS();
}

TEST changing_priority_alone_changes_the_route_mark_in_both_families(void) {
    make_links();
    ASSERT_EQ(MT_OK, prepare_links());
    mt_ipset_to_link_set_priority(fixture.links[1], 1000);
    ASSERT_EQ(MT_OK, prepare_links());
    for (unsigned family = 0; family < 2; family++) {
        packet_t overlap = packet(family, true, true, false);
        ASSERT(evaluate_chain("PREROUTING", &overlap, 0));
        ASSERT_EQ(202u, overlap.mark);
        ASSERT_EQ(202u, overlap.connmark);
    }
    mt_ipset_to_link_set_priority(fixture.links[1], 1);
    ASSERT_EQ(MT_OK, prepare_links());
    for (unsigned family = 0; family < 2; family++) {
        packet_t overlap = packet(family, true, true, false);
        ASSERT(evaluate_chain("PREROUTING", &overlap, 0));
        ASSERT_EQ(101u, overlap.mark);
        ASSERT_EQ(101u, overlap.connmark);
    }
    PASS();
}

TEST disabling_winner_exposes_lower_priority_without_new_dns(void) {
    make_links();
    ASSERT_EQ(MT_OK, prepare_links());
    ASSERT_EQ(MT_OK, mt_ipset_to_link_disable(fixture.links[0]));
    for (unsigned family = 0; family < 2; family++) {
        packet_t overlap = packet(family, true, true, false);
        ASSERT(evaluate_chain("PREROUTING", &overlap, 0));
        ASSERT_EQ(202u, overlap.mark);
        ASSERT_EQ(202u, overlap.connmark);
        ASSERT_FALSE(mt_fake_ipt_chain_exists(fixture.fake[family], "mangle", jump_a[1]));
    }
    /* Bypass route allocation exactly as at setup; existing DNS membership
     * is still present and staging restores its priority after re-enable. */
    fixture.links[0]->enabled = true;
    ASSERT_EQ(MT_OK, prepare_links());
    for (unsigned family = 0; family < 2; family++) {
        packet_t overlap = packet(family, true, true, false);
        ASSERT(evaluate_chain("PREROUTING", &overlap, 0));
        ASSERT_EQ(101u, overlap.mark);
        ASSERT_EQ(101u, overlap.connmark);
    }
    PASS();
}

TEST live_profile_reconfiguration_preserves_rule_priority(void) {
    make_links();
    ASSERT_EQ(MT_OK, prepare_links());
    const char *interfaces[] = {"vpn_primary", "vpn_backup"};
    ASSERT_EQ(MT_OK, mt_ipset_to_link_set_interfaces(fixture.links[1], interfaces, 2));
    ASSERT(route_reads > 0); /* Exercise the enabled profile-reconfigure path. */
    const char *const *group_wins[] = {jump_b, jump_a};
    const size_t lengths[] = {2, 2};
    for (unsigned family = 0; family < 2; family++) {
        ASSERT(same_rules(family, group_wins, lengths, 2));
        packet_t overlap = packet(family, true, true, false);
        ASSERT(evaluate_chain("PREROUTING", &overlap, 0));
        ASSERT_EQ(101u, overlap.mark);
        ASSERT_EQ(101u, overlap.connmark);
    }

    mt_ipset_to_link_set_priority(fixture.links[1], 900);
    ASSERT_EQ(MT_OK, prepare_links());
    const char *reordered[] = {"vpn_backup", "vpn_primary"};
    ASSERT_EQ(MT_OK, mt_ipset_to_link_set_interfaces(fixture.links[1], reordered, 2));
    const char *const *subscription_wins[] = {jump_a, jump_b};
    for (unsigned family = 0; family < 2; family++) {
        ASSERT(same_rules(family, subscription_wins, lengths, 2));
        packet_t overlap = packet(family, true, true, false);
        ASSERT(evaluate_chain("PREROUTING", &overlap, 0));
        ASSERT_EQ(202u, overlap.mark);
        ASSERT_EQ(202u, overlap.connmark);
        /* This fake profile has no usable candidate. The winning mark
         * still selects its terminal blackhole, not a lower-priority group. */
        ASSERT_EQ(SIZE_MAX, family == 0 ? fixture.links[1]->v4.selected
                                        : fixture.links[1]->v6.selected);
    }
    ASSERT_STR_EQ("vpn_backup", fixture.links[1]->interfaces[0]);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    SET_SETUP(fixture_setup, NULL);
    SET_TEARDOWN(fixture_teardown, NULL);
    RUN_TEST(live_priority_changes_reorder_existing_jumps);
    RUN_TEST(priority_reorder_preserves_foreign_slots_and_normal_append);
    RUN_TEST(duplicate_managed_jumps_are_removed_without_touching_other_rules);
    RUN_TEST(equal_priorities_are_stable_across_disable_and_rebuild);
    RUN_TEST(failed_or_canceled_reorder_can_be_retried);
    RUN_TEST(dns_host_group_beats_cloud_subscription_and_saves_final_connmark);
    RUN_TEST(changing_priority_alone_changes_the_route_mark_in_both_families);
    RUN_TEST(disabling_winner_exposes_lower_priority_without_new_dns);
    RUN_TEST(live_profile_reconfiguration_preserves_rule_priority);
    GREATEST_MAIN_END();
}
