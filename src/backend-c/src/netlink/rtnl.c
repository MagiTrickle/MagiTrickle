/* See rtnl.h. */
#include "magitrickle/rtnl.h"
#include "magitrickle/nlattr_iter.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <libmnl/libmnl.h>
#include <linux/fib_rules.h>
#include <linux/if.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define MT_RTNL_REQBUF 1024
#define MT_RTNL_RECVBUF 8192
#define MT_IFA_FLAGS_ATTR 8u

struct mt_rtnl {
    struct mnl_socket *nl;
    uint32_t seq;
    uint32_t *owned_tables;
    size_t n_owned_tables, cap_owned_tables;
};

mt_rtnl_t *mt_rtnl_open(void) {
    mt_rtnl_t *r = calloc(1, sizeof(*r));
    if (!r) { return NULL; }
    r->nl = mnl_socket_open(NETLINK_ROUTE);
    if (!r->nl) {
        free(r);
        return NULL;
    }
    if (mnl_socket_bind(r->nl, 0, MNL_SOCKET_AUTOPID) < 0) {
        mnl_socket_close(r->nl);
        free(r);
        return NULL;
    }
    return r;
}

void mt_rtnl_close(mt_rtnl_t *r) {
    if (!r) { return; }
    if (r->nl) { mnl_socket_close(r->nl); }
    free(r->owned_tables);
    free(r);
}

static mt_err_t nl_execute(mt_rtnl_t *r, struct nlmsghdr *nlh, int *out_code,
                           void (*msg_cb)(const struct nlmsghdr *, void *), void *cb_ud) {
    *out_code = 0;
    const uint32_t seq = nlh->nlmsg_seq;
    const bool dump = (nlh->nlmsg_flags & NLM_F_DUMP) == NLM_F_DUMP;
    if (mnl_socket_sendto(r->nl, nlh, nlh->nlmsg_len) < 0) { return mt_err_from_errno(errno); }

    uint8_t buf[MT_RTNL_RECVBUF];
    for (;;) {
        ssize_t ret = mnl_socket_recvfrom(r->nl, buf, sizeof(buf));
        if (ret < 0) {
            if (errno == EINTR) { continue; }
            return mt_err_from_errno(errno);
        }
        if (ret == 0) { return MT_ERR_PROTO; }

        int len = (int)ret;
        struct nlmsghdr *h = (struct nlmsghdr *)buf;
        bool done = false, received = false;
        while (mnl_nlmsg_ok(h, len)) {
            if (h->nlmsg_seq != seq) {
                h = mnl_nlmsg_next(h, &len);
                continue;
            }
            if (h->nlmsg_flags & NLM_F_DUMP_INTR) { return MT_ERR_AGAIN; }
            if (h->nlmsg_type == NLMSG_ERROR) {
                if (mnl_nlmsg_get_payload_len(h) < sizeof(struct nlmsgerr)) {
                    return MT_ERR_PROTO;
                }
                const struct nlmsgerr *e = mnl_nlmsg_get_payload(h);
                if (e->error == INT_MIN) { return MT_ERR_PROTO; }
                *out_code = e->error < 0 ? -e->error : e->error;
                if (*out_code != 0 || !dump) { done = true; break; }
            } else if (h->nlmsg_type == NLMSG_DONE) {
                /* A dump can finish with an error even without DUMP_INTR.
                 * Rtnetlink DONE starts with a native-endian int status;
                 * optional extack attributes may follow it. */
                if (mnl_nlmsg_get_payload_len(h) < sizeof(int)) { return MT_ERR_PROTO; }
                int status;
                memcpy(&status, mnl_nlmsg_get_payload(h), sizeof(status));
                if (status == INT_MIN) { return MT_ERR_PROTO; }
                *out_code = status < 0 ? -status : status;
                done = true;
                break;
            } else {
                if (msg_cb) { msg_cb(h, cb_ud); }
                received = true;
            }
            h = mnl_nlmsg_next(h, &len);
        }
        if (len != 0 && !done) { return MT_ERR_PROTO; }
        if (done) { return MT_OK; }
        if (!dump && received) { return MT_OK; }
        if (!dump) { return MT_ERR_PROTO; }
    }
}

