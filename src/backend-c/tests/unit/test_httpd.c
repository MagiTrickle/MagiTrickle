/* Runs the real epoll loop on a background thread (pthread) while this
 * thread acts as a plain blocking HTTP client -- the only practical way
 * to integration-test an event-loop-driven server synchronously from a
 * unit test. */
#include "greatest.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "magitrickle/httpd.h"
#include "magitrickle/loop.h"

static void h_hello(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)ud;
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "method", mt_http_req_method(req));
    cJSON_AddStringToObject(obj, "path", mt_http_req_path(req));
    if (mt_http_req_query_is_true(req, "with_rules")) {
        cJSON_AddBoolToObject(obj, "withRules", true);
    }
    mt_http_res_write_json(res, 200, obj);
}

static void h_param(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)ud;
    const char *id = mt_http_req_param(req, "groupID");
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", id ? id : "");
    mt_http_res_write_json(res, 200, obj);
}

static void h_echo_body(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)ud;
    size_t len;
    const uint8_t *body = mt_http_req_body(req, &len);
    mt_http_res_write(res, 200, "application/json; charset=utf-8", body, len);
}

static void h_not_found(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)ud;
    (void)req;
    mt_http_res_write(res, 404, "text/html", (const uint8_t *)"<h1>nope</h1>", 13);
}

static bool mw_reject_blocked(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)ud;
    if (strcmp(mt_http_req_path(req), "/blocked") == 0) {
        mt_http_res_write_error(res, 401, "Unauthorized");
        return false;
    }
    return true;
}

typedef struct harness {
    mt_loop_t *loop;
    mt_httpd_t *tcp;
    mt_httpd_t *unix_srv;
    pthread_t thread;
} harness_t;

static void *loop_thread(void *ud) {
    harness_t *h = ud;
    mt_loop_run(h->loop);
    return NULL;
}

#define TEST_PORT 18080
#define TEST_UNIX_PATH "/tmp/mt_httpd_test.sock"

static harness_t *harness_start(void) {
    harness_t *h = calloc(1, sizeof(*h));
    if (mt_loop_create(&h->loop) != MT_OK) { return NULL; }

    if (mt_httpd_create(h->loop, &h->tcp) != MT_OK) { return NULL; }
    mt_httpd_route(h->tcp, "GET", "/hello", h_hello, NULL);
    mt_httpd_route(h->tcp, "GET", "/groups/{groupID}", h_param, NULL);
    mt_httpd_route(h->tcp, "POST", "/echo", h_echo_body, NULL);
    mt_httpd_route(h->tcp, "GET", "/blocked", h_hello, NULL);
    mt_httpd_set_not_found(h->tcp, h_not_found, NULL);
    mt_httpd_set_middleware(h->tcp, mw_reject_blocked, NULL);
    if (mt_httpd_listen_tcp(h->tcp, "127.0.0.1", TEST_PORT) != MT_OK) { return NULL; }

    if (mt_httpd_create(h->loop, &h->unix_srv) != MT_OK) { return NULL; }
    mt_httpd_route(h->unix_srv, "GET", "/hello", h_hello, NULL);
    if (mt_httpd_listen_unix(h->unix_srv, TEST_UNIX_PATH) != MT_OK) { return NULL; }

    pthread_create(&h->thread, NULL, loop_thread, h);
    return h;
}

static void harness_stop(harness_t *h) {
    mt_loop_stop(h->loop);
    pthread_join(h->thread, NULL);
    mt_httpd_destroy(h->tcp);
    mt_httpd_destroy(h->unix_srv);
    mt_loop_destroy(h->loop);
    free(h);
}

/* ---- tiny blocking client helpers ---------------------------------------- */

static int connect_tcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    for (int i = 0; i < 50; i++) {
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) { return fd; }
        struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL);
    }
    close(fd);
    return -1;
}

static int connect_unix(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    for (int i = 0; i < 50; i++) {
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) { return fd; }
        struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL);
    }
    close(fd);
    return -1;
}

/* Reads until the declared Content-Length is fully consumed (or a 2s
 * timeout), returning total bytes read. Good enough for these small,
 * single-response test exchanges. */
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

