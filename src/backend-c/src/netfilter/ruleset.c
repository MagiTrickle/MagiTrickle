/* See ruleset.h. Port of rule_set.go. */
#include "magitrickle/ruleset.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "magitrickle/log.h"
#include "magitrickle/match.h"

struct mt_ruleset {
    const mt_group_t *group;
    mt_ruleset_deps_t deps;
    bool enabled; /* runtime flag, mirrors Go's RuleSet.enabled atomic.Bool */
    mt_ipset_t *ipset;
    mt_ipset_to_link_t *ipset_to_link;
};

static bool configured_enabled(const mt_ruleset_t *rs) {
    return rs->group->enable;
}

/* ---- growable new-subnet lists (sync() scratch state) ------------------- */

typedef struct new4_entry {
    mt_ipv4_subnet_t subnet;
    bool has_ttl;
    uint32_t ttl;
} new4_entry_t;

typedef struct new4_list {
    new4_entry_t *items;
    size_t n;
    size_t cap;
} new4_list_t;

typedef struct new6_entry {
    mt_ipv6_subnet_t subnet;
    bool has_ttl;
    uint32_t ttl;
} new6_entry_t;

typedef struct new6_list {
    new6_entry_t *items;
    size_t n;
    size_t cap;
} new6_list_t;

/* improve_only==false: unconditional overwrite (subnet/subnet6 static rules,
 * always permanent -- matches Go's unconditional `newList[subnet] = nil`).
 * improve_only==true: only overwrite when the existing entry has a TTL and
 * the new TTL is strictly greater (matches the domain-rule loop's
 * `!exists || (oldTTL != nil && ttl > *oldTTL)` check in rule_set.go). */
static mt_err_t new4_upsert(new4_list_t *l, mt_ipv4_subnet_t key, bool has_ttl, uint32_t ttl,
                            bool improve_only) {
    for (size_t i = 0; i < l->n; i++) {
        if (memcmp(&l->items[i].subnet, &key, sizeof(key)) != 0) { continue; }
        if (improve_only) {
            bool should_overwrite = l->items[i].has_ttl && has_ttl && ttl > l->items[i].ttl;
            if (!should_overwrite) { return MT_OK; }
        }
        l->items[i].has_ttl = has_ttl;
        l->items[i].ttl = ttl;
        return MT_OK;
    }
    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 16;
        new4_entry_t *p = realloc(l->items, ncap * sizeof(*p));
        if (!p) { return MT_ERR_NOMEM; }
        l->items = p;
        l->cap = ncap;
    }
    l->items[l->n].subnet = key;
    l->items[l->n].has_ttl = has_ttl;
    l->items[l->n].ttl = ttl;
    l->n++;
    return MT_OK;
}

static mt_err_t new6_upsert(new6_list_t *l, mt_ipv6_subnet_t key, bool has_ttl, uint32_t ttl,
                            bool improve_only) {
    for (size_t i = 0; i < l->n; i++) {
        if (memcmp(&l->items[i].subnet, &key, sizeof(key)) != 0) { continue; }
        if (improve_only) {
            bool should_overwrite = l->items[i].has_ttl && has_ttl && ttl > l->items[i].ttl;
            if (!should_overwrite) { return MT_OK; }
        }
        l->items[i].has_ttl = has_ttl;
        l->items[i].ttl = ttl;
        return MT_OK;
    }
    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 16;
        new6_entry_t *p = realloc(l->items, ncap * sizeof(*p));
        if (!p) { return MT_ERR_NOMEM; }
        l->items = p;
        l->cap = ncap;
    }
    l->items[l->n].subnet = key;
    l->items[l->n].has_ttl = has_ttl;
    l->items[l->n].ttl = ttl;
    l->n++;
    return MT_OK;
}

/* ---- subnet/subnet6 rule text parsing ("a.b.c.d[/n]") -------------------- */

/* Parses either "ip/cidr" or a bare "ip" (implicit /32), matching Go's
 * net.ParseCIDR-then-net.ParseIP fallback in rule_set.go's sync(). Returns
 * false when the rule text isn't a valid IPv4 CIDR/address (Go: `continue`,
 * i.e. this rule contributes nothing). */
