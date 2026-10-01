/* Port of utils/netfilterTools/port-remap.go: DNAT's a set of local
 * addresses' `from` port to `to` (used to redirect port 53 to the DNS
 * MITM proxy's listen port without an explicit socket bind on 53).
 */
#ifndef MAGITRICKLE_PORT_REMAP_H
#define MAGITRICKLE_PORT_REMAP_H

#include <stdint.h>

#include "magitrickle/err.h"
#include "magitrickle/iptables.h"

typedef struct mt_remap_addr {
    int family; /* AF_INET or AF_INET6 */
    uint8_t ip[16];
    uint8_t iplen; /* 4 or 16 */
} mt_remap_addr_t;

typedef struct mt_port_remap mt_port_remap_t;

/* The owned chain name is always "<chain_prefix>DNSOR", preserving the
 * configured netfilter naming contract. ipt4/ipt6 are borrowed, nullable;
 * they must already have "nat"/PREROUTING registered as a patch chain by
 * the caller (see start.go's ordering). addrs is deep-copied. */
mt_port_remap_t *mt_port_remap_new(const char *chain_prefix, uint16_t from, uint16_t to,
                                   const mt_remap_addr_t *addrs, size_t n_addrs, mt_ipt_t *ipt4,
                                   mt_ipt_t *ipt6);
void mt_port_remap_free(mt_port_remap_t *p);

mt_err_t mt_port_remap_enable(mt_port_remap_t *p);
mt_err_t mt_port_remap_disable(mt_port_remap_t *p);

/* Re-stages the remap chain and its PREROUTING jump for a full table
 * rebuild, leaving the write to the caller's single commit. No-op when
 * the remap is disabled (or p is NULL, i.e. remap53 turned off). */
mt_err_t mt_port_remap_prepare_iptables(mt_port_remap_t *p);

#endif /* MAGITRICKLE_PORT_REMAP_H */
