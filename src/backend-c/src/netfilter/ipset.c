/* High-level IPSet wrapper — port of utils/netfilterTools/ipset.go's
 * IPSet type (Enable/Disable/Add/Del/List gating). Go guards every method
 * with atomic.Bool + sync.Mutex because multiple goroutines can share one
 * *IPSet; this port has no internal locking, matching the single-thread
 * (or single owning worker thread) model established for the whole
 * netfilter layer in Phase 5 (decisions.md D-17/D-19/D-20).
 */
#include "magitrickle/ipset.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define MT_IPSET_DEFAULT_TIMEOUT 300u /* matches Go's ipsetCreate: 300s */

struct mt_ipset {
    mt_ipset_nl_t *nl;
    char *name4; /* name + "_4" */
    char *name6; /* name + "_6" */
    bool enabled;
};

static char *concat(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *out = malloc(la + lb + 1);
    if (!out) { return NULL; }
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = '\0';
    return out;
}

mt_ipset_t *mt_ipset_new(mt_ipset_nl_t *nl, const char *name) {
    mt_ipset_t *r = calloc(1, sizeof(*r));
    if (!r) { return NULL; }
    r->nl = nl;
    r->name4 = concat(name, "_4");
    r->name6 = concat(name, "_6");
    if (!r->name4 || !r->name6) {
        free(r->name4);
        free(r->name6);
        mt_ipset_nl_free(nl);
        free(r);
        return NULL;
    }
    return r;
}

void mt_ipset_free(mt_ipset_t *r) {
    if (!r) { return; }
    free(r->name4);
    free(r->name6);
    mt_ipset_nl_free(r->nl);
    free(r);
}

bool mt_ipset_enabled(const mt_ipset_t *r) {
    return r->enabled;
}

const char *mt_ipset_name4(const mt_ipset_t *r) {
    return r->name4;
}

const char *mt_ipset_name6(const mt_ipset_t *r) {
    return r->name6;
}

static mt_err_t ipset_destroy_both(mt_ipset_t *r) {
    mt_err_t e4 = r->nl->ops->destroy(r->nl, r->name4);
    mt_err_t e6 = r->nl->ops->destroy(r->nl, r->name6);
    if (e4 != MT_OK) { return e4; }
    return e6;
}

static mt_err_t ipset_create_both(mt_ipset_t *r) {
    mt_err_t err = r->nl->ops->create(r->nl, r->name4, AF_INET, MT_IPSET_DEFAULT_TIMEOUT);
    if (err != MT_OK) { return err; }
    return r->nl->ops->create(r->nl, r->name6, AF_INET6, MT_IPSET_DEFAULT_TIMEOUT);
}

mt_err_t mt_ipset_enable(mt_ipset_t *r) {
    if (r->enabled) { return MT_OK; }

    mt_err_t err = ipset_destroy_both(r);
    if (err == MT_OK) { err = ipset_create_both(r); }

    if (err != MT_OK) {
        /* Matches Go's Enable(): err := r.enable(); if err != nil {
         * r.disable() } -- a best-effort destroy cleans up any partially
         * created set (e.g. "_4" succeeded, "_6" failed) rather than
         * leaking it. */
        ipset_destroy_both(r);
        return err;
    }

    r->enabled = true;
    return MT_OK;
}

mt_err_t mt_ipset_disable(mt_ipset_t *r) {
    if (!r->enabled) { return MT_OK; }
    r->enabled = false;
    return ipset_destroy_both(r);
}

mt_err_t mt_ipset_add4(mt_ipset_t *r, mt_ipv4_subnet_t subnet, const uint32_t *timeout) {
    if (!r->enabled) { return MT_OK; }
    uint32_t tv = timeout ? *timeout : 0;
    return r->nl->ops->add(r->nl, r->name4, subnet.addr, 4, subnet.cidr, true, tv, true);
}

mt_err_t mt_ipset_add6(mt_ipset_t *r, mt_ipv6_subnet_t subnet, const uint32_t *timeout) {
    if (!r->enabled) { return MT_OK; }
    uint32_t tv = timeout ? *timeout : 0;
    return r->nl->ops->add(r->nl, r->name6, subnet.addr, 16, subnet.cidr, true, tv, true);
}

mt_err_t mt_ipset_del4(mt_ipset_t *r, mt_ipv4_subnet_t subnet) {
    if (!r->enabled) { return MT_OK; }
    return r->nl->ops->del(r->nl, r->name4, subnet.addr, 4, subnet.cidr);
}

mt_err_t mt_ipset_del6(mt_ipset_t *r, mt_ipv6_subnet_t subnet) {
    if (!r->enabled) { return MT_OK; }
    return r->nl->ops->del(r->nl, r->name6, subnet.addr, 16, subnet.cidr);
}

mt_err_t mt_ipset_list4(mt_ipset_t *r, mt_ipset_entry4_t **out, size_t *out_n) {
    if (!r->enabled) {
        *out = NULL;
        *out_n = 0;
        return MT_OK;
    }
    return r->nl->ops->list4(r->nl, r->name4, out, out_n);
}

mt_err_t mt_ipset_list6(mt_ipset_t *r, mt_ipset_entry6_t **out, size_t *out_n) {
    if (!r->enabled) {
        *out = NULL;
        *out_n = 0;
        return MT_OK;
    }
    return r->nl->ops->list6(r->nl, r->name6, out, out_n);
}
