#include "magitrickle/keenetic_policy.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>

bool mt_kn_policy_config_valid(const mt_kn_policy_config_t *c)
{
    if (!c || (c->mode != MT_KN_POLICY_INCLUDE && c->mode != MT_KN_POLICY_EXCLUDE)) { return false; }
    size_t n = strnlen(c->policy_id, sizeof(c->policy_id));
    if (n == sizeof(c->policy_id) || (c->enabled && !n)) { return false; }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)c->policy_id[i] < 32u || c->policy_id[i] == 127) { return false; }
    }
    return true;
}
bool mt_kn_policy_config_equal(const mt_kn_policy_config_t *a, const mt_kn_policy_config_t *b)
{
    return a->enabled == b->enabled && a->mode == b->mode && strcmp(a->policy_id, b->policy_id) == 0;
}
mt_err_t mt_kn_policy_scope_init(mt_kn_policy_scope_t *p, const mt_kn_policy_config_t *c, const char *prefix)
{
    if (!p || !prefix || !mt_kn_policy_config_valid(c)) { return MT_ERR_INVAL; }
    memset(p, 0, sizeof(*p));
    p->config = *c;
    int a = snprintf(p->gate, sizeof(p->gate), "%sPOLICY", prefix);
    int b = snprintf(p->dispatch, sizeof(p->dispatch), "%sPOLICY_RULES", prefix);
    /* Kernel user-chain names are limited to 28 characters. Disabled legacy
     * configurations need not satisfy a constraint for unused chains. */
    return c->enabled && (a < 0 || b < 0 || a > 28 || b > 28) ? MT_ERR_INVAL : MT_OK;
}
mt_err_t mt_kn_policy_guard(const mt_kn_policy_scope_t *p, mt_ipt_t *ipt,
                            const char *table, const char *chain)
{
    if (!ipt || !p || !p->config.enabled) { return MT_OK; }
    if (!p->ready) {
        const char *args[] = {"-j", "RETURN"};
        return mt_ipt_append(ipt, table, chain, args, 2);
    }
    char mark[32];
    snprintf(mark, sizeof(mark), "0x%x/0x0fffffff", p->mark);
    if (p->config.mode == MT_KN_POLICY_INCLUDE) {
        const char *args[] = {"-m", "mark", "!", "--mark", mark, "-j", "RETURN"};
        return mt_ipt_append(ipt, table, chain, args, 7);
    }
    const char *args[] = {"-m", "mark", "--mark", mark, "-j", "RETURN"};
    return mt_ipt_append(ipt, table, chain, args, 6);
}
mt_err_t mt_kn_policy_route_guard(const mt_kn_policy_scope_t *p, mt_ipt_t *ipt,
                                  const char *table, const char *chain, uint32_t route_mark)
{
    if (!ipt || !p || !p->config.enabled) { return MT_OK; }
    if (!p->ready) { return mt_kn_policy_guard(p, ipt, table, chain); }
    char mark[32];
    snprintf(mark, sizeof(mark), "0x%x/0x0fffffff", route_mark & MT_KN_POLICY_MASK);
    const char *args[] = {"-m", "connmark", "!", "--mark", mark, "-j", "RETURN"};
    return mt_ipt_append(ipt, table, chain, args, 7);
}
/* Keenetic 1.4.21 can parse addrtype in userspace while its kernel rejects
 * the match at COMMIT. Use core destination matches for assigned router
 * addresses instead; no new xtables extension or module loader is required.
 * Re-enumerate on each rebuild, including address/link change notifications. */
static const void *local_address(const struct ifaddrs *ifa, int family)
{
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != family) { return NULL; }
    if (family == AF_INET) {
        return &((const struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
    }
    return &((const struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr;
}

static mt_err_t guard_local_addresses(mt_ipt_t *ipt, const char *chain)
{
    struct ifaddrs *addresses = NULL;
    if (getifaddrs(&addresses) != 0) { return mt_err_from_errno(errno); }
    int family = mt_ipt_proto(ipt) == MT_IPT_PROTO_IPV4 ? AF_INET : AF_INET6;
    size_t width = family == AF_INET ? 4u : 16u;
    mt_err_t err = MT_OK;
    for (const struct ifaddrs *ifa = addresses; ifa; ifa = ifa->ifa_next) {
        const void *address = local_address(ifa, family);
        if (!address) { continue; }
        bool duplicate = false;
        for (const struct ifaddrs *before = addresses; before != ifa; before = before->ifa_next) {
            const void *other = local_address(before, family);
            if (other && memcmp(address, other, width) == 0) { duplicate = true; break; }
        }
        if (duplicate) { continue; }
        char host[INET6_ADDRSTRLEN], destination[INET6_ADDRSTRLEN + 4];
        if (!inet_ntop(family, address, host, sizeof(host))) {
            err = mt_err_from_errno(errno);
            break;
        }
        snprintf(destination, sizeof(destination), "%s/%u", host, family == AF_INET ? 32u : 128u);
        const char *args[] = {"-d", destination, "-j", "RETURN"};
        err = mt_ipt_append(ipt, "mangle", chain, args, 4);
        if (err != MT_OK) { break; }
    }
    freeifaddrs(addresses);
    return err;
}

mt_err_t mt_kn_policy_prepare(const mt_kn_policy_scope_t *p, mt_ipt_t *ipt)
{
    if (!ipt || !p || !p->config.enabled) { return MT_OK; }
    mt_err_t err = mt_ipt_register_chain_patch(ipt, "mangle", p->dispatch);
    if (err != MT_OK) { return err; }
    /* A patch chain with no commands is not created by the engine. Keep an
     * ordered terminal entry even with zero enabled groups. INT_MAX also makes
     * later group additions sort before this entry, unlike an ordinary RETURN. */
    const char *terminal[] = {"-j", "RETURN"};
    err = mt_ipt_append_ordered(ipt, "mangle", p->dispatch, INT_MAX, terminal, 2);
    if (err != MT_OK) { return err; }
    err = mt_ipt_register_chain_override(ipt, "mangle", p->gate);
    if (err != MT_OK) { return err; }
    if (p->ready) {
        const char *reply[] = {"-m", "conntrack", "--ctdir", "REPLY", "-j", "RETURN"};
        err = mt_ipt_append(ipt, "mangle", p->gate, reply, 6);
        if (err != MT_OK) { return err; }
        /* Keep the original Keenetic mark for DNS to the router. The nat
         * guard must see it before deciding whether to redirect to MT. */
        err = guard_local_addresses(ipt, p->gate);
        if (err != MT_OK) { return err; }
    }
    /* Until RCI resolves the policy, RETURN alone closes the scope. Do not
     * make startup depend on address enumeration for an inactive gate. */
    err = mt_kn_policy_guard(p, ipt, "mangle", p->gate);
    if (err != MT_OK) { return err; }
    const char *dispatch[] = {"-j", p->dispatch};
    err = mt_ipt_append(ipt, "mangle", p->gate, dispatch, 2);
    if (err != MT_OK) { return err; }
    const char *jump[] = {"-j", p->gate};
    return mt_ipt_append(ipt, "mangle", "PREROUTING", jump, 2);
}
