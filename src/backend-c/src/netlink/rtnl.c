/* See rtnl.h. */
#include "magitrickle/rtnl.h"
#include "magitrickle/nlattr_iter.h"

#include <arpa/inet.h>
#include <errno.h>
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

struct mt_rtnl {
    struct mnl_socket *nl;
    uint32_t seq;
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
    free(r);
}

static mt_err_t nl_execute(mt_rtnl_t *r, struct nlmsghdr *nlh, int *out_code,
                          void (*msg_cb)(const struct nlmsghdr *, void *), void *cb_ud) {
    *out_code = 0;
    if (mnl_socket_sendto(r->nl, nlh, nlh->nlmsg_len) < 0) { return mt_err_from_errno(errno); }

    uint8_t buf[MT_RTNL_RECVBUF];
    for (;;) {
        ssize_t ret = mnl_socket_recvfrom(r->nl, buf, sizeof(buf));
        if (ret < 0) { return mt_err_from_errno(errno); }
        if (ret == 0) { break; }

        int len = (int)ret;
        struct nlmsghdr *h = (struct nlmsghdr *)buf;
        bool done = false;
        while (mnl_nlmsg_ok(h, len)) {
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = (struct nlmsgerr *)mnl_nlmsg_get_payload(h);
                *out_code = e->error < 0 ? -e->error : e->error;
                done = true;
                break;
            }
            if (h->nlmsg_type == NLMSG_DONE) {
                done = true;
                break;
            }
            if (msg_cb) { msg_cb(h, cb_ud); }
            h = mnl_nlmsg_next(h, &len);
        }
        if (done) { break; }
        if (!(nlh->nlmsg_flags & NLM_F_DUMP)) { break; }
    }
    return MT_OK;
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
} link_ctx_t;

static void link_msg_cb(const struct nlmsghdr *h, void *ud) {
    link_ctx_t *ctx = ud;
    if (h->nlmsg_type != RTM_NEWLINK) { return; }
    const struct ifinfomsg *ifi = mnl_nlmsg_get_payload(h);
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
    *found = true;
    return MT_OK;
}

/* ---- gateway lookup (route dump filtered by oif) ------------------------ */

typedef struct gw_ctx {
    int want_oif;
    bool found;
    uint8_t gw[16];
    uint8_t gw_len;
} gw_ctx_t;

static void gw_msg_cb(const struct nlmsghdr *h, void *ud) {
    gw_ctx_t *ctx = ud;
    if (ctx->found || h->nlmsg_type != RTM_NEWROUTE) { return; }

    int oif = -1;
    uint8_t gw[16] = {0};
    uint8_t gwlen = 0;

    mt_nlattr_iter_t attr_it;
    const struct nlattr *attr;
    if (!mt_nlattr_iter_init_nlmsg(&attr_it, h, sizeof(struct rtmsg))) { return; }
    while (mt_nlattr_iter_next(&attr_it, &attr)) {
        switch (mnl_attr_get_type(attr)) {
        case RTA_OIF:
            if (mnl_attr_get_payload_len(attr) == 4) {
                oif = (int)*(const uint32_t *)mnl_attr_get_payload(attr);
            }
            break;
        case RTA_GATEWAY: {
            uint16_t len = mnl_attr_get_payload_len(attr);
            if (len == 4 || len == 16) {
                memcpy(gw, mnl_attr_get_payload(attr), len);
                gwlen = (uint8_t)len;
            }
            break;
        }
        default:
            break;
        }
    }

    if (oif == ctx->want_oif && gwlen > 0) {
        memcpy(ctx->gw, gw, gwlen);
        ctx->gw_len = gwlen;
        ctx->found = true;
    }
}

mt_err_t mt_rtnl_gateway_for_iface(mt_rtnl_t *r, int family, int ifindex, bool *found, uint8_t *gw,
                                   uint8_t *gw_len) {
    *found = false;
    uint8_t buf[MT_RTNL_REQBUF];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type = RTM_GETROUTE;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_DUMP;
    nlh->nlmsg_seq = ++r->seq;
    struct rtmsg *rtm = mnl_nlmsg_put_extra_header(nlh, sizeof(*rtm));
    memset(rtm, 0, sizeof(*rtm));
    rtm->rtm_family = (uint8_t)family;

    gw_ctx_t ctx = {0};
    ctx.want_oif = ifindex;

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, gw_msg_cb, &ctx);
    if (err != MT_OK) { return err; }
    if (code != 0) { return mt_err_from_errno(code); }

    if (ctx.found) {
        memcpy(gw, ctx.gw, ctx.gw_len);
        *gw_len = ctx.gw_len;
        *found = true;
    }
    return MT_OK;
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
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_DUMP;
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
            if (!used_set_contains(&ctx.tables, idx) && !used_set_contains(&ctx.marks, idx)) {
                break;
            }
        }
        *out_idx = idx;
    }

    free(ctx.marks.vals);
    free(ctx.tables.vals);
    return err;
}
