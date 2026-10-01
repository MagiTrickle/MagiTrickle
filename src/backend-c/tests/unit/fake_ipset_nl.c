/* See fake_ipset_nl.h. */
#include "fake_ipset_nl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fake_entry {
    uint8_t ip[16];
    uint8_t iplen;
    uint8_t cidr;
    bool has_timeout;
    uint32_t timeout;
} fake_entry_t;

typedef struct fake_set {
    char name[64];
    bool exists;
    fake_entry_t *entries;
    size_t n, cap;
} fake_set_t;

struct mt_fake_ipset_nl {
    mt_ipset_nl_t base;
    fake_set_t *sets;
    size_t n_sets, cap_sets;
    fake_ipset_call_t *calls;
    size_t n_calls, cap_calls;
    bool fail_pending;
    fake_ipset_call_kind_t fail_kind;
    mt_err_t fail_err;
};

static fake_set_t *find_set(mt_fake_ipset_nl_t *f, const char *name) {
    for (size_t i = 0; i < f->n_sets; i++) {
        if (strcmp(f->sets[i].name, name) == 0) { return &f->sets[i]; }
    }
    return NULL;
}

static fake_set_t *find_or_create_set(mt_fake_ipset_nl_t *f, const char *name) {
    fake_set_t *s = find_set(f, name);
    if (s) { return s; }
    if (f->n_sets + 1 > f->cap_sets) {
        size_t newcap = f->cap_sets == 0 ? 4 : f->cap_sets * 2;
        fake_set_t *tmp = realloc(f->sets, newcap * sizeof(*tmp));
        if (!tmp) { return NULL; }
        f->sets = tmp;
        f->cap_sets = newcap;
    }
    fake_set_t *ns = &f->sets[f->n_sets];
    memset(ns, 0, sizeof(*ns));
    snprintf(ns->name, sizeof(ns->name), "%s", name);
    f->n_sets++;
    return ns;
}

static void log_call(mt_fake_ipset_nl_t *f, const fake_ipset_call_t *call) {
    if (f->n_calls + 1 > f->cap_calls) {
        size_t newcap = f->cap_calls == 0 ? 16 : f->cap_calls * 2;
        fake_ipset_call_t *tmp = realloc(f->calls, newcap * sizeof(*tmp));
        if (!tmp) { return; }
        f->calls = tmp;
        f->cap_calls = newcap;
    }
    f->calls[f->n_calls++] = *call;
}

static bool consume_failure(mt_fake_ipset_nl_t *f, fake_ipset_call_kind_t kind, mt_err_t *out) {
    if (f->fail_pending && f->fail_kind == kind) {
        f->fail_pending = false;
        *out = f->fail_err;
        return true;
    }
    return false;
}

static mt_err_t fake_create(mt_ipset_nl_t *self, const char *name, int family,
                            uint32_t default_timeout) {
    mt_fake_ipset_nl_t *f = (mt_fake_ipset_nl_t *)self;
    (void)family;
    fake_ipset_call_t call = {0};
    call.kind = FAKE_IPSET_CALL_CREATE;
    snprintf(call.name, sizeof(call.name), "%s", name);
    call.has_timeout = true;
    call.timeout = default_timeout;
    log_call(f, &call);

    mt_err_t forced;
    if (consume_failure(f, FAKE_IPSET_CALL_CREATE, &forced)) { return forced; }

    fake_set_t *s = find_or_create_set(f, name);
    if (!s) { return MT_ERR_NOMEM; }
    s->exists = true;
    s->n = 0;
    return MT_OK;
}

static mt_err_t fake_destroy(mt_ipset_nl_t *self, const char *name) {
    mt_fake_ipset_nl_t *f = (mt_fake_ipset_nl_t *)self;
    fake_ipset_call_t call = {0};
    call.kind = FAKE_IPSET_CALL_DESTROY;
    snprintf(call.name, sizeof(call.name), "%s", name);
    log_call(f, &call);

    mt_err_t forced;
    if (consume_failure(f, FAKE_IPSET_CALL_DESTROY, &forced)) { return forced; }

    fake_set_t *s = find_set(f, name);
    if (s) {
        s->exists = false;
        s->n = 0;
    }
    return MT_OK; /* "didn't exist" is also MT_OK, matching Go's os.IsNotExist swallow */
}

