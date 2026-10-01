/* Real ipset transport: hand-built NFNETLINK/NFNL_SUBSYS_IPSET messages
 * over libmnl (spec: no libnetfilter_ipset dependency, no shell-exec for
 * ipset -- see ipset.h for the wire-format provenance notes and the
 * documented "cannot be exercised in this sandbox" limitation).
 */
#include "magitrickle/ipset.h"
#include "magitrickle/ipset_nl_parser.h"
#include "magitrickle/log.h"
#include "magitrickle/nlattr_iter.h"

#include <arpa/inet.h>
#include <errno.h>
#include <libmnl/libmnl.h>
#include <linux/netfilter/ipset/ip_set.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netlink.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Pinned to match what github.com/vishvananda/netlink actually sends
 * (nl.IPSET_PROTOCOL = 6), not the current kernel UAPI header's
 * IPSET_PROTOCOL (7) -- see ipset.h. */
#define MT_IPSET_PROTOCOL 6

#define MT_IPSET_REQBUF 1024  /* set names are <=32 bytes; commands are small and fixed-shape */
#define MT_IPSET_RECVBUF 8192 /* matches libmnl's own MNL_SOCKET_BUFFER_SIZE guidance */

typedef struct nl_real {
    mt_ipset_nl_t base;
    struct mnl_socket *nl;
    uint32_t seq;
} nl_real_t;

static struct nlmsghdr *put_header(uint8_t *buf, uint16_t cmd, uint16_t flags, uint32_t seq) {
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type = (uint16_t)(cmd | (NFNL_SUBSYS_IPSET << 8));
    nlh->nlmsg_flags = flags;
    nlh->nlmsg_seq = seq;

    struct nfgenmsg *nfg = mnl_nlmsg_put_extra_header(nlh, sizeof(*nfg));
    nfg->nfgen_family = AF_NETLINK; /* matches Go: NfgenFamily = unix.AF_NETLINK */
    nfg->version = NFNETLINK_V0;
    nfg->res_id = 0;

    mnl_attr_put_u8(nlh, IPSET_ATTR_PROTOCOL, MT_IPSET_PROTOCOL);
    return nlh;
}

static uint16_t ipset_flags(uint16_t cmd) {
    switch (cmd) {
    case IPSET_CMD_CREATE:
        return NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE;
    case IPSET_CMD_DESTROY:
    case IPSET_CMD_ADD:
    case IPSET_CMD_DEL:
        return NLM_F_REQUEST | NLM_F_ACK;
    case IPSET_CMD_LIST:
        return NLM_F_REQUEST | NLM_F_ACK | NLM_F_ROOT | NLM_F_MATCH;
    default:
        return NLM_F_REQUEST;
    }
}

/* Sends nlh and processes the reply stream. For ACK-style commands
 * (create/destroy/add/del) *out_code receives the ack code: 0 on
 * success, otherwise the positive magnitude of the (negative) errno the
 * kernel returned (which for ipset can be a plain errno like ENOENT, or
 * an extended IPSET_ERR_* >= IPSET_ERR_PRIVATE). For dump commands
 * (list), pass msg_cb to receive every non-error/non-done message;
 * *out_code stays 0 unless the kernel itself returned an error. */
static mt_err_t nl_execute(nl_real_t *r, struct nlmsghdr *nlh, int *out_code,
                          void (*msg_cb)(const struct nlmsghdr *, void *), void *cb_ud) {
    *out_code = 0;

    if (mnl_socket_sendto(r->nl, nlh, nlh->nlmsg_len) < 0) { return mt_err_from_errno(errno); }

    uint8_t buf[MT_IPSET_RECVBUF];
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

static mt_err_t real_create(mt_ipset_nl_t *self, const char *name, int family,
                            uint32_t default_timeout) {
    nl_real_t *r = (nl_real_t *)self;
    uint8_t buf[MT_IPSET_REQBUF];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh =
        put_header(buf, IPSET_CMD_CREATE, ipset_flags(IPSET_CMD_CREATE), ++r->seq);
    mnl_attr_put_strz(nlh, IPSET_ATTR_SETNAME, name);
    mnl_attr_put_strz(nlh, IPSET_ATTR_TYPENAME, "hash:net");
    mnl_attr_put_u8(nlh, IPSET_ATTR_REVISION, 0); /* matches Go: no revision table for hash:net -> 0 */
    mnl_attr_put_u8(nlh, IPSET_ATTR_FAMILY, (uint8_t)family);

    struct nlattr *data = mnl_attr_nest_start(nlh, IPSET_ATTR_DATA);
    mnl_attr_put_u32(nlh, IPSET_ATTR_TIMEOUT | NLA_F_NET_BYTEORDER, htonl(default_timeout));
    mnl_attr_nest_end(nlh, data);

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, NULL, NULL);
    if (err != MT_OK) { return err; }
    if (code == 0) { return MT_OK; }
    return mt_err_from_errno(code);
}

