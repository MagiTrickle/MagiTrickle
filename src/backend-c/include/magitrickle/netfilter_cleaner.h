/* Port of utils/netfilterTools/iptables-cleaner.go: on startup, sweeps
 * every table for chains this daemon previously created (by prefix) and
 * schedules them for deletion, plus removes any dangling "-j <prefix>*"
 * jump rules left in chains it doesn't own (patching in a patch-chain
 * registration on the fly if the referencing chain wasn't already
 * registered, exactly matching Go's ErrChainNotInitialized recovery).
 */
#ifndef MAGITRICKLE_NETFILTER_CLEANER_H
#define MAGITRICKLE_NETFILTER_CLEANER_H

#include "magitrickle/err.h"
#include "magitrickle/iptables.h"

/* ipt4/ipt6 borrowed, nullable. Commits each non-NULL one. */
mt_err_t mt_netfilter_clean_iptables(mt_ipt_t *ipt4, mt_ipt_t *ipt6, const char *chain_prefix);

/* Declares the kernel chains we hang our jumps off (filter/FORWARD,
 * mangle/PREROUTING, nat/PREROUTING, nat/POSTROUTING) as patch chains, so
 * rules belonging to anyone else in them are left alone.
 *
 * Registering a chain replaces its previous registration, so calling this
 * again at the start of a full rebuild also discards the jump operations
 * staged by the pass before it -- which is what makes a rebuild
 * independent of the one that came before. */
mt_err_t mt_netfilter_register_base_chains(mt_ipt_t *ipt4, mt_ipt_t *ipt6);

#endif /* MAGITRICKLE_NETFILTER_CLEANER_H */
