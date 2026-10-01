/* add_years_utc vectors captured from the real Go time package
 * (time.Time.AddDate(20,0,0)) -- see phase-6-report.md for how. */
#include "greatest.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "magitrickle/auth.h"
#include "magitrickle/httpd.h"
#include "magitrickle/jwt.h"
#include "magitrickle/loop.h"

TEST add_years_matches_go(void) {
    ASSERT_EQ(2331152000, mt_auth_add_years_utc(1700000000, 20));
    ASSERT_EQ(631152000, mt_auth_add_years_utc(0, 20));
    ASSERT_EQ(1582934400, mt_auth_add_years_utc(951782400, 20));
    ASSERT_EQ(2214086400, mt_auth_add_years_utc(1582934400, 20));
    /* Feb 29 2080 (leap) + 20y -> 2100 (NOT leap, div100 not div400):
     * Go rolls over to March 1, matching this pure-arithmetic formula. */
    ASSERT_EQ(4107585600, mt_auth_add_years_utc(3476433600, 20));
    ASSERT_EQ(1551398399, mt_auth_add_years_utc(920246399, 20));
    PASS();
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    fputs(content, f);
    fclose(f);
}

#define SHADOW_FIXTURE "/tmp/mt_test_shadow"
#define PASSWD_FIXTURE "/tmp/mt_test_passwd"

TEST load_password_hash_cases(void) {
    write_file(SHADOW_FIXTURE,
              "# comment\n"
              "root:$6$abcdefghijklmnop$EC.xeLW9zNWcX0r23FSpQaV7PG.Ibd4QnLe3w6UC47i3/"
              "vkPQouEDwvUpGtqFiad5mzQG96cD/LywQiXv9WfH/:19000:0:99999:7:::\n"
              "nopass:x:19000:0:99999:7:::\n"
              "locked:*:19000:0:99999:7:::\n"
              "empty::19000:0:99999:7:::\n");

    char out[128];
    ASSERT_EQ(MT_OK, mt_auth_load_password_hash_from(SHADOW_FIXTURE, PASSWD_FIXTURE, "root", out,
                                                     sizeof(out)));
    ASSERT_STR_EQ(
        "$6$abcdefghijklmnop$EC.xeLW9zNWcX0r23FSpQaV7PG.Ibd4QnLe3w6UC47i3/"
        "vkPQouEDwvUpGtqFiad5mzQG96cD/LywQiXv9WfH/",
        out);

    ASSERT_EQ(MT_ERR_NOENT, mt_auth_load_password_hash_from(SHADOW_FIXTURE, PASSWD_FIXTURE,
                                                            "nopass", out, sizeof(out)));
    ASSERT_EQ(MT_ERR_NOENT, mt_auth_load_password_hash_from(SHADOW_FIXTURE, PASSWD_FIXTURE,
                                                            "locked", out, sizeof(out)));
    ASSERT_EQ(MT_ERR_NOENT, mt_auth_load_password_hash_from(SHADOW_FIXTURE, PASSWD_FIXTURE,
                                                            "empty", out, sizeof(out)));
    ASSERT_EQ(MT_ERR_NOENT, mt_auth_load_password_hash_from(SHADOW_FIXTURE, PASSWD_FIXTURE,
                                                            "ghost", out, sizeof(out)));

    unlink(SHADOW_FIXTURE);
    PASS();
}

TEST load_password_hash_falls_back_to_passwd(void) {
    unlink(SHADOW_FIXTURE); /* shadow missing entirely */
    write_file(PASSWD_FIXTURE, "admin:$1$abcdefgh$vhxKZ/s1ygZHyCEDPyqtQ/:1000:1000::/root:/bin/sh\n");

    char out[128];
    ASSERT_EQ(MT_OK, mt_auth_load_password_hash_from(SHADOW_FIXTURE, PASSWD_FIXTURE, "admin", out,
                                                     sizeof(out)));
    ASSERT_STR_EQ("$1$abcdefgh$vhxKZ/s1ygZHyCEDPyqtQ/", out);

    unlink(PASSWD_FIXTURE);
    PASS();
}

