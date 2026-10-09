/* See ipset_to_link.h. Port of utils/netfilterTools/ipset-to-link.go. */
#include "magitrickle/ipset_to_link.h"
#include "magitrickle/log.h"
#include "magitrickle/failover.h"
#include "magitrickle/models.h"

#include <stdio.h>
#include <linux/rtnetlink.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

typedef struct family_state {
    bool rule_added;
    bool blackhole_added;
    bool iface_route_present;
    bool iface_has_gw;
    uint8_t gw[16];
    uint8_t gw_len;
    int ifindex;
    size_t selected; /* SIZE_MAX means blackhole */
} family_state_t;

struct mt_ipset_to_link {
    char *chain_name;
    char *iface_name; /* direct-interface compatibility path */
    char **interfaces; /* copied profile candidates, when present */
    size_t n_interfaces;
    mt_ipset_t *ipset; /* borrowed */
    mt_ipt_t *ipt4;    /* borrowed, nullable */
    mt_ipt_t *ipt6;    /* borrowed, nullable */
    mt_rtnl_t *rtnl;   /* borrowed */
    uint32_t start_idx;
    uint16_t priority;

    bool enabled;
    uint32_t mark;
    uint32_t table;
    family_state_t v4, v6;
};

mt_ipset_to_link_t *mt_ipset_to_link_new(const char *chain_name, const char *iface_name,
                                         mt_ipset_t *ipset, mt_ipt_t *ipt4, mt_ipt_t *ipt6,
                                         mt_rtnl_t *rtnl, uint32_t start_idx) {
    mt_ipset_to_link_t *l = calloc(1, sizeof(*l));
    if (!l) { return NULL; }
    l->chain_name = strdup(chain_name);
    l->iface_name = strdup(iface_name);
    if (!l->chain_name || !l->iface_name) {
        free(l->chain_name);
        free(l->iface_name);
        free(l);
        return NULL;
    }
    l->ipset = ipset;
    l->ipt4 = ipt4;
    l->ipt6 = ipt6;
    l->rtnl = rtnl;
    l->start_idx = start_idx;
    l->v4.selected = l->v6.selected = SIZE_MAX;
    l->priority = MT_GROUP_DEFAULT_PRIORITY;
    return l;
}

void mt_ipset_to_link_free(mt_ipset_to_link_t *l) {
    if (!l) { return; }
    free(l->chain_name);
    free(l->iface_name);
    for (size_t i = 0; i < l->n_interfaces; i++) { free(l->interfaces[i]); }
    free(l->interfaces);
    free(l);
}

void mt_ipset_to_link_set_priority(mt_ipset_to_link_t *l, uint16_t priority) {
    l->priority = priority;
}

/* ---- iptables chain rules ------------------------------------------------ */

/* Stages the group's chains and rules without writing them: a full table
 * rebuild stages every group first and writes the result in one commit.  */
static mt_err_t build_iptables_rules(mt_ipset_to_link_t *l, mt_ipt_t *ipt, const char *ipset_name) {
    if (!ipt) { return MT_OK; }

    mt_err_t err = mt_ipt_register_chain_override(ipt, "filter", l->chain_name);
    if (err != MT_OK) { return err; }

    size_t count = l->n_interfaces ? l->n_interfaces : 1;
    for (size_t i = 0; i < count; i++) {
        const char *name = l->n_interfaces ? l->interfaces[i] : l->iface_name;
        if (strcmp(name, MT_IPSET_TO_LINK_BLACKHOLE) == 0) { continue; }
        const char *args[] = {"-o", name, "-m", "set", "--match-set",
                              ipset_name,          "dst", "-j", "ACCEPT"};
        err = mt_ipt_append(ipt, "filter", l->chain_name, args, 9);
        if (err != MT_OK) { return err; }
    }
    const char *fwd_args[] = {"-j", l->chain_name};
    err = mt_ipt_append(ipt, "filter", "FORWARD", fwd_args, 2);
    if (err != MT_OK) { return err; }

    err = mt_ipt_register_chain_override(ipt, "mangle", l->chain_name);
    if (err != MT_OK) { return err; }

    char mark_str[16];
    snprintf(mark_str, sizeof(mark_str), "%u", l->mark);
    const char *mangle1[] = {"-m", "conntrack", "--ctdir", "REPLY", "-j", "RETURN"};
    err = mt_ipt_append(ipt, "mangle", l->chain_name, mangle1, 6);
    if (err != MT_OK) { return err; }
    const char *mangle2[] = {"-m", "set", "--match-set", ipset_name,
                             "dst", "-j", "MARK",        "--set-mark", mark_str};
    err = mt_ipt_append(ipt, "mangle", l->chain_name, mangle2, 9);
    if (err != MT_OK) { return err; }
    /* Without this rule, routing on Keenetic routers did not work; DO NOT REMOVE! */
    const char *mangle3[] = {"-m", "set", "--match-set", ipset_name,
                             "dst", "-j", "CONNMARK",    "--save-mark"};
    err = mt_ipt_append(ipt, "mangle", l->chain_name, mangle3, 8);
    if (err != MT_OK) { return err; }
    const char *pre_args[] = {"-j", l->chain_name};
    /* MARK is non-terminating: the last matching jump supplies both the
     * packet and connection mark. Order all groups and subscriptions
     * together so the highest configured priority runs last. */
    err = mt_ipt_append_ordered(ipt, "mangle", "PREROUTING", l->priority, pre_args, 2);
    if (err != MT_OK) { return err; }

    err = mt_ipt_register_chain_override(ipt, "nat", l->chain_name);
    if (err != MT_OK) { return err; }
    const char *nat1[] = {"-m", "set", "--match-set", ipset_name, "dst", "-j", "MASQUERADE"};
    err = mt_ipt_append(ipt, "nat", l->chain_name, nat1, 7);
    if (err != MT_OK) { return err; }
    const char *post_args[] = {"-j", l->chain_name};
    err = mt_ipt_append(ipt, "nat", "POSTROUTING", post_args, 2);
    if (err != MT_OK) { return err; }

    return MT_OK;
}

