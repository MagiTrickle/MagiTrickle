/* Policy API contract over real loopback HTTP; detached RCI transport is faked. */
#include "greatest.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cjson/cJSON.h>

#include "magitrickle/app.h"
#include "magitrickle/dns_cache.h"
#include "magitrickle/loop.h"
#include "magitrickle/system.h"
#include "magitrickle/groups.h"
#include "magitrickle/subscriptions_api.h"

#ifdef MT_ENTWARE_KN
#include "magitrickle/keenetic_policy_api.h"
static atomic_int fetch_calls, fetch_mode;
static atomic_bool hold_fetch;
static mt_err_t mock_fetch(mt_kn_policy_entry_t **entries, size_t *count, const atomic_bool *stop) {
    atomic_fetch_add(&fetch_calls, 1);
    for (int i = 0; i < 100 && atomic_load(&hold_fetch) && !atomic_load(stop); i++) {
        struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL);
    }
    int mode = atomic_load(&fetch_mode);
    if (mode == 1) { *entries = NULL; *count = 0; return MT_ERR_UPSTREAM; }
    const char *json = mode == 2 ? "{}" :
      "{\"Policy0\":{\"description\":\"VPN\",\"mark\":\"ffffd00\"},\"Waiting\":{}}";
    return mt_kn_policy_parse(json, strlen(json), entries, count);
}
#endif

typedef struct harness {
#ifdef MT_ENTWARE_KN
    mt_kn_policy_service_t *service;
#endif
    mt_loop_t *loop;
    mt_httpd_t *tcp;
    mt_config_t cfg;
    mt_cache_t *cache;
    mt_app_t *app;
    mt_system_ctx_t ctx;
    mt_groups_ctx_t groups;
    mt_subs_ctx_t subs;
    char config_path[64];
    pthread_t thread;
} harness_t;

static void *loop_thread(void *ud) {
    harness_t *h = ud;
    mt_loop_run(h->loop);
    return NULL;
}

#define TEST_PORT 18129

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
#ifdef MT_ENTWARE_KN
    atomic_store(&fetch_calls, 0); atomic_store(&fetch_mode, 0); atomic_store(&hold_fetch, false);
    if (mt_kn_policy_service_create(h->loop, h->app, h->ctx.config_path, "0.8", mock_fetch, &h->service) != MT_OK) { return NULL; }
    if (mt_kn_policy_register_routes(h->tcp, h->service) != MT_OK) { return NULL; }
#endif
    mt_system_register_routes(h->tcp, &h->ctx);
    h->groups.app = h->app; h->subs.app = h->app;
    mt_groups_register_routes(h->tcp, &h->groups);
    mt_subs_register_routes(h->tcp, &h->subs);
    if (mt_httpd_listen_tcp(h->tcp, "127.0.0.1", TEST_PORT) != MT_OK) { return NULL; }

    pthread_create(&h->thread, NULL, loop_thread, h);
    return h;
}

