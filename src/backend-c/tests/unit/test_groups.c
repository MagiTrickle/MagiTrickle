/* HTTP-level tests for groups.c -- same background-loop-thread +
 * blocking-client harness pattern as test_httpd.c/test_auth.c. The app
 * under test is never set "running" (mt_app_set_running is never called),
 * so mt_ruleset_enable/disable/sync are all no-ops here regardless of a
 * group's `enable` field -- real-netfilter-backed enable/disable/sync
 * failure handling in these handlers is a thin pass-through of already
 *-tested mt_ruleset_t/mt_app_t return codes (see test_app.c's
 * add_group_while_running_rolls_back_on_failure for the underlying
 * fault-injection test), so it isn't re-tested at the HTTP layer here.
 */
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
#include "magitrickle/dnspipeline.h"
#include "magitrickle/groups.h"
#include "magitrickle/yamlio.h"
#include "magitrickle/lookup.h"
#include "magitrickle/loop.h"

typedef struct captured_match {
    bool matched;
    mt_id_t group_id;
} captured_match_t;

static void capture_match(const mt_match_action_t *action, void *ud) {
    captured_match_t *cap = ud;
    cap->matched = true;
    cap->group_id = action->group_id;
}

typedef struct harness {
    mt_loop_t *loop;
    mt_httpd_t *tcp;
    mt_config_t cfg;
    mt_cache_t *cache;
    mt_dns_pipeline_t *pipeline;
    captured_match_t cap;
    mt_app_t *app;
    mt_groups_ctx_t ctx;
    pthread_t thread;
    char save_dir[64], save_path[96];
} harness_t;

static void *loop_thread(void *ud) {
    harness_t *h = ud;
    mt_loop_run(h->loop);
    return NULL;
}

#define TEST_PORT 18081

static harness_t *harness_start_saved(bool save) {
    harness_t *h = calloc(1, sizeof(*h));
    mt_config_init_defaults(&h->cfg);
    h->cache = mt_cache_create(0);
    h->pipeline = mt_dns_pipeline_create(h->cache, 0, capture_match, &h->cap);
    mt_app_deps_t deps = {.cfg = &h->cfg, .cache = h->cache, .pipeline = h->pipeline};
    h->app = mt_app_create(&deps);
    h->ctx.app = h->app;
    h->ctx.config_path = NULL;
    h->ctx.config_version = "0.8.2.2";
    if (save) {
        snprintf(h->save_dir, sizeof(h->save_dir), "/tmp/mt-groups-XXXXXX");
        if (!mkdtemp(h->save_dir)) { free(h); return NULL; }
        snprintf(h->save_path, sizeof(h->save_path), "%s/config.yaml", h->save_dir);
        h->ctx.config_path = h->save_path;
    }

    if (mt_loop_create(&h->loop) != MT_OK) { return NULL; }
    if (mt_httpd_create(h->loop, &h->tcp) != MT_OK) { return NULL; }
    mt_groups_register_routes(h->tcp, &h->ctx);
    if (mt_httpd_listen_tcp(h->tcp, "127.0.0.1", TEST_PORT) != MT_OK) { return NULL; }

    pthread_create(&h->thread, NULL, loop_thread, h);
    return h;
}

static harness_t *harness_start(void) { return harness_start_saved(false); }

static void harness_stop(harness_t *h) {
    mt_loop_stop(h->loop);
    pthread_join(h->thread, NULL);
    mt_httpd_destroy(h->tcp);
    mt_loop_destroy(h->loop);
    mt_app_destroy(h->app);
    mt_dns_pipeline_destroy(h->pipeline);
    mt_cache_destroy(h->cache);
    mt_config_clear(&h->cfg);
    if (h->save_path[0]) { unlink(h->save_path); rmdir(h->save_dir); }
    free(h);
}

/* Wire-encodes a plain dotted domain (no escaping needed for these tests)
 * as length-prefixed labels terminated by a zero-length root label. */
static size_t wire_name(const char *domain, uint8_t *buf, size_t cap) {
    size_t out = 0;
    const char *label = domain;
    while (*label) {
        const char *dot = strchr(label, '.');
        size_t label_len = dot ? (size_t)(dot - label) : strlen(label);
        if (out + 1 + label_len + 1 > cap) { return 0; }
        buf[out++] = (uint8_t)label_len;
        memcpy(buf + out, label, label_len);
        out += label_len;
        label += label_len;
        if (*label == '.') { label++; }
    }
    if (out + 1 > cap) { return 0; }
    buf[out++] = 0;
    return out;
}