static mt_err_t insert_iptables_rules(mt_ipset_to_link_t *l, mt_ipt_t *ipt, const char *ipset_name) {
    if (!ipt) { return MT_OK; }

    mt_err_t err = build_iptables_rules(l, ipt, ipset_name);
    if (err != MT_OK) { return err; }
    return mt_ipt_commit(ipt);
}

mt_err_t mt_ipset_to_link_prepare_iptables(mt_ipset_to_link_t *l) {
    if (!l || !l->enabled) { return MT_OK; }

    mt_err_t e4 = build_iptables_rules(l, l->ipt4, mt_ipset_name4(l->ipset));
    mt_err_t e6 = build_iptables_rules(l, l->ipt6, mt_ipset_name6(l->ipset));
    return e4 != MT_OK ? e4 : e6;
}

static mt_err_t delete_iptables_rules(mt_ipset_to_link_t *l, mt_ipt_t *ipt) {
    if (!ipt) { return MT_OK; }
    mt_err_t first_err = MT_OK;

    mt_err_t err = mt_ipt_register_chain_delete(ipt, "filter", l->chain_name);
    if (err != MT_OK && first_err == MT_OK) { first_err = err; }
    const char *fwd_args[] = {"-j", l->chain_name};
    err = mt_ipt_delete(ipt, "filter", "FORWARD", fwd_args, 2);
    if (err != MT_OK && err != MT_ERR_NOENT && first_err == MT_OK) { first_err = err; }

    err = mt_ipt_register_chain_delete(ipt, "mangle", l->chain_name);
    if (err != MT_OK && first_err == MT_OK) { first_err = err; }
    const char *pre_args[] = {"-j", l->chain_name};
    err = mt_ipt_delete(ipt, "mangle", "PREROUTING", pre_args, 2);
    if (err != MT_OK && err != MT_ERR_NOENT && first_err == MT_OK) { first_err = err; }

    err = mt_ipt_register_chain_delete(ipt, "nat", l->chain_name);
    if (err != MT_OK && first_err == MT_OK) { first_err = err; }
    const char *post_args[] = {"-j", l->chain_name};
    err = mt_ipt_delete(ipt, "nat", "POSTROUTING", post_args, 2);
    if (err != MT_OK && err != MT_ERR_NOENT && first_err == MT_OK) { first_err = err; }

    err = mt_ipt_commit(ipt);
    if (err != MT_OK && first_err == MT_OK) { first_err = err; }
    return first_err;
}

/* ---- ip rule ---------------------------------------------------------- */

