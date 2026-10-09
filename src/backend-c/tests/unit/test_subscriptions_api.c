/* HTTP-level tests for subscriptions_api.c -- same harness pattern as
 * test_groups.c/test_system.c. */
#include "greatest.h"

#include <arpa/inet.h>
#include <pthread.h>
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
#include "magitrickle/sub_fetch.h"
#include "magitrickle/subscriptions_api.h"
#include "magitrickle/yamlio.h"

typedef struct harness {
    mt_loop_t *loop;
    mt_httpd_t *tcp;
    pthread_t thread;

    /* Separate fixture server; API handlers use the production async fetcher. */
    mt_loop_t *stub_loop;
    mt_httpd_t *stub;
    pthread_t stub_thread;

    mt_config_t cfg;
    mt_cache_t *cache;
    mt_app_t *app;
    mt_subs_ctx_t ctx;
    char *large_list;
    char save_dir[128];
    char save_path[256];
} harness_t;

static void *loop_thread(void *ud) {
    mt_loop_t *loop = ud;
    mt_loop_run(loop);
    return NULL;
}

#define TEST_PORT 18084
#define STUB_PORT 18085

static void h_stub_list(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    const char *body = ud ? ud : "one.example\ntwo.example";
    mt_http_res_write(res, 200, "text/plain", (const uint8_t *)body, strlen(body));
}

static void h_stub_404(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    (void)ud;
    mt_http_res_write_error(res, 404, "nope");
}

static void h_stub_large(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req; const harness_t *h = ud;
    mt_http_res_write(res, 200, "text/plain", (const uint8_t *)h->large_list, strlen(h->large_list));
}

static char g_stub_url[128];
static const char *stub_url(const char *path) {
    snprintf(g_stub_url, sizeof(g_stub_url), "http://127.0.0.1:%d%s", STUB_PORT, path);
    return g_stub_url;
}

static harness_t *harness_start_mode(int persistence) {
    harness_t *h = calloc(1, sizeof(*h));
    if (!h) { return NULL; }
    h->large_list = malloc(50000u * 32u + 1); if (!h->large_list) { free(h); return NULL; }
    size_t used = 0;
    for (unsigned i = 0; i < 50000; i++) {
        int n = snprintf(h->large_list + used, 50000u * 32u + 1 - used,
                         "10.%u.%u.0/24\r\n", i / 256, i % 256);
        if (n < 0) { free(h->large_list); free(h); return NULL; }
        used += (size_t)n;
    }
    mt_config_init_defaults(&h->cfg);
    h->cache = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &h->cfg, .cache = h->cache};
    h->app = mt_app_create(&deps);
    h->ctx.app = h->app;
    h->ctx.config_path = NULL;
    h->ctx.config_version = "0.8.3-test-large";
    if (persistence) {
        snprintf(h->save_dir, sizeof(h->save_dir), "/tmp/mt-large-api-XXXXXX");
        if (!mkdtemp(h->save_dir)) { free(h->large_list); free(h); return NULL; }
        snprintf(h->save_path, sizeof(h->save_path), "%s/%sconfig.yaml", h->save_dir,
                 persistence == 2 ? "absent/" : "");
        h->ctx.config_path = h->save_path;
    }

    if (mt_loop_create(&h->loop) != MT_OK) { return NULL; }
    if (mt_sub_fetcher_create(h->loop, &h->ctx.fetcher) != MT_OK) { return NULL; }
    if (mt_httpd_create(h->loop, &h->tcp) != MT_OK) { return NULL; }
    mt_subs_register_routes(h->tcp, &h->ctx);
    if (mt_httpd_listen_tcp(h->tcp, "127.0.0.1", TEST_PORT) != MT_OK) { return NULL; }
    pthread_create(&h->thread, NULL, loop_thread, h->loop);

    if (mt_loop_create(&h->stub_loop) != MT_OK) { return NULL; }
    if (mt_httpd_create(h->stub_loop, &h->stub) != MT_OK) { return NULL; }
    mt_httpd_route(h->stub, "GET", "/list", h_stub_list, NULL);
    mt_httpd_route(h->stub, "GET", "/list2", h_stub_list, NULL);
    mt_httpd_route(h->stub, "GET", "/list-updated", h_stub_list, "one.example\ntwo.example\nthree.example");
    mt_httpd_route(h->stub, "GET", "/404", h_stub_404, NULL);
    mt_httpd_route(h->stub, "GET", "/large", h_stub_large, h);
    if (mt_httpd_listen_tcp(h->stub, "127.0.0.1", STUB_PORT) != MT_OK) { return NULL; }
    pthread_create(&h->stub_thread, NULL, loop_thread, h->stub_loop);

    return h;
}