static char *mkdtemp_dup(const char *tmpl) {
    char *path = strdup(tmpl);
    return mkdtemp(path);
}

TEST authenticate_and_verify_round_trip(void) {
    write_file(SHADOW_FIXTURE,
              "admin:$6$abcdefghijklmnop$EC.xeLW9zNWcX0r23FSpQaV7PG.Ibd4QnLe3w6UC47i3/"
              "vkPQouEDwvUpGtqFiad5mzQG96cD/LywQiXv9WfH/:19000:0:99999:7:::\n");
    char *state_dir = mkdtemp_dup("/tmp/mt_auth_state_XXXXXX");
    ASSERT(state_dir != NULL);

    char token[MT_JWT_MAX_TOKEN];
    ASSERT_EQ(MT_OK, mt_auth_authenticate_from(SHADOW_FIXTURE, PASSWD_FIXTURE, state_dir, "admin",
                                               "hunter2", token, sizeof(token)));
    ASSERT(strlen(token) > 0);

    ASSERT_EQ(MT_OK, mt_auth_verify_token_from(SHADOW_FIXTURE, PASSWD_FIXTURE, state_dir, token));

    /* wrong password */
    ASSERT_EQ(MT_ERR_INVAL, mt_auth_authenticate_from(SHADOW_FIXTURE, PASSWD_FIXTURE, state_dir,
                                                      "admin", "wrongpw", token, sizeof(token)));
    /* unknown user */
    ASSERT_EQ(MT_ERR_NOENT, mt_auth_authenticate_from(SHADOW_FIXTURE, PASSWD_FIXTURE, state_dir,
                                                      "ghost", "hunter2", token, sizeof(token)));
    /* missing credentials */
    ASSERT_EQ(MT_ERR_INVAL, mt_auth_authenticate_from(SHADOW_FIXTURE, PASSWD_FIXTURE, state_dir,
                                                      "", "hunter2", token, sizeof(token)));

    unlink(SHADOW_FIXTURE);
    free(state_dir);
    PASS();
}

TEST verify_rejects_token_for_unknown_user(void) {
    /* A syntactically valid JWT whose subject doesn't exist in the
     * shadow fixture must fail verification (mirrors Go's Middleware:
     * parseTokenSubject's loadPasswordHash failure -> 401). */
    write_file(SHADOW_FIXTURE,
              "admin:$6$abcdefghijklmnop$EC.xeLW9zNWcX0r23FSpQaV7PG.Ibd4QnLe3w6UC47i3/"
              "vkPQouEDwvUpGtqFiad5mzQG96cD/LywQiXv9WfH/:19000:0:99999:7:::\n");
    char *state_dir = mkdtemp_dup("/tmp/mt_auth_state_XXXXXX");
    ASSERT(state_dir != NULL);

    char token[MT_JWT_MAX_TOKEN];
    ASSERT_EQ(MT_OK, mt_auth_authenticate_from(SHADOW_FIXTURE, PASSWD_FIXTURE, state_dir, "admin",
                                               "hunter2", token, sizeof(token)));

    /* Remove admin from the shadow fixture entirely: the same token's
     * subject can no longer be looked up, so verification must fail. */
    unlink(SHADOW_FIXTURE);
    write_file(SHADOW_FIXTURE, "someoneelse:$1$abcdefgh$vhxKZ/s1ygZHyCEDPyqtQ/:19000:0:99999:7:::\n");
    ASSERT_EQ(MT_ERR_INVAL,
             mt_auth_verify_token_from(SHADOW_FIXTURE, PASSWD_FIXTURE, state_dir, token));

    unlink(SHADOW_FIXTURE);
    free(state_dir);
    PASS();
}