static mt_err_t real_destroy(mt_ipset_nl_t *self, const char *name) {
    nl_real_t *r = (nl_real_t *)self;
    uint8_t buf[MT_IPSET_REQBUF];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh =
        put_header(buf, IPSET_CMD_DESTROY, ipset_flags(IPSET_CMD_DESTROY), ++r->seq);
    mnl_attr_put_strz(nlh, IPSET_ATTR_SETNAME, name);

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, NULL, NULL);
    if (err != MT_OK) { return err; }
    if (code == 0 || code == ENOENT) { return MT_OK; } /* matches Go's os.IsNotExist swallow */
    return mt_err_from_errno(code);
}

static mt_err_t real_add(mt_ipset_nl_t *self, const char *name, const uint8_t *ip, uint8_t iplen,
                         uint8_t cidr, bool has_timeout, uint32_t timeout, bool replace) {
    nl_real_t *r = (nl_real_t *)self;
    uint8_t buf[MT_IPSET_REQBUF];
    memset(buf, 0, sizeof(buf));

    uint16_t flags = ipset_flags(IPSET_CMD_ADD);
    if (!replace) { flags |= NLM_F_EXCL; }

    struct nlmsghdr *nlh = put_header(buf, IPSET_CMD_ADD, flags, ++r->seq);
    mnl_attr_put_strz(nlh, IPSET_ATTR_SETNAME, name);

    struct nlattr *data = mnl_attr_nest_start(nlh, IPSET_ATTR_DATA);
    if (has_timeout) {
        mnl_attr_put_u32(nlh, IPSET_ATTR_TIMEOUT | NLA_F_NET_BYTEORDER, htonl(timeout));
    }
    struct nlattr *ipattr = mnl_attr_nest_start(nlh, IPSET_ATTR_IP);
    uint16_t addr_type = (uint16_t)((iplen == 4 ? IPSET_ATTR_IPADDR_IPV4 : IPSET_ATTR_IPADDR_IPV6) |
                                    NLA_F_NET_BYTEORDER);
    mnl_attr_put(nlh, addr_type, iplen, ip);
    mnl_attr_nest_end(nlh, ipattr);
    if (cidr != 0) { mnl_attr_put_u8(nlh, IPSET_ATTR_CIDR, cidr); }
    mnl_attr_put_u32(nlh, IPSET_ATTR_LINENO | NLA_F_NET_BYTEORDER, htonl(0));
    mnl_attr_nest_end(nlh, data);

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, NULL, NULL);
    if (err != MT_OK) { return err; }
    if (code == 0) { return MT_OK; }
    return mt_err_from_errno(code);
}

static mt_err_t real_del(mt_ipset_nl_t *self, const char *name, const uint8_t *ip, uint8_t iplen,
                         uint8_t cidr) {
    nl_real_t *r = (nl_real_t *)self;
    uint8_t buf[MT_IPSET_REQBUF];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh = put_header(buf, IPSET_CMD_DEL, ipset_flags(IPSET_CMD_DEL), ++r->seq);
    mnl_attr_put_strz(nlh, IPSET_ATTR_SETNAME, name);

    struct nlattr *data = mnl_attr_nest_start(nlh, IPSET_ATTR_DATA);
    struct nlattr *ipattr = mnl_attr_nest_start(nlh, IPSET_ATTR_IP);
    uint16_t addr_type = (uint16_t)((iplen == 4 ? IPSET_ATTR_IPADDR_IPV4 : IPSET_ATTR_IPADDR_IPV6) |
                                    NLA_F_NET_BYTEORDER);
    mnl_attr_put(nlh, addr_type, iplen, ip);
    mnl_attr_nest_end(nlh, ipattr);
    if (cidr != 0) { mnl_attr_put_u8(nlh, IPSET_ATTR_CIDR, cidr); }
    mnl_attr_put_u32(nlh, IPSET_ATTR_LINENO | NLA_F_NET_BYTEORDER, htonl(0));
    mnl_attr_nest_end(nlh, data);

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, NULL, NULL);
    if (err != MT_OK) { return err; }
    if (code == 0 || code == IPSET_ERR_EXIST) { return MT_OK; } /* matches Go's IPSET_ERR_EXIST swallow */
    return mt_err_from_errno(code);
}