/* ---- ip rule add/del ----------------------------------------------------- */

static struct nlmsghdr *put_rule_header(uint8_t *buf, uint16_t type, uint16_t flags, uint32_t seq,
                                        int family, uint32_t table) {
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type = type;
    nlh->nlmsg_flags = flags;
    nlh->nlmsg_seq = seq;

    struct fib_rule_hdr *frh = mnl_nlmsg_put_extra_header(nlh, sizeof(*frh));
    memset(frh, 0, sizeof(*frh));
    frh->family = (uint8_t)family;
    frh->table = table < 256 ? (uint8_t)table : RT_TABLE_UNSPEC;
    frh->action = (type == RTM_NEWRULE) ? FR_ACT_TO_TBL : FR_ACT_UNSPEC;
    return nlh;
}

mt_err_t mt_rtnl_rule_add(mt_rtnl_t *r, int family, uint32_t mark, uint32_t table) {
    uint8_t buf[MT_RTNL_REQBUF];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = put_rule_header(
        buf, RTM_NEWRULE, NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK, ++r->seq, family,
        table);
    mnl_attr_put_u32(nlh, FRA_FWMARK, mark);
    if (table >= 256) { mnl_attr_put_u32(nlh, FRA_TABLE, table); }

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, NULL, NULL);
    if (err != MT_OK) { return err; }
    if (code == 0) { return MT_OK; }
    return mt_err_from_errno(code);
}

mt_err_t mt_rtnl_rule_del(mt_rtnl_t *r, int family, uint32_t mark, uint32_t table) {
    uint8_t buf[MT_RTNL_REQBUF];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh =
        put_rule_header(buf, RTM_DELRULE, NLM_F_REQUEST | NLM_F_ACK, ++r->seq, family, table);
    mnl_attr_put_u32(nlh, FRA_FWMARK, mark);
    if (table >= 256) { mnl_attr_put_u32(nlh, FRA_TABLE, table); }

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, NULL, NULL);
    if (err != MT_OK) { return err; }
    if (code == 0 || code == ENOENT) { return MT_OK; }
    return mt_err_from_errno(code);
}

/* ---- routes --------------------------------------------------------------- */

static struct nlmsghdr *put_route_header(uint8_t *buf, uint16_t type, uint16_t flags, uint32_t seq,
                                         int family, uint32_t table, uint8_t rtm_type) {
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type = type;
    nlh->nlmsg_flags = flags;
    nlh->nlmsg_seq = seq;

    struct rtmsg *rtm = mnl_nlmsg_put_extra_header(nlh, sizeof(*rtm));
    memset(rtm, 0, sizeof(*rtm));
    rtm->rtm_family = (uint8_t)family;
    rtm->rtm_table = table < 256 ? (uint8_t)table : RT_TABLE_UNSPEC;
    rtm->rtm_protocol = RTPROT_BOOT;
    rtm->rtm_scope = RT_SCOPE_UNIVERSE;
    rtm->rtm_type = rtm_type;
    return nlh;
}

mt_err_t mt_rtnl_route_add_blackhole(mt_rtnl_t *r, int family, uint32_t table, uint32_t priority) {
    uint8_t buf[MT_RTNL_REQBUF];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = put_route_header(
        buf, RTM_NEWROUTE, NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK, ++r->seq, family,
        table, RTN_BLACKHOLE);
    if (table >= 256) { mnl_attr_put_u32(nlh, RTA_TABLE, table); }
    mnl_attr_put_u32(nlh, RTA_PRIORITY, priority);

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, NULL, NULL);
    if (err != MT_OK) { return err; }
    if (code == 0 || code == EEXIST) { return MT_OK; }
    return mt_err_from_errno(code);
}