static bool parse_ipv4_rule(const char *rule, mt_ipv4_subnet_t *out) {
    char buf[256];
    if (snprintf(buf, sizeof(buf), "%s", rule) >= (int)sizeof(buf)) { return false; }

    char *slash = strchr(buf, '/');
    long ones = 32;
    if (slash != NULL) {
        *slash = '\0';
        char *end = NULL;
        ones = strtol(slash + 1, &end, 10);
        if (end == slash + 1 || *end != '\0' || ones < 0 || ones > 32) { return false; }
    }

    struct in_addr a;
    if (inet_pton(AF_INET, buf, &a) != 1) { return false; }

    uint32_t ipval = ntohl(a.s_addr);
    uint32_t mask = (ones == 0) ? 0u : (uint32_t)(0xFFFFFFFFu << (32 - ones));
    uint32_t masked = ipval & mask;

    out->addr[0] = (uint8_t)(masked >> 24);
    out->addr[1] = (uint8_t)(masked >> 16);
    out->addr[2] = (uint8_t)(masked >> 8);
    out->addr[3] = (uint8_t)masked;
    out->cidr = (uint8_t)ones;
    return true;
}

static bool parse_ipv6_rule(const char *rule, mt_ipv6_subnet_t *out) {
    char buf[256];
    if (snprintf(buf, sizeof(buf), "%s", rule) >= (int)sizeof(buf)) { return false; }

    char *slash = strchr(buf, '/');
    long ones = 128;
    if (slash != NULL) {
        *slash = '\0';
        char *end = NULL;
        ones = strtol(slash + 1, &end, 10);
        if (end == slash + 1 || *end != '\0' || ones < 0 || ones > 128) { return false; }
    }

    struct in6_addr a;
    if (inet_pton(AF_INET6, buf, &a) != 1) { return false; }

    uint8_t full_bytes = (uint8_t)(ones / 8);
    uint8_t rem_bits = (uint8_t)(ones % 8);
    for (int i = 0; i < 16; i++) {
        if (i < full_bytes) {
            out->addr[i] = a.s6_addr[i];
        } else if (i == full_bytes && rem_bits > 0) {
            uint8_t mask = (uint8_t)(0xFFu << (8 - rem_bits));
            out->addr[i] = (uint8_t)(a.s6_addr[i] & mask);
        } else {
            out->addr[i] = 0;
        }
    }
    out->cidr = (uint8_t)ones;
    return true;
}

/* ---- construction / lifecycle -------------------------------------------- */

mt_ruleset_t *mt_ruleset_new(const mt_group_t *group, const mt_ruleset_deps_t *deps) {
    mt_ruleset_t *rs = calloc(1, sizeof(*rs));
    if (!rs) { return NULL; }
    rs->group = group;
    rs->deps = *deps;
    return rs;
}

void mt_ruleset_free(mt_ruleset_t *rs) {
    if (!rs) { return; }
    mt_ipset_to_link_free(rs->ipset_to_link);
    mt_ipset_free(rs->ipset);
    free(rs);
}

const mt_group_t *mt_ruleset_group(const mt_ruleset_t *rs) {
    return rs->group;
}

mt_group_t *mt_ruleset_group_mut(mt_ruleset_t *rs) {
    return (mt_group_t *)rs->group;
}

bool mt_ruleset_runtime_enabled(const mt_ruleset_t *rs) {
    return rs->enabled;
}

/* ---- enable / disable ----------------------------------------------------- */