/* ---- tiny blocking HTTP/1.1 client ------------------------------------------ */

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
    struct timeval tv = {30, 0};
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

/* Sends one request over a fresh connection, returns the status code and
 * (if out_json != NULL) the parsed body -- caller owns *out_json via
 * cJSON_Delete. method/path/body may embed a Content-Length-computed body;
 * body may be NULL for no body. */
static int do_request(const char *method, const char *path, const char *body, cJSON **out_json) {
    int fd = connect_tcp(TEST_PORT);
    if (fd < 0) { return -1; }
    char header[1024];
    size_t len = body ? strlen(body) : 0;
    int n = snprintf(header, sizeof(header),
                     "%s %s HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n", method, path, len);
    if (n < 0 || (size_t)n >= sizeof(header)) { close(fd); return -1; }
    const char *parts[] = {header, body}; size_t sizes[] = {(size_t)n, len};
    for (size_t i = 0; i < 2; i++) {
        size_t sent = 0;
        while (sent < sizes[i]) {
            ssize_t wrote = send(fd, parts[i] + sent, sizes[i] - sent, MSG_NOSIGNAL);
            if (wrote <= 0) { close(fd); return -1; }
            sent += (size_t)wrote;
        }
    }
    size_t cap = 16u * 1024u * 1024u;
    char *resp = malloc(cap); if (!resp) { close(fd); return -1; }
    ssize_t got = recv_response(fd, resp, cap); close(fd);
    int status = got > 0 ? status_code_of(resp) : -1;
    if (out_json) { *out_json = got > 0 && *body_of(resp) ? cJSON_Parse(body_of(resp)) : NULL; }
    free(resp); return status;
}

static const char *jstr(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool jbool(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsTrue(v);
}

/* ---- tests -------------------------------------------------------------------- */

TEST get_groups_starts_empty(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/groups", NULL, &out));
    cJSON *groups = cJSON_GetObjectItemCaseSensitive(out, "groups");
    ASSERT(cJSON_IsArray(groups));
    ASSERT_EQ(0, cJSON_GetArraySize(groups));
    cJSON_Delete(out);
    harness_stop(h);
    PASS();
}

TEST create_group_normalizes_color_and_defaults_enable(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups",
                             "{\"name\":\"g1\",\"color\":\"#ABCDEF\",\"interface\":\"eth0\"}", &out));
    ASSERT_STR_EQ("g1", jstr(out, "name"));
    ASSERT_STR_EQ("#abcdef", jstr(out, "color"));
    ASSERT_STR_EQ("eth0", jstr(out, "interface"));
    ASSERT(jbool(out, "enable")); /* GroupReq.Enable absent -> defaults true */
    cJSON *priority = cJSON_GetObjectItemCaseSensitive(out, "priority");
    ASSERT(cJSON_IsNumber(priority));
    ASSERT_EQ(MT_GROUP_DEFAULT_PRIORITY, priority->valueint);
    cJSON *rules = cJSON_GetObjectItemCaseSensitive(out, "rules");
    ASSERT(cJSON_IsArray(rules));
    ASSERT_EQ(0, cJSON_GetArraySize(rules));
    ASSERT(jstr(out, "id") != NULL);
    ASSERT_EQ(8u, strlen(jstr(out, "id")));
    cJSON_Delete(out);
    harness_stop(h);
    PASS();
}

TEST create_group_invalid_color_falls_back_to_white(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    cJSON *out = NULL;
    ASSERT_EQ(200,
             do_request("POST", "/api/v1/groups", "{\"name\":\"g\",\"color\":\"not-a-color\"}", &out));
    ASSERT_STR_EQ("#ffffff", jstr(out, "color"));
    cJSON_Delete(out);
    harness_stop(h);
    PASS();
}

TEST get_group_unknown_and_invalid_id(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    ASSERT_EQ(400, do_request("GET", "/api/v1/groups/nothex!", NULL, NULL));
    ASSERT_EQ(404, do_request("GET", "/api/v1/groups/deadbeef", NULL, NULL));
    harness_stop(h);
    PASS();
}

