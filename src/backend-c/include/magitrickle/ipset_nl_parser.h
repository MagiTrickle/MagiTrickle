/* Internal parser seam for deterministic tests of real ipset dump messages.
 * This does not open a netlink socket; it parses one NLMSG payload exactly
 * as the production list callback does.
 */
#ifndef MAGITRICKLE_IPSET_NL_PARSER_H
#define MAGITRICKLE_IPSET_NL_PARSER_H

#include <linux/netlink.h>
#include <stddef.h>
#include <stdint.h>

#include "magitrickle/err.h"
#include "magitrickle/ipset.h"

mt_err_t mt_ipset_nl_parse_list_message(const struct nlmsghdr *h, uint8_t iplen,
                                        mt_ipset_entry4_t **out4, mt_ipset_entry6_t **out6,
                                        size_t *out_n);

#endif /* MAGITRICKLE_IPSET_NL_PARSER_H */