static mt_err_t ruleset_enable_locked(mt_ruleset_t *rs) {
    if (rs->enabled) { return MT_OK; }
    rs->enabled = true;
    if (!configured_enabled(rs)) { return MT_OK; }

    char id_buf[MT_ID_STR_LEN];
    mt_id_format(rs->group->id, id_buf);
    char ipset_name[128];
    char chain_name[128];
    snprintf(ipset_name, sizeof(ipset_name), "%s%s", rs->deps.ipset_prefix, id_buf);
    snprintf(chain_name, sizeof(chain_name), "%s%s", rs->deps.chain_prefix, id_buf);

    mt_ipset_nl_t *nl = mt_ipset_nl_real_new();
    if (!nl) { return MT_ERR_NOMEM; }
    mt_ipset_t *ipset = mt_ipset_new(nl, ipset_name);
    if (!ipset) { return MT_ERR_NOMEM; } /* mt_ipset_new frees nl on failure */

    const char *iface = rs->group->iface != NULL ? rs->group->iface : "";
    mt_ipset_to_link_t *link = mt_ipset_to_link_new(chain_name, iface, ipset, rs->deps.ipt4,
                                                    rs->deps.ipt6, rs->deps.rtnl,
                                                    rs->deps.start_idx);
    if (!link) {
        mt_ipset_free(ipset);
        return MT_ERR_NOMEM;
    }

    mt_err_t err = mt_ipset_to_link_clear_if_disabled(link);
    if (err != MT_OK) {
        mt_ipset_to_link_free(link);
        mt_ipset_free(ipset);
        return err;
    }

    err = mt_ipset_enable(ipset);
    if (err != MT_OK) {
        mt_ipset_to_link_free(link);
        mt_ipset_free(ipset);
        return err;
    }
    rs->ipset = ipset;

    err = mt_ipset_to_link_enable(link);
    if (err != MT_OK) {
        /* Matches Go: g.ipset is left assigned here (only g.ipsetToLink
         * stays unset on this failure path) -- the caller's disable() will
         * clean up rs->ipset. mt_ipset_to_link_enable() already rolls back
         * any partial iptables/rule/route state it created itself. */
        mt_ipset_to_link_free(link);
        return err;
    }
    rs->ipset_to_link = link;
    return MT_OK;
}

mt_err_t mt_ruleset_enable(mt_ruleset_t *rs) {
    mt_err_t err = ruleset_enable_locked(rs);
    if (err != MT_OK) {
        mt_ruleset_disable(rs);
        return err;
    }
    return MT_OK;
}

mt_err_t mt_ruleset_disable(mt_ruleset_t *rs) {
    if (!rs->enabled) { return MT_OK; }
    rs->enabled = false;
    if (!configured_enabled(rs)) { return MT_OK; }

    mt_err_t e1 = MT_OK;
    mt_err_t e2 = MT_OK;
    if (rs->ipset_to_link) {
        e1 = mt_ipset_to_link_disable(rs->ipset_to_link);
        mt_ipset_to_link_free(rs->ipset_to_link);
        rs->ipset_to_link = NULL;
    }
    if (rs->ipset) {
        e2 = mt_ipset_disable(rs->ipset);
        mt_ipset_free(rs->ipset);
        rs->ipset = NULL;
    }
    return e1 != MT_OK ? e1 : e2;
}

mt_err_t mt_ruleset_prepare_iptables(mt_ruleset_t *rs) {
    if (!rs->enabled) { return MT_OK; }
    if (!configured_enabled(rs)) { return MT_OK; }
    if (!rs->ipset_to_link) { return MT_OK; }
    return mt_ipset_to_link_prepare_iptables(rs->ipset_to_link);
}

/* ---- direct add (DNS hot path) -------------------------------------------- */

mt_err_t mt_ruleset_add_ipv4(mt_ruleset_t *rs, mt_ipv4_subnet_t subnet, const uint32_t *ttl) {
    if (!rs->enabled) { return MT_OK; }
    if (!configured_enabled(rs)) { return MT_OK; }
    return mt_ipset_add4(rs->ipset, subnet, ttl);
}

mt_err_t mt_ruleset_add_ipv6(mt_ruleset_t *rs, mt_ipv6_subnet_t subnet, const uint32_t *ttl) {
    if (!rs->enabled) { return MT_OK; }
    if (!configured_enabled(rs)) { return MT_OK; }
    return mt_ipset_add6(rs->ipset, subnet, ttl);
}

/* ---- link/addr hooks ------------------------------------------------------ */

mt_err_t mt_ruleset_on_link_up(mt_ruleset_t *rs) {
    if (!rs->enabled) { return MT_OK; }
    if (!configured_enabled(rs)) { return MT_OK; }
    if (!rs->ipset_to_link) { return MT_OK; }
    return mt_ipset_to_link_on_link_up(rs->ipset_to_link);
}

