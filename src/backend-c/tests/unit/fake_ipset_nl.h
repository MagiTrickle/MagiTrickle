/* Test-only in-memory ipset transport (mt_ipset_nl_t backend) -- lets the
 * unit tests exercise mt_ipset_t's Enable/Disable/Add/Del/List gating
 * logic without a real ip_set kernel module (confirmed unavailable in
 * this sandbox, see ipset.h). Also records every call so tests can
 * assert exactly what mt_ipset_t asked the transport to do.
 */
#ifndef MT_TEST_FAKE_IPSET_NL_H
#define MT_TEST_FAKE_IPSET_NL_H

#include <stddef.h>

#include "magitrickle/ipset.h"

typedef enum fake_ipset_call_kind {
    FAKE_IPSET_CALL_CREATE,
    FAKE_IPSET_CALL_DESTROY,
    FAKE_IPSET_CALL_ADD,
    FAKE_IPSET_CALL_DEL,
    FAKE_IPSET_CALL_LIST,
} fake_ipset_call_kind_t;

typedef struct fake_ipset_call {
    fake_ipset_call_kind_t kind;
    char name[64];
    uint8_t ip[16];
    uint8_t iplen;
    uint8_t cidr;
    bool has_timeout;
    uint32_t timeout;
    bool replace;
} fake_ipset_call_t;

typedef struct mt_fake_ipset_nl mt_fake_ipset_nl_t;

mt_fake_ipset_nl_t *mt_fake_ipset_nl_new(void);
mt_ipset_nl_t *mt_fake_ipset_nl_as_transport(mt_fake_ipset_nl_t *f);

/* Call log for assertions (borrowed, valid until the fake is freed). */
const fake_ipset_call_t *mt_fake_ipset_nl_calls(const mt_fake_ipset_nl_t *f, size_t *out_n);

/* Simulated kernel state, for assertions. */
bool mt_fake_ipset_nl_set_exists(const mt_fake_ipset_nl_t *f, const char *name);
size_t mt_fake_ipset_nl_entry_count(const mt_fake_ipset_nl_t *f, const char *name);

/* Force the next call of the given kind to fail with `err` (one-shot). */
void mt_fake_ipset_nl_fail_next(mt_fake_ipset_nl_t *f, fake_ipset_call_kind_t kind, mt_err_t err);

#endif /* MT_TEST_FAKE_IPSET_NL_H */