static harness_t *harness_start(void) { return harness_start_mode(0); }

static void harness_stop(harness_t *h) {
    mt_loop_stop(h->loop);
    pthread_join(h->thread, NULL);
    mt_sub_fetcher_destroy(h->ctx.fetcher);
    mt_httpd_destroy(h->tcp);
    mt_loop_destroy(h->loop);

    mt_loop_stop(h->stub_loop);
    pthread_join(h->stub_thread, NULL);
    mt_httpd_destroy(h->stub);
    mt_loop_destroy(h->stub_loop);

    mt_app_destroy(h->app);
    mt_cache_destroy(h->cache);
    mt_config_clear(&h->cfg);
    if (h->save_path[0]) { unlink(h->save_path); rmdir(h->save_dir); }
    free(h->large_list); free(h);
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
    struct timeval tv = {15, 0};
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
    const size_t capacity = 16u * 1024u * 1024u;
    char *resp = malloc(capacity);
    if (!resp) { close(fd); return -1; }
    ssize_t got = recv_response(fd, resp, capacity);
    close(fd);
    if (got <= 0) { free(resp); return -1; }
    int status = status_code_of(resp);
    if (out_json) {
        const char *b = body_of(resp);
        *out_json = b[0] ? cJSON_Parse(b) : NULL;
    }
    free(resp); return status;
}

static const char *jstr(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* ---- tests -------------------------------------------------------------------- */

TEST get_subscriptions_starts_empty(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &out));
    cJSON *subs = cJSON_GetObjectItemCaseSensitive(out, "subscriptions");
    ASSERT(cJSON_IsArray(subs));
    ASSERT_EQ(0, cJSON_GetArraySize(subs));
    cJSON_Delete(out);
    harness_stop(h);
    PASS();
}

TEST create_subscription_requires_url(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    ASSERT_EQ(400, do_request("POST", "/api/v1/subscriptions", "{\"name\":\"s1\"}", NULL));
    harness_stop(h);
    PASS();
}

TEST create_subscription_defaults_and_appears_in_list(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    ASSERT_EQ(200,
             do_request("POST", "/api/v1/subscriptions",
                       "{\"name\":\"s1\",\"interface\":\"eth0\",\"url\":\"https://example.com/list.txt\"}",
                       NULL));

    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &out));
    cJSON *subs = cJSON_GetObjectItemCaseSensitive(out, "subscriptions");
    ASSERT_EQ(1, cJSON_GetArraySize(subs));
    cJSON *s = cJSON_GetArrayItem(subs, 0);
    ASSERT_STR_EQ("s1", jstr(s, "name"));
    ASSERT_STR_EQ("https://example.com/list.txt", jstr(s, "url"));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s, "enable"))); /* *bool absent -> default true */
    cJSON *priority = cJSON_GetObjectItemCaseSensitive(s, "priority");
    ASSERT(cJSON_IsNumber(priority));
    ASSERT_EQ(MT_SUBSCRIPTION_DEFAULT_PRIORITY, priority->valueint);
    mt_id_t id; ASSERT_EQ(MT_OK, mt_id_parse(jstr(s, "id"), &id));
    mt_ruleset_t *synth = mt_app_find_subscription_ruleset_by_id(h->app, id);
    ASSERT(synth);
    ASSERT_EQ(MT_SUBSCRIPTION_DEFAULT_PRIORITY, mt_ruleset_group(synth)->priority);
    cJSON *rules = cJSON_GetObjectItemCaseSensitive(s, "rules");
    ASSERT(cJSON_IsArray(rules));
    ASSERT_EQ(0, cJSON_GetArraySize(rules));
    cJSON_Delete(out);
    harness_stop(h);
    PASS();
}