mt_err_t mt_ruleset_on_addr_change(mt_ruleset_t *rs) {
    if (!rs->enabled) { return MT_OK; }
    if (!configured_enabled(rs)) { return MT_OK; }
    if (!rs->ipset_to_link) { return MT_OK; }
    return mt_ipset_to_link_on_addr_change(rs->ipset_to_link);
}

/* ---- sync ------------------------------------------------------------------ */

static void sync_build_new_lists(mt_ruleset_t *rs, mt_cache_t *cache, int64_t now,
                                 new4_list_t *new4, new6_list_t *new6, mt_err_t *err) {
    char **known = NULL;
    size_t n_known = 0;
    *err = mt_cache_list_known_domains(cache, &known, &n_known);
    if (*err != MT_OK) { return; }

    const mt_group_t *g = rs->group;
    for (size_t i = 0; i < g->n_rules && *err == MT_OK; i++) {
        const mt_rule_t *rule = g->rules[i];
        if (!rule->enable) { continue; }

        if (strcmp(rule->type, MT_RULE_SUBNET) == 0) {
            mt_ipv4_subnet_t subnet;
            if (!parse_ipv4_rule(rule->rule, &subnet)) { continue; }
            if (subnet.cidr == 0) {
                /* TODO: matches Go's dirty hack for a full 0.0.0.0/0 rule,
                 * splitting into two /1 halves; see decisions.md. */
                mt_ipv4_subnet_t a = {.addr = {0x00, 0, 0, 0}, .cidr = 1};
                mt_ipv4_subnet_t b = {.addr = {0x80, 0, 0, 0}, .cidr = 1};
                *err = new4_upsert(new4, a, false, 0, false);
                if (*err == MT_OK) { *err = new4_upsert(new4, b, false, 0, false); }
            } else {
                *err = new4_upsert(new4, subnet, false, 0, false);
            }
            continue;
        }

        if (strcmp(rule->type, MT_RULE_SUBNET6) == 0) {
            mt_ipv6_subnet_t subnet;
            if (!parse_ipv6_rule(rule->rule, &subnet)) { continue; }
            if (subnet.cidr == 0) {
                mt_ipv6_subnet_t a = {0};
                a.cidr = 1;
                mt_ipv6_subnet_t b = {0};
                b.addr[0] = 0x80;
                b.cidr = 1;
                *err = new6_upsert(new6, a, false, 0, false);
                if (*err == MT_OK) { *err = new6_upsert(new6, b, false, 0, false); }
            } else {
                *err = new6_upsert(new6, subnet, false, 0, false);
            }
            continue;
        }

        mt_rule_matcher_t *rm = mt_rule_matcher_new(rule->type, rule->rule);
        if (!rm) {
            *err = MT_ERR_NOMEM;
            continue;
        }
        for (size_t k = 0; k < n_known && *err == MT_OK; k++) {
            if (!mt_rule_matcher_match(rm, known[k])) { continue; }

            mt_cache_addr_t *addrs = NULL;
            size_t n_addrs = 0;
            *err = mt_cache_get_addresses(cache, known[k], now, &addrs, &n_addrs);
            if (*err != MT_OK) { break; }

            for (size_t a = 0; a < n_addrs; a++) {
                int64_t remain = addrs[a].deadline - now;
                if (remain <= 0) { continue; }
                uint32_t ttl = (uint32_t)remain;
                if (addrs[a].addr_len == 4) {
                    mt_ipv4_subnet_t subnet = {.cidr = 0};
                    memcpy(subnet.addr, addrs[a].addr, 4);
                    *err = new4_upsert(new4, subnet, true, ttl, true);
                } else if (addrs[a].addr_len == 16) {
                    mt_ipv6_subnet_t subnet = {.cidr = 0};
                    memcpy(subnet.addr, addrs[a].addr, 16);
                    *err = new6_upsert(new6, subnet, true, ttl, true);
                }
                if (*err != MT_OK) { break; }
            }
            free(addrs);
        }
        mt_rule_matcher_free(rm);
    }

    mt_cache_free_strings(known, n_known);
}