/* Note: the process-lifetime secret cache (mt_auth_load_secret caches
 * after its first call, matching Go's sync.Once) means a single test
 * binary can only ever exercise the *first* state_dir it happens to load
 * a secret for -- Go itself never has more than one state_dir in a
 * process, so there is nothing meaningful to test about a second one
 * without forking a fresh process, which isn't worth it for this edge
 * case. */

/* ---- HTTP-level integration (status + login handlers over a real
 * server, matching the test_httpd.c pattern: real epoll loop on a
 * background pthread, this thread as a blocking client). ---- */

typedef struct auth_test_state {
    bool enabled;
    char state_dir[256];
} auth_test_state_t;

static bool test_auth_enabled(void *ud) {
    return ((auth_test_state_t *)ud)->enabled;
}
static const char *test_auth_state_dir(void *ud) {
    return ((auth_test_state_t *)ud)->state_dir;
}

typedef struct harness {
    mt_loop_t *loop;
    mt_httpd_t *tcp;
    pthread_t thread;
    auth_test_state_t state;
    mt_auth_ctx_t ctx;
} harness_t;

static void *loop_thread(void *ud) {
    harness_t *h = ud;
    mt_loop_run(h->loop);
    return NULL;
}

#define AUTH_TEST_PORT 18081

static harness_t *harness_start(bool enabled) {
    harness_t *h = calloc(1, sizeof(*h));
    h->state.enabled = enabled;
    char *dir = mkdtemp_dup("/tmp/mt_auth_http_XXXXXX");
    snprintf(h->state.state_dir, sizeof(h->state.state_dir), "%s", dir);
    free(dir);
    h->ctx.enabled = test_auth_enabled;
    h->ctx.state_dir = test_auth_state_dir;
    h->ctx.ud = &h->state;

    if (mt_loop_create(&h->loop) != MT_OK) { return NULL; }
    if (mt_httpd_create(h->loop, &h->tcp) != MT_OK) { return NULL; }
    mt_httpd_route(h->tcp, "GET", "/api/v1/auth", mt_auth_status_handler, &h->ctx);
    mt_httpd_route(h->tcp, "POST", "/api/v1/auth", mt_auth_login_handler, &h->ctx);
    mt_httpd_set_middleware(h->tcp, mt_auth_middleware, &h->ctx);
    if (mt_httpd_listen_tcp(h->tcp, "127.0.0.1", AUTH_TEST_PORT) != MT_OK) { return NULL; }

    pthread_create(&h->thread, NULL, loop_thread, h);
    return h;
}

static void harness_stop(harness_t *h) {
    mt_loop_stop(h->loop);
    pthread_join(h->thread, NULL);
    mt_httpd_destroy(h->tcp);
    mt_loop_destroy(h->loop);
    free(h);
}

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

TEST status_endpoint_reflects_enabled_flag(void) {
    harness_t *h = harness_start(true);
    ASSERT(h != NULL);
    int fd = connect_tcp(AUTH_TEST_PORT);
    ASSERT(fd >= 0);
    const char *req = "GET /api/v1/auth HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    send(fd, req, strlen(req), 0);
    char resp[2048];
    ASSERT(recv_response(fd, resp, sizeof(resp)) > 0);
    ASSERT_EQ(200, status_code_of(resp));
    ASSERT_STR_EQ("{\"enabled\":true}", body_of(resp));
    close(fd);
    harness_stop(h);
    PASS();
}

TEST login_disabled_returns_404(void) {
    harness_t *h = harness_start(false);
    ASSERT(h != NULL);
    int fd = connect_tcp(AUTH_TEST_PORT);
    ASSERT(fd >= 0);
    const char *body = "{\"login\":\"admin\",\"password\":\"x\"}";
    char req[512];
    snprintf(req, sizeof(req),
            "POST /api/v1/auth HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n"
            "Connection: close\r\n\r\n%s",
            strlen(body), body);
    send(fd, req, strlen(req), 0);
    char resp[2048];
    ASSERT(recv_response(fd, resp, sizeof(resp)) > 0);
    ASSERT_EQ(404, status_code_of(resp));
    close(fd);
    harness_stop(h);
    PASS();
}