TEST put_group_updates_fields_and_keeps_rules_when_absent(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    cJSON *created = NULL;
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups",
                             "{\"name\":\"g1\",\"color\":\"#111111\",\"interface\":\"eth0\","
                             "\"rules\":[{\"name\":\"r1\",\"type\":\"domain\",\"rule\":\"example.com\","
                             "\"enable\":true}]}",
                             &created));
    char id[9];
    snprintf(id, sizeof(id), "%s", jstr(created, "id"));
    cJSON *rules0 = cJSON_GetObjectItemCaseSensitive(created, "rules");
    const char *rule_id = jstr(cJSON_GetArrayItem(rules0, 0), "id");
    char rule_id_buf[9];
    snprintf(rule_id_buf, sizeof(rule_id_buf), "%s", rule_id);
    cJSON_Delete(created);

    char path[64];
    snprintf(path, sizeof(path), "/api/v1/groups/%s", id);
    cJSON *updated = NULL;
    ASSERT_EQ(200,
             do_request("PUT", path, "{\"name\":\"g1-renamed\",\"color\":\"#222222\",\"interface\":\"eth1\"}",
                       &updated));
    ASSERT_STR_EQ("g1-renamed", jstr(updated, "name"));
    ASSERT_STR_EQ("#222222", jstr(updated, "color"));
    ASSERT_STR_EQ("eth1", jstr(updated, "interface"));
    cJSON *rules1 = cJSON_GetObjectItemCaseSensitive(updated, "rules");
    ASSERT_EQ(1, cJSON_GetArraySize(rules1));
    ASSERT_STR_EQ(rule_id_buf, jstr(cJSON_GetArrayItem(rules1, 0), "id"));
    cJSON_Delete(updated);
    harness_stop(h);
    PASS();
}

TEST put_group_id_mismatch_is_400(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    cJSON *created = NULL;
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups", "{\"name\":\"g1\"}", &created));
    char id[9];
    snprintf(id, sizeof(id), "%s", jstr(created, "id"));
    cJSON_Delete(created);

    char path[64];
    snprintf(path, sizeof(path), "/api/v1/groups/%s", id);
    ASSERT_EQ(400, do_request("PUT", path, "{\"name\":\"x\",\"id\":\"deadbeef\"}", NULL));
    harness_stop(h);
    PASS();
}

TEST delete_group_removes_it(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    cJSON *created = NULL;
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups", "{\"name\":\"g1\"}", &created));
    char id[9];
    snprintf(id, sizeof(id), "%s", jstr(created, "id"));
    cJSON_Delete(created);

    char path[64];
    snprintf(path, sizeof(path), "/api/v1/groups/%s", id);
    ASSERT_EQ(200, do_request("DELETE", path, NULL, NULL));
    ASSERT_EQ(404, do_request("GET", path, NULL, NULL));
    harness_stop(h);
    PASS();
}

TEST put_groups_bulk_replace_reuses_ids_and_validates(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);

    /* missing "groups" key -> 400 */
    ASSERT_EQ(400, do_request("PUT", "/api/v1/groups", "{}", NULL));

    cJSON *g1 = NULL;
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups",
                             "{\"name\":\"g1\",\"rules\":[{\"name\":\"r1\",\"type\":\"domain\","
                             "\"rule\":\"example.com\",\"enable\":true}]}",
                             &g1));
    char g1_id[9];
    snprintf(g1_id, sizeof(g1_id), "%s", jstr(g1, "id"));
    char r1_id[9];
    snprintf(r1_id, sizeof(r1_id), "%s", jstr(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(g1, "rules"), 0), "id"));
    cJSON_Delete(g1);

    ASSERT_EQ(200, do_request("POST", "/api/v1/groups", "{\"name\":\"g2\"}", NULL));

    char body[512];
    snprintf(body, sizeof(body),
            "{\"groups\":[{\"id\":\"%s\",\"name\":\"g1-kept\",\"rules\":[{\"id\":\"%s\",\"name\":\"r1-kept\","
            "\"type\":\"domain\",\"rule\":\"example.com\",\"enable\":true}]},{\"name\":\"g3-new\"}]}",
            g1_id, r1_id);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("PUT", "/api/v1/groups", body, &out));
    cJSON *groups = cJSON_GetObjectItemCaseSensitive(out, "groups");
    ASSERT_EQ(2, cJSON_GetArraySize(groups));
    ASSERT_STR_EQ("g1-kept", jstr(cJSON_GetArrayItem(groups, 0), "name"));
    ASSERT_STR_EQ(g1_id, jstr(cJSON_GetArrayItem(groups, 0), "id")); /* id preserved */
    cJSON *kept_rules = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(groups, 0), "rules");
    ASSERT_STR_EQ(r1_id, jstr(cJSON_GetArrayItem(kept_rules, 0), "id")); /* rule id reused */
    ASSERT_STR_EQ("g3-new", jstr(cJSON_GetArrayItem(groups, 1), "name"));
    cJSON_Delete(out);

    /* g2 (never referenced by the PUT) is gone */
    cJSON *listed = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/groups", NULL, &listed));
    ASSERT_EQ(2, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(listed, "groups")));
    cJSON_Delete(listed);

    harness_stop(h);
    PASS();
}