/* ---- LIST: parse the dump into (subnet, timeout) entries ---------------- */

typedef struct list_ctx {
    uint8_t iplen; /* 4 or 16 */
    mt_ipset_entry4_t *out4;
    mt_ipset_entry6_t *out6;
    size_t n, cap;
    bool oom;
} list_ctx_t;

static bool list_ctx_grow(list_ctx_t *ctx) {
    if (ctx->n + 1 <= ctx->cap) { return true; }
    size_t newcap = ctx->cap == 0 ? 16 : ctx->cap * 2;
    if (ctx->iplen == 4) {
        mt_ipset_entry4_t *tmp = realloc(ctx->out4, newcap * sizeof(*tmp));
        if (!tmp) { return false; }
        ctx->out4 = tmp;
    } else {
        mt_ipset_entry6_t *tmp = realloc(ctx->out6, newcap * sizeof(*tmp));
        if (!tmp) { return false; }
        ctx->out6 = tmp;
    }
    ctx->cap = newcap;
    return true;
}

static int parse_ip_attr_cb(const struct nlattr *attr, void *data) {
    uint8_t *out_addr = data;
    uint16_t type = mnl_attr_get_type(attr);
    if (type == IPSET_ATTR_IPADDR_IPV4 && mnl_attr_get_payload_len(attr) == 4) {
        memcpy(out_addr, mnl_attr_get_payload(attr), 4);
    } else if (type == IPSET_ATTR_IPADDR_IPV6 && mnl_attr_get_payload_len(attr) == 16) {
        memcpy(out_addr, mnl_attr_get_payload(attr), 16);
    }
    return MNL_CB_OK;
}

/* Parses one IPSET_ATTR_DATA entry (nested) into a single output row. */
static void parse_one_entry(const struct nlattr *entry_attr, list_ctx_t *ctx) {
    uint8_t addr[16] = {0};
    bool has_timeout = false;
    uint32_t timeout = 0;
    uint8_t cidr = (uint8_t)(ctx->iplen == 4 ? 32 : 128);

    mt_nlattr_iter_t child_it;
    const struct nlattr *child;
    if (!mt_nlattr_iter_init_nested(&child_it, entry_attr)) { return; }
    while (mt_nlattr_iter_next(&child_it, &child)) {
        uint16_t ctype = mnl_attr_get_type(child);
        /* mnl_attr_get_type() intentionally strips NLA_F_NESTED and
         * NLA_F_NET_BYTEORDER. Inspect the raw UAPI field when a flag is
         * required; checking the normalized type made every IP child look
         * non-nested and silently produced an all-zero address. */
        bool nested = (child->nla_type & NLA_F_NESTED) != 0;
        switch (ctype) {
        case IPSET_ATTR_IP:
            if (nested) { mnl_attr_parse_nested(child, parse_ip_attr_cb, addr); }
            break;
        case IPSET_ATTR_CIDR:
            if (mnl_attr_get_payload_len(child) == 1) {
                cidr = *(const uint8_t *)mnl_attr_get_payload(child);
            }
            break;
        case IPSET_ATTR_TIMEOUT:
            if (mnl_attr_get_payload_len(child) == 4) {
                has_timeout = true;
                timeout = ntohl(*(const uint32_t *)mnl_attr_get_payload(child));
            }
            break;
        default:
            break;
        }
    }

    if (!list_ctx_grow(ctx)) {
        ctx->oom = true;
        return;
    }
    /* Go: `if entry.Timeout != nil && *entry.Timeout == 0 { addresses[subnet] = nil }` --
     * a reported timeout of exactly 0 collapses to "no timeout" (permanent). */
    if (has_timeout && timeout == 0) { has_timeout = false; }

    if (ctx->iplen == 4) {
        mt_ipset_entry4_t *e = &ctx->out4[ctx->n++];
        memcpy(e->subnet.addr, addr, 4);
        e->subnet.cidr = cidr;
        e->has_timeout = has_timeout;
        e->timeout = timeout;
    } else {
        mt_ipset_entry6_t *e = &ctx->out6[ctx->n++];
        memcpy(e->subnet.addr, addr, 16);
        e->subnet.cidr = cidr;
        e->has_timeout = has_timeout;
        e->timeout = timeout;
    }
}

