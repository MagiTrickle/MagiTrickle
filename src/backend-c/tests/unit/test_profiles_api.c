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
#include "magitrickle/groups.h"
#include "magitrickle/subscriptions_api.h"

typedef struct harness {
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

#define TEST_PORT 18109

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


static const char *definition = "{\"profiles\":[{\"id\":\"vpn\",\"name\":\"VPN\",\"interfaces\":[\"tun0\",\"tun1\",\"tun2\"]}]}";
static const char *reordered = "{\"profiles\":[{\"id\":\"vpn\",\"name\":\"Renamed\",\"interfaces\":[\"tun2\",\"tun0\"]}]}";

TEST profiles_api_and_compatibility_shadows(void) {
    harness_t *h = harness_start(true); ASSERT(h != NULL);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/profiles", definition, NULL));
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups", "{\"id\":\"aabbccdd\",\"name\":\"G\",\"interface\":\"stale\",\"profile\":\"vpn\",\"priority\":731}", NULL));
    ASSERT_EQ(200, do_request("PUT", "/api/v1/subscriptions", "{\"subscriptions\":[{\"id\":\"11223344\",\"name\":\"S\",\"url\":\"https://example.test\",\"profile\":\"vpn\",\"priority\":219,\"rules\":[]}]}", NULL));
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/profiles", NULL, &out));
    cJSON *p = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "profiles"), 0);
    cJSON *usage = cJSON_GetObjectItemCaseSensitive(p, "usage");
    ASSERT_EQ(1, cJSON_GetObjectItemCaseSensitive(usage, "groups")->valueint);
    ASSERT_EQ(1, cJSON_GetObjectItemCaseSensitive(usage, "subscriptions")->valueint);
    cJSON_Delete(out);
    ASSERT_EQ(731, mt_ruleset_group(mt_app_user_group_at(h->app, 0))->priority);
    ASSERT_EQ(219, mt_ruleset_group(mt_app_subscription_ruleset_at(h->app, 0))->priority);
    /* A priority-only bulk edit must preserve the profile and bypass reuse
     * of an otherwise identical group with the same collection position. */
    ASSERT_EQ(200, do_request("PUT", "/api/v1/groups", "{\"groups\":[{\"id\":\"aabbccdd\",\"name\":\"G\",\"profile\":\"vpn\",\"priority\":842,\"ruleChanges\":[]}]}", &out));
    p = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "groups"), 0);
    ASSERT_EQ(842, cJSON_GetObjectItemCaseSensitive(p, "priority")->valueint);
    ASSERT_STR_EQ("vpn", cJSON_GetObjectItemCaseSensitive(p, "profile")->valuestring);
    ASSERT_STR_EQ("tun0", cJSON_GetObjectItemCaseSensitive(p, "interface")->valuestring);
    cJSON_Delete(out);
    /* Older payloads may omit priority while still carrying their route. */
    ASSERT_EQ(200, do_request("PUT", "/api/v1/subscriptions", "{\"subscriptions\":[{\"id\":\"11223344\",\"name\":\"S\",\"url\":\"https://example.test\",\"profile\":\"vpn\",\"ruleChanges\":[]}]}", NULL));
    ASSERT_EQ(200, do_request("PUT", "/api/v1/profiles", reordered, NULL));
    ASSERT_EQ(200, do_request("GET", "/api/v1/groups", NULL, &out));
    p = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "groups"), 0);
    ASSERT_STR_EQ("tun2", cJSON_GetObjectItemCaseSensitive(p, "interface")->valuestring);
    ASSERT_STR_EQ("vpn", cJSON_GetObjectItemCaseSensitive(p, "profile")->valuestring);
    ASSERT_EQ(842, cJSON_GetObjectItemCaseSensitive(p, "priority")->valueint);
    cJSON_Delete(out);
    ASSERT_EQ(200, do_request("GET", "/api/v1/subscriptions", NULL, &out));
    p = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "subscriptions"), 0);
    ASSERT_STR_EQ("tun2", cJSON_GetObjectItemCaseSensitive(p, "interface")->valuestring);
    ASSERT_STR_EQ("vpn", cJSON_GetObjectItemCaseSensitive(p, "profile")->valuestring);
    ASSERT_EQ(219, cJSON_GetObjectItemCaseSensitive(p, "priority")->valueint);
    const mt_group_t *synth = mt_ruleset_group(mt_app_subscription_ruleset_at(h->app, 0));
    ASSERT_STR_EQ("vpn", synth->profile); ASSERT_STR_EQ("tun2", synth->iface);
    ASSERT_EQ(219, synth->priority);
    cJSON_Delete(out);
    mt_config_t loaded = {0}; ASSERT_EQ(MT_OK, mt_config_init_defaults(&loaded));
    ASSERT_EQ(MT_OK, mt_config_load_file(&loaded, h->config_path));
    ASSERT_STR_EQ("tun2", loaded.groups[0]->iface);
    ASSERT_STR_EQ("tun2", loaded.subscriptions[0]->iface);
    ASSERT_STR_EQ("vpn", loaded.groups[0]->profile);
    ASSERT_EQ(842, loaded.groups[0]->priority);
    ASSERT_EQ(219, loaded.subscriptions[0]->priority);
    mt_config_clear(&loaded); harness_stop(h); PASS();
}
TEST used_profile_cannot_disappear_and_direct_selection_clears_reference(void) {
    harness_t *h = harness_start(false); ASSERT(h != NULL);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/profiles", definition, NULL));
    ASSERT_EQ(200, do_request("POST", "/api/v1/groups", "{\"id\":\"aabbccdd\",\"profile\":\"vpn\"}", NULL));
    ASSERT_EQ(409, do_request("PUT", "/api/v1/profiles", "{\"profiles\":[]}", NULL));
    ASSERT_EQ(400, do_request("PUT", "/api/v1/groups/aabbccdd", "{\"profile\":\"missing\",\"interface\":\"tun0\"}", NULL));
    ASSERT_EQ(200, do_request("PUT", "/api/v1/groups/aabbccdd", "{\"interface\":\"blackhole\"}", NULL));
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/groups", NULL, &out));
    cJSON *p = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "groups"), 0);
    ASSERT(cJSON_GetObjectItemCaseSensitive(p, "profile") == NULL);
    ASSERT_STR_EQ("blackhole", cJSON_GetObjectItemCaseSensitive(p, "interface")->valuestring);
    cJSON_Delete(out);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/profiles", "{\"profiles\":[]}", NULL));
    harness_stop(h); PASS();
}
TEST invalid_profile_requests_do_not_replace_applied_collection(void) {
    harness_t *h = harness_start(false); ASSERT(h != NULL);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/profiles", definition, NULL));
    const char *bad[] = {
        "{}", "{\"profiles\":null}", "{\"profiles\":[{\"id\":\"x\",\"name\":\"X\",\"interfaces\":[]}]}",
        "{\"profiles\":[{\"id\":\"x\",\"name\":\"X\",\"interfaces\":[\"tun0\",\"tun0\"]}]}",
        "{\"profiles\":[{\"id\":\"x\",\"name\":\"X\",\"interfaces\":[\"tun0\"],\"on_unavailable\":\"direct\"}]}"
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        ASSERT_EQ(400, do_request("PUT", "/api/v1/profiles", bad[i], NULL));
    }
    cJSON *out = NULL; ASSERT_EQ(200, do_request("GET", "/api/v1/profiles", NULL, &out));
    cJSON *p = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "profiles"), 0);
    ASSERT_STR_EQ("vpn", cJSON_GetObjectItemCaseSensitive(p, "id")->valuestring);
    cJSON_Delete(out); harness_stop(h); PASS();
}
TEST persistence_failure_is_explicit_and_retryable(void) {
    harness_t *h = harness_start(true); ASSERT(h != NULL);
    unlink(h->config_path);
    h->ctx.config_path = "/proc/mt-profile-test/config.yaml";
    cJSON *out = NULL;
    ASSERT_EQ(500, do_request("PUT", "/api/v1/profiles", definition, &out));
    ASSERT_STR_EQ("PERSISTENCE_FAILED", cJSON_GetObjectItemCaseSensitive(out, "code")->valuestring);
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out, "applied")));
    ASSERT_EQ(1, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(out, "profiles")));
    cJSON_Delete(out);
    h->ctx.config_path = h->config_path;
    ASSERT_EQ(200, do_request("PUT", "/api/v1/profiles", definition, NULL));
    ASSERT_EQ(0, access(h->config_path, F_OK));
    harness_stop(h); PASS();
}
TEST missing_profile_id_is_generated_and_persisted(void) {
    harness_t *h = harness_start(true); ASSERT(h != NULL);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("PUT", "/api/v1/profiles",
        "{\"profiles\":[{\"name\":\"Generated\",\"interfaces\":[\"tun0\"]}]}", &out));
    cJSON *p = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "profiles"), 0);
    cJSON *id = cJSON_GetObjectItemCaseSensitive(p, "id");
    ASSERT(cJSON_IsString(id));
    mt_id_t parsed = {{0, 0, 0, 0}};
    ASSERT_EQ(MT_OK, mt_id_parse(id->valuestring, &parsed));
    mt_config_t loaded = {0};
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&loaded));
    ASSERT_EQ(MT_OK, mt_config_load_file(&loaded, h->config_path));
    ASSERT_EQ(1, loaded.n_profiles);
    ASSERT_STR_EQ(id->valuestring, loaded.profiles[0]->id);
    ASSERT_STR_EQ("tun0", loaded.profiles[0]->interfaces[0]);
    mt_config_clear(&loaded);
    cJSON_Delete(out); harness_stop(h); PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(missing_profile_id_is_generated_and_persisted);
    RUN_TEST(profiles_api_and_compatibility_shadows);
    RUN_TEST(used_profile_cannot_disappear_and_direct_selection_clears_reference);
    RUN_TEST(invalid_profile_requests_do_not_replace_applied_collection);
    RUN_TEST(persistence_failure_is_explicit_and_retryable);
    GREATEST_MAIN_END();
}
