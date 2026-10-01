/* ipset via netlink (NFNETLINK/NFNL_SUBSYS_IPSET) — port of
 * utils/netfilterTools/ipset.go.
 *
 * The wire format (attribute set, flags, "hash:net" revision=0, protocol
 * version pinned to 6) is ported byte-for-byte from what
 * github.com/vishvananda/netlink actually sends (nl/ipset_linux.go,
 * ipset_linux.go) rather than from the current kernel UAPI header's
 * IPSET_PROTOCOL=7 default, so the C backend produces the same requests
 * the Go binary already sends in production.
 *
 * Known limitation (see docs/c-rewrite/phase-5-report.md): this sandbox
 * has no `ip_set` kernel module (confirmed in Phase 0 and re-confirmed
 * here), so none of this can be exercised against a real kernel in this
 * environment; only the fake transport (tests/unit/fake_ipset_nl.c) is
 * exercised by the unit tests. On-device validation is required before
 * this is trusted on a real router.
 *
 * The mt_ipset_nl_t transport is a seam so tests can inject a fake
 * in-memory backend instead of the real libmnl/netlink one.
 */
#ifndef MAGITRICKLE_IPSET_H
#define MAGITRICKLE_IPSET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "magitrickle/err.h"

typedef struct mt_ipv4_subnet {
    uint8_t addr[4];
    uint8_t cidr;
} mt_ipv4_subnet_t;

typedef struct mt_ipv6_subnet {
    uint8_t addr[16];
    uint8_t cidr;
} mt_ipv6_subnet_t;

/* has_timeout==false mirrors Go's IPSetTimeout(nil) map value: "no
 * timeout was reported/requested" is distinct from has_timeout==true
 * with timeout==0, which means "permanent entry" (Go's zeroTimeout /
 * ListIPv4Subnets' "*Timeout == 0 -> nil" collapse). */
typedef struct mt_ipset_entry4 {
    mt_ipv4_subnet_t subnet;
    bool has_timeout;
    uint32_t timeout;
} mt_ipset_entry4_t;

typedef struct mt_ipset_entry6 {
    mt_ipv6_subnet_t subnet;
    bool has_timeout;
    uint32_t timeout;
} mt_ipset_entry6_t;

/* ---- netlink transport seam --------------------------------------------- */

typedef struct mt_ipset_nl mt_ipset_nl_t;

typedef struct mt_ipset_nl_ops {
    /* family: AF_INET or AF_INET6. default_timeout: set-level default
     * (300s, matching Go's ipsetCreate). */
    mt_err_t (*create)(mt_ipset_nl_t *self, const char *name, int family,
                       uint32_t default_timeout);
    /* MT_OK both on success and on "didn't exist" (matches Go's
     * os.IsNotExist swallow in ipsetDestroy). */
    mt_err_t (*destroy)(mt_ipset_nl_t *self, const char *name);
    /* has_timeout==false is Go's "timeout==nil -> zeroTimeout" substitution
     * already applied by the caller (mt_ipset_add4/6); ip must be 4 or 16
     * bytes matching iplen. cidr==0 omits the CIDR attribute entirely
     * (matches Go's `if entry.CIDR != 0`). */
    mt_err_t (*add)(mt_ipset_nl_t *self, const char *name, const uint8_t *ip,
                    uint8_t iplen, uint8_t cidr, bool has_timeout, uint32_t timeout,
                    bool replace);
    /* MT_OK both on success and on "entry not present" (matches Go's
     * IPSET_ERR_EXIST swallow in DelIPv4Subnet/DelIPv6Subnet). */
    mt_err_t (*del)(mt_ipset_nl_t *self, const char *name, const uint8_t *ip,
                    uint8_t iplen, uint8_t cidr);
    /* Populates *out (malloc'd array, caller frees) / *out_n. */
    mt_err_t (*list4)(mt_ipset_nl_t *self, const char *name, mt_ipset_entry4_t **out,
                      size_t *out_n);
    mt_err_t (*list6)(mt_ipset_nl_t *self, const char *name, mt_ipset_entry6_t **out,
                      size_t *out_n);
    void (*destroy_self)(mt_ipset_nl_t *self);
} mt_ipset_nl_ops_t;

struct mt_ipset_nl {
    const mt_ipset_nl_ops_t *ops;
};

static inline void mt_ipset_nl_free(mt_ipset_nl_t *nl) {
    if (nl) { nl->ops->destroy_self(nl); }
}

/* Real backend: NFNETLINK socket via libmnl, no shell/subprocess involved. */
mt_ipset_nl_t *mt_ipset_nl_real_new(void);

/* ---- high-level IPSet (Enable/Disable/Add/Del/List) --------------------- */

typedef struct mt_ipset mt_ipset_t;

/* name: the already-prefixed set name (e.g. "mt_<runtime-key>"), WITHOUT
 * the "_4"/"_6" suffix -- mirrors Go's ipsetName+"_4"/"_6" convention.
 * Takes ownership of nl (freed by mt_ipset_free). */
mt_ipset_t *mt_ipset_new(mt_ipset_nl_t *nl, const char *name);
void mt_ipset_free(mt_ipset_t *r);

/* The kernel set names this object manages ("<name>_4"/"<name>_6"), for
 * building iptables --match-set args (matches Go's r.ipsetName+"_4"/"_6"
 * usage at the ipset-to-link call site). */
const char *mt_ipset_name4(const mt_ipset_t *r);
const char *mt_ipset_name6(const mt_ipset_t *r);

bool mt_ipset_enabled(const mt_ipset_t *r);
/* CompareAndSwap-style: no-op (MT_OK) if already enabled, matching Go. */
mt_err_t mt_ipset_enable(mt_ipset_t *r);
mt_err_t mt_ipset_disable(mt_ipset_t *r);

/* No-ops (MT_OK) while disabled, matching Go's `if !enabled return nil`. */
mt_err_t mt_ipset_add4(mt_ipset_t *r, mt_ipv4_subnet_t subnet, const uint32_t *timeout);
mt_err_t mt_ipset_add6(mt_ipset_t *r, mt_ipv6_subnet_t subnet, const uint32_t *timeout);
mt_err_t mt_ipset_del4(mt_ipset_t *r, mt_ipv4_subnet_t subnet);
mt_err_t mt_ipset_del6(mt_ipset_t *r, mt_ipv6_subnet_t subnet);
/* out/out_n are set to NULL/0 (not an error) while disabled, matching
 * Go's ListIPv4Subnets/ListIPv6Subnets returning (nil, nil). */
mt_err_t mt_ipset_list4(mt_ipset_t *r, mt_ipset_entry4_t **out, size_t *out_n);
mt_err_t mt_ipset_list6(mt_ipset_t *r, mt_ipset_entry6_t **out, size_t *out_n);

#endif /* MAGITRICKLE_IPSET_H */