TEST rule_crud_roundtrip(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    cJSON *g = NULL;
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups", "{\"name\":\"g1\"}", &g));
    char gid[9];
    snprintf(gid, sizeof(gid), "%s", jstr(g, "id"));
    cJSON_Delete(g);

    char rules_path[64];
    snprintf(rules_path, sizeof(rules_path), "/api/v1/groups/%s/rules", gid);

    cJSON *created = NULL;
    ASSERT_EQ(200, do_request("POST", rules_path,
                             "{\"name\":\"r1\",\"type\":\"domain\",\"rule\":\"example.com\",\"enable\":true}",
                             &created));
    ASSERT_STR_EQ("r1", jstr(created, "name"));
    char rid[9];
    snprintf(rid, sizeof(rid), "%s", jstr(created, "id"));
    cJSON_Delete(created);

    char rule_path[80];
    snprintf(rule_path, sizeof(rule_path), "%s/%s", rules_path, rid);

    cJSON *got = NULL;
    ASSERT_EQ(200, do_request("GET", rule_path, NULL, &got));
    ASSERT_STR_EQ("r1", jstr(got, "name"));
    cJSON_Delete(got);

    cJSON *updated = NULL;
    ASSERT_EQ(200, do_request("PUT", rule_path,
                             "{\"name\":\"r1-renamed\",\"type\":\"domain\",\"rule\":\"example.org\","
                             "\"enable\":false}",
                             &updated));
    ASSERT_STR_EQ("r1-renamed", jstr(updated, "name"));
    ASSERT_STR_EQ(rid, jstr(updated, "id")); /* id unchanged by PUT */
    ASSERT(!jbool(updated, "enable"));
    cJSON_Delete(updated);

    /* unknown rule id under a real group -> 404 */
    char bogus_path[96];
    snprintf(bogus_path, sizeof(bogus_path), "%s/deadbeef", rules_path);
    ASSERT_EQ(404, do_request("GET", bogus_path, NULL, NULL));

    ASSERT_EQ(200, do_request("DELETE", rule_path, NULL, NULL));
    cJSON *after = NULL;
    ASSERT_EQ(200, do_request("GET", rules_path, NULL, &after));
    ASSERT_EQ(0, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(after, "rules")));
    cJSON_Delete(after);

    harness_stop(h);
    PASS();
}

TEST put_rules_strict_id_validation(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    cJSON *g = NULL;
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups",
                             "{\"name\":\"g1\",\"rules\":[{\"name\":\"r1\",\"type\":\"domain\","
                             "\"rule\":\"example.com\",\"enable\":true}]}",
                             &g));
    char gid[9];
    snprintf(gid, sizeof(gid), "%s", jstr(g, "id"));
    char rid[9];
    snprintf(rid, sizeof(rid), "%s", jstr(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(g, "rules"), 0), "id"));
    cJSON_Delete(g);

    char rules_path[64];
    snprintf(rules_path, sizeof(rules_path), "/api/v1/groups/%s/rules", gid);

    /* missing "rules" key -> 400 */
    ASSERT_EQ(400, do_request("PUT", rules_path, "{}", NULL));

    /* an id that doesn't match any current rule -> 404 (strict, unlike the
     * lenient nested-rules reuse used when creating/updating a group) */
    ASSERT_EQ(404, do_request("PUT", rules_path,
                             "{\"rules\":[{\"id\":\"deadbeef\",\"name\":\"x\",\"type\":\"domain\","
                             "\"rule\":\"x.com\",\"enable\":true}]}",
                             NULL));

    char body[256];
    snprintf(body, sizeof(body),
            "{\"rules\":[{\"id\":\"%s\",\"name\":\"r1-kept\",\"type\":\"domain\",\"rule\":\"example.com\","
            "\"enable\":true}]}",
            rid);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("PUT", rules_path, body, &out));
    cJSON *rules = cJSON_GetObjectItemCaseSensitive(out, "rules");
    ASSERT_EQ(1, cJSON_GetArraySize(rules));
    ASSERT_STR_EQ(rid, jstr(cJSON_GetArrayItem(rules, 0), "id"));
    ASSERT_STR_EQ("r1-kept", jstr(cJSON_GetArrayItem(rules, 0), "name"));
    cJSON_Delete(out);

    harness_stop(h);
    PASS();
}