static mt_err_t fake_add(mt_ipset_nl_t *self, const char *name, const uint8_t *ip, uint8_t iplen,
                         uint8_t cidr, bool has_timeout, uint32_t timeout, bool replace) {
    mt_fake_ipset_nl_t *f = (mt_fake_ipset_nl_t *)self;
    fake_ipset_call_t call = {0};
    call.kind = FAKE_IPSET_CALL_ADD;
    snprintf(call.name, sizeof(call.name), "%s", name);
    memcpy(call.ip, ip, iplen);
    call.iplen = iplen;
    call.cidr = cidr;
    call.has_timeout = has_timeout;
    call.timeout = timeout;
    call.replace = replace;
    log_call(f, &call);

    mt_err_t forced;
    if (consume_failure(f, FAKE_IPSET_CALL_ADD, &forced)) { return forced; }

    fake_set_t *s = find_or_create_set(f, name);
    if (!s) { return MT_ERR_NOMEM; }

    for (size_t i = 0; i < s->n; i++) {
        if (s->entries[i].iplen == iplen && s->entries[i].cidr == cidr &&
            memcmp(s->entries[i].ip, ip, iplen) == 0) {
            if (!replace) { return MT_ERR_EXIST; }
            s->entries[i].has_timeout = has_timeout;
            s->entries[i].timeout = timeout;
            return MT_OK;
        }
    }

    if (s->n + 1 > s->cap) {
        size_t newcap = s->cap == 0 ? 8 : s->cap * 2;
        fake_entry_t *tmp = realloc(s->entries, newcap * sizeof(*tmp));
        if (!tmp) { return MT_ERR_NOMEM; }
        s->entries = tmp;
        s->cap = newcap;
    }
    fake_entry_t *e = &s->entries[s->n++];
    memset(e, 0, sizeof(*e));
    memcpy(e->ip, ip, iplen);
    e->iplen = iplen;
    e->cidr = cidr;
    e->has_timeout = has_timeout;
    e->timeout = timeout;
    return MT_OK;
}

static mt_err_t fake_del(mt_ipset_nl_t *self, const char *name, const uint8_t *ip, uint8_t iplen,
                         uint8_t cidr) {
    mt_fake_ipset_nl_t *f = (mt_fake_ipset_nl_t *)self;
    fake_ipset_call_t call = {0};
    call.kind = FAKE_IPSET_CALL_DEL;
    snprintf(call.name, sizeof(call.name), "%s", name);
    memcpy(call.ip, ip, iplen);
    call.iplen = iplen;
    call.cidr = cidr;
    log_call(f, &call);

    mt_err_t forced;
    if (consume_failure(f, FAKE_IPSET_CALL_DEL, &forced)) { return forced; }

    fake_set_t *s = find_set(f, name);
    if (!s) { return MT_OK; } /* "entry not present" swallowed, matches Go IPSET_ERR_EXIST swallow */

    for (size_t i = 0; i < s->n; i++) {
        if (s->entries[i].iplen == iplen && s->entries[i].cidr == cidr &&
            memcmp(s->entries[i].ip, ip, iplen) == 0) {
            for (size_t k = i; k + 1 < s->n; k++) { s->entries[k] = s->entries[k + 1]; }
            s->n--;
            return MT_OK;
        }
    }
    return MT_OK;
}

static mt_err_t fake_list4(mt_ipset_nl_t *self, const char *name, mt_ipset_entry4_t **out,
                           size_t *out_n) {
    mt_fake_ipset_nl_t *f = (mt_fake_ipset_nl_t *)self;
    fake_ipset_call_t call = {0};
    call.kind = FAKE_IPSET_CALL_LIST;
    snprintf(call.name, sizeof(call.name), "%s", name);
    log_call(f, &call);

    mt_err_t forced;
    if (consume_failure(f, FAKE_IPSET_CALL_LIST, &forced)) { return forced; }

    *out = NULL;
    *out_n = 0;
    fake_set_t *s = find_set(f, name);
    if (!s || !s->exists || s->n == 0) { return MT_OK; }

    mt_ipset_entry4_t *arr = calloc(s->n, sizeof(*arr));
    if (!arr) { return MT_ERR_NOMEM; }
    size_t n = 0;
    for (size_t i = 0; i < s->n; i++) {
        if (s->entries[i].iplen != 4) { continue; }
        memcpy(arr[n].subnet.addr, s->entries[i].ip, 4);
        arr[n].subnet.cidr = s->entries[i].cidr;
        arr[n].has_timeout = s->entries[i].has_timeout;
        arr[n].timeout = s->entries[i].timeout;
        n++;
    }
    *out = arr;
    *out_n = n;
    return MT_OK;
}