TEST create_subscription_duplicate_id_is_409(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions",
                             "{\"id\":\"aabbccdd\",\"url\":\"https://example.com/a.txt\"}", NULL));
    ASSERT_EQ(409, do_request("POST", "/api/v1/subscriptions",
                             "{\"id\":\"aabbccdd\",\"url\":\"https://example.com/b.txt\"}", NULL));
    harness_stop(h);
    PASS();
}

TEST delete_subscription_removes_it_and_404s_unknown(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions",
                             "{\"id\":\"aabbccdd\",\"url\":\"https://example.com/a.txt\"}", NULL));
    ASSERT_EQ(200, do_request("DELETE", "/api/v1/subscriptions/aabbccdd", NULL, NULL));

    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &out));
    ASSERT_EQ(0, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(out, "subscriptions")));
    cJSON_Delete(out);

    ASSERT_EQ(404, do_request("DELETE", "/api/v1/subscriptions/aabbccdd", NULL, NULL));
    ASSERT_EQ(400, do_request("DELETE", "/api/v1/subscriptions/nothex!", NULL, NULL));
    harness_stop(h);
    PASS();
}

TEST put_subscriptions_missing_key_and_missing_url(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    ASSERT_EQ(400, do_request("PUT", "/api/v1/subscriptions", "{}", NULL));
    ASSERT_EQ(400, do_request("PUT", "/api/v1/subscriptions", "{\"subscriptions\":[{\"name\":\"no-url\"}]}",
                             NULL));
    harness_stop(h);
    PASS();
}

TEST put_subscriptions_bulk_replace_reuses_ids_and_seeds_last_update(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);

    /* CreateSubscription passes existing=nil to SubscriptionFromReq, so
     * (matching Go exactly) any "id" given for a *nested* rule on create
     * is discarded in favor of a fresh random one -- lenient reuse only
     * ever matches against a real *baseline*. So capture the
     * server-assigned rule id from a GET rather than assuming our
     * request-supplied "11223344" survives creation. */
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions",
                             "{\"id\":\"aabbccdd\",\"name\":\"s1\",\"url\":\"https://example.com/a.txt\","
                             "\"lastUpdate\":1700000000,"
                             "\"rules\":[{\"rule\":\"example.com\",\"type\":\"domain\","
                             "\"enable\":true}]}",
                             NULL));
    cJSON *before = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &before));
    cJSON *before_subs = cJSON_GetObjectItemCaseSensitive(before, "subscriptions");
    cJSON *before_rules = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(before_subs, 0), "rules");
    char rule_id[9];
    snprintf(rule_id, sizeof(rule_id), "%s", jstr(cJSON_GetArrayItem(before_rules, 0), "id"));
    cJSON_Delete(before);

    /* PUT references the existing subscription+rule ids (lenient reuse,
     * matching Go's SubscriptionFromReq/SubscriptionRuleFromReq) and adds
     * a brand new one; also seeds lastUpdate from the existing record
     * since the PUT body omits it. */
    char body[512];
    snprintf(body, sizeof(body),
            "{\"subscriptions\":["
            "{\"id\":\"aabbccdd\",\"name\":\"s1-renamed\",\"url\":\"https://example.com/a.txt\","
            "\"rules\":[{\"id\":\"%s\",\"rule\":\"example.com\",\"type\":\"domain\",\"enable\":true}]},"
            "{\"name\":\"s2-new\",\"url\":\"https://example.com/b.txt\"}"
            "]}",
            rule_id);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("PUT", "/api/v1/subscriptions", body, &out));
    ASSERT_STR_EQ("ok", jstr(out, "status"));
    cJSON_Delete(out);

    cJSON *listed = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &listed));
    cJSON *subs = cJSON_GetObjectItemCaseSensitive(listed, "subscriptions");
    ASSERT_EQ(2, cJSON_GetArraySize(subs));
    cJSON *s1 = cJSON_GetArrayItem(subs, 0);
    ASSERT_STR_EQ("aabbccdd", jstr(s1, "id"));
    ASSERT_STR_EQ("s1-renamed", jstr(s1, "name"));
    ASSERT_EQ(1700000000, (long)cJSON_GetObjectItemCaseSensitive(s1, "lastUpdate")->valuedouble);
    cJSON *s1_rules = cJSON_GetObjectItemCaseSensitive(s1, "rules");
    ASSERT_EQ(1, cJSON_GetArraySize(s1_rules));
    ASSERT_STR_EQ(rule_id, jstr(cJSON_GetArrayItem(s1_rules, 0), "id"));
    ASSERT_STR_EQ("s2-new", jstr(cJSON_GetArrayItem(subs, 1), "name"));
    cJSON_Delete(listed);

    harness_stop(h);
    PASS();
}