/* Regression test for the Phase-6 bug where an HTTP-driven group/rule
 * mutation updated netfilter but never republished the DNS-matching
 * snapshot (mt_ruleset_snapshot_build was only ever called once, at
 * daemon startup). A rule created here via POST must be immediately
 * matchable through the DNS pipeline without any snapshot rebuild call
 * outside of groups.c's own handlers. */
TEST rule_created_via_http_is_dns_matchable(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);

    cJSON *g = NULL;
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups",
                             "{\"name\":\"g1\",\"interface\":\"eth0\","
                             "\"rules\":[{\"name\":\"r1\",\"type\":\"domain\","
                             "\"rule\":\"example.com\",\"enable\":true}]}",
                             &g));
    mt_id_t group_id;
    ASSERT_EQ(MT_OK, mt_id_parse(jstr(g, "id"), &group_id));
    cJSON_Delete(g);

    uint8_t name[MT_DNS_MAX_NAME + 1];
    size_t name_len = wire_name("example.com", name, sizeof(name));
    ASSERT(name_len > 0);
    uint8_t addr[4] = {93, 184, 216, 34};

    mt_dns_rr_t rr = {0};
    memcpy(rr.name, name, name_len);
    rr.name_len = name_len;
    rr.rtype = MT_DNS_TYPE_A;
    rr.rclass = 1;
    rr.ttl = 300;
    rr.rdata = addr;
    rr.rdata_len = sizeof(addr);

    mt_dns_msg_t msg = {0};
    msg.answers = &rr;
    msg.n_answers = 1;

    mt_dns_pipeline_handle_message(h->pipeline, &msg, 0);

    ASSERT(h->cap.matched);
    ASSERT(mt_id_equal(h->cap.group_id, group_id));

    harness_stop(h);
    PASS();
}


static double monotonic_seconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
}

