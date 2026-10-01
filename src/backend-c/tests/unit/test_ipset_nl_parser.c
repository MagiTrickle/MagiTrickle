#include "greatest.h"

#include <arpa/inet.h>
#include <libmnl/libmnl.h>
#include <linux/netfilter/ipset/ip_set.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netlink.h>
#include <stdlib.h>
#include <string.h>

#include "magitrickle/ipset_nl_parser.h"

static void put_entry(struct nlmsghdr *nlh, const uint8_t *addr, uint8_t iplen, uint8_t cidr,
                      uint32_t timeout) {
    struct nlattr *data = mnl_attr_nest_start(nlh, IPSET_ATTR_DATA);
    struct nlattr *ip = mnl_attr_nest_start(nlh, IPSET_ATTR_IP);
    uint16_t addr_type = iplen == 4 ? IPSET_ATTR_IPADDR_IPV4 : IPSET_ATTR_IPADDR_IPV6;
    /* Kernel LIST replies do not set NLA_F_NET_BYTEORDER on the address
     * child, even though ADD/DEL requests do. This matches the on-device
     * wire dump instead of reusing the request shape. */
    mnl_attr_put(nlh, addr_type, iplen, addr);
    mnl_attr_nest_end(nlh, ip);
    mnl_attr_put_u8(nlh, IPSET_ATTR_CIDR, cidr);
    mnl_attr_put_u32(nlh, IPSET_ATTR_TIMEOUT | NLA_F_NET_BYTEORDER, htonl(timeout));
    mnl_attr_nest_end(nlh, data);
}

static struct nlmsghdr *list_message(uint8_t *buf, const uint8_t *addr, uint8_t iplen,
                                    uint8_t cidr, uint32_t timeout) {
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    struct nfgenmsg *nfg = mnl_nlmsg_put_extra_header(nlh, sizeof(*nfg));
    nfg->nfgen_family = AF_NETLINK;
    nfg->version = NFNETLINK_V0;
    nfg->res_id = 0;

    struct nlattr *adt = mnl_attr_nest_start(nlh, IPSET_ATTR_ADT);
    put_entry(nlh, addr, iplen, cidr, timeout);
    mnl_attr_nest_end(nlh, adt);
    return nlh;
}

TEST parses_nested_ipv4_entry(void) {
    uint8_t buf[512] = {0};
    const uint8_t addr[4] = {192, 0, 2, 128};
    struct nlmsghdr *nlh = list_message(buf, addr, 4, 25, 123);

    mt_ipset_entry4_t *out4 = NULL;
    mt_ipset_entry6_t *out6 = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_ipset_nl_parse_list_message(nlh, 4, &out4, &out6, &n));
    ASSERT_EQ(1u, (unsigned)n);
    ASSERT(out4 != NULL);
    ASSERT(out6 == NULL);
    ASSERT_EQ(0, memcmp(addr, out4[0].subnet.addr, sizeof(addr)));
    ASSERT_EQ(25, out4[0].subnet.cidr);
    ASSERT(out4[0].has_timeout);
    ASSERT_EQ(123u, (unsigned)out4[0].timeout);

    free(out4);
    PASS();
}

TEST parses_nested_ipv6_entry(void) {
    uint8_t buf[512] = {0};
    const uint8_t addr[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                              0,    0,    0,    0,    0, 0, 0, 1};
    struct nlmsghdr *nlh = list_message(buf, addr, 16, 64, 0);

    mt_ipset_entry4_t *out4 = NULL;
    mt_ipset_entry6_t *out6 = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_ipset_nl_parse_list_message(nlh, 16, &out4, &out6, &n));
    ASSERT_EQ(1u, (unsigned)n);
    ASSERT(out4 == NULL);
    ASSERT(out6 != NULL);
    ASSERT_EQ(0, memcmp(addr, out6[0].subnet.addr, sizeof(addr)));
    ASSERT_EQ(64, out6[0].subnet.cidr);
    /* Kernel timeout zero represents a permanent entry. */
    ASSERT_FALSE(out6[0].has_timeout);
    ASSERT_EQ(0u, (unsigned)out6[0].timeout);

    free(out6);
    PASS();
}

TEST parses_every_ipv4_entry_in_a_kernel_dump(void) {
    uint8_t buf[512] = {0};
    const uint8_t addr1[4] = {192, 0, 2, 1};
    const uint8_t addr2[4] = {198, 51, 100, 2};

    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    struct nfgenmsg *nfg = mnl_nlmsg_put_extra_header(nlh, sizeof(*nfg));
    nfg->nfgen_family = AF_NETLINK;
    nfg->version = NFNETLINK_V0;
    nfg->res_id = 0;
    struct nlattr *adt = mnl_attr_nest_start(nlh, IPSET_ATTR_ADT);
    put_entry(nlh, addr1, 4, 32, 100);
    put_entry(nlh, addr2, 4, 24, 200);
    mnl_attr_nest_end(nlh, adt);

    mt_ipset_entry4_t *out4 = NULL;
    mt_ipset_entry6_t *out6 = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_ipset_nl_parse_list_message(nlh, 4, &out4, &out6, &n));
    ASSERT_EQ(2u, (unsigned)n);
    ASSERT(out4 != NULL);
    ASSERT(out6 == NULL);
    ASSERT_EQ(0, memcmp(addr1, out4[0].subnet.addr, sizeof(addr1)));
    ASSERT_EQ(32, out4[0].subnet.cidr);
    ASSERT_EQ(0, memcmp(addr2, out4[1].subnet.addr, sizeof(addr2)));
    ASSERT_EQ(24, out4[1].subnet.cidr);

    free(out4);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(parses_nested_ipv4_entry);
    RUN_TEST(parses_nested_ipv6_entry);
    RUN_TEST(parses_every_ipv4_entry_in_a_kernel_dump);
    GREATEST_MAIN_END();
}