/* ---- fetch-backed endpoints (Phase 7) ------------------------------------------ */

TEST sync_unknown_subscription_is_404(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    ASSERT_EQ(404, do_request("POST", "/api/v1/subscriptions/aabbccdd/sync", NULL, NULL));
    ASSERT_EQ(400, do_request("POST", "/api/v1/subscriptions/nothex!/sync", NULL, NULL));
    harness_stop(h);
    PASS();
}

TEST sync_without_body_uses_subscription_url(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    char body[256];
    snprintf(body, sizeof(body), "{\"id\":\"aabbccdd\",\"url\":\"%s\"}", stub_url("/list"));
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions", body, NULL));

    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions/aabbccdd/sync", NULL, &out));
    cJSON *rules = cJSON_GetObjectItemCaseSensitive(out, "rules");
    ASSERT_EQ(2, cJSON_GetArraySize(rules));
    ASSERT_STR_EQ("one.example", jstr(cJSON_GetArrayItem(rules, 0), "rule"));
    ASSERT_STR_EQ(stub_url("/list"), jstr(out, "url"));
    cJSON_Delete(out);

    cJSON *listed = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &listed));
    cJSON *sub = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(listed, "subscriptions"), 0);
    ASSERT_EQ(2, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(sub, "rules")));
    cJSON_Delete(listed);

    harness_stop(h);
    PASS();
}

TEST sync_with_body_url_overrides_and_updates_subscription(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    char body[256];
    snprintf(body, sizeof(body), "{\"id\":\"aabbccdd\",\"url\":\"%s\"}", stub_url("/list"));
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions", body, NULL));

    char sync_body[256];
    snprintf(sync_body, sizeof(sync_body), "{\"url\":\"%s\"}", stub_url("/list2"));
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions/aabbccdd/sync", sync_body, &out));
    ASSERT_STR_EQ(stub_url("/list2"), jstr(out, "url"));
    cJSON_Delete(out);

    cJSON *listed = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &listed));
    cJSON *sub = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(listed, "subscriptions"), 0);
    ASSERT_STR_EQ(stub_url("/list2"), jstr(sub, "url"));
    cJSON_Delete(listed);

    harness_stop(h);
    PASS();
}

TEST sync_fetch_failure_is_502(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    char body[256];
    snprintf(body, sizeof(body), "{\"id\":\"aabbccdd\",\"url\":\"%s\"}", stub_url("/404"));
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions", body, NULL));
    ASSERT_EQ(502, do_request("POST", "/api/v1/subscriptions/aabbccdd/sync", NULL, NULL));
    harness_stop(h);
    PASS();
}

TEST get_subscription_rules_requires_url(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    ASSERT_EQ(400, do_request("GET", "/api/v1/subscriptions/rules", NULL, NULL));
    harness_stop(h);
    PASS();
}

TEST get_subscription_rules_returns_parsed_rules(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    char path[256];
    snprintf(path, sizeof(path), "/api/v1/subscriptions/rules?url=%s", stub_url("/list"));
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", path, NULL, &out));
    cJSON *rules = cJSON_GetObjectItemCaseSensitive(out, "rules");
    ASSERT_EQ(2, cJSON_GetArraySize(rules));
    ASSERT_STR_EQ("one.example", jstr(cJSON_GetArrayItem(rules, 0), "rule"));
    ASSERT_STR_EQ("two.example", jstr(cJSON_GetArrayItem(rules, 1), "rule"));
    cJSON_Delete(out);
    harness_stop(h);
    PASS();
}

TEST get_subscription_rules_fetch_failure_is_502(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    char path[256];
    snprintf(path, sizeof(path), "/api/v1/subscriptions/rules?url=%s", stub_url("/404"));
    ASSERT_EQ(502, do_request("GET", path, NULL, NULL));
    harness_stop(h);
    PASS();
}