TEST fifty_thousand_group_rules_full_compact_strict_and_persist(void) {
    harness_t *h = harness_start_saved(true); ASSERT(h);
    cJSON *request = cJSON_CreateObject();
    cJSON *groups = cJSON_AddArrayToObject(request, "groups");
    cJSON *group = cJSON_CreateObject(); cJSON_AddItemToArray(groups, group);
    cJSON_AddStringToObject(group, "id", "aabbccdd");
    cJSON_AddStringToObject(group, "name", "Large group");
    cJSON_AddStringToObject(group, "interface", "blackhole");
    cJSON_AddBoolToObject(group, "enable", true);
    cJSON *rules = cJSON_AddArrayToObject(group, "rules");
    for (size_t i = 0; i < 50000; i++) {
        char text[64]; snprintf(text, sizeof(text), "item%zu.example.com", i);
        cJSON *rule = cJSON_CreateObject(); cJSON_AddItemToArray(rules, rule);
        cJSON_AddStringToObject(rule, "name", ""); cJSON_AddStringToObject(rule, "rule", text);
        cJSON_AddStringToObject(rule, "type", "namespace"); cJSON_AddBoolToObject(rule, "enable", true);
    }
    char *body = cJSON_PrintUnformatted(request); ASSERT(body);
    ASSERT(strlen(body) > MT_HTTPD_MAX_BODY_BYTES);
    cJSON *out = NULL; double begin = monotonic_seconds();
    ASSERT_EQ(200, do_request("PUT", "/api/v1/groups?save=true", body, &out));
    printf("50k group full import+save HTTP: %.3fs; request_bytes=%zu\n", monotonic_seconds()-begin, strlen(body));
    free(body); cJSON_Delete(request);
    cJSON *saved = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "groups"), 0);
    rules = cJSON_GetObjectItemCaseSensitive(saved, "rules"); ASSERT_EQ(50000, cJSON_GetArraySize(rules));
    char first_id[9]; snprintf(first_id, sizeof(first_id), "%s", jstr(cJSON_GetArrayItem(rules, 0), "id"));
    mt_lookup_t ids = {0};
    cJSON *item; cJSON_ArrayForEach(item, rules) {
        bool inserted; const char *id = jstr(item, "id"); ASSERT(id);
        ASSERT_EQ(MT_OK, mt_lookup_put(&ids, id, strlen(id), 0, &inserted)); ASSERT(inserted);
    }
    mt_lookup_clear(&ids);
    // Strict full replacement preserves IDs and order, then append exercises
    // the geometric-capacity invariant that exactly-N allocation violated.
    cJSON *strict = cJSON_CreateObject(); cJSON_AddItemToObject(strict, "rules", cJSON_Duplicate(rules, true));
    body = cJSON_PrintUnformatted(strict); cJSON_Delete(strict);
    begin = monotonic_seconds();
    ASSERT_EQ(200, do_request("PUT", "/api/v1/groups/aabbccdd/rules?save=true", body, NULL));
    printf("50k group strict replacement+save HTTP: %.3fs\n", monotonic_seconds()-begin);
    free(body);
    cJSON_Delete(out); out = NULL;
    char compact[1024];
    snprintf(compact, sizeof(compact),
        "{\"groups\":[{\"id\":\"aabbccdd\",\"name\":\"Edited\",\"interface\":\"blackhole\",\"enable\":true,"
        "\"ruleChanges\":[{\"id\":\"%s\",\"name\":\"first\",\"rule\":\"item0.example.com\",\"type\":\"namespace\",\"enable\":false,"
        "\"previous\":{\"name\":\"\",\"rule\":\"item0.example.com\",\"type\":\"namespace\",\"enable\":true}}]}]}", first_id);
    begin = monotonic_seconds();
    ASSERT_EQ(200, do_request("PUT", "/api/v1/groups?save=true", compact, &out));
    printf("50k group compact edit+save HTTP: %.3fs; request_bytes=%zu\n", monotonic_seconds()-begin, strlen(compact));
    saved = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "groups"), 0);
    rules = cJSON_GetObjectItemCaseSensitive(saved, "rules"); ASSERT_EQ(50000, cJSON_GetArraySize(rules));
    ASSERT_STR_EQ(first_id, jstr(cJSON_GetArrayItem(rules, 0), "id"));
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rules, 0), "enable")));
    cJSON_Delete(out); out = NULL;
    ASSERT_EQ(409, do_request("PUT", "/api/v1/groups?save=true", compact, NULL));
    mt_config_t loaded; ASSERT_EQ(MT_OK, mt_config_init_defaults(&loaded));
    begin = monotonic_seconds();
    ASSERT_EQ(MT_OK, mt_config_load_file(&loaded, h->save_path));
    printf("50k group YAML reload: %.3fs\n", monotonic_seconds()-begin);
    ASSERT_EQ(1u, loaded.n_groups); ASSERT_EQ(50000u, loaded.groups[0]->n_rules);
    ASSERT_STR_EQ("Edited", loaded.groups[0]->name); ASSERT(!loaded.groups[0]->rules[0]->enable);
    mt_config_clear(&loaded);
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups/aabbccdd/rules", "{\"rule\":\"last.example\",\"type\":\"namespace\",\"enable\":true}", NULL));
    ASSERT_EQ(200, do_request("GET", "/api/v1/groups/aabbccdd/rules", NULL, &out));
    ASSERT_EQ(50001, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(out, "rules")));
    cJSON_Delete(out); harness_stop(h); PASS();
}

