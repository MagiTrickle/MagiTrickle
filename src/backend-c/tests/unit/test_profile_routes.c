/* Real Linux policy-route integration. Never runs in the host network namespace.
 * CI: sudo unshare --net env MT_TEST_PROFILE_NETNS=1 build/host/tests/test_profile_routes
 * iptables/ipset transports are in-memory; links, addresses, fwmark rules and
 * IPv4/IPv6 routing tables are real kernel objects. */
#include "greatest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cjson/cJSON.h>
#include <libmnl/libmnl.h>
#include <linux/rtnetlink.h>
#include "fake_iptables.h"
#include "fake_ipset_nl.h"
#include "magitrickle/ipset_to_link.h"
#include "magitrickle/netfilter_cleaner.h"
#include "magitrickle/netlink_watcher.h"
#include "magitrickle/loop.h"

/* Observe actual route writes, without replacing their kernel transport. */
ssize_t __real_mnl_socket_sendto(const struct mnl_socket *nl, const void *data, size_t len);
ssize_t __wrap_mnl_socket_sendto(const struct mnl_socket *nl, const void *data, size_t len);
static unsigned route_writes;
ssize_t __wrap_mnl_socket_sendto(const struct mnl_socket *nl, const void *data, size_t len)
{
    ssize_t result = __real_mnl_socket_sendto(nl, data, len);
    if (result > 0 && len >= sizeof(struct nlmsghdr)) {
        const struct nlmsghdr *h = data;
        if (h->nlmsg_type == RTM_NEWROUTE || h->nlmsg_type == RTM_DELROUTE) { route_writes++; }
    }
    return result;
}

static bool isolated(void) {
    struct stat self, init;
    return getuid() == 0 && stat("/proc/self/ns/net", &self) == 0 &&
        stat("/proc/1/ns/net", &init) == 0 && self.st_ino != init.st_ino;
}

static bool route_is(int family, const char *dev) {
    const char *cmd = family == AF_INET ? "ip -j -4 route show table 1000" : "ip -j -6 route show table 1000";
    FILE *f = popen(cmd, "r"); if (!f) { return false; }
    char text[4096]; size_t n = fread(text, 1, sizeof(text)-1, f); text[n] = '\0';
    if (pclose(f) != 0) { return false; }
    cJSON *json = cJSON_Parse(text); if (!json) { return false; }
    size_t unicast = 0, blackholes = 0; bool found = false;
    cJSON *item;
    cJSON_ArrayForEach(item, json) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        const cJSON *oif = cJSON_GetObjectItemCaseSensitive(item, "dev");
        const cJSON *metric = cJSON_GetObjectItemCaseSensitive(item, "metric");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "blackhole") == 0) {
            blackholes++; if (!metric || metric->valueint != 20) { blackholes += 100; }
        } else {
            unicast++;
            if (dev && cJSON_IsString(oif) && strcmp(dev, oif->valuestring) == 0 && metric && metric->valueint == 10) { found = true; }
        }
    }
    if (blackholes != 1 || (dev ? !(unicast == 1 && found) : unicast != 0)) {
        fprintf(stderr, "unexpected family %d route state: %s\n", family, text);
    }
    cJSON_Delete(json);
    return blackholes == 1 && (dev ? unicast == 1 && found : unicast == 0);
}

static int create_link(unsigned i) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "ip link add fp%u type dummy && ip link set fp%u up && "
        "ip addr add 10.220.%u.2/24 dev fp%u && ip -6 addr add fd42:%u::2/64 dev fp%u nodad && "
        "ip route add default via 10.220.%u.1 dev fp%u metric %u && "
        "ip -6 route add default via fd42:%u::1 dev fp%u metric %u",
        i,i,i,i,i+1,i,i,i,100+i,i+1,i,100+i);
    return system(cmd);
}