TEST large_summary_creation_sparse_save_and_reload(void) {
    harness_t *h = harness_start_mode(1); ASSERT(h);
    char path[256], body[1024];
    snprintf(path, sizeof(path), "/api/v1/subscriptions/rules?summary=true&url=%s", stub_url("/large"));
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", path, NULL, &out)); ASSERT(out);
    ASSERT_EQ(50000, cJSON_GetObjectItemCaseSensitive(out, "count")->valueint);
    ASSERT(!cJSON_GetObjectItemCaseSensitive(out, "rules"));
    ASSERT_EQ(50000, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(out, "types"), "subnet")->valueint);
    cJSON_Delete(out);
    snprintf(body, sizeof(body), "{\"id\":\"12345678\",\"url\":\"%s\",\"name\":\"large\",\"enable\":false,\"interval\":86400}", stub_url("/large"));
    ASSERT(strlen(body) < 1024);
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions?fetch=true", body, &out)); ASSERT(out);
    cJSON *sub = cJSON_GetObjectItemCaseSensitive(out, "subscription"); ASSERT(sub);
    cJSON *rules = cJSON_GetObjectItemCaseSensitive(sub, "rules"); ASSERT_EQ(50000, cJSON_GetArraySize(rules));
    char rule_id[9]; snprintf(rule_id, sizeof(rule_id), "%s", jstr(cJSON_GetArrayItem(rules, 0), "id"));
    ASSERT_STR_EQ("10.0.0.0/24", jstr(cJSON_GetArrayItem(rules, 0), "rule"));
    cJSON_Delete(out);
    snprintf(body, sizeof(body), "{\"subscriptions\":[{\"id\":\"12345678\",\"url\":\"%s\",\"name\":\"edited\",\"enable\":false,\"interval\":86400,\"ruleChanges\":[{\"id\":\"%s\",\"rule\":\"10.0.0.0/24\",\"previousType\":\"subnet\",\"previousEnable\":true,\"type\":\"subnet\",\"enable\":false}]}]}", stub_url("/large"), rule_id);
    ASSERT(strlen(body) < 1024);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/subscriptions", body, NULL));
    /* A stale repeat is rejected, not silently applied to a refreshed baseline. */
    ASSERT_EQ(409, do_request("PUT", "/api/v1/subscriptions", body, NULL));
    ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &out)); ASSERT(out);
    sub = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "subscriptions"), 0);
    rules = cJSON_GetObjectItemCaseSensitive(sub, "rules"); ASSERT_EQ(50000, cJSON_GetArraySize(rules));
    ASSERT_STR_EQ(rule_id, jstr(cJSON_GetArrayItem(rules, 0), "id"));
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rules, 0), "enable")));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rules, 1), "enable")));
    cJSON_Delete(out);
    mt_config_t reloaded; ASSERT_EQ(MT_OK, mt_config_init_defaults(&reloaded));
    ASSERT_EQ(MT_OK, mt_config_load_file(&reloaded, h->save_path));
    ASSERT_EQ(1u, reloaded.n_subscriptions); ASSERT_EQ(50000u, reloaded.subscriptions[0]->n_rules);
    ASSERT_STR_EQ("edited", reloaded.subscriptions[0]->name);
    ASSERT(!reloaded.subscriptions[0]->rules[0]->enable);
    ASSERT(reloaded.subscriptions[0]->rules[49999]->enable);
    mt_config_clear(&reloaded);
    harness_stop(h); PASS();
}

TEST url_creation_failure_does_not_leave_an_empty_subscription(void) {
    harness_t *h = harness_start(); ASSERT(h);
    char body[256]; snprintf(body, sizeof(body), "{\"url\":\"%s\"}", stub_url("/404"));
    ASSERT_EQ(502, do_request("POST", "/api/v1/subscriptions?fetch=true", body, NULL));
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &out));
    ASSERT_EQ(0, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(out, "subscriptions")));
    cJSON_Delete(out);
    ASSERT_EQ(400, do_request("POST", "/api/v1/subscriptions?fetch=true", "{\"url\":\"https://example.com\",\"rules\":[]}", NULL));
    harness_stop(h); PASS();
}