mt_err_t mt_rtnl_route_del_blackhole(mt_rtnl_t *r, int family, uint32_t table, uint32_t priority) {
    uint8_t buf[MT_RTNL_REQBUF];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = put_route_header(buf, RTM_DELROUTE, NLM_F_REQUEST | NLM_F_ACK, ++r->seq,
                                            family, table, RTN_BLACKHOLE);
    if (table >= 256) { mnl_attr_put_u32(nlh, RTA_TABLE, table); }
    mnl_attr_put_u32(nlh, RTA_PRIORITY, priority);

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, NULL, NULL);
    if (err != MT_OK) { return err; }
    if (code == 0 || code == ESRCH) { return MT_OK; }
    return mt_err_from_errno(code);
}

mt_err_t mt_rtnl_route_add_iface(mt_rtnl_t *r, int family, uint32_t table, uint32_t priority,
                                 int oif, const uint8_t *gw, uint8_t gw_len, bool *enodev) {
    *enodev = false;
    uint8_t buf[MT_RTNL_REQBUF];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = put_route_header(
        buf, RTM_NEWROUTE, NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK, ++r->seq, family,
        table, RTN_UNICAST);
    if (table >= 256) { mnl_attr_put_u32(nlh, RTA_TABLE, table); }
    mnl_attr_put_u32(nlh, RTA_PRIORITY, priority);
    mnl_attr_put_u32(nlh, RTA_OIF, (uint32_t)oif);
    if (gw && gw_len > 0) { mnl_attr_put(nlh, RTA_GATEWAY, gw_len, gw); }

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, NULL, NULL);
    if (err != MT_OK) { return err; }
    if (code == ENODEV) {
        *enodev = true;
        return MT_OK;
    }
    if (code == 0 || code == EEXIST) { return MT_OK; }
    return mt_err_from_errno(code);
}

mt_err_t mt_rtnl_route_replace_iface(mt_rtnl_t *r, int family, uint32_t table, uint32_t priority,
                                 int oif, const uint8_t *gw, uint8_t gw_len, bool *enodev) {
    *enodev = false;
    uint8_t buf[MT_RTNL_REQBUF];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = put_route_header(
        buf, RTM_NEWROUTE, NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK, ++r->seq, family,
        table, RTN_UNICAST);
    if (table >= 256) { mnl_attr_put_u32(nlh, RTA_TABLE, table); }
    mnl_attr_put_u32(nlh, RTA_PRIORITY, priority);
    mnl_attr_put_u32(nlh, RTA_OIF, (uint32_t)oif);
    if (gw && gw_len > 0) { mnl_attr_put(nlh, RTA_GATEWAY, gw_len, gw); }

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, NULL, NULL);
    if (err != MT_OK) { return err; }
    if (code == ENODEV || code == ENETDOWN || code == ENETUNREACH) {
        *enodev = true;
        return MT_OK;
    }
    if (code == 0) { return MT_OK; }
    return mt_err_from_errno(code);
}

mt_err_t mt_rtnl_route_del_iface(mt_rtnl_t *r, int family, uint32_t table, uint32_t priority,
                                 int oif, const uint8_t *gw, uint8_t gw_len) {
    uint8_t buf[MT_RTNL_REQBUF];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = put_route_header(buf, RTM_DELROUTE, NLM_F_REQUEST | NLM_F_ACK, ++r->seq,
                                            family, table, RTN_UNICAST);
    if (table >= 256) { mnl_attr_put_u32(nlh, RTA_TABLE, table); }
    mnl_attr_put_u32(nlh, RTA_PRIORITY, priority);
    mnl_attr_put_u32(nlh, RTA_OIF, (uint32_t)oif);
    if (gw && gw_len > 0) { mnl_attr_put(nlh, RTA_GATEWAY, gw_len, gw); }

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, NULL, NULL);
    if (err != MT_OK) { return err; }
    if (code == 0 || code == ESRCH) { return MT_OK; }
    return mt_err_from_errno(code);
}

