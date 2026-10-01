/* Regression sequences from the Go -> C review; exercise production code,
 * not copied implementations. Run under ASan/UBSan as well as make test. */
#include "greatest.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "magitrickle/app.h"
#include "magitrickle/dnsproxy.h"
#include "magitrickle/httpd.h"
#include "magitrickle/sub_fetch.h"

static mt_subscription_t *new_sub(unsigned id) {
    mt_subscription_t *s = mt_subscription_new();
    if (!s) { return NULL; }
    s->id.b[0] = (uint8_t)(id >> 24); s->id.b[1] = (uint8_t)(id >> 16);
    s->id.b[2] = (uint8_t)(id >> 8); s->id.b[3] = (uint8_t)id;
    mt_strset(&s->name, "regression"); mt_strset(&s->iface, "");
    mt_strset(&s->url, "http://127.0.0.1:18903/list");
    return s;
}

TEST replace_then_append_at_small_and_non_power_of_two_sizes(void) {
    const size_t sizes[] = {0, 1, 2, 3, 7, 8, 9, 15, 17, 31, 33, 65};
    mt_config_t cfg;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    mt_app_deps_t deps = {.cfg = &cfg};
    mt_app_t *app = mt_app_create(&deps);
    ASSERT(app);
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        size_t n = sizes[k];
        mt_subscription_t **subs = n ? calloc(n, sizeof(*subs)) : NULL;
        ASSERT(n == 0 || subs);
        for (size_t i = 0; i < n; i++) { subs[i] = new_sub((unsigned)i + 1); ASSERT(subs[i]); }
        ASSERT_EQ(MT_OK, mt_app_replace_subscriptions(app, subs, n));
        for (unsigned i = 0; i < 10; i++) {
            ASSERT_EQ(MT_OK, mt_app_add_subscription(app, new_sub(1000 + i)));
        }
        ASSERT_EQ(n + 10, mt_app_subscription_count(app));
    }
    mt_app_destroy(app); mt_config_clear(&cfg);
    PASS();
}

static int fd_count(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) { return -1; }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) { if (e->d_name[0] != '.') { n++; } }
    closedir(d);
    return n;
}
static void stop_loop(mt_loop_t *loop, void *ud) { (void)ud; mt_loop_stop(loop); }
struct remove_ctx { int fds[2]; unsigned hits; };
static void remove_ready_peer(mt_loop_t *loop, int fd, uint32_t events, void *ud) {
    (void)fd; (void)events;
    struct remove_ctx *ctx = ud;
    ctx->hits++;
    (void)mt_loop_del_fd(loop, ctx->fds[0]);
    (void)mt_loop_del_fd(loop, ctx->fds[1]);
}
TEST deleting_a_watch_invalidates_events_already_in_the_ready_batch(void) {
    for (int attempt = 0; attempt < 20; attempt++) {
        mt_loop_t *loop;
        ASSERT_EQ(MT_OK, mt_loop_create(&loop));
        int a[2], b[2]; ASSERT_EQ(0, pipe(a)); ASSERT_EQ(0, pipe(b));
        struct remove_ctx ctx = {.fds = {a[0], b[0]}};
        ASSERT_EQ(MT_OK, mt_loop_add_fd(loop, a[0], EPOLLIN, remove_ready_peer, &ctx));
        ASSERT_EQ(MT_OK, mt_loop_add_fd(loop, b[0], EPOLLIN, remove_ready_peer, &ctx));
        ASSERT_EQ(1, write(a[1], "a", 1)); ASSERT_EQ(1, write(b[1], "b", 1));
        ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 5, 0, stop_loop, NULL, NULL));
        ASSERT_EQ(MT_OK, mt_loop_run(loop));
        ASSERT_EQ(1u, ctx.hits);
        close(a[0]); close(a[1]); close(b[0]); close(b[1]);
        mt_loop_destroy(loop);
    }
    PASS();
}
static void count_timer(mt_loop_t *loop, void *ud) { (void)loop; (*(unsigned *)ud)++; }
TEST expired_oneshots_release_descriptors_before_loop_destroy(void) {
    mt_loop_t *loop;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    int baseline = fd_count(); ASSERT(baseline >= 0);
    unsigned fired = 0;
    int ids[128];
    for (size_t i = 0; i < 128; i++) {
        ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 1, 0, count_timer, &fired, &ids[i]));
    }
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 20, 0, stop_loop, NULL, NULL));
    ASSERT_EQ(MT_OK, mt_loop_run(loop));
    ASSERT_EQ(128u, fired);
    ASSERT_EQ(baseline, fd_count());
    for (size_t i = 0; i < 128; i++) { ASSERT_EQ(MT_ERR_NOENT, mt_loop_del_timer(loop, ids[i])); }
    mt_loop_destroy(loop);
    PASS();
}