TEST failed_group_save_returns_error_and_keeps_dirty_retry_possible(void) {
    harness_t *h = harness_start_saved(true); ASSERT(h);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups?save=true",
        "{\"id\":\"aabbccdd\",\"name\":\"before\",\"rules\":[{\"name\":\"one\",\"rule\":\"one.example\",\"type\":\"domain\",\"enable\":true}]}", &out));
    cJSON_Delete(out);
    ASSERT_EQ(0, unlink(h->save_path));
    ASSERT_EQ(0, rmdir(h->save_dir));
    /* A full import must return the generated (not client-only) rule ID even
     * when saving fails, otherwise subsequent compact edits cannot address it. */
    ASSERT_EQ(500, do_request("PUT", "/api/v1/groups?save=true",
        "{\"groups\":[{\"id\":\"aabbccdd\",\"name\":\"after\",\"rules\":[{\"id\":\"12345678\",\"name\":\"edited\",\"rule\":\"two.example\",\"type\":\"domain\",\"enable\":false}]}]}", &out));
    ASSERT_STR_EQ("PERSISTENCE_FAILED", jstr(out, "code"));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out, "applied")));
    cJSON *groups = cJSON_GetObjectItemCaseSensitive(out, "groups");
    cJSON *group = cJSON_GetArrayItem(groups, 0);
    cJSON *rules = cJSON_GetObjectItemCaseSensitive(group, "rules");
    ASSERT_EQ(1, cJSON_GetArraySize(rules));
    char id[MT_ID_STR_LEN]; snprintf(id, sizeof(id), "%s", jstr(cJSON_GetArrayItem(rules, 0), "id"));
    ASSERT(strcmp(id, "12345678") != 0);
    cJSON_DeleteItemFromObjectCaseSensitive(out, "error");
    cJSON_DeleteItemFromObjectCaseSensitive(out, "code");
    cJSON_DeleteItemFromObjectCaseSensitive(out, "applied");
    cJSON_DeleteItemFromObjectCaseSensitive(group, "rules");
    cJSON_AddArrayToObject(group, "ruleChanges");
    char *retry = cJSON_PrintUnformatted(out); ASSERT(retry); cJSON_Delete(out);
    ASSERT_EQ(500, do_request("PUT", "/api/v1/groups?save=true", retry, &out));
    ASSERT_STR_EQ("PERSISTENCE_FAILED", jstr(out, "code")); cJSON_Delete(out);
    ASSERT_EQ(0, mkdir(h->save_dir, 0700));
    ASSERT_EQ(200, do_request("PUT", "/api/v1/groups?save=true", retry, &out));
    ASSERT(!cJSON_HasObjectItem(out, "code")); cJSON_Delete(out); free(retry);
    mt_config_t loaded; mt_config_init_defaults(&loaded);
    ASSERT_EQ(MT_OK, mt_config_load_file(&loaded, h->save_path));
    ASSERT_EQ(1, loaded.n_groups); ASSERT_EQ(1, loaded.groups[0]->n_rules);
    ASSERT_STR_EQ("after", loaded.groups[0]->name);
    ASSERT_STR_EQ("edited", loaded.groups[0]->rules[0]->name);
    ASSERT(!loaded.groups[0]->rules[0]->enable);
    char persisted_id[MT_ID_STR_LEN]; mt_id_format(loaded.groups[0]->rules[0]->id, persisted_id);
    ASSERT_STR_EQ(id, persisted_id);
    mt_config_clear(&loaded); harness_stop(h); PASS();
}

TEST invalid_batch_preserves_existing_groups(void) {
    harness_t *h = harness_start(); ASSERT(h);
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups", "{\"id\":\"aabbccdd\",\"name\":\"original\"}", NULL));
    ASSERT_EQ(500, do_request("PUT", "/api/v1/groups", "{\"groups\":[{\"id\":\"aabbccdd\"},{\"id\":\"aabbccdd\"}]}", NULL));
    cJSON *out = NULL; ASSERT_EQ(200, do_request("GET", "/api/v1/groups?with_rules=true", NULL, &out));
    cJSON *groups = cJSON_GetObjectItemCaseSensitive(out, "groups"); ASSERT_EQ(1, cJSON_GetArraySize(groups));
    ASSERT_STR_EQ("original", jstr(cJSON_GetArrayItem(groups, 0), "name"));
    cJSON_Delete(out); harness_stop(h); PASS();
}

TEST group_priority_updates_persists_and_omission_preserves_it(void) {
    harness_t *h = harness_start_saved(true); ASSERT(h);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups?save=true",
        "{\"id\":\"aabbccdd\",\"name\":\"priority-group\",\"priority\":1000,"
        "\"rules\":[{\"name\":\"one\",\"rule\":\"one.example\",\"type\":\"domain\",\"enable\":true}]}", &out));
    ASSERT_EQ(1000, cJSON_GetObjectItemCaseSensitive(out, "priority")->valueint);
    cJSON_Delete(out);
    ASSERT_EQ(200, do_request("GET", "/api/v1/groups/aabbccdd", NULL, &out));
    ASSERT_EQ(1000, cJSON_GetObjectItemCaseSensitive(out, "priority")->valueint);
    cJSON_Delete(out);

    /* Existing clients omit the additive field: both individual edits and
     * compact bulk saves retain the current priority instead of resetting it. */
    ASSERT_EQ(200, do_request("PUT", "/api/v1/groups/aabbccdd?save=true",
        "{\"name\":\"renamed\"}", &out));
    ASSERT_EQ(1000, cJSON_GetObjectItemCaseSensitive(out, "priority")->valueint);
    ASSERT_EQ(1, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(out, "rules")));
    cJSON_Delete(out);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/groups?save=true",
        "{\"groups\":[{\"id\":\"aabbccdd\",\"name\":\"renamed\",\"ruleChanges\":[]}]}", &out));
    cJSON *group = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "groups"), 0);
    ASSERT_EQ(1000, cJSON_GetObjectItemCaseSensitive(group, "priority")->valueint);
    cJSON_Delete(out);
    /* Same group order, metadata and rules; priority alone must bypass the
     * app's unchanged-group reuse optimization and publish the replacement. */
    ASSERT_EQ(200, do_request("PUT", "/api/v1/groups?save=true",
        "{\"groups\":[{\"id\":\"aabbccdd\",\"name\":\"renamed\",\"priority\":555,\"ruleChanges\":[]}]}", &out));
    group = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "groups"), 0);
    ASSERT_EQ(555, cJSON_GetObjectItemCaseSensitive(group, "priority")->valueint);
    ASSERT_EQ(1, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(group, "rules")));
    cJSON_Delete(out);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/groups/aabbccdd?save=true",
        "{\"name\":\"renamed\",\"priority\":1}", &out));
    ASSERT_EQ(1, cJSON_GetObjectItemCaseSensitive(out, "priority")->valueint);
    cJSON_Delete(out);
    mt_config_t loaded; ASSERT_EQ(MT_OK, mt_config_init_defaults(&loaded));
    ASSERT_EQ(MT_OK, mt_config_load_file(&loaded, h->save_path));
    ASSERT_EQ(1, loaded.n_groups);
    ASSERT_EQ(1, loaded.groups[0]->priority);
    ASSERT_EQ(1, loaded.groups[0]->n_rules);
    mt_config_clear(&loaded); harness_stop(h); PASS();
}