/* ---- link lookup ------------------------------------------------------- */

typedef struct link_ctx {
    bool found;
    unsigned flags;
    uint8_t operstate;
} link_ctx_t;

static void link_msg_cb(const struct nlmsghdr *h, void *ud) {
    link_ctx_t *ctx = ud;
    if (h->nlmsg_type != RTM_NEWLINK) { return; }
    if (mnl_nlmsg_get_payload_len(h) < sizeof(struct ifinfomsg)) { return; }
    const struct ifinfomsg *ifi = mnl_nlmsg_get_payload(h);
    mt_nlattr_iter_t it;
    const struct nlattr *attr;
    if (!mt_nlattr_iter_init_nlmsg(&it, h, sizeof(*ifi))) { return; }
    while (mt_nlattr_iter_next(&it, &attr)) {
        if (mnl_attr_get_type(attr) == IFLA_OPERSTATE && mnl_attr_get_payload_len(attr) == 1) {
            ctx->operstate = mnl_attr_get_u8(attr);
        }
    }
    ctx->found = true;
    ctx->flags = ifi->ifi_flags;
}

mt_err_t mt_rtnl_link_by_name(mt_rtnl_t *r, const char *name, mt_link_info_t *out, bool *found) {
    *found = false;
    memset(out, 0, sizeof(*out));

    unsigned idx = if_nametoindex(name);
    if (idx == 0) { return MT_OK; /* ENODEV: interface doesn't exist yet */ }

    uint8_t buf[MT_RTNL_REQBUF];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type = RTM_GETLINK;
    /* NLM_F_REQUEST only: a by-index GETLINK is a single-reply "get", not
     * a mutation or a dump. Adding NLM_F_ACK here (as this code
     * originally did) makes the kernel send an extra trailing ACK that
     * this non-dump code path's early-break never consumes, leaving it
     * stuck in the socket's receive queue -- the next unrelated request
     * on this same mt_rtnl_t then reads that stale ACK first and every
     * read after it is off by one message. Found via a real-kernel
     * smoke test in this sandbox (rtnetlink, unlike ipset, works here). */
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_seq = ++r->seq;
    struct ifinfomsg *ifi = mnl_nlmsg_put_extra_header(nlh, sizeof(*ifi));
    memset(ifi, 0, sizeof(*ifi));
    ifi->ifi_index = (int)idx;

    link_ctx_t ctx = {0};
    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, link_msg_cb, &ctx);
    if (err != MT_OK) { return err; }
    if (code != 0 && code != ENODEV) { return mt_err_from_errno(code); }
    if (!ctx.found) { return MT_OK; }

    out->ifindex = (int)idx;
    out->up = (ctx.flags & IFF_UP) != 0;
    out->point_to_point = (ctx.flags & IFF_POINTOPOINT) != 0;
    out->operational = out->up && (ctx.operstate == IF_OPER_UP || ctx.operstate == IF_OPER_UNKNOWN);
    *found = true;
    return MT_OK;
}

/* ---- gateway lookup (route dump filtered by oif) ------------------------ */

static bool owns_table(const mt_rtnl_t *r, uint32_t table)
{
    for (size_t i = 0; i < r->n_owned_tables; i++) {
        if (r->owned_tables[i] == table) { return true; }
    }
    return false;
}

mt_err_t mt_rtnl_reserve_table(mt_rtnl_t *r, uint32_t table)
{
    if (!r || !table) { return MT_ERR_INVAL; }
    if (owns_table(r, table)) { return MT_OK; }
    if (r->n_owned_tables == r->cap_owned_tables) {
        if (r->cap_owned_tables > SIZE_MAX / 2 / sizeof(uint32_t)) { return MT_ERR_NOMEM; }
        size_t cap = r->cap_owned_tables ? r->cap_owned_tables * 2 : 8;
        uint32_t *next = realloc(r->owned_tables, cap * sizeof(*next));
        if (!next) { return MT_ERR_NOMEM; }
        r->owned_tables = next; r->cap_owned_tables = cap;
    }
    r->owned_tables[r->n_owned_tables++] = table;
    return MT_OK;
}

