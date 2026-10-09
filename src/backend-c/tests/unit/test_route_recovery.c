/* Production watcher -> daemon event queue -> retry/recovery callbacks.
 * The clock and failing I/O boundaries are deterministic, not sleeps.
 * The app result is injected (AGAIN models committer contention); actual
 * policy-route writes are covered separately by test_profile_routes.c. */
#include "greatest.h"

int mt_test_daemon_main(int argc, char **argv);
#define main mt_test_daemon_main
#include "../../src/main/main.c"
#undef main

#include <libmnl/libmnl.h>
#include <linux/rtnetlink.h>
#include <sys/epoll.h>

mt_err_t __wrap_mt_loop_add_timer(mt_loop_t *loop, uint64_t initial_ms,
                                  uint64_t interval_ms, mt_timer_cb cb, void *ud, int *out_id);
mt_err_t __wrap_mt_loop_del_timer(mt_loop_t *loop, int id);
mt_err_t __wrap_mt_loop_add_fd(mt_loop_t *loop, int fd, uint32_t events, mt_fd_cb cb, void *ud);
mt_err_t __wrap_mt_loop_del_fd(mt_loop_t *loop, int fd);
void __wrap_mt_loop_stop(mt_loop_t *loop);
mt_err_t __wrap_mt_app_reconcile_routes(mt_app_t *app, const char *name);
struct mnl_socket *__real_mnl_socket_open(int bus);
struct mnl_socket *__wrap_mnl_socket_open(int bus);
ssize_t __wrap_mnl_socket_recvfrom(const struct mnl_socket *nl, void *data, size_t len);

typedef struct test_timer {
    int id;
    uint64_t due;
    mt_timer_cb cb;
    void *ud;
} test_timer_t;
static struct {
    test_timer_t timers[4];
    uint64_t now;
    int serial, fd, recv_error;
    unsigned opens, registrations, removals, reconciles, open_failures;
    mt_fd_cb readable;
    void *read_ud;
    mt_err_t result;
    uint16_t event;
    bool malformed, fail_timer, stopped;
} fx;
static struct daemon d;

mt_err_t __wrap_mt_loop_add_timer(mt_loop_t *loop, uint64_t initial_ms,
                                  uint64_t interval_ms, mt_timer_cb cb, void *ud, int *out_id)
{
    (void)loop;
    if (fx.fail_timer) { fx.fail_timer = false; return MT_ERR_NOMEM; }
    if (interval_ms != 0) { return MT_ERR_INVAL; }
    for (size_t i = 0; i < 4; i++) {
        if (fx.timers[i].id) { continue; }
        fx.timers[i] = (test_timer_t){++fx.serial, fx.now + initial_ms, cb, ud};
        *out_id = fx.serial;
        return MT_OK;
    }
    return MT_ERR_LIMIT;
}
mt_err_t __wrap_mt_loop_del_timer(mt_loop_t *loop, int id)
{
    (void)loop;
    for (size_t i = 0; i < 4; i++) {
        if (fx.timers[i].id == id) { fx.timers[i].id = 0; return MT_OK; }
    }
    return MT_ERR_NOENT;
}
mt_err_t __wrap_mt_loop_add_fd(mt_loop_t *loop, int fd, uint32_t events, mt_fd_cb cb, void *ud)
{
    (void)loop;
    if (fx.readable || !(events & EPOLLIN)) { return MT_ERR_STATE; }
    fx.fd = fd; fx.readable = cb; fx.read_ud = ud; fx.registrations++;
    return MT_OK;
}
mt_err_t __wrap_mt_loop_del_fd(mt_loop_t *loop, int fd)
{
    (void)loop;
    if (!fx.readable || fx.fd != fd) { return MT_ERR_NOENT; }
    fx.readable = NULL; fx.read_ud = NULL; fx.removals++;
    return MT_OK;
}
void __wrap_mt_loop_stop(mt_loop_t *loop) { (void)loop; fx.stopped = true; }
mt_err_t __wrap_mt_app_reconcile_routes(mt_app_t *app, const char *name)
{
    (void)app;
    fx.reconciles++;
    return name ? MT_ERR_INVAL : fx.result;
}
struct mnl_socket *__wrap_mnl_socket_open(int bus)
{
    fx.opens++;
    if (fx.open_failures) { fx.open_failures--; errno = EMFILE; return NULL; }
    return __real_mnl_socket_open(bus);
}
ssize_t __wrap_mnl_socket_recvfrom(const struct mnl_socket *nl, void *data, size_t len)
{
    (void)nl;
    if (fx.recv_error) { errno = fx.recv_error; fx.recv_error = 0; return -1; }
    if (!fx.event) { errno = EAGAIN; return -1; }
    if (len < 128) { errno = EMSGSIZE; return -1; }
    memset(data, 0, len);
    struct nlmsghdr *h = mnl_nlmsg_put_header(data);
    h->nlmsg_type = fx.event;
    if (!fx.malformed && (fx.event == RTM_NEWROUTE || fx.event == RTM_DELROUTE)) {
        struct rtmsg *rt = mnl_nlmsg_put_extra_header(h, sizeof(*rt));
        rt->rtm_family = AF_INET;
    }
    fx.event = 0; fx.malformed = false;
    return (ssize_t)h->nlmsg_len;
}