TEST invalid_group_priorities_do_not_change_live_or_persisted_state(void) {
    harness_t *h = harness_start_saved(true); ASSERT(h);
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups?save=true",
        "{\"id\":\"aabbccdd\",\"name\":\"original\",\"priority\":777}", NULL));
    const char *invalid[] = {
        "0", "-1", "1001", "1.5", "1e309", "null", "true", "\"300\"", "{}", "[]"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char body[512];
        snprintf(body, sizeof(body), "{\"name\":\"invalid\",\"priority\":%s}", invalid[i]);
        ASSERT_EQ(400, do_request("POST", "/api/v1/groups?save=true", body, NULL));
        ASSERT_EQ(400, do_request("PUT", "/api/v1/groups/aabbccdd?save=true", body, NULL));
        snprintf(body, sizeof(body), "{\"groups\":["
            "{\"id\":\"aabbccdd\",\"name\":\"would-change\",\"priority\":1},"
            "{\"id\":\"bbbb0001\",\"priority\":%s}]}", invalid[i]);
        ASSERT_EQ(400, do_request("PUT", "/api/v1/groups?save=true", body, NULL));
        cJSON *out = NULL;
        ASSERT_EQ(200, do_request("GET", "/api/v1/groups", NULL, &out));
        cJSON *groups = cJSON_GetObjectItemCaseSensitive(out, "groups");
        ASSERT_EQ(1, cJSON_GetArraySize(groups));
        cJSON *group = cJSON_GetArrayItem(groups, 0);
        ASSERT_STR_EQ("original", jstr(group, "name"));
        ASSERT_EQ(777, cJSON_GetObjectItemCaseSensitive(group, "priority")->valueint);
        cJSON_Delete(out);
    }
    mt_config_t loaded; ASSERT_EQ(MT_OK, mt_config_init_defaults(&loaded));
    ASSERT_EQ(MT_OK, mt_config_load_file(&loaded, h->save_path));
    ASSERT_EQ(1, loaded.n_groups);
    ASSERT_EQ(777, loaded.groups[0]->priority);
    ASSERT_STR_EQ("original", loaded.groups[0]->name);
    mt_config_clear(&loaded); harness_stop(h); PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(fifty_thousand_group_rules_full_compact_strict_and_persist);
    RUN_TEST(failed_group_save_returns_error_and_keeps_dirty_retry_possible);
    RUN_TEST(invalid_batch_preserves_existing_groups);
    RUN_TEST(group_priority_updates_persists_and_omission_preserves_it);
    RUN_TEST(invalid_group_priorities_do_not_change_live_or_persisted_state);
    RUN_TEST(get_groups_starts_empty);
    RUN_TEST(create_group_normalizes_color_and_defaults_enable);
    RUN_TEST(create_group_invalid_color_falls_back_to_white);
    RUN_TEST(get_group_unknown_and_invalid_id);
    RUN_TEST(put_group_updates_fields_and_keeps_rules_when_absent);
    RUN_TEST(put_group_id_mismatch_is_400);
    RUN_TEST(delete_group_removes_it);
    RUN_TEST(put_groups_bulk_replace_reuses_ids_and_validates);
    RUN_TEST(rule_crud_roundtrip);
    RUN_TEST(put_rules_strict_id_validation);
    RUN_TEST(rule_created_via_http_is_dns_matchable);
    GREATEST_MAIN_END();
}