TEST disk_failure_is_http_error_and_url_creation_rolls_back(void) {
    harness_t *h = harness_start_mode(2); ASSERT(h);
    char body[256]; snprintf(body, sizeof(body), "{\"url\":\"%s\"}", stub_url("/list"));
    ASSERT_EQ(500, do_request("POST", "/api/v1/subscriptions?fetch=true", body, NULL));
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &out));
    ASSERT_EQ(0, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(out, "subscriptions")));
    cJSON_Delete(out);
    /* Legacy mutation must no longer report 200 when persistence failed. */
    ASSERT_EQ(500, do_request("POST", "/api/v1/subscriptions", body, NULL));
    harness_stop(h); PASS();
}

TEST sparse_update_rejects_deleted_subscription_and_unknown_rule_id(void) {
    harness_t *h = harness_start(); ASSERT(h);
    const char *missing = "{\"subscriptions\":[{\"id\":\"aabbccdd\",\"url\":\"https://example.com/list\",\"ruleChanges\":[]}]}";
    ASSERT_EQ(409, do_request("PUT", "/api/v1/subscriptions", missing, NULL));
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions", "{\"id\":\"aabbccdd\",\"url\":\"https://example.com/list\"}", NULL));
    ASSERT_EQ(409, do_request("PUT", "/api/v1/subscriptions", "{\"subscriptions\":[{\"id\":\"aabbccdd\",\"url\":\"https://example.com/list\",\"ruleChanges\":[{\"id\":\"11223344\",\"rule\":\"one.example\",\"previousType\":\"domain\",\"previousEnable\":true,\"type\":\"domain\",\"enable\":false}]}]}", NULL));
    harness_stop(h); PASS();
}

TEST persistence_failure_returns_applied_baseline_for_retry(void) {
    harness_t *h = harness_start_mode(1); ASSERT(h);
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions",
        "{\"id\":\"aabbccdd\",\"url\":\"https://example.com/list\",\"rules\":[{\"id\":\"11223344\",\"rule\":\"one.example\",\"type\":\"domain\",\"enable\":true}]}", NULL));
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &out));
    cJSON *initial_sub = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "subscriptions"), 0);
    cJSON *initial_rule = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(initial_sub, "rules"), 0);
    char rule_id[9]; snprintf(rule_id, sizeof(rule_id), "%s", jstr(initial_rule, "id"));
    cJSON_Delete(out); out = NULL;
    ASSERT_EQ(0, unlink(h->save_path)); ASSERT_EQ(0, rmdir(h->save_dir));
    char change[512];
    snprintf(change, sizeof(change), "{\"subscriptions\":[{\"id\":\"aabbccdd\",\"url\":\"https://example.com/list\",\"ruleChanges\":[{\"id\":\"%s\",\"rule\":\"one.example\",\"previousType\":\"domain\",\"previousEnable\":true,\"type\":\"namespace\",\"enable\":false}]}]}", rule_id);
    ASSERT_EQ(500, do_request("PUT", "/api/v1/subscriptions", change, &out));
    ASSERT_STR_EQ("PERSISTENCE_FAILED", jstr(out, "code"));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out, "applied")));
    cJSON *sub = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "subscriptions"), 0);
    cJSON *rule = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(sub, "rules"), 0);
    ASSERT_STR_EQ(rule_id, jstr(rule, "id")); ASSERT_STR_EQ("namespace", jstr(rule, "type"));
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(rule, "enable")));
    cJSON_DeleteItemFromObjectCaseSensitive(out, "error");
    cJSON_DeleteItemFromObjectCaseSensitive(out, "code");
    cJSON_DeleteItemFromObjectCaseSensitive(out, "applied");
    cJSON_DeleteItemFromObjectCaseSensitive(sub, "rules");
    cJSON_AddArrayToObject(sub, "ruleChanges");
    char *retry = cJSON_PrintUnformatted(out); ASSERT(retry); cJSON_Delete(out);
    /* Preconditions stay strict: it is the editor's live baseline that moves. */
    ASSERT_EQ(409, do_request("PUT", "/api/v1/subscriptions", change, &out));
    ASSERT(!cJSON_HasObjectItem(out, "applied")); cJSON_Delete(out);
    ASSERT_EQ(500, do_request("PUT", "/api/v1/subscriptions", retry, &out));
    ASSERT_STR_EQ("PERSISTENCE_FAILED", jstr(out, "code")); cJSON_Delete(out);
    ASSERT_EQ(0, mkdir(h->save_dir, 0700));
    ASSERT_EQ(200, do_request("PUT", "/api/v1/subscriptions", retry, NULL)); free(retry);
    mt_config_t loaded; mt_config_init_defaults(&loaded);
    ASSERT_EQ(MT_OK, mt_config_load_file(&loaded, h->save_path));
    ASSERT_EQ(1, loaded.n_subscriptions); ASSERT_EQ(1, loaded.subscriptions[0]->n_rules);
    ASSERT_STR_EQ("namespace", loaded.subscriptions[0]->rules[0]->type);
    ASSERT(!loaded.subscriptions[0]->rules[0]->enable);
    mt_config_clear(&loaded); harness_stop(h); PASS();
}