static int tcp_connect(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return -1; }
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
    return fd;
}
static mt_dnsproxy_config_t dns_config(uint16_t port) {
    mt_dnsproxy_config_t c = {.listen_addr = "127.0.0.1", .listen_port = port,
        .upstream_addr = "127.0.0.1", .upstream_port = 9, .timeout_ms = 30,
        .max_concurrent = 2, .max_idle_conns = 1};
    return c;
}
struct dns_ctx { mt_dnsproxy_t *proxy; int udp; uint64_t held; bool replied; };
static void check_slots(mt_loop_t *loop, void *ud) {
    (void)loop; struct dns_ctx *ctx = ud; ctx->held = mt_dnsproxy_inflight(ctx->proxy);
}
static void send_ptr(mt_loop_t *loop, void *ud) {
    (void)loop; struct dns_ctx *ctx = ud;
    static const uint8_t query[] = {0x12,0x34,1,0,0,1,0,0,0,0,0,0,1,'x',0,0,12,0,1};
    (void)send(ctx->udp, query, sizeof(query), 0);
}
static void ptr_reply(mt_loop_t *loop, int fd, uint32_t events, void *ud) {
    (void)events; struct dns_ctx *ctx = ud;
    uint8_t reply[512]; ssize_t n = recv(fd, reply, sizeof(reply), 0);
    ctx->replied = n >= 12 && reply[0] == 0x12 && reply[1] == 0x34 && (reply[3] & 15) == 3;
    mt_loop_stop(loop);
}
TEST partial_tcp_requests_expire_and_udp_recovers(void) {
    mt_loop_t *loop; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    mt_dnsproxy_config_t cfg = dns_config(18901);
    mt_dnsproxy_t *proxy; ASSERT_EQ(MT_OK, mt_dnsproxy_create(&cfg, loop, NULL, NULL, &proxy));
    ASSERT_EQ(MT_OK, mt_dnsproxy_start(proxy));
    int a = tcp_connect(18901), b = tcp_connect(18901); ASSERT(a >= 0 && b >= 0);
    ASSERT_EQ(1, send(a, "\0", 1, 0)); /* incomplete length */
    const uint8_t partial[] = {0, 20, 1};
    ASSERT_EQ(3, send(b, partial, sizeof(partial), 0)); /* incomplete body */
    int udp = socket(AF_INET, SOCK_DGRAM, 0); ASSERT(udp >= 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(18901)};
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    ASSERT_EQ(0, connect(udp, (struct sockaddr *)&sa, sizeof(sa)));
    struct dns_ctx ctx = {.proxy = proxy, .udp = udp};
    ASSERT_EQ(MT_OK, mt_loop_add_fd(loop, udp, EPOLLIN, ptr_reply, &ctx));
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 5, 0, check_slots, &ctx, NULL));
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 100, 0, send_ptr, &ctx, NULL));
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 500, 0, stop_loop, NULL, NULL));
    ASSERT_EQ(MT_OK, mt_loop_run(loop));
    ASSERT_EQ(2u, ctx.held); ASSERT(ctx.replied);
    ASSERT_EQ(0u, mt_dnsproxy_inflight(proxy));
    char byte; ASSERT_EQ(0, recv(a, &byte, 1, MSG_DONTWAIT)); ASSERT_EQ(0, recv(b, &byte, 1, MSG_DONTWAIT));
    mt_loop_del_fd(loop, udp); close(udp); close(a); close(b);
    mt_dnsproxy_destroy(proxy); mt_loop_destroy(loop);
    PASS();
}
TEST dns_destroy_closes_active_exchanges_and_owns_startup_addresses(void) {
    int baseline = fd_count();
    mt_loop_t *loop; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    mt_dnsproxy_config_t cfg = dns_config(18902);
    char *listen = strdup("127.0.0.1"), *up = strdup("localhost");
    cfg.listen_addr = listen; cfg.upstream_addr = up; cfg.timeout_ms = 1000;
    mt_dnsproxy_t *proxy; ASSERT_EQ(MT_OK, mt_dnsproxy_create(&cfg, loop, NULL, NULL, &proxy));
    free(listen); free(up);
    ASSERT_EQ(MT_OK, mt_dnsproxy_start(proxy));
    int fd = tcp_connect(18902); ASSERT(fd >= 0);
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 10, 0, stop_loop, NULL, NULL));
    ASSERT_EQ(MT_OK, mt_loop_run(loop)); ASSERT_EQ(1u, mt_dnsproxy_inflight(proxy));
    mt_dnsproxy_destroy(proxy); close(fd); mt_loop_destroy(loop);
    ASSERT_EQ(baseline, fd_count());
    PASS();
}