static mt_err_t fake_list6(mt_ipset_nl_t *self, const char *name, mt_ipset_entry6_t **out,
                           size_t *out_n) {
    mt_fake_ipset_nl_t *f = (mt_fake_ipset_nl_t *)self;
    fake_ipset_call_t call = {0};
    call.kind = FAKE_IPSET_CALL_LIST;
    snprintf(call.name, sizeof(call.name), "%s", name);
    log_call(f, &call);

    mt_err_t forced;
    if (consume_failure(f, FAKE_IPSET_CALL_LIST, &forced)) { return forced; }

    *out = NULL;
    *out_n = 0;
    fake_set_t *s = find_set(f, name);
    if (!s || !s->exists || s->n == 0) { return MT_OK; }

    mt_ipset_entry6_t *arr = calloc(s->n, sizeof(*arr));
    if (!arr) { return MT_ERR_NOMEM; }
    size_t n = 0;
    for (size_t i = 0; i < s->n; i++) {
        if (s->entries[i].iplen != 16) { continue; }
        memcpy(arr[n].subnet.addr, s->entries[i].ip, 16);
        arr[n].subnet.cidr = s->entries[i].cidr;
        arr[n].has_timeout = s->entries[i].has_timeout;
        arr[n].timeout = s->entries[i].timeout;
        n++;
    }
    *out = arr;
    *out_n = n;
    return MT_OK;
}

static void fake_destroy_self(mt_ipset_nl_t *self) {
    mt_fake_ipset_nl_t *f = (mt_fake_ipset_nl_t *)self;
    if (!f) { return; }
    for (size_t i = 0; i < f->n_sets; i++) { free(f->sets[i].entries); }
    free(f->sets);
    free(f->calls);
    free(f);
}

static const mt_ipset_nl_ops_t k_fake_ops = {
    .create = fake_create,
    .destroy = fake_destroy,
    .add = fake_add,
    .del = fake_del,
    .list4 = fake_list4,
    .list6 = fake_list6,
    .destroy_self = fake_destroy_self,
};

mt_fake_ipset_nl_t *mt_fake_ipset_nl_new(void) {
    mt_fake_ipset_nl_t *f = calloc(1, sizeof(*f));
    if (!f) { return NULL; }
    f->base.ops = &k_fake_ops;
    return f;
}

mt_ipset_nl_t *mt_fake_ipset_nl_as_transport(mt_fake_ipset_nl_t *f) {
    return &f->base;
}

const fake_ipset_call_t *mt_fake_ipset_nl_calls(const mt_fake_ipset_nl_t *f, size_t *out_n) {
    *out_n = f->n_calls;
    return f->calls;
}

bool mt_fake_ipset_nl_set_exists(const mt_fake_ipset_nl_t *f, const char *name) {
    for (size_t i = 0; i < f->n_sets; i++) {
        if (strcmp(f->sets[i].name, name) == 0) { return f->sets[i].exists; }
    }
    return false;
}

size_t mt_fake_ipset_nl_entry_count(const mt_fake_ipset_nl_t *f, const char *name) {
    for (size_t i = 0; i < f->n_sets; i++) {
        if (strcmp(f->sets[i].name, name) == 0) { return f->sets[i].n; }
    }
    return 0;
}

void mt_fake_ipset_nl_fail_next(mt_fake_ipset_nl_t *f, fake_ipset_call_kind_t kind, mt_err_t err) {
    f->fail_pending = true;
    f->fail_kind = kind;
    f->fail_err = err;
}