void mt_rtnl_release_table(mt_rtnl_t *r, uint32_t table)
{
    if (!r) { return; }
    for (size_t i = 0; i < r->n_owned_tables; i++) {
        if (r->owned_tables[i] == table) {
            r->owned_tables[i] = r->owned_tables[--r->n_owned_tables];
            return;
        }
    }
}

typedef struct gw_ctx {
    mt_rtnl_t *r;
    int family, want_oif;
    bool profile, found;
    unsigned rank;
    uint32_t metric;
    uint8_t gw[16], gw_len;
} gw_ctx_t;

static void gw_msg_cb(const struct nlmsghdr *h, void *ud) {
    gw_ctx_t *ctx = ud;
    if ((ctx->found && !ctx->profile) || h->nlmsg_type != RTM_NEWROUTE ||
        mnl_nlmsg_get_payload_len(h) < sizeof(struct rtmsg)) { return; }
    const struct rtmsg *rtm = mnl_nlmsg_get_payload(h);
    if (rtm->rtm_family != ctx->family || rtm->rtm_type != RTN_UNICAST) { return; }
    int oif = -1;
    uint8_t gw[16] = {0}, gwlen = 0;
    uint32_t table = rtm->rtm_table, metric = 0;
    mt_nlattr_iter_t it;
    const struct nlattr *attr;
    if (!mt_nlattr_iter_init_nlmsg(&it, h, sizeof(*rtm))) { return; }
    while (mt_nlattr_iter_next(&it, &attr)) {
        uint16_t type = mnl_attr_get_type(attr), len = mnl_attr_get_payload_len(attr);
        if (type == RTA_OIF && len == 4) { oif = (int)mnl_attr_get_u32(attr); }
        else if (type == RTA_TABLE && len == 4) { table = mnl_attr_get_u32(attr); }
        else if (type == RTA_PRIORITY && len == 4) { metric = mnl_attr_get_u32(attr); }
        else if (type == RTA_GATEWAY &&
                 ((ctx->family == AF_INET && len == 4) || (ctx->family == AF_INET6 && len == 16))) {
            memcpy(gw, mnl_attr_get_payload(attr), len); gwlen = (uint8_t)len;
        }
    }
    if (oif != ctx->want_oif || !gwlen || (ctx->profile && owns_table(ctx->r, table))) { return; }
    /* Prefer the manager's default route, then main-table routes, then
     * other routes via this interface. Never depend on kernel dump order. */
    unsigned rank = (rtm->rtm_dst_len == 0 ? 2u : 0u) + (table == RT_TABLE_MAIN ? 1u : 0u);
    if (!ctx->found || rank > ctx->rank || (rank == ctx->rank && metric < ctx->metric)) {
        memcpy(ctx->gw, gw, gwlen); ctx->gw_len = gwlen;
        ctx->found = true; ctx->rank = rank; ctx->metric = metric;
    }
}

static mt_err_t gateway_for_iface(mt_rtnl_t *r, int family, int ifindex, bool profile,
                                  bool *found, uint8_t *gw, uint8_t *gw_len) {
    *found = false;
    uint8_t buf[MT_RTNL_REQBUF] = {0};
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type = RTM_GETROUTE;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_seq = ++r->seq;
    struct rtmsg *rtm = mnl_nlmsg_put_extra_header(nlh, sizeof(*rtm));
    memset(rtm, 0, sizeof(*rtm));
    rtm->rtm_family = (uint8_t)family;
    gw_ctx_t ctx = {.r = r, .family = family, .want_oif = ifindex, .profile = profile};
    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, gw_msg_cb, &ctx);
    if (err != MT_OK) { return err; }
    if (code != 0) { return mt_err_from_errno(code); }
    if (ctx.found) {
        memcpy(gw, ctx.gw, ctx.gw_len); *gw_len = ctx.gw_len; *found = true;
    }
    return MT_OK;
}