static void unreachable_handler(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    (void)ud;
    /* Middleware must reject before this ever runs. */
    mt_http_res_write_error(res, 500, "should never be called");
}

TEST protected_route_requires_bearer_token(void) {
    harness_t *h = harness_start(true);
    ASSERT(h != NULL);
    mt_httpd_route(h->tcp, "GET", "/api/v1/groups", unreachable_handler, NULL);

    int fd = connect_tcp(AUTH_TEST_PORT);
    ASSERT(fd >= 0);
    const char *req = "GET /api/v1/groups HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    send(fd, req, strlen(req), 0);
    char resp[2048];
    ASSERT(recv_response(fd, resp, sizeof(resp)) > 0);
    ASSERT_EQ(401, status_code_of(resp));
    close(fd);
    harness_stop(h);
    PASS();
}

TEST auth_path_itself_is_exempt_even_when_enabled(void) {
    /* GET /api/v1/auth must work with no Authorization header even when
     * auth is enabled -- matches Go's explicit path == "/api/v1/auth"
     * exemption in http.go. */
    harness_t *h = harness_start(true);
    ASSERT(h != NULL);
    int fd = connect_tcp(AUTH_TEST_PORT);
    ASSERT(fd >= 0);
    const char *req = "GET /api/v1/auth HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    send(fd, req, strlen(req), 0);
    char resp[2048];
    ASSERT(recv_response(fd, resp, sizeof(resp)) > 0);
    ASSERT_EQ(200, status_code_of(resp));
    close(fd);
    harness_stop(h);
    PASS();
}

TEST bare_lf_request_line_endings_are_accepted(void) {
    /* The entware_kn ndm netfilter.d self-heal hook posts its request via a
     * shell here-doc, which emits bare-LF (not CRLF) line endings. Go's
     * net/http accepted those; the C server must too, or ndm's periodic
     * flush of the iptables tables on Keenetic is never healed. A POST with
     * a body exercises the header/body split on the bare-LF terminator. */
    harness_t *h = harness_start(false);
    ASSERT(h != NULL);
    int fd = connect_tcp(AUTH_TEST_PORT);
    ASSERT(fd >= 0);
    const char *body = "{\"login\":\"admin\",\"password\":\"x\"}";
    char req[512];
    snprintf(req, sizeof(req),
            "POST /api/v1/auth HTTP/1.1\nHost:\nContent-Type: application/json\n"
            "Content-Length: %zu\n\n%s",
            strlen(body), body);
    send(fd, req, strlen(req), 0);
    char resp[2048];
    /* Without bare-LF support the request never completes and recv times
     * out with nothing (0 bytes); with it the route is reached (404 here,
     * since login is disabled -- the point is that it was dispatched). */
    ASSERT(recv_response(fd, resp, sizeof(resp)) > 0);
    ASSERT_EQ(404, status_code_of(resp));
    close(fd);
    harness_stop(h);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(add_years_matches_go);
    RUN_TEST(load_password_hash_cases);
    RUN_TEST(load_password_hash_falls_back_to_passwd);
    RUN_TEST(authenticate_and_verify_round_trip);
    RUN_TEST(verify_rejects_token_for_unknown_user);
    RUN_TEST(status_endpoint_reflects_enabled_flag);
    RUN_TEST(login_disabled_returns_404);
    RUN_TEST(protected_route_requires_bearer_token);
    RUN_TEST(auth_path_itself_is_exempt_even_when_enabled);
    RUN_TEST(bare_lf_request_line_endings_are_accepted);
    GREATEST_MAIN_END();
}