TEST subscription_priority_survives_fetch_sync_compact_save_and_reload(void) {
    harness_t *h = harness_start_mode(1); ASSERT(h);
    char body[512];
    snprintf(body, sizeof(body),
        "{\"id\":\"aabbccdd\",\"name\":\"priority-sub\",\"url\":\"%s\",\"priority\":1000}",
        stub_url("/list"));
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions?fetch=true", body, &out));
    cJSON *sub = cJSON_GetObjectItemCaseSensitive(out, "subscription");
    ASSERT_EQ(1000, cJSON_GetObjectItemCaseSensitive(sub, "priority")->valueint);
    ASSERT_EQ(2, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(sub, "rules")));
    cJSON_Delete(out);
    mt_id_t id; ASSERT_EQ(MT_OK, mt_id_parse("aabbccdd", &id));
    mt_ruleset_t *synth = mt_app_find_subscription_ruleset_by_id(h->app, id);
    ASSERT(synth);
    ASSERT_EQ(1000, mt_ruleset_group(synth)->priority);
    /* The new source adds a rule, forcing sync to rebuild the synthetic
     * ruleset rather than just touching the last-check timestamp. */
    snprintf(body, sizeof(body), "{\"url\":\"%s\"}", stub_url("/list-updated"));
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions/aabbccdd/sync", body, NULL));
    synth = mt_app_find_subscription_ruleset_by_id(h->app, id);
    ASSERT(synth);
    ASSERT_EQ(1000, mt_ruleset_group(synth)->priority);
    ASSERT_EQ(3, mt_ruleset_group(synth)->n_rules);
    snprintf(body, sizeof(body), "{\"subscriptions\":[{\"id\":\"aabbccdd\","
        "\"name\":\"renamed\",\"url\":\"%s\",\"ruleChanges\":[]}]}", stub_url("/list-updated"));
    ASSERT_EQ(200, do_request("PUT", "/api/v1/subscriptions", body, NULL));
    synth = mt_app_find_subscription_ruleset_by_id(h->app, id);
    ASSERT(synth);
    ASSERT_EQ(1000, mt_ruleset_group(synth)->priority);
    ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &out));
    sub = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "subscriptions"), 0);
    ASSERT_EQ(1000, cJSON_GetObjectItemCaseSensitive(sub, "priority")->valueint);
    ASSERT_EQ(3, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(sub, "rules")));
    cJSON_Delete(out);
    snprintf(body, sizeof(body), "{\"subscriptions\":[{\"id\":\"aabbccdd\","
        "\"name\":\"renamed\",\"url\":\"%s\",\"priority\":1,\"ruleChanges\":[]}]}", stub_url("/list-updated"));
    ASSERT_EQ(200, do_request("PUT", "/api/v1/subscriptions", body, NULL));
    synth = mt_app_find_subscription_ruleset_by_id(h->app, id);
    ASSERT(synth);
    ASSERT_EQ(1, mt_ruleset_group(synth)->priority);
    mt_config_t loaded; ASSERT_EQ(MT_OK, mt_config_init_defaults(&loaded));
    ASSERT_EQ(MT_OK, mt_config_load_file(&loaded, h->save_path));
    ASSERT_EQ(1, loaded.n_subscriptions);
    ASSERT_EQ(1, loaded.subscriptions[0]->priority);
    ASSERT_EQ(3, loaded.subscriptions[0]->n_rules);
    mt_config_clear(&loaded); harness_stop(h); PASS();
}