mt_err_t mt_rtnl_gateway_for_iface(mt_rtnl_t *r, int family, int ifindex, bool *found,
                                   uint8_t *gw, uint8_t *gw_len) {
    return gateway_for_iface(r, family, ifindex, false, found, gw, gw_len);
}

mt_err_t mt_rtnl_gateway_for_profile(mt_rtnl_t *r, int family, int ifindex, bool *found,
                                     uint8_t *gw, uint8_t *gw_len) {
    return gateway_for_iface(r, family, ifindex, true, found, gw, gw_len);
}

/* ---- unused mark/table allocation --------------------------------------- */

typedef struct used_set {
    uint32_t *vals;
    size_t n, cap;
} used_set_t;

static bool used_set_contains(const used_set_t *s, uint32_t v) {
    for (size_t i = 0; i < s->n; i++) {
        if (s->vals[i] == v) { return true; }
    }
    return false;
}

static void used_set_add(used_set_t *s, uint32_t v) {
    if (used_set_contains(s, v)) { return; }
    if (s->n + 1 > s->cap) {
        size_t newcap = s->cap == 0 ? 32 : s->cap * 2;
        uint32_t *tmp = realloc(s->vals, newcap * sizeof(*tmp));
        if (!tmp) { return; /* best-effort: a missed "used" entry only risks reusing an in-use
                              * idx, extremely unlikely given the scan range; never crashes */
        }
        s->vals = tmp;
        s->cap = newcap;
    }
    s->vals[s->n++] = v;
}

typedef struct scan_ctx {
    used_set_t marks;
    used_set_t tables;
} scan_ctx_t;

static void rule_scan_cb(const struct nlmsghdr *h, void *ud) {
    scan_ctx_t *ctx = ud;
    if (h->nlmsg_type != RTM_NEWRULE) { return; }
    const struct fib_rule_hdr *frh = mnl_nlmsg_get_payload(h);
    used_set_add(&ctx->tables, frh->table);

    mt_nlattr_iter_t attr_it;
    const struct nlattr *attr;
    if (!mt_nlattr_iter_init_nlmsg(&attr_it, h, sizeof(struct fib_rule_hdr))) { return; }
    while (mt_nlattr_iter_next(&attr_it, &attr)) {
        uint16_t type = mnl_attr_get_type(attr);
        if (type == FRA_FWMARK && mnl_attr_get_payload_len(attr) == 4) {
            used_set_add(&ctx->marks, *(const uint32_t *)mnl_attr_get_payload(attr));
        } else if (type == FRA_TABLE && mnl_attr_get_payload_len(attr) == 4) {
            used_set_add(&ctx->tables, *(const uint32_t *)mnl_attr_get_payload(attr));
        }
    }
}

static void route_scan_cb(const struct nlmsghdr *h, void *ud) {
    scan_ctx_t *ctx = ud;
    if (h->nlmsg_type != RTM_NEWROUTE) { return; }
    const struct rtmsg *rtm = mnl_nlmsg_get_payload(h);
    used_set_add(&ctx->tables, rtm->rtm_table);

    mt_nlattr_iter_t attr_it;
    const struct nlattr *attr;
    if (!mt_nlattr_iter_init_nlmsg(&attr_it, h, sizeof(struct rtmsg))) { return; }
    while (mt_nlattr_iter_next(&attr_it, &attr)) {
        if (mnl_attr_get_type(attr) == RTA_TABLE && mnl_attr_get_payload_len(attr) == 4) {
            used_set_add(&ctx->tables, *(const uint32_t *)mnl_attr_get_payload(attr));
        }
    }
}