static void harness_stop(harness_t *h) {
    mt_loop_stop(h->loop);
    pthread_join(h->thread, NULL);
#ifdef MT_ENTWARE_KN
    mt_kn_policy_service_destroy(h->service);
#endif
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



TEST generic_has_no_policy_routes(void) {
#ifndef MT_ENTWARE_KN
    harness_t *h = harness_start(false); ASSERT(h);
    ASSERT_EQ(404, do_request("GET", "/api/v1/keenetic/policy", NULL, NULL));
    ASSERT_EQ(404, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    ASSERT_EQ(404, do_request("PUT", "/api/v1/keenetic/policy", "{}", NULL));
    harness_stop(h);
#endif
    PASS();
}
#ifdef MT_ENTWARE_KN
static const char *include = "{\"enabled\":true,\"policy_id\":\"Policy0\",\"mode\":\"include\"}";
static const char *exclude = "{\"enabled\":true,\"policy_id\":\"Policy0\",\"mode\":\"exclude\"}";
static const char *disabled = "{\"enabled\":false,\"policy_id\":\"Policy0\",\"mode\":\"exclude\"}";
static cJSON *settings(cJSON *out) { return cJSON_GetObjectItemCaseSensitive(out, "settings"); }
static const char *state(cJSON *out) {
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(out, "runtime"), "state")->valuestring;
}
TEST defaults_no_rci_and_invalid_requests_have_no_side_effects(void) {
    harness_t *h = harness_start(false); ASSERT(h);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings(out), "enabled")));
    ASSERT_STR_EQ("disabled", state(out)); cJSON_Delete(out);
    const char *bad[] = {"{}", "[]", "{\"enabled\":1,\"policy_id\":\"Policy0\",\"mode\":\"include\"}",
        "{\"enabled\":true,\"policy_id\":\"\",\"mode\":\"include\"}",
        "{\"enabled\":true,\"policy_id\":\"Policy0\",\"mode\":\"unknown\"}"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); i++) {
        ASSERT_EQ(400, do_request("PUT", "/api/v1/keenetic/policy", bad[i], NULL));
    }
    ASSERT_EQ(0, atomic_load(&fetch_calls));
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, &out));
    cJSON *list = cJSON_GetObjectItemCaseSensitive(out, "policies"); ASSERT_EQ(2, cJSON_GetArraySize(list));
    ASSERT_STR_EQ("Policy0", cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list,0),"id")->valuestring);
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list,1),"available")));
    ASSERT_FALSE(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list,0),"mark"));
    cJSON_Delete(out); harness_stop(h); PASS();
}
TEST both_modes_persist_and_unavailable_policy_never_falls_back_to_all(void) {
    harness_t *h = harness_start(true); ASSERT(h);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", include, &out));
    ASSERT_STR_EQ("ready", state(out)); cJSON_Delete(out);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", exclude, &out));
    ASSERT_STR_EQ("exclude", cJSON_GetObjectItemCaseSensitive(settings(out),"mode")->valuestring); cJSON_Delete(out);
    atomic_store(&fetch_mode, 1);
    ASSERT_EQ(502, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
    ASSERT_STR_EQ("unavailable", state(out));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings(out),"enabled")));
    ASSERT_STR_EQ("Policy0", cJSON_GetObjectItemCaseSensitive(settings(out),"policy_id")->valuestring); cJSON_Delete(out);
    ASSERT_EQ(502, do_request("PUT", "/api/v1/keenetic/policy", include, NULL));
    atomic_store(&fetch_mode, 2);
    ASSERT_EQ(409, do_request("PUT", "/api/v1/keenetic/policy", include, NULL));
    int calls = atomic_load(&fetch_calls);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", disabled, &out));
    ASSERT_STR_EQ("disabled", state(out)); cJSON_Delete(out);
    ASSERT_EQ(calls, atomic_load(&fetch_calls));
    mt_config_t persisted; ASSERT_EQ(MT_OK, mt_config_init_defaults(&persisted));
    ASSERT_EQ(MT_OK, mt_config_load_file(&persisted, h->config_path));
    ASSERT_FALSE(persisted.app.keenetic_policy.enabled);
    ASSERT_STR_EQ("Policy0", persisted.app.keenetic_policy.policy_id);
    ASSERT_EQ(MT_KN_POLICY_EXCLUDE, persisted.app.keenetic_policy.mode);
    mt_config_clear(&persisted); harness_stop(h); PASS();
}
TEST disable_supersedes_late_activation_without_blocking_the_loop(void) {
    harness_t *h = harness_start(false); ASSERT(h);
    atomic_store(&hold_fetch, true);
    int fd = connect_tcp(TEST_PORT); ASSERT(fd >= 0);
    char req[1024];
    snprintf(req, sizeof(req), "PUT /api/v1/keenetic/policy HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(include), include);
    ASSERT(send(fd, req, strlen(req), 0) > 0);
    for (unsigned i=0; i<50 && atomic_load(&fetch_calls)==0; i++) {
        struct timespec ts={0,1000000}; nanosleep(&ts,NULL);
    }
    ASSERT_EQ(1, atomic_load(&fetch_calls));
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", disabled, &out));
    ASSERT_STR_EQ("disabled", state(out)); cJSON_Delete(out);
    char response[4096]; ASSERT(recv_response(fd, response, sizeof(response)) > 0);
    ASSERT_EQ(409, status_code_of(response)); close(fd);
    atomic_store(&hold_fetch, false);
    /* A list request coalesces with/finishes the same fetch before verification. */
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
    ASSERT_STR_EQ("disabled", state(out)); cJSON_Delete(out);
    harness_stop(h); PASS();
}
TEST persistence_error_acknowledges_memory_state_and_supports_retry(void) {
    harness_t *h = harness_start(true); ASSERT(h);
    ASSERT_EQ(0, unlink(h->config_path));
    ASSERT_EQ(0, mkdir(h->config_path, 0700));
    cJSON *out = NULL;
    ASSERT_EQ(500, do_request("PUT", "/api/v1/keenetic/policy", include, &out));
    ASSERT_STR_EQ("PERSISTENCE_FAILED", cJSON_GetObjectItemCaseSensitive(out,"code")->valuestring);
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out,"applied")));
    ASSERT_STR_EQ("Policy0", cJSON_GetObjectItemCaseSensitive(settings(out),"policy_id")->valuestring);
    cJSON_Delete(out);
    ASSERT_EQ(0, rmdir(h->config_path));
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", include, &out));
    ASSERT_FALSE(cJSON_GetObjectItemCaseSensitive(out,"code")); cJSON_Delete(out);
    harness_stop(h); PASS();
}
#endif
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(generic_has_no_policy_routes);
#ifdef MT_ENTWARE_KN
    RUN_TEST(defaults_no_rci_and_invalid_requests_have_no_side_effects);
    RUN_TEST(both_modes_persist_and_unavailable_policy_never_falls_back_to_all);
    RUN_TEST(disable_supersedes_late_activation_without_blocking_the_loop);
    RUN_TEST(persistence_error_acknowledges_memory_state_and_supports_retry);
#endif
    GREATEST_MAIN_END();
}