TEST invalid_subscription_priorities_do_not_change_live_or_persisted_state(void) {
    harness_t *h = harness_start_mode(1); ASSERT(h);
    ASSERT_EQ(200, do_request("POST", "/api/v1/subscriptions",
        "{\"id\":\"aabbccdd\",\"name\":\"original\",\"url\":\"https://example.com/list\",\"priority\":222}", NULL));
    const char *invalid[] = {
        "0", "-1", "1001", "1.5", "1e309", "null", "true", "\"100\"", "{}", "[]"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char body[512];
        snprintf(body, sizeof(body), "{\"name\":\"invalid\",\"url\":\"https://example.com/list\",\"priority\":%s}", invalid[i]);
        ASSERT_EQ(400, do_request("POST", "/api/v1/subscriptions", body, NULL));
        /* Invalid settings must be rejected before any fetch is submitted. */
        ASSERT_EQ(400, do_request("POST", "/api/v1/subscriptions?fetch=true", body, NULL));
        snprintf(body, sizeof(body), "{\"subscriptions\":["
            "{\"id\":\"aabbccdd\",\"name\":\"would-change\",\"url\":\"https://example.com/list\",\"priority\":1},"
            "{\"id\":\"bbbb0001\",\"url\":\"https://example.com/list\",\"priority\":%s}]}", invalid[i]);
        ASSERT_EQ(400, do_request("PUT", "/api/v1/subscriptions", body, NULL));
        cJSON *out = NULL;
        ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &out));
        cJSON *subs = cJSON_GetObjectItemCaseSensitive(out, "subscriptions");
        ASSERT_EQ(1, cJSON_GetArraySize(subs));
        cJSON *sub = cJSON_GetArrayItem(subs, 0);
        ASSERT_STR_EQ("original", jstr(sub, "name"));
        ASSERT_EQ(222, cJSON_GetObjectItemCaseSensitive(sub, "priority")->valueint);
        cJSON_Delete(out);
    }
    mt_config_t loaded; ASSERT_EQ(MT_OK, mt_config_init_defaults(&loaded));
    ASSERT_EQ(MT_OK, mt_config_load_file(&loaded, h->save_path));
    ASSERT_EQ(1, loaded.n_subscriptions);
    ASSERT_EQ(222, loaded.subscriptions[0]->priority);
    ASSERT_STR_EQ("original", loaded.subscriptions[0]->name);
    mt_config_clear(&loaded); harness_stop(h); PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    mt_sub_fetch_global_init();
    GREATEST_MAIN_BEGIN();
    RUN_TEST(persistence_failure_returns_applied_baseline_for_retry);
    RUN_TEST(subscription_priority_survives_fetch_sync_compact_save_and_reload);
    RUN_TEST(invalid_subscription_priorities_do_not_change_live_or_persisted_state);
    RUN_TEST(large_summary_creation_sparse_save_and_reload);
    RUN_TEST(url_creation_failure_does_not_leave_an_empty_subscription);
    RUN_TEST(disk_failure_is_http_error_and_url_creation_rolls_back);
    RUN_TEST(sparse_update_rejects_deleted_subscription_and_unknown_rule_id);
    RUN_TEST(get_subscriptions_starts_empty);
    RUN_TEST(create_subscription_requires_url);
    RUN_TEST(create_subscription_defaults_and_appears_in_list);
    RUN_TEST(create_subscription_duplicate_id_is_409);
    RUN_TEST(delete_subscription_removes_it_and_404s_unknown);
    RUN_TEST(put_subscriptions_missing_key_and_missing_url);
    RUN_TEST(put_subscriptions_bulk_replace_reuses_ids_and_seeds_last_update);
    RUN_TEST(sync_unknown_subscription_is_404);
    RUN_TEST(sync_without_body_uses_subscription_url);
    RUN_TEST(sync_with_body_url_overrides_and_updates_subscription);
    RUN_TEST(sync_fetch_failure_is_502);
    RUN_TEST(get_subscription_rules_requires_url);
    RUN_TEST(get_subscription_rules_returns_parsed_rules);
    RUN_TEST(get_subscription_rules_fetch_failure_is_502);
    GREATEST_MAIN_END();
}