static mt_err_t dump_family(mt_rtnl_t *r, uint16_t msg_type, int family,
                            void (*cb)(const struct nlmsghdr *, void *), void *ud) {
    uint8_t buf[MT_RTNL_REQBUF];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type = msg_type;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_seq = ++r->seq;

    if (msg_type == RTM_GETRULE) {
        struct fib_rule_hdr *frh = mnl_nlmsg_put_extra_header(nlh, sizeof(*frh));
        memset(frh, 0, sizeof(*frh));
        frh->family = (uint8_t)family;
    } else {
        struct rtmsg *rtm = mnl_nlmsg_put_extra_header(nlh, sizeof(*rtm));
        memset(rtm, 0, sizeof(*rtm));
        rtm->rtm_family = (uint8_t)family;
    }

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, cb, ud);
    if (err != MT_OK) { return err; }
    if (code != 0) { return mt_err_from_errno(code); }
    return MT_OK;
}

typedef struct current_route_ctx {
    int family;
    uint32_t table, priority;
    mt_rtnl_default_route_t *out;
    bool duplicate;
} current_route_ctx_t;

static void current_route_cb(const struct nlmsghdr *h, void *ud)
{
    current_route_ctx_t *ctx = ud;
    if (h->nlmsg_type != RTM_NEWROUTE ||
        mnl_nlmsg_get_payload_len(h) < sizeof(struct rtmsg)) { return; }
    const struct rtmsg *rtm = mnl_nlmsg_get_payload(h);
    if (rtm->rtm_family != ctx->family || rtm->rtm_dst_len || rtm->rtm_src_len ||
        rtm->rtm_tos) { return; }
    uint32_t table = rtm->rtm_table, priority = 0;
    mt_rtnl_default_route_t candidate = {.type = rtm->rtm_type, .found = true};
    mt_nlattr_iter_t it;
    const struct nlattr *attr;
    if (!mt_nlattr_iter_init_nlmsg(&it, h, sizeof(*rtm))) { return; }
    while (mt_nlattr_iter_next(&it, &attr)) {
        uint16_t type = mnl_attr_get_type(attr), len = mnl_attr_get_payload_len(attr);
        if (type == RTA_TABLE && len == 4) { table = mnl_attr_get_u32(attr); }
        else if (type == RTA_PRIORITY && len == 4) { priority = mnl_attr_get_u32(attr); }
        else if (type == RTA_OIF && len == 4) { candidate.ifindex = (int)mnl_attr_get_u32(attr); }
        else if (type == RTA_MULTIPATH) { candidate.multipath = true; }
        else if (type == RTA_GATEWAY &&
                 ((ctx->family == AF_INET && len == 4) ||
                  (ctx->family == AF_INET6 && len == 16))) {
            memcpy(candidate.gateway, mnl_attr_get_payload(attr), len);
            candidate.gateway_len = (uint8_t)len;
        }
    }
    if (table != ctx->table || priority != ctx->priority) { return; }
    if (ctx->out->found) { ctx->duplicate = true; }
    *ctx->out = candidate;
}

mt_err_t mt_rtnl_get_default_route(mt_rtnl_t *r, int family, uint32_t table,
                                   uint32_t priority, mt_rtnl_default_route_t *out)
{
    if (!r || !out || (family != AF_INET && family != AF_INET6)) { return MT_ERR_INVAL; }
    memset(out, 0, sizeof(*out));
    /* Do not expose a partial snapshot when a later DONE reports failure. */
    mt_rtnl_default_route_t snapshot = {0};
    current_route_ctx_t ctx = {.family = family, .table = table, .priority = priority,
                               .out = &snapshot};
    mt_err_t err = dump_family(r, RTM_GETROUTE, family, current_route_cb, &ctx);
    if (err != MT_OK) { return err; }
    if (ctx.duplicate) { return MT_ERR_STATE; }
    *out = snapshot;
    return MT_OK;
}