TEST real_profile_failover_and_failback(void) {
    if (!getenv("MT_TEST_PROFILE_NETNS")) { SKIP(); }
    ASSERTm("Refusing to change the host network namespace", isolated());
    ASSERT_EQ(0, system("ip link set lo up"));
    for (unsigned i=0; i<3; i++) { ASSERT_EQ(0, create_link(i)); }
    mt_rtnl_t *rtnl = mt_rtnl_open(); ASSERT(rtnl);
    mt_fake_ipt_t *f4 = mt_fake_ipt_new(MT_IPT_PROTO_IPV4), *f6 = mt_fake_ipt_new(MT_IPT_PROTO_IPV6);
    ASSERT(f4 && f6);
    mt_ipt_t *ipt4 = mt_ipt_new(mt_fake_ipt_as_executable(f4));
    mt_ipt_t *ipt6 = mt_ipt_new(mt_fake_ipt_as_executable(f6)); ASSERT(ipt4 && ipt6);
    ASSERT_EQ(MT_OK, mt_netfilter_register_base_chains(ipt4, ipt6));
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new(); ASSERT(fake);
    mt_ipset_t *set = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "profile_test"); ASSERT(set);
    ASSERT_EQ(MT_OK, mt_ipset_enable(set));
    mt_ipv4_subnet_t subnet = {.addr={198,51,100,0}, .cidr=24};
    ASSERT_EQ(MT_OK, mt_ipset_add4(set, subnet, NULL));
    mt_ipset_to_link_t *link = mt_ipset_to_link_new("MT_profile", "fp0", set, ipt4, ipt6, rtnl, 1000); ASSERT(link);
    const char *chain[] = {"fp0", "fp1", "fp2"};
    ASSERT_EQ(MT_OK, mt_ipset_to_link_set_interfaces(link, chain, 3));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_enable(link));
    ASSERT(route_is(AF_INET,"fp0")); ASSERT(route_is(AF_INET6,"fp0"));

    /* A terminal-route failure in one family must not stop the OTHER
     * family's failover. Preserve/report the conflict rather than quietly
     * deleting it. Test both directions, then recovery after repair. */
    ASSERT_EQ(0, system("ip -6 route del table 1000 blackhole default metric 20 && "
                       "ip -6 route add table 1000 unreachable default metric 20 && "
                       "ip -4 route del default via 10.220.0.1 dev fp0 metric 100"));
    ASSERT_EQ(MT_ERR_STATE, mt_ipset_to_link_on_link_up(link));
    ASSERT(route_is(AF_INET,"fp1"));
    ASSERT_EQ(0, system("ip -6 route del table 1000 unreachable default metric 20 && "
                       "ip -6 route add table 1000 blackhole default metric 20 && "
                       "ip -4 route add default via 10.220.0.1 dev fp0 metric 100"));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_on_link_up(link));
    ASSERT(route_is(AF_INET,"fp0")); ASSERT(route_is(AF_INET6,"fp0"));
    ASSERT_EQ(0, system("ip -4 route del table 1000 blackhole default metric 20 && "
                       "ip -4 route add table 1000 unreachable default metric 20 && "
                       "ip -6 route del default via fd42:1::1 dev fp0 metric 100"));
    ASSERT_EQ(MT_ERR_STATE, mt_ipset_to_link_on_link_up(link));
    ASSERT(route_is(AF_INET6,"fp1"));
    ASSERT_EQ(0, system("ip -4 route del table 1000 unreachable default metric 20 && "
                       "ip -4 route add table 1000 blackhole default metric 20 && "
                       "ip -6 route add default via fd42:1::1 dev fp0 metric 100"));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_on_link_up(link));
    ASSERT(route_is(AF_INET,"fp0")); ASSERT(route_is(AF_INET6,"fp0"));

    /* Family readiness is independent. */
    ASSERT_EQ(0, system("ip -6 addr del fd42:1::2/64 dev fp0"));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_on_addr_change(link));
    ASSERT(route_is(AF_INET,"fp0")); ASSERT(route_is(AF_INET6,"fp1"));
    ASSERT_EQ(0, system("ip -6 addr add fd42:1::2/64 dev fp0 nodad"));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_on_addr_change(link));
    ASSERT(route_is(AF_INET6,"fp0"));

    /* A vanished upstream default must not be resurrected from our own table. */
    ASSERT_EQ(0, system("ip -4 route del default via 10.220.0.1 dev fp0 metric 100"));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_on_link_up(link));
    ASSERT(route_is(AF_INET,"fp1")); ASSERT(route_is(AF_INET6,"fp0"));
    ASSERT_EQ(0, system("ip -4 route add default via 10.220.0.1 dev fp0 metric 100"));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_on_link_up(link)); ASSERT(route_is(AF_INET,"fp0"));

    ASSERT_EQ(0, system("ip link set fp0 down"));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_on_link_up(link));
    ASSERT(route_is(AF_INET,"fp1")); ASSERT(route_is(AF_INET6,"fp1"));
    ASSERT_EQ(0, system("ip link set fp1 down"));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_on_link_up(link)); ASSERT(route_is(AF_INET,"fp2"));
    ASSERT_EQ(0, system("ip link set fp2 down"));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_on_link_up(link));
    ASSERT(route_is(AF_INET,NULL)); ASSERT(route_is(AF_INET6,NULL));
    ASSERT(system("ip -4 route get 198.51.100.9 mark 1000 >/dev/null 2>&1") != 0);
    ASSERT(system("ip -6 route get fdff::9 mark 1000 >/dev/null 2>&1") != 0);

    /* Recreated link keeps its configured name but gets a different ifindex. */
    ASSERT_EQ(0, system("ip link del fp0")); ASSERT_EQ(0, create_link(0));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_on_link_up(link));
    ASSERT(route_is(AF_INET,"fp0")); ASSERT(route_is(AF_INET6,"fp0"));
    ASSERT_EQ(0, system("ip link del fp2")); ASSERT_EQ(0, create_link(2));
    const char *reordered[] = {"fp2", "fp0", "fp1"};
    ASSERT_EQ(MT_OK, mt_ipset_to_link_set_interfaces(link, reordered, 3));
    ASSERT(route_is(AF_INET,"fp2")); ASSERT(route_is(AF_INET6,"fp2"));
    ASSERT_EQ((size_t)1, mt_fake_ipset_nl_entry_count(fake, "profile_test_4"));

    /* An external removal or replacement must be repaired by an event-driven pass. */
    ASSERT_EQ(0, system("ip -4 route del table 1000 default metric 10"));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_on_link_up(link)); ASSERT(route_is(AF_INET,"fp2"));
    ASSERT_EQ(0, system("ip -4 route replace table 1000 default dev fp0 metric 10"));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_on_link_up(link)); ASSERT(route_is(AF_INET,"fp2"));
    /* The permanent blackhole is also recreated if removed externally. */
    ASSERT_EQ(0, system("ip -4 route del table 1000 blackhole default metric 20"));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_on_link_up(link)); ASSERT(route_is(AF_INET,"fp2"));
    /* Count actual RTM_NEWROUTE/DELROUTE sends, not merely an OK return. */
    ASSERT(route_writes > 0);
    unsigned stable_writes = route_writes;
    for (unsigned i = 0; i < 3; i++) {
        ASSERT_EQ(MT_OK, mt_ipset_to_link_on_link_up(link));
        ASSERT_EQ(stable_writes, route_writes);
    }
    ASSERT_EQ(MT_OK, mt_ipset_to_link_disable(link));
    mt_ipset_to_link_free(link); ASSERT_EQ(MT_OK, mt_ipset_disable(set)); mt_ipset_free(set);
    mt_ipt_free(ipt4); mt_ipt_free(ipt6); mt_rtnl_close(rtnl);
    for (unsigned i=0;i<3;i++) { char cmd[64]; snprintf(cmd,sizeof(cmd),"ip link del fp%u",i); ASSERT_EQ(0,system(cmd)); }
    PASS();
}