static void list_msg_cb(const struct nlmsghdr *h, void *ud) {
    list_ctx_t *ctx = ud;
    if (ctx->oom) { return; }

    mt_nlattr_iter_t top_it;
    const struct nlattr *top;
    if (!mt_nlattr_iter_init_nlmsg(&top_it, h, sizeof(struct nfgenmsg))) { return; }
    while (mt_nlattr_iter_next(&top_it, &top)) {
        uint16_t ttype = mnl_attr_get_type(top);
        bool nested = (top->nla_type & NLA_F_NESTED) != 0;
        if (ttype != IPSET_ATTR_ADT || !nested) { continue; }

        mt_nlattr_iter_t entry_it;
        const struct nlattr *entry;
        if (!mt_nlattr_iter_init_nested(&entry_it, top)) { continue; }
        while (mt_nlattr_iter_next(&entry_it, &entry)) {
            uint16_t etype = mnl_attr_get_type(entry);
            bool enested = (entry->nla_type & NLA_F_NESTED) != 0;
            if (etype != IPSET_ATTR_DATA || !enested) { continue; }
            parse_one_entry(entry, ctx);
            if (ctx->oom) { return; }
        }
    }
}

mt_err_t mt_ipset_nl_parse_list_message(const struct nlmsghdr *h, uint8_t iplen,
                                        mt_ipset_entry4_t **out4, mt_ipset_entry6_t **out6,
                                        size_t *out_n) {
    if (!h || (iplen != 4 && iplen != 16) || !out4 || !out6 || !out_n) {
        return MT_ERR_INVAL;
    }

    *out4 = NULL;
    *out6 = NULL;
    *out_n = 0;

    list_ctx_t ctx = {.iplen = iplen};
    list_msg_cb(h, &ctx);
    if (ctx.oom) {
        free(ctx.out4);
        free(ctx.out6);
        return MT_ERR_NOMEM;
    }

    *out4 = ctx.out4;
    *out6 = ctx.out6;
    *out_n = ctx.n;
    return MT_OK;
}

static mt_err_t real_list(nl_real_t *r, const char *name, uint8_t iplen, mt_ipset_entry4_t **out4,
                          mt_ipset_entry6_t **out6, size_t *out_n) {
    uint8_t buf[MT_IPSET_REQBUF];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh = put_header(buf, IPSET_CMD_LIST, ipset_flags(IPSET_CMD_LIST), ++r->seq);
    mnl_attr_put_strz(nlh, IPSET_ATTR_SETNAME, name);

    list_ctx_t ctx = {0};
    ctx.iplen = iplen;

    int code = 0;
    mt_err_t err = nl_execute(r, nlh, &code, list_msg_cb, &ctx);
    if (err != MT_OK) {
        free(ctx.out4);
        free(ctx.out6);
        return err;
    }
    if (ctx.oom) {
        free(ctx.out4);
        free(ctx.out6);
        return MT_ERR_NOMEM;
    }
    if (code != 0) {
        free(ctx.out4);
        free(ctx.out6);
        return mt_err_from_errno(code);
    }

    *out4 = ctx.out4;
    *out6 = ctx.out6;
    *out_n = ctx.n;
    return MT_OK;
}

static mt_err_t real_list4(mt_ipset_nl_t *self, const char *name, mt_ipset_entry4_t **out,
                           size_t *out_n) {
    nl_real_t *r = (nl_real_t *)self;
    mt_ipset_entry6_t *unused6 = NULL;
    return real_list(r, name, 4, out, &unused6, out_n);
}

static mt_err_t real_list6(mt_ipset_nl_t *self, const char *name, mt_ipset_entry6_t **out,
                           size_t *out_n) {
    nl_real_t *r = (nl_real_t *)self;
    mt_ipset_entry4_t *unused4 = NULL;
    return real_list(r, name, 16, &unused4, out, out_n);
}

static void real_destroy_self(mt_ipset_nl_t *self) {
    nl_real_t *r = (nl_real_t *)self;
    if (!r) { return; }
    if (r->nl) { mnl_socket_close(r->nl); }
    free(r);
}

static const mt_ipset_nl_ops_t k_real_ops = {
    .create = real_create,
    .destroy = real_destroy,
    .add = real_add,
    .del = real_del,
    .list4 = real_list4,
    .list6 = real_list6,
    .destroy_self = real_destroy_self,
};

mt_ipset_nl_t *mt_ipset_nl_real_new(void) {
    nl_real_t *r = calloc(1, sizeof(*r));
    if (!r) { return NULL; }
    r->base.ops = &k_real_ops;

    r->nl = mnl_socket_open(NETLINK_NETFILTER);
    if (!r->nl) {
        free(r);
        return NULL;
    }
    if (mnl_socket_bind(r->nl, 0, MNL_SOCKET_AUTOPID) < 0) {
        mnl_socket_close(r->nl);
        free(r);
        return NULL;
    }
    return &r->base;
}