mt_err_t mt_rtnl_alloc_mark_table(mt_rtnl_t *r, uint32_t start_idx, uint32_t *out_idx) {
    scan_ctx_t ctx = {0};
    used_set_add(&ctx.tables, RT_TABLE_UNSPEC);
    used_set_add(&ctx.tables, 253);
    used_set_add(&ctx.tables, 254);
    used_set_add(&ctx.tables, 255);

    mt_err_t err = dump_family(r, RTM_GETRULE, AF_INET, rule_scan_cb, &ctx);
    if (err == MT_OK) { err = dump_family(r, RTM_GETRULE, AF_INET6, rule_scan_cb, &ctx); }
    if (err == MT_OK) { err = dump_family(r, RTM_GETROUTE, AF_INET, route_scan_cb, &ctx); }
    if (err == MT_OK) { err = dump_family(r, RTM_GETROUTE, AF_INET6, route_scan_cb, &ctx); }

    if (err == MT_OK) {
        uint32_t idx;
        for (idx = start_idx; idx < 0x7ffffffeu; idx++) {
            if (!owns_table(r, idx) && !used_set_contains(&ctx.tables, idx) && !used_set_contains(&ctx.marks, idx)) {
                break;
            }
        }
        *out_idx = idx;
    }

    free(ctx.marks.vals);
    free(ctx.tables.vals);
    return err;
}

/* ---- per-family readiness for routing profiles ---- */

typedef struct addr_ctx {
    int family;
    int ifindex;
    bool found;
} addr_ctx_t;

static void addr_msg_cb(const struct nlmsghdr *h, void *ud)
{
    addr_ctx_t *ctx = ud;
    if (ctx->found || h->nlmsg_type != RTM_NEWADDR ||
        mnl_nlmsg_get_payload_len(h) < sizeof(struct ifaddrmsg)) { return; }
    const struct ifaddrmsg *ifa = mnl_nlmsg_get_payload(h);
    if (ifa->ifa_family != ctx->family || ifa->ifa_index != (unsigned)ctx->ifindex ||
        ifa->ifa_scope >= RT_SCOPE_LINK) { return; }
    uint32_t flags = ifa->ifa_flags;
    bool nonzero = false;
    mt_nlattr_iter_t it;
    const struct nlattr *attr;
    if (!mt_nlattr_iter_init_nlmsg(&it, h, sizeof(*ifa))) { return; }
    while (mt_nlattr_iter_next(&it, &attr)) {
        uint16_t type = mnl_attr_get_type(attr), len = mnl_attr_get_payload_len(attr);
        if (type == MT_IFA_FLAGS_ATTR && len == sizeof(uint32_t)) { flags = mnl_attr_get_u32(attr); }
        if ((type == IFA_LOCAL || type == IFA_ADDRESS) &&
            ((ctx->family == AF_INET && len == 4) || (ctx->family == AF_INET6 && len == 16))) {
            const uint8_t *bytes = mnl_attr_get_payload(attr);
            for (uint16_t i = 0; i < len; i++) { if (bytes[i]) { nonzero = true; } }
        }
    }
    ctx->found = nonzero && !(flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED));
}

mt_err_t mt_rtnl_iface_has_address(mt_rtnl_t *r, int family, int ifindex, bool *found)
{
    *found = false;
    uint8_t buf[MT_RTNL_REQBUF] = {0};
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type = RTM_GETADDR;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_seq = ++r->seq;
    struct ifaddrmsg *ifa = mnl_nlmsg_put_extra_header(nlh, sizeof(*ifa));
    memset(ifa, 0, sizeof(*ifa));
    ifa->ifa_family = (uint8_t)family;
    addr_ctx_t ctx = {.family = family, .ifindex = ifindex};
    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, addr_msg_cb, &ctx);
    if (err != MT_OK) { return err; }
    if (code) { return mt_err_from_errno(code); }
    *found = ctx.found;
    return MT_OK;
}
