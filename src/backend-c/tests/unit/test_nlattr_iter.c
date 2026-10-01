#include "greatest.h"

#include <libmnl/libmnl.h>
#include <linux/rtnetlink.h>
#include <stdint.h>
#include <string.h>

#include "magitrickle/nlattr_iter.h"

TEST iterates_every_nlmsg_attribute(void) {
    uint8_t buf[256] = {0};
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    struct rtmsg *rtm = mnl_nlmsg_put_extra_header(nlh, sizeof(*rtm));
    memset(rtm, 0, sizeof(*rtm));
    mnl_attr_put_u32(nlh, RTA_OIF, 17);
    mnl_attr_put_u32(nlh, RTA_TABLE, 1298229097u);
    const uint8_t gateway[4] = {192, 0, 2, 1};
    mnl_attr_put(nlh, RTA_GATEWAY, sizeof(gateway), gateway);

    mt_nlattr_iter_t it;
    ASSERT(mt_nlattr_iter_init_nlmsg(&it, nlh, sizeof(*rtm)));

    const struct nlattr *attr = NULL;
    ASSERT(mt_nlattr_iter_next(&it, &attr));
    ASSERT_EQ(RTA_OIF, mnl_attr_get_type(attr));
    ASSERT_EQ(17u, mnl_attr_get_u32(attr));

    ASSERT(mt_nlattr_iter_next(&it, &attr));
    ASSERT_EQ(RTA_TABLE, mnl_attr_get_type(attr));
    ASSERT_EQ(1298229097u, mnl_attr_get_u32(attr));

    ASSERT(mt_nlattr_iter_next(&it, &attr));
    ASSERT_EQ(RTA_GATEWAY, mnl_attr_get_type(attr));
    ASSERT_EQ(0, memcmp(gateway, mnl_attr_get_payload(attr), sizeof(gateway)));

    ASSERT_FALSE(mt_nlattr_iter_next(&it, &attr));
    ASSERT(attr == NULL);
    PASS();
}

TEST iterates_every_nested_attribute(void) {
    uint8_t buf[256] = {0};
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    struct nlattr *nest = mnl_attr_nest_start(nlh, 100);
    mnl_attr_put_u8(nlh, 1, 11);
    mnl_attr_put_u8(nlh, 2, 22);
    mnl_attr_put_u8(nlh, 3, 33);
    mnl_attr_nest_end(nlh, nest);

    mt_nlattr_iter_t it;
    ASSERT(mt_nlattr_iter_init_nested(&it, nest));

    const struct nlattr *attr = NULL;
    for (uint16_t type = 1; type <= 3; type++) {
        ASSERT(mt_nlattr_iter_next(&it, &attr));
        ASSERT_EQ(type, mnl_attr_get_type(attr));
        ASSERT_EQ((unsigned)(type * 11u), (unsigned)mnl_attr_get_u8(attr));
    }
    ASSERT_FALSE(mt_nlattr_iter_next(&it, &attr));
    PASS();
}

TEST rejects_a_truncated_attribute(void) {
    uint8_t payload[sizeof(struct nlattr)] = {0};
    struct nlattr *attr = (struct nlattr *)payload;
    attr->nla_len = (uint16_t)(sizeof(struct nlattr) + 1u);
    attr->nla_type = 1;

    mt_nlattr_iter_t it = {.cursor = payload, .remaining = sizeof(payload)};
    const struct nlattr *out = NULL;
    ASSERT_FALSE(mt_nlattr_iter_next(&it, &out));
    ASSERT(out == NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(iterates_every_nlmsg_attribute);
    RUN_TEST(iterates_every_nested_attribute);
    RUN_TEST(rejects_a_truncated_attribute);
    GREATEST_MAIN_END();
}