TEST get_with_query_param(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    int fd = connect_tcp(TEST_PORT);
    ASSERT(fd >= 0);

    const char *req = "GET /hello?with_rules=true HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ASSERT(send(fd, req, strlen(req), 0) > 0);
    char resp[4096];
    ssize_t n = recv_response(fd, resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT_EQ(200, status_code_of(resp));
    ASSERT(strstr(body_of(resp), "\"method\":\"GET\"") != NULL);
    ASSERT(strstr(body_of(resp), "\"withRules\":true") != NULL);
    close(fd);
    harness_stop(h);
    PASS();
}

TEST path_param_extraction(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    int fd = connect_tcp(TEST_PORT);
    ASSERT(fd >= 0);
    const char *req = "GET /groups/aabbccdd HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ASSERT(send(fd, req, strlen(req), 0) > 0);
    char resp[4096];
    ASSERT(recv_response(fd, resp, sizeof(resp)) > 0);
    ASSERT_EQ(200, status_code_of(resp));
    ASSERT(strstr(body_of(resp), "\"id\":\"aabbccdd\"") != NULL);
    close(fd);
    harness_stop(h);
    PASS();
}

TEST post_body_roundtrip(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    int fd = connect_tcp(TEST_PORT);
    ASSERT(fd >= 0);
    const char *body = "{\"name\":\"test\"}";
    char req[512];
    snprintf(req, sizeof(req),
            "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
            strlen(body), body);
    ASSERT(send(fd, req, strlen(req), 0) > 0);
    char resp[4096];
    ASSERT(recv_response(fd, resp, sizeof(resp)) > 0);
    ASSERT_EQ(200, status_code_of(resp));
    ASSERT_STR_EQ(body, body_of(resp));
    close(fd);
    harness_stop(h);
    PASS();
}

TEST not_found_fallback(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    int fd = connect_tcp(TEST_PORT);
    ASSERT(fd >= 0);
    const char *req = "GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ASSERT(send(fd, req, strlen(req), 0) > 0);
    char resp[4096];
    ASSERT(recv_response(fd, resp, sizeof(resp)) > 0);
    ASSERT_EQ(404, status_code_of(resp));
    ASSERT_STR_EQ("<h1>nope</h1>", body_of(resp));
    close(fd);
    harness_stop(h);
    PASS();
}

TEST middleware_can_short_circuit(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    int fd = connect_tcp(TEST_PORT);
    ASSERT(fd >= 0);
    const char *req = "GET /blocked HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ASSERT(send(fd, req, strlen(req), 0) > 0);
    char resp[4096];
    ASSERT(recv_response(fd, resp, sizeof(resp)) > 0);
    ASSERT_EQ(401, status_code_of(resp));
    ASSERT(strstr(body_of(resp), "Unauthorized") != NULL);
    close(fd);
    harness_stop(h);
    PASS();
}

TEST keep_alive_multiple_requests_one_connection(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    int fd = connect_tcp(TEST_PORT);
    ASSERT(fd >= 0);

    const char *req1 = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";
    ASSERT(send(fd, req1, strlen(req1), 0) > 0);
    char resp[4096];
    ASSERT(recv_response(fd, resp, sizeof(resp)) > 0);
    ASSERT_EQ(200, status_code_of(resp));

    const char *req2 = "GET /groups/deadbeef HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ASSERT(send(fd, req2, strlen(req2), 0) > 0);
    ASSERT(recv_response(fd, resp, sizeof(resp)) > 0);
    ASSERT_EQ(200, status_code_of(resp));
    ASSERT(strstr(body_of(resp), "\"id\":\"deadbeef\"") != NULL);

    close(fd);
    harness_stop(h);
    PASS();
}

TEST unix_socket_serves_same_routes(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    int fd = connect_unix(TEST_UNIX_PATH);
    ASSERT(fd >= 0);
    const char *req = "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ASSERT(send(fd, req, strlen(req), 0) > 0);
    char resp[4096];
    ASSERT(recv_response(fd, resp, sizeof(resp)) > 0);
    ASSERT_EQ(200, status_code_of(resp));
    close(fd);
    harness_stop(h);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(get_with_query_param);
    RUN_TEST(path_param_extraction);
    RUN_TEST(post_body_roundtrip);
    RUN_TEST(not_found_fallback);
    RUN_TEST(middleware_can_short_circuit);
    RUN_TEST(keep_alive_multiple_requests_one_connection);
    RUN_TEST(unix_socket_serves_same_routes);
    GREATEST_MAIN_END();
}