static mt_err_t insert_ip_rule(mt_ipset_to_link_t *l) {
    /* The best-effort delete before each add mirrors Go's
     * insertIPRule(): `_ = netlink.RuleDel(rule)` (error deliberately
     * ignored) before `netlink.RuleAdd(rule)`. RTM_NEWRULE carries
     * NLM_F_EXCL, so without it a leftover rule with the same mark/table
     * -- e.g. from a previous daemon instance killed before it could
     * clean up -- makes every subsequent enable fail with EEXIST
     * ("already exists") instead of being taken over. */
    if (l->ipt4) {
        (void)mt_rtnl_rule_del(l->rtnl, AF_INET, l->mark, l->table);
        mt_err_t err = mt_rtnl_rule_add(l->rtnl, AF_INET, l->mark, l->table);
        if (err != MT_OK) { return err; }
        l->v4.rule_added = true;
    }
    if (l->ipt6) {
        (void)mt_rtnl_rule_del(l->rtnl, AF_INET6, l->mark, l->table);
        mt_err_t err = mt_rtnl_rule_add(l->rtnl, AF_INET6, l->mark, l->table);
        if (err != MT_OK) { return err; }
        l->v6.rule_added = true;
    }
    return MT_OK;
}

static mt_err_t delete_ip_rule(mt_ipset_to_link_t *l) {
    mt_err_t first_err = MT_OK;
    if (l->v4.rule_added) {
        mt_err_t err = mt_rtnl_rule_del(l->rtnl, AF_INET, l->mark, l->table);
        if (err != MT_OK && first_err == MT_OK) { first_err = err; }
        l->v4.rule_added = false;
    }
    if (l->v6.rule_added) {
        mt_err_t err = mt_rtnl_rule_del(l->rtnl, AF_INET6, l->mark, l->table);
        if (err != MT_OK && first_err == MT_OK) { first_err = err; }
        l->v6.rule_added = false;
    }
    return first_err;
}

/* ---- ip route ----------------------------------------------------------- */

static family_state_t *family_state(mt_ipset_to_link_t *l, int family)
{
    return family == AF_INET ? &l->v4 : &l->v6;
}

static mt_err_t probe_candidate(void *ctx, const char *name, int family,
                                mt_route_candidate_t *candidate, bool *available)
{
    mt_ipset_to_link_t *l = ctx;
    *available = false;
    if (!*name || strcmp(name, MT_IPSET_TO_LINK_BLACKHOLE) == 0) { return MT_OK; }
    mt_link_info_t li;
    bool found;
    mt_err_t err = mt_rtnl_link_by_name(l->rtnl, name, &li, &found);
    if (err != MT_OK || !found || !li.up) { return err; }
    if (l->n_interfaces) {
        if (!li.operational) { return MT_OK; }
        err = mt_rtnl_iface_has_address(l->rtnl, family, li.ifindex, &found);
        if (err != MT_OK || !found) { return err; }
    }
    candidate->ifindex = li.ifindex;
    if (!li.point_to_point) {
        err = (l->n_interfaces ? mt_rtnl_gateway_for_profile : mt_rtnl_gateway_for_iface)(l->rtnl, family, li.ifindex, &found,
                                         candidate->gateway, &candidate->gateway_len);
        if (l->n_interfaces && (err != MT_OK || !found)) { return err; }
        if (err != MT_OK || !found) { candidate->gateway_len = 0; }
    }
    *available = true;
    return MT_OK;
}

static mt_err_t replace_candidate(void *ctx, int family, const mt_route_candidate_t *candidate,
                                  bool *unavailable)
{
    mt_ipset_to_link_t *l = ctx;
    family_state_t *fs = family_state(l, family);
    mt_rtnl_default_route_t actual;
    *unavailable = false;
    mt_err_t err = mt_rtnl_get_default_route(l->rtnl, family, l->table, 10, &actual);
    if (err != MT_OK) { return err; }

    bool same = actual.found && !actual.multipath && actual.type == RTN_UNICAST &&
                actual.ifindex == candidate->ifindex &&
                actual.gateway_len == candidate->gateway_len &&
                (!actual.gateway_len ||
                 memcmp(actual.gateway, candidate->gateway, actual.gateway_len) == 0);
    if (!same) {
        err = mt_rtnl_route_replace_iface(l->rtnl, family, l->table, 10,
                                          candidate->ifindex,
                                          candidate->gateway_len ? candidate->gateway : NULL,
                                          candidate->gateway_len, unavailable);
        if (err != MT_OK || *unavailable) { return err; }
    }
    fs->iface_route_present = true;
    fs->ifindex = candidate->ifindex;
    fs->iface_has_gw = candidate->gateway_len != 0;
    fs->gw_len = candidate->gateway_len;
    memcpy(fs->gw, candidate->gateway, sizeof(fs->gw));
    return MT_OK;
}