static unsigned timer_count(void)
{
    unsigned count = 0;
    for (size_t i = 0; i < 4; i++) { if (fx.timers[i].id) { count++; } }
    return count;
}
static size_t next_timer(void)
{
    size_t next = 4;
    for (size_t i = 0; i < 4; i++) {
        if (fx.timers[i].id && (next == 4 || fx.timers[i].due < fx.timers[next].due)) { next = i; }
    }
    return next;
}
static uint64_t next_delay(void)
{
    size_t i = next_timer();
    return i == 4 ? UINT64_MAX : fx.timers[i].due - fx.now;
}
static bool fire_next(void)
{
    size_t i = next_timer();
    if (i == 4) { return false; }
    test_timer_t timer = fx.timers[i];
    fx.timers[i].id = 0; fx.now = timer.due;
    timer.cb(d.loop, timer.ud);
    return true;
}
static void notify(uint16_t event, int error, uint32_t mask)
{
    fx.event = event; fx.recv_error = error;
    if (fx.readable) { fx.readable(d.loop, fx.fd, mask, fx.read_ud); }
}
static bool setup(void)
{
    memset(&fx, 0, sizeof(fx)); memset(&d, 0, sizeof(d));
    if (mt_loop_create(&d.loop) != MT_OK) { return false; }
    return mt_nl_watcher_create_with_routes(d.loop, on_link_up, &d, on_addr_change, &d,
                                            on_route_change, &d, on_netlink_resync, &d,
                                            &d.watcher) == MT_OK;
}

TEST lost_and_malformed_events_coalesce_without_periodic_polling(void)
{
    ASSERT(setup());
    notify(RTM_NEWROUTE, ENOBUFS, EPOLLIN);
    notify(RTM_DELROUTE, 0, EPOLLIN);
    notify(NLMSG_OVERRUN, 0, EPOLLIN);
    fx.malformed = true; notify(RTM_NEWROUTE, 0, EPOLLIN);
    on_link_up("tun0", false, &d);
    on_addr_change("tun0", &d);
    ASSERT_EQ(1u, timer_count()); ASSERT_EQ(20u, next_delay());
    ASSERT_EQ(0u, fx.reconciles);
    ASSERT(fire_next());
    ASSERT_EQ(1u, fx.reconciles); ASSERT_EQ(0u, timer_count());
    ASSERT_EQ(0u, d.route_retry_ms); ASSERT(!fx.stopped);
    daemon_teardown(&d);
    ASSERT_EQ(fx.registrations, fx.removals);
    PASS();
}