typedef struct event_probe {
    mt_loop_t *loop;
    unsigned links, routes;
    int command_status;
} event_probe_t;

static void event_link(const char *name, bool up, void *ud)
{
    event_probe_t *p = ud;
    if (up && strcmp(name, "fpevent0") == 0) { p->links++; }
    if (p->links && p->routes) { mt_loop_stop(p->loop); }
}

static void event_route(void *ud)
{
    event_probe_t *p = ud;
    p->routes++;
    if (p->links && p->routes) { mt_loop_stop(p->loop); }
}

static void create_event_link(mt_loop_t *loop, void *ud)
{
    (void)loop;
    event_probe_t *p = ud;
    p->command_status = system("ip link add fpevent0 type dummy && "
                               "ip link set fpevent0 up && "
                               "ip addr add 192.0.2.1/24 dev fpevent0");
}

static void stop_event_test(mt_loop_t *loop, void *ud)
{
    (void)ud;
    mt_loop_stop(loop);
}

TEST netlink_events_delivered_from_real_kernel(void)
{
    if (!getenv("MT_TEST_PROFILE_NETNS")) { SKIP(); }
    ASSERTm("Refusing to change the host network namespace", isolated());
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    event_probe_t p = {.loop = loop, .command_status = -1};
    mt_nl_watcher_t *watcher = NULL;
    ASSERT_EQ(MT_OK, mt_nl_watcher_create_with_routes(loop, event_link, &p,
                                                       NULL, NULL, event_route, &p,
                                                       NULL, NULL, &watcher));
    int change_timer, timeout_timer;
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 10, 0, create_event_link, &p, &change_timer));
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 3000, 0, stop_event_test, &p, &timeout_timer));
    ASSERT_EQ(MT_OK, mt_loop_run(loop));
    mt_nl_watcher_destroy(watcher);
    mt_loop_destroy(loop);
    ASSERT_EQ(0, p.command_status);
    ASSERT(p.links > 0);
    ASSERT(p.routes > 0);
    ASSERT_EQ(0, system("ip link del fpevent0"));
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(real_profile_failover_and_failback);
    RUN_TEST(netlink_events_delivered_from_real_kernel);
    GREATEST_MAIN_END();
}