static mt_err_t block_family(void *ctx, int family)
{
    mt_ipset_to_link_t *l = ctx;
    family_state_t *fs = family_state(l, family);
    mt_rtnl_default_route_t actual;
    mt_err_t err = mt_rtnl_get_default_route(l->rtnl, family, l->table, 10, &actual);
    if (err != MT_OK) { return err; }
    if (actual.found) {
        if (actual.type != RTN_UNICAST || actual.multipath || actual.ifindex <= 0) {
            return MT_ERR_STATE;
        }
        err = mt_rtnl_route_del_iface(l->rtnl, family, l->table, 10, actual.ifindex,
                                      actual.gateway_len ? actual.gateway : NULL,
                                      actual.gateway_len);
        if (err != MT_OK) { return err; }
    }
    fs->iface_route_present = false;
    fs->selected = SIZE_MAX;
    return MT_OK;
}

static mt_err_t reconcile_family(mt_ipset_to_link_t *l, int family)
{
    const mt_failover_ops_t ops = {probe_candidate, replace_candidate, block_family};
    family_state_t *fs = family_state(l, family);
    size_t selected = SIZE_MAX, previous = fs->selected;
    const char *direct[] = {l->iface_name};
    const char *const *names = l->n_interfaces ? (const char *const *)l->interfaces : direct;
    size_t count = l->n_interfaces ? l->n_interfaces : 1;
    mt_err_t err = mt_failover_reconcile(names, count, family, &ops, l, &selected);
    if (err == MT_OK) {
        fs->selected = selected;
        if (l->n_interfaces && previous != selected) {
            MT_INFO("route profile switched: chain=%s family=%d interface=%s", l->chain_name,
                    family, selected == SIZE_MAX ? MT_IPSET_TO_LINK_BLACKHOLE : names[selected]);
        }
    }
    return err;
}

static mt_err_t insert_family_routes(mt_ipset_to_link_t *l, int family)
{
    mt_rtnl_default_route_t actual;
    mt_err_t err = mt_rtnl_get_default_route(l->rtnl, family, l->table, 20, &actual);
    if (err != MT_OK) { return err; }
    if (actual.found && actual.type != RTN_BLACKHOLE) { return MT_ERR_STATE; }
    if (!actual.found) { err = mt_rtnl_route_add_blackhole(l->rtnl, family, l->table, 20); }
    if (err != MT_OK) { return err; }
    family_state(l, family)->blackhole_added = true;
    return reconcile_family(l, family);
}

static mt_err_t insert_ip_route(mt_ipset_to_link_t *l) {
    /* Isolate the WHOLE operation, including terminal-route preparation.
     * A v6 snapshot/blackhole error must not prevent v4 failover (or vice
     * versa). Still report failure so the event owner schedules recovery. */
    mt_err_t e4 = l->ipt4 ? insert_family_routes(l, AF_INET) : MT_OK;
    mt_err_t e6 = l->ipt6 ? insert_family_routes(l, AF_INET6) : MT_OK;
    return e4 != MT_OK ? e4 : e6;
}

static mt_err_t delete_ip_route(mt_ipset_to_link_t *l) {
    mt_err_t first_err = MT_OK;

    mt_err_t e4 = block_family(l, AF_INET);
    mt_err_t e6 = block_family(l, AF_INET6);
    if (e4 != MT_OK) { first_err = e4; }
    if (e6 != MT_OK && first_err == MT_OK) { first_err = e6; }
    if (l->v4.blackhole_added) {
        mt_err_t err = mt_rtnl_route_del_blackhole(l->rtnl, AF_INET, l->table, 20);
        if (err != MT_OK && first_err == MT_OK) { first_err = err; }
        l->v4.blackhole_added = false;
    }
    if (l->v6.blackhole_added) {
        mt_err_t err = mt_rtnl_route_del_blackhole(l->rtnl, AF_INET6, l->table, 20);
        if (err != MT_OK && first_err == MT_OK) { first_err = err; }
        l->v6.blackhole_added = false;
    }
    return first_err;
}

/* ---- public API --------------------------------------------------------- */

static mt_err_t teardown(mt_ipset_to_link_t *l) {
    mt_err_t e1 = delete_ip_route(l);
    mt_err_t e2 = delete_ip_rule(l);
    mt_err_t e3 = delete_iptables_rules(l, l->ipt4);
    mt_err_t e4 = delete_iptables_rules(l, l->ipt6);
    if (e1 == MT_OK && e2 == MT_OK) { mt_rtnl_release_table(l->rtnl, l->table); }
    if (e1 != MT_OK) { return e1; }
    if (e2 != MT_OK) { return e2; }
    if (e3 != MT_OK) { return e3; }
    return e4;
}

