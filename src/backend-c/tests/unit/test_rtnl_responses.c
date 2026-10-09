/* Exercise the production rtnetlink exchange through its public readers.
 * Only send/recv are wrapped; libmnl builds/parses real wire messages.
 * No privileged operation or production-only test hook is needed. */
#include "greatest.h"
#include "magitrickle/rtnl.h"

#include <errno.h>
#include <limits.h>
#include <libmnl/libmnl.h>
#include <linux/rtnetlink.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

ssize_t __wrap_mnl_socket_sendto(const struct mnl_socket *nl, const void *data, size_t len);
ssize_t __wrap_mnl_socket_recvfrom(const struct mnl_socket *nl, void *data, size_t len);

static struct {
    uint32_t seq;
    unsigned part, reads;
    int status, recv_error;
    size_t status_size;
    bool interrupted, early_ack, stale_done, extack;
} reply;

ssize_t __wrap_mnl_socket_sendto(const struct mnl_socket *nl, const void *data, size_t len)
{
    (void)nl;
    if (len < sizeof(struct nlmsghdr)) { errno = EINVAL; return -1; }
    const struct nlmsghdr *h = data;
    if (h->nlmsg_type != RTM_GETROUTE) { errno = EINVAL; return -1; }
    reply.seq = h->nlmsg_seq;
    reply.part = 0;
    return (ssize_t)len;
}

ssize_t __wrap_mnl_socket_recvfrom(const struct mnl_socket *nl, void *data, size_t len)
{
    (void)nl;
    reply.reads++;
    if (reply.recv_error) {
        errno = reply.recv_error;
        reply.recv_error = 0;
        return -1;
    }
    if (len < 256) { errno = EMSGSIZE; return -1; }
    memset(data, 0, len);
    struct nlmsghdr *h = mnl_nlmsg_put_header(data);
    h->nlmsg_seq = reply.seq;
    h->nlmsg_flags = NLM_F_MULTI;
    if (reply.stale_done) {
        reply.stale_done = false;
        h->nlmsg_seq--;
        h->nlmsg_type = NLMSG_DONE;
        int status = -EIO;
        memcpy(mnl_nlmsg_put_extra_header(h, sizeof(status)), &status, sizeof(status));
    } else if (reply.early_ack) {
        reply.early_ack = false;
        h->nlmsg_type = NLMSG_ERROR;
        struct nlmsgerr *ack = mnl_nlmsg_put_extra_header(h, sizeof(*ack));
        ack->error = 0;
    } else if (reply.part++ == 0) {
        h->nlmsg_type = RTM_NEWROUTE;
        struct rtmsg *rt = mnl_nlmsg_put_extra_header(h, sizeof(*rt));
        rt->rtm_family = AF_INET;
        rt->rtm_type = RTN_UNICAST;
        mnl_attr_put_u32(h, RTA_TABLE, 1000);
        mnl_attr_put_u32(h, RTA_PRIORITY, 10);
        mnl_attr_put_u32(h, RTA_OIF, 7);
    } else {
        h->nlmsg_type = NLMSG_DONE;
        if (reply.interrupted) { h->nlmsg_flags |= NLM_F_DUMP_INTR; }
        if (reply.status_size) {
            /* put_extra_header aligns the length: override it to also test
             * deliberately truncated, unaligned DONE payloads. */
            void *payload = mnl_nlmsg_put_extra_header(h, reply.status_size);
            memcpy(payload, &reply.status, reply.status_size);
            h->nlmsg_len = (uint32_t)(sizeof(*h) + reply.status_size);
        }
        if (reply.extack) { mnl_attr_put_strz(h, 1, "test completion detail"); }
    }
    return (ssize_t)h->nlmsg_len;
}

static void reset_reply(void)
{
    memset(&reply, 0, sizeof(reply));
    reply.status_size = sizeof(int);
}

TEST done_status_is_required_before_publishing_snapshot(void)
{
    const int statuses[] = {0, -EIO, -EAGAIN, -EINTR, -ENOMEM, INT_MIN};
    const mt_err_t expected[] = {MT_OK, MT_ERR_IO, MT_ERR_AGAIN, MT_ERR_AGAIN,
                                MT_ERR_NOMEM, MT_ERR_PROTO};
    mt_rtnl_t *r = mt_rtnl_open(); ASSERT(r);
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        reset_reply(); reply.status = statuses[i];
        mt_rtnl_default_route_t out = {.found = true, .ifindex = 99};
        ASSERT_EQ(expected[i], mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
        ASSERT_EQ(2u, reply.reads);
        ASSERT_EQ(expected[i] == MT_OK, out.found);
        ASSERT_EQ(expected[i] == MT_OK ? 7 : 0, out.ifindex);
    }
    mt_rtnl_close(r); PASS();
}

TEST truncated_done_is_not_an_empty_or_successful_dump(void)
{
    mt_rtnl_t *r = mt_rtnl_open(); ASSERT(r);
    for (size_t size = 0; size < sizeof(int); size++) {
        reset_reply(); reply.status_size = size;
        mt_rtnl_default_route_t out;
        ASSERT_EQ(MT_ERR_PROTO, mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
        ASSERT(!out.found); ASSERT_EQ(0, out.ifindex);
    }
    mt_rtnl_close(r); PASS();
}

TEST interrupted_dump_recovers_with_a_fresh_sequence(void)
{
    mt_rtnl_t *r = mt_rtnl_open(); ASSERT(r);
    reset_reply(); reply.interrupted = true;
    mt_rtnl_default_route_t out;
    ASSERT_EQ(MT_ERR_AGAIN, mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
    ASSERT(!out.found);
    uint32_t old_seq = reply.seq;
    reset_reply(); reply.stale_done = true;
    ASSERT_EQ(MT_OK, mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
    ASSERT_EQ(old_seq + 1u, reply.seq);
    ASSERT_EQ(3u, reply.reads);
    ASSERT(out.found); ASSERT_EQ(7, out.ifindex);
    mt_rtnl_close(r); PASS();
}

TEST ack_and_extack_do_not_hide_completion_error(void)
{
    mt_rtnl_t *r = mt_rtnl_open(); ASSERT(r);
    reset_reply(); reply.early_ack = true; reply.extack = true; reply.status = -EIO;
    mt_rtnl_default_route_t out;
    ASSERT_EQ(MT_ERR_IO, mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
    ASSERT_EQ(3u, reply.reads); ASSERT(!out.found);
    reset_reply(); reply.extack = true;
    ASSERT_EQ(MT_OK, mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
    ASSERT(out.found);
    mt_rtnl_close(r); PASS();
}

TEST receive_interrupt_retries_but_receive_loss_is_an_error(void)
{
    mt_rtnl_t *r = mt_rtnl_open(); ASSERT(r);
    reset_reply(); reply.recv_error = EINTR;
    mt_rtnl_default_route_t out;
    ASSERT_EQ(MT_OK, mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
    ASSERT_EQ(3u, reply.reads); ASSERT(out.found);
    reset_reply(); reply.recv_error = ENOBUFS;
    ASSERT(mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out) != MT_OK);
    ASSERT(!out.found);
    mt_rtnl_close(r); PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(done_status_is_required_before_publishing_snapshot);
    RUN_TEST(truncated_done_is_not_an_empty_or_successful_dump);
    RUN_TEST(interrupted_dump_recovers_with_a_fresh_sequence);
    RUN_TEST(ack_and_extack_do_not_hide_completion_error);
    RUN_TEST(receive_interrupt_retries_but_receive_loss_is_an_error);
    GREATEST_MAIN_END();
}