static void sync_diff_v4(mt_ruleset_t *rs, new4_list_t *new4, mt_err_t *err) {
    mt_ipset_entry4_t *old = NULL;
    size_t n_old = 0;
    *err = mt_ipset_list4(rs->ipset, &old, &n_old);
    if (*err != MT_OK) { return; }

    bool *kept = n_old ? calloc(n_old, sizeof(bool)) : NULL;
    if (n_old && !kept) {
        free(old);
        *err = MT_ERR_NOMEM;
        return;
    }

    for (size_t i = 0; i < new4->n; i++) {
        bool skip = false;
        for (size_t j = 0; j < n_old; j++) {
            if (memcmp(old[j].subnet.addr, new4->items[i].subnet.addr, 4) != 0 ||
                old[j].subnet.cidr != new4->items[i].subnet.cidr) {
                continue;
            }
            kept[j] = true;
            skip = !old[j].has_timeout ||
                   (new4->items[i].has_ttl && new4->items[i].ttl < old[j].timeout);
            break;
        }
        if (skip) { continue; }
        const uint32_t *ttlp = new4->items[i].has_ttl ? &new4->items[i].ttl : NULL;
        mt_err_t aerr = mt_ipset_add4(rs->ipset, new4->items[i].subnet, ttlp);
        if (aerr != MT_OK) {
            MT_ERROR("ruleset %s: failed to add ipv4 subnet: %s", rs->group->name,
                     mt_err_str(aerr));
        }
    }
    for (size_t j = 0; j < n_old; j++) {
        if (kept[j]) { continue; }
        mt_err_t derr = mt_ipset_del4(rs->ipset, old[j].subnet);
        if (derr != MT_OK) {
            MT_ERROR("ruleset %s: failed to delete ipv4 subnet: %s", rs->group->name,
                     mt_err_str(derr));
        }
    }

    free(old);
    free(kept);
}

static void sync_diff_v6(mt_ruleset_t *rs, new6_list_t *new6, mt_err_t *err) {
    mt_ipset_entry6_t *old = NULL;
    size_t n_old = 0;
    *err = mt_ipset_list6(rs->ipset, &old, &n_old);
    if (*err != MT_OK) { return; }

    bool *kept = n_old ? calloc(n_old, sizeof(bool)) : NULL;
    if (n_old && !kept) {
        free(old);
        *err = MT_ERR_NOMEM;
        return;
    }

    for (size_t i = 0; i < new6->n; i++) {
        bool skip = false;
        for (size_t j = 0; j < n_old; j++) {
            if (memcmp(old[j].subnet.addr, new6->items[i].subnet.addr, 16) != 0 ||
                old[j].subnet.cidr != new6->items[i].subnet.cidr) {
                continue;
            }
            kept[j] = true;
            skip = !old[j].has_timeout ||
                   (new6->items[i].has_ttl && new6->items[i].ttl < old[j].timeout);
            break;
        }
        if (skip) { continue; }
        const uint32_t *ttlp = new6->items[i].has_ttl ? &new6->items[i].ttl : NULL;
        mt_err_t aerr = mt_ipset_add6(rs->ipset, new6->items[i].subnet, ttlp);
        if (aerr != MT_OK) {
            MT_ERROR("ruleset %s: failed to add ipv6 subnet: %s", rs->group->name,
                     mt_err_str(aerr));
        }
    }
    for (size_t j = 0; j < n_old; j++) {
        if (kept[j]) { continue; }
        mt_err_t derr = mt_ipset_del6(rs->ipset, old[j].subnet);
        if (derr != MT_OK) {
            MT_ERROR("ruleset %s: failed to delete ipv6 subnet: %s", rs->group->name,
                     mt_err_str(derr));
        }
    }

    free(old);
    free(kept);
}

mt_err_t mt_ruleset_sync(mt_ruleset_t *rs, mt_cache_t *cache, int64_t now) {
    if (!rs->enabled) { return MT_OK; }
    if (!configured_enabled(rs)) { return MT_OK; }

    new4_list_t new4 = {0};
    new6_list_t new6 = {0};
    mt_err_t err = MT_OK;

    sync_build_new_lists(rs, cache, now, &new4, &new6, &err);
    if (err == MT_OK) { sync_diff_v4(rs, &new4, &err); }
    if (err == MT_OK) { sync_diff_v6(rs, &new6, &err); }

    free(new4.items);
    free(new6.items);
    return err;
}