mt_err_t mt_ipset_to_link_enable(mt_ipset_to_link_t *l) {
    if (l->enabled) { return MT_OK; }

    uint32_t idx;
    mt_err_t err = mt_rtnl_alloc_mark_table(l->rtnl, l->start_idx, &idx);
    if (err != MT_OK) { return err; }
    l->mark = idx;
    l->table = idx;

    err = mt_rtnl_reserve_table(l->rtnl, idx);
    if (err != MT_OK) { return err; }
    err = insert_ip_rule(l);
    if (err == MT_OK) { err = insert_ip_route(l); }
    if (err == MT_OK) {
        err = insert_iptables_rules(l, l->ipt4, mt_ipset_name4(l->ipset));
    }
    if (err == MT_OK) {
        err = insert_iptables_rules(l, l->ipt6, mt_ipset_name6(l->ipset));
    }

    if (err != MT_OK) {
        teardown(l);
        return err;
    }

    l->enabled = true;
    MT_DEBUG("using ip table and mark 0x%x for chain %s", l->mark, l->chain_name);
    return MT_OK;
}

mt_err_t mt_ipset_to_link_disable(mt_ipset_to_link_t *l) {
    if (!l->enabled) { return MT_OK; }
    l->enabled = false;
    return teardown(l);
}

mt_err_t mt_ipset_to_link_clear_if_disabled(mt_ipset_to_link_t *l) {
    if (l->enabled) { return MT_OK; }
    return teardown(l);
}

mt_err_t mt_ipset_to_link_on_link_up(mt_ipset_to_link_t *l) {
    if (!l->enabled) { return MT_OK; }
    return insert_ip_route(l);
}

mt_err_t mt_ipset_to_link_on_addr_change(mt_ipset_to_link_t *l) {
    if (!l->enabled) { return MT_OK; }
    return insert_ip_route(l);
}

bool mt_ipset_to_link_uses_interface(const mt_ipset_to_link_t *l, const char *name)
{
    if (!l || !name) { return false; }
    if (!l->n_interfaces) { return strcmp(l->iface_name, name) == 0; }
    for (size_t i = 0; i < l->n_interfaces; i++) {
        if (strcmp(l->interfaces[i], name) == 0) { return true; }
    }
    return false;
}

static void free_interfaces(char **names, size_t count)
{
    for (size_t i = 0; i < count; i++) { free(names[i]); }
    free(names);
}

mt_err_t mt_ipset_to_link_set_interfaces(mt_ipset_to_link_t *l,
                                       const char *const *names, size_t count)
{
    if (!l || !names || !count) { return MT_ERR_INVAL; }
    bool same = count == l->n_interfaces;
    for (size_t i = 0; same && i < count; i++) { same = strcmp(names[i], l->interfaces[i]) == 0; }
    if (same) { return MT_OK; }
    char **next = calloc(count, sizeof(*next));
    if (!next) { return MT_ERR_NOMEM; }
    for (size_t i = 0; i < count; i++) {
        next[i] = strdup(names[i]);
        if (!next[i]) { free_interfaces(next, count); return MT_ERR_NOMEM; }
    }
    char **old = l->interfaces;
    size_t old_count = l->n_interfaces;
    l->interfaces = next; l->n_interfaces = count;
    mt_err_t err = MT_OK;
    if (l->enabled) {
        /* Authorise the new possible outputs before changing the route.
         * The permanent blackhole protects any unsuccessful transition. */
        err = insert_iptables_rules(l, l->ipt4, mt_ipset_name4(l->ipset));
        if (err == MT_OK) { err = insert_iptables_rules(l, l->ipt6, mt_ipset_name6(l->ipset)); }
        if (err == MT_OK) { err = insert_ip_route(l); }
    }
    if (err != MT_OK) {
        l->interfaces = old; l->n_interfaces = old_count;
        mt_err_t e4 = insert_iptables_rules(l, l->ipt4, mt_ipset_name4(l->ipset));
        mt_err_t e6 = insert_iptables_rules(l, l->ipt6, mt_ipset_name6(l->ipset));
        mt_err_t er = insert_ip_route(l);
        if (e4 != MT_OK || e6 != MT_OK || er != MT_OK) {
            MT_ERROR("failed to restore profile route: %s", l->chain_name);
        }
        free_interfaces(next, count);
        return err;
    }
    free_interfaces(old, old_count);
    return MT_OK;
}