TEST contention_and_apply_error_retry_then_stop_on_success(void)
{
    ASSERT(setup()); fx.result = MT_ERR_AGAIN;
    notify(RTM_NEWROUTE, 0, EPOLLIN);
    ASSERT(fire_next()); ASSERT_EQ(100u, next_delay());
    int pending = d.route_reconcile_timer;
    for (unsigned i = 0; i < 20; i++) { notify(RTM_NEWROUTE, 0, EPOLLIN); }
    ASSERT_EQ(pending, d.route_reconcile_timer); ASSERT_EQ(1u, timer_count());
    fx.result = MT_ERR_IO;
    ASSERT(fire_next()); ASSERT_EQ(200u, next_delay());
    fx.result = MT_OK;
    ASSERT(fire_next()); ASSERT_EQ(3u, fx.reconciles);
    ASSERT_EQ(0u, timer_count()); ASSERT_EQ(0u, d.route_retry_ms);
    notify(RTM_DELROUTE, 0, EPOLLIN); ASSERT_EQ(20u, next_delay());
    ASSERT(fire_next()); ASSERT_EQ(0u, timer_count());
    daemon_teardown(&d); PASS();
}

TEST reconnect_backoff_is_bounded_and_success_resynchronizes(void)
{
    ASSERT(setup());
    notify(0, 0, EPOLLHUP);
    ASSERT_EQ(1u, fx.removals); ASSERT_EQ(2u, timer_count());
    ASSERT(fire_next()); /* initial resync, before reconnect */
    ASSERT_EQ(1u, fx.reconciles);
    fx.open_failures = 12;
    const uint64_t delays[] = {80, 200, 400, 800, 1600, 3200, 6400, 12800,
                              25600, 30000, 30000, 30000};
    for (size_t i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
        ASSERT_EQ(delays[i], next_delay());
        ASSERT(fire_next()); ASSERT_EQ(1u, timer_count());
        ASSERT_EQ(1u, fx.registrations);
    }
    ASSERT_EQ(0u, fx.open_failures); ASSERT_EQ(30000u, next_delay());
    ASSERT(fire_next());
    ASSERT_EQ(2u, fx.registrations); ASSERT_EQ(20u, next_delay());
    ASSERT(fire_next()); ASSERT_EQ(2u, fx.reconciles);
    ASSERT_EQ(0u, timer_count()); ASSERT(!fx.stopped);
    /* A new disconnect starts at 100 ms again; teardown cancels BOTH
     * the route resync and watcher reconnect, leaving no stale callback. */
    notify(0, 0, EPOLLERR);
    ASSERT(fire_next()); ASSERT_EQ(80u, next_delay());
    daemon_teardown(&d);
    ASSERT_EQ(0u, timer_count()); ASSERT(!fire_next());
    ASSERT_EQ(fx.registrations, fx.removals);
    PASS();
}

TEST route_retry_delay_caps_and_scheduler_failure_is_not_hidden(void)
{
    ASSERT(setup()); fx.result = MT_ERR_AGAIN;
    on_route_change(&d); ASSERT(fire_next());
    for (unsigned i = 0; i < 12; i++) {
        ASSERT(next_delay() <= 30000u); ASSERT(fire_next());
    }
    ASSERT_EQ(30000u, next_delay());
    fx.fail_timer = true;
    ASSERT(fire_next()); ASSERT(fx.stopped); ASSERT_EQ(0u, timer_count());
    daemon_teardown(&d); PASS();
}

TEST watcher_scheduler_failure_stops_instead_of_losing_recovery(void)
{
    ASSERT(setup());
    on_route_change(&d); /* existing resync means only reconnect needs a timer */
    fx.fail_timer = true;
    notify(0, 0, EPOLLHUP);
    ASSERT(fx.stopped); ASSERT_EQ(1u, timer_count());
    daemon_teardown(&d); ASSERT_EQ(0u, timer_count());
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(lost_and_malformed_events_coalesce_without_periodic_polling);
    RUN_TEST(contention_and_apply_error_retry_then_stop_on_success);
    RUN_TEST(reconnect_backoff_is_bounded_and_success_resynchronizes);
    RUN_TEST(route_retry_delay_caps_and_scheduler_failure_is_not_hidden);
    RUN_TEST(watcher_scheduler_failure_stops_instead_of_losing_recovery);
    GREATEST_MAIN_END();
}
