/* HTTP-level tests for system.c -- same harness pattern as
 * test_groups.c/test_httpd.c/test_auth.c. */
#include "greatest.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cjson/cJSON.h>

#include "magitrickle/app.h"
#include "magitrickle/dns_cache.h"
#include "magitrickle/loop.h"
#include "magitrickle/system.h"

typedef struct harness {
    mt_loop_t *loop;
    mt_httpd_t *tcp;
    mt_config_t cfg;
    mt_cache_t *cache;
    mt_app_t *app;
    mt_system_ctx_t ctx;
    char config_path[64];
    pthread_t thread;
} harness_t;

static void *loop_thread(void *ud) {
    harness_t *h = ud;
    mt_loop_run(h->loop);
    return NULL;
}

#define TEST_PORT 18082

static harness_t *harness_start(bool with_config_path) {
    harness_t *h = calloc(1, sizeof(*h));
    mt_config_init_defaults(&h->cfg);
    h->cache = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &h->cfg, .cache = h->cache};
    h->app = mt_app_create(&deps);
    h->ctx.app = h->app;
    if (with_config_path) {
        snprintf(h->config_path, sizeof(h->config_path), "/tmp/mt_system_test_XXXXXX");
        int fd = mkstemp(h->config_path);
        if (fd >= 0) { close(fd); }
        h->ctx.config_path = h->config_path;
        h->ctx.config_version = "0.1";
    }

    if (mt_loop_create(&h->loop) != MT_OK) { return NULL; }
    if (mt_httpd_create(h->loop, &h->tcp) != MT_OK) { return NULL; }
    mt_system_register_routes(h->tcp, &h->ctx);
    if (mt_httpd_listen_tcp(h->tcp, "127.0.0.1", TEST_PORT) != MT_OK) { return NULL; }

    pthread_create(&h->thread, NULL, loop_thread, h);
    return h;
}

static void harness_stop(harness_t *h) {
    mt_loop_stop(h->loop);
    pthread_join(h->thread, NULL);
    mt_httpd_destroy(h->tcp);
    mt_loop_destroy(h->loop);
    mt_app_destroy(h->app);
    mt_cache_destroy(h->cache);
    mt_config_clear(&h->cfg);
    if (h->config_path[0]) { unlink(h->config_path); }
    free(h);
}

/* ---- tiny blocking HTTP/1.1 client (see test_groups.c) ----------------------- */

static int connect_tcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    for (int i = 0; i < 50; i++) {
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) { return fd; }
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
    }
    close(fd);
    return -1;
}

static ssize_t recv_response(int fd, char *buf, size_t cap) {
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    size_t total = 0;
    char *body_start = NULL;
    long content_length = -1;
    for (;;) {
        if (total >= cap - 1) { break; }
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0) { break; }
        total += (size_t)n;
        buf[total] = '\0';
        if (!body_start) {
            char *marker = strstr(buf, "\r\n\r\n");
            if (marker) {
                body_start = marker + 4;
                char *cl = strstr(buf, "Content-Length:");
                if (cl && cl < marker) { content_length = strtol(cl + 15, NULL, 10); }
            }
        }
        if (body_start && content_length >= 0) {
            size_t body_have = (size_t)(buf + total - body_start);
            if ((long)body_have >= content_length) { break; }
        }
    }
    return (ssize_t)total;
}

static int status_code_of(const char *resp) {
    int code = 0;
    sscanf(resp, "HTTP/1.1 %d", &code);
    return code;
}

static const char *body_of(const char *resp) {
    const char *marker = strstr(resp, "\r\n\r\n");
    return marker ? marker + 4 : "";
}

static int do_request(const char *method, const char *path, const char *body, cJSON **out_json) {
    int fd = connect_tcp(TEST_PORT);
    if (fd < 0) { return -1; }
    char req[8192];
    size_t body_len = body ? strlen(body) : 0;
    snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
            method, path, body_len, body ? body : "");
    if (send(fd, req, strlen(req), 0) <= 0) {
        close(fd);
        return -1;
    }
    char resp[16384];
    ssize_t got = recv_response(fd, resp, sizeof(resp));
    close(fd);
    if (got <= 0) { return -1; }
    int status = status_code_of(resp);
    if (out_json) {
        const char *b = body_of(resp);
        *out_json = b[0] ? cJSON_Parse(b) : NULL;
    }
    return status;
}