static bool write_config(const char *path, const char *text) {
    FILE *f = fopen(path, "w"); if (!f) { return false; }
    bool ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}
TEST reload_overlay_save_restart_and_missing_subscriptions(void) {
    mt_config_t cfg; ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    cfg.app.dns_proxy.disable_fake_ptr = true;
    mt_app_deps_t deps = {.cfg = &cfg}; mt_app_t *app = mt_app_create(&deps); ASSERT(app);
    mt_group_t *g = mt_group_new(); ASSERT(g); g->id = mt_id_random();
    mt_strset(&g->name, "preserved"); mt_strset(&g->color, "#ffffff"); mt_strset(&g->iface, "");
    ASSERT_EQ(MT_OK, mt_app_add_group(app, g));
    ASSERT_EQ(MT_OK, mt_app_add_subscription(app, new_sub(1)));
    char path[] = "/tmp/mt-reload-regression-XXXXXX";
    int fd = mkstemp(path); ASSERT(fd >= 0); close(fd);
    ASSERT(write_config(path, "configVersion: 0.1\napp:\n  httpWeb:\n    auth: {enabled: true}\n    skin: changed\n  dnsProxy: {disableDropAAAA: true}\n"));
    ASSERT_EQ(MT_OK, mt_app_reload_config(app, path));
    ASSERT(cfg.app.http_web.auth.enabled); ASSERT_STR_EQ("changed", cfg.app.http_web.skin);
    ASSERT(cfg.app.dns_proxy.disable_drop_aaaa); ASSERT(cfg.app.dns_proxy.disable_fake_ptr);
    ASSERT_EQ(1u, mt_app_user_group_count(app)); ASSERT_EQ(0u, mt_app_subscription_count(app));
    ASSERT_EQ(MT_OK, mt_app_save_config(app, path, "0.1"));
    mt_config_t restart; ASSERT_EQ(MT_OK, mt_config_init_defaults(&restart));
    ASSERT_EQ(MT_OK, mt_config_load_file(&restart, path));
    ASSERT(restart.app.http_web.auth.enabled); ASSERT(restart.app.dns_proxy.disable_drop_aaaa);
    ASSERT_STR_EQ("changed", restart.app.http_web.skin); mt_config_clear(&restart);
    ASSERT(write_config(path, "app: [bad\n"));
    ASSERT(mt_app_reload_config(app, path) != MT_OK);
    ASSERT(cfg.app.http_web.auth.enabled); ASSERT_STR_EQ("changed", cfg.app.http_web.skin);
    ASSERT_EQ(MT_OK, mt_app_add_subscription(app, new_sub(2)));
    ASSERT(write_config(path, "configVersion: 0.1\nsubscriptions: null\n"));
    ASSERT_EQ(MT_OK, mt_app_reload_config(app, path)); ASSERT_EQ(0u, mt_app_subscription_count(app));
    ASSERT_EQ(1u, mt_app_user_group_count(app)); ASSERT(cfg.app.http_web.auth.enabled);
    unlink(path); mt_app_destroy(app); mt_config_clear(&cfg);
    PASS();
}

struct hostname_probe { bool upstream_seen; bool reply_seen; };
static void hostname_upstream(mt_loop_t *loop, int fd, uint32_t events, void *ud) {
    (void)loop; (void)events;
    struct hostname_probe *probe = ud;
    uint8_t msg[512]; struct sockaddr_storage peer; socklen_t plen = sizeof(peer);
    ssize_t n = recvfrom(fd, msg, sizeof(msg), 0, (struct sockaddr *)&peer, &plen);
    if (n < 12) { return; }
    probe->upstream_seen = true;
    msg[2] |= 0x80; msg[3] |= 0x80; /* valid empty successful answer */
    (void)sendto(fd, msg, (size_t)n, 0, (struct sockaddr *)&peer, plen);
}
static void hostname_reply(mt_loop_t *loop, int fd, uint32_t events, void *ud) {
    (void)events; struct hostname_probe *probe = ud;
    uint8_t msg[512]; ssize_t n = recv(fd, msg, sizeof(msg), 0);
    probe->reply_seen = n >= 12 && msg[0] == 0x56 && msg[1] == 0x78 && (msg[2] & 0x80);
    mt_loop_stop(loop);
}
TEST hostname_upstream_serves_a_real_dns_exchange(void) {
    mt_loop_t *loop; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    /* Dual stack stub accepts whichever localhost address libc selects. */
    int upstream = socket(AF_INET6, SOCK_DGRAM, 0); ASSERT(upstream >= 0);
    int zero = 0; ASSERT_EQ(0, setsockopt(upstream, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero)));
    struct sockaddr_in6 up = {.sin6_family = AF_INET6};
    ASSERT_EQ(0, bind(upstream, (struct sockaddr *)&up, sizeof(up)));
    socklen_t ulen = sizeof(up); ASSERT_EQ(0, getsockname(upstream, (struct sockaddr *)&up, &ulen));
    mt_dnsproxy_config_t cfg = dns_config(18904);
    cfg.upstream_addr = "localhost"; cfg.upstream_port = ntohs(up.sin6_port); cfg.timeout_ms = 300;
    mt_dnsproxy_t *proxy; ASSERT_EQ(MT_OK, mt_dnsproxy_create(&cfg, loop, NULL, NULL, &proxy));
    ASSERT_EQ(MT_OK, mt_dnsproxy_start(proxy));
    int client = socket(AF_INET, SOCK_DGRAM, 0); ASSERT(client >= 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(18904)};
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    ASSERT_EQ(0, connect(client, (struct sockaddr *)&sa, sizeof(sa)));
    struct hostname_probe probe = {0};
    ASSERT_EQ(MT_OK, mt_loop_add_fd(loop, upstream, EPOLLIN, hostname_upstream, &probe));
    ASSERT_EQ(MT_OK, mt_loop_add_fd(loop, client, EPOLLIN, hostname_reply, &probe));
    static const uint8_t query[] = {0x56,0x78,1,0,0,1,0,0,0,0,0,0,1,'x',0,0,1,0,1};
    ASSERT_EQ((ssize_t)sizeof(query), send(client, query, sizeof(query), 0));
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 1000, 0, stop_loop, NULL, NULL));
    ASSERT_EQ(MT_OK, mt_loop_run(loop)); ASSERT(probe.upstream_seen); ASSERT(probe.reply_seen);
    mt_loop_del_fd(loop, upstream); mt_loop_del_fd(loop, client); close(upstream); close(client);
    mt_dnsproxy_destroy(proxy); mt_loop_destroy(loop); PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(replace_then_append_at_small_and_non_power_of_two_sizes);
    RUN_TEST(deleting_a_watch_invalidates_events_already_in_the_ready_batch);
    RUN_TEST(expired_oneshots_release_descriptors_before_loop_destroy);
    RUN_TEST(partial_tcp_requests_expire_and_udp_recovers);
    RUN_TEST(dns_destroy_closes_active_exchanges_and_owns_startup_addresses);
    RUN_TEST(reload_overlay_save_restart_and_missing_subscriptions);
    RUN_TEST(hostname_upstream_serves_a_real_dns_exchange);
    GREATEST_MAIN_END();
}