/* ---- tests -------------------------------------------------------------------- */

TEST list_interfaces_includes_blackhole_first(void) {
    harness_t *h = harness_start(false);
    ASSERT(h != NULL);
    h->cfg.app.show_all_interfaces = true;

    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/interfaces", NULL, &out));
    cJSON *ifaces = cJSON_GetObjectItemCaseSensitive(out, "interfaces");
    ASSERT(cJSON_IsArray(ifaces));
    ASSERT(cJSON_GetArraySize(ifaces) >= 1);
    cJSON *first = cJSON_GetArrayItem(ifaces, 0);
    ASSERT_STR_EQ("blackhole", cJSON_GetObjectItemCaseSensitive(first, "id")->valuestring);
    /* blackhole has no friendly name -> "name" key omitted (omitempty) */
    ASSERT(cJSON_GetObjectItemCaseSensitive(first, "name") == NULL);
    cJSON_Delete(out);
    harness_stop(h);
    PASS();
}

TEST save_config_writes_file(void) {
    harness_t *h = harness_start(true);
    ASSERT(h != NULL);
    ASSERT_EQ(200, do_request("POST", "/api/v1/system/config/save", NULL, NULL));

    mt_config_t reloaded;
    mt_config_init_defaults(&reloaded);
    ASSERT_EQ(MT_OK, mt_config_load_file(&reloaded, h->config_path));
    mt_config_clear(&reloaded);
    harness_stop(h);
    PASS();
}

TEST save_config_noop_without_path(void) {
    harness_t *h = harness_start(false);
    ASSERT(h != NULL);
    ASSERT_EQ(200, do_request("POST", "/api/v1/system/config/save", NULL, NULL));
    harness_stop(h);
    PASS();
}

TEST netfilterd_hook_ok_and_bad_json(void) {
    harness_t *h = harness_start(false);
    ASSERT(h != NULL);
    ASSERT_EQ(200, do_request("POST", "/api/v1/system/hooks/netfilterd",
                             "{\"type\":\"iptables\",\"table\":\"nat\"}", NULL));
    ASSERT_EQ(400, do_request("POST", "/api/v1/system/hooks/netfilterd", "not json", NULL));
    harness_stop(h);
    PASS();
}

/* Mirrors Go's constant.IgnoredInterfaces (empty by default, a fixed
 * Keenetic virtual-interface list under the entware_kn build tag --
 * src/backend/constant/iface-ignore_entware_kn.go). Go's own dedicated
 * entware_kn test (keenetic_router_specific_test.go) only covers the RCI
 * alias lookup, not this list, so there is no Go test to differentially
 * match here either -- this asserts the C side's behavior is correct for
 * whichever mode it was built in. */
TEST ignored_interfaces_list(void) {
#ifdef MT_ENTWARE_KN
    ASSERT(mt_iface_is_ignored_for_test("ra0"));
    ASSERT(mt_iface_is_ignored_for_test("ra15"));
    ASSERT(mt_iface_is_ignored_for_test("ezcfg0"));
    ASSERT(!mt_iface_is_ignored_for_test("br0"));
    ASSERT(!mt_iface_is_ignored_for_test("ra16"));
#else
    ASSERT(!mt_iface_is_ignored_for_test("ra0"));
    ASSERT(!mt_iface_is_ignored_for_test("ezcfg0"));
    ASSERT(!mt_iface_is_ignored_for_test("br0"));
#endif
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(list_interfaces_includes_blackhole_first);
    RUN_TEST(save_config_writes_file);
    RUN_TEST(save_config_noop_without_path);
    RUN_TEST(netfilterd_hook_ok_and_bad_json);
    RUN_TEST(ignored_interfaces_list);
    GREATEST_MAIN_END();
}
