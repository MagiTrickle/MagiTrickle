/* mt_sub_fetch_list tests against a real local mt_httpd_t server (same
 * background-loop-thread harness pattern as test_httpd.c) -- exercises
 * the real libcurl request path, not a mock. */
#include "greatest.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "magitrickle/loop.h"
#include "magitrickle/httpd.h"
#include "magitrickle/sub_fetch.h"

#define TEST_PORT 18100

static void h_list(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    (void)ud;
    static const char body[] = "example.com\nexample.org";
    mt_http_res_write(res, 200, "text/plain", (const uint8_t *)body, sizeof(body) - 1);
}

static void h_empty(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    (void)ud;
    mt_http_res_write(res, 200, "text/plain", NULL, 0);
}

static void h_notfound_status(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    (void)ud;
    static const char body[] = "nope";
    mt_http_res_write(res, 404, "text/plain", (const uint8_t *)body, sizeof(body) - 1);
}

static char g_big_body[MT_SUB_FETCH_MAX_BODY_BYTES + 1024];

static void h_big(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    (void)ud;
    mt_http_res_write(res, 200, "text/plain", (const uint8_t *)g_big_body, sizeof(g_big_body));
}

typedef struct redirect_ud {
    int status;
    char location[128];
} redirect_ud_t;

static void h_redirect(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    redirect_ud_t *r = ud;
    mt_http_res_set_header(res, "Location", r->location);
    mt_http_res_write(res, r->status, NULL, NULL, 0);
}

typedef struct harness {
    mt_loop_t *loop;
    mt_httpd_t *srv;
    pthread_t thread;
    redirect_ud_t redirects[32];
    size_t n_redirects;
} harness_t;

static void *loop_thread(void *ud) {
    harness_t *h = ud;
    mt_loop_run(h->loop);
    return NULL;
}

static void add_redirect(harness_t *h, const char *path, int status, const char *location_path) {
    redirect_ud_t *r = &h->redirects[h->n_redirects++];
    r->status = status;
    snprintf(r->location, sizeof(r->location), "http://127.0.0.1:%d%s", TEST_PORT, location_path);
    mt_httpd_route(h->srv, "GET", path, h_redirect, r);
}

static harness_t *harness_start(void) {
    harness_t *h = calloc(1, sizeof(*h));
    memset(g_big_body, 'a', sizeof(g_big_body));

    if (mt_loop_create(&h->loop) != MT_OK) { return NULL; }
    if (mt_httpd_create(h->loop, &h->srv) != MT_OK) { return NULL; }

    mt_httpd_route(h->srv, "GET", "/list", h_list, NULL);
    mt_httpd_route(h->srv, "GET", "/empty", h_empty, NULL);
    mt_httpd_route(h->srv, "GET", "/notfound", h_notfound_status, NULL);
    mt_httpd_route(h->srv, "GET", "/big", h_big, NULL);

    add_redirect(h, "/redirect301", 301, "/list");
    add_redirect(h, "/redirect302", 302, "/list");
    add_redirect(h, "/loopA", 302, "/loopB");
    add_redirect(h, "/loopB", 302, "/loopA");
    /* exactly 5 hops (allowed): chain_ok0..chain_ok4 -> /list */
    add_redirect(h, "/chain_ok0", 302, "/chain_ok1");
    add_redirect(h, "/chain_ok1", 302, "/chain_ok2");
    add_redirect(h, "/chain_ok2", 302, "/chain_ok3");
    add_redirect(h, "/chain_ok3", 302, "/chain_ok4");
    add_redirect(h, "/chain_ok4", 302, "/list");
    /* 6 hops (one too many): chain_over0..chain_over5 -> /list */
    add_redirect(h, "/chain_over0", 302, "/chain_over1");
    add_redirect(h, "/chain_over1", 302, "/chain_over2");
    add_redirect(h, "/chain_over2", 302, "/chain_over3");
    add_redirect(h, "/chain_over3", 302, "/chain_over4");
    add_redirect(h, "/chain_over4", 302, "/chain_over5");
    add_redirect(h, "/chain_over5", 302, "/list");

    if (mt_httpd_listen_tcp(h->srv, "127.0.0.1", TEST_PORT) != MT_OK) { return NULL; }
    pthread_create(&h->thread, NULL, loop_thread, h);
    return h;
}

static void harness_stop(harness_t *h) {
    mt_loop_stop(h->loop);
    pthread_join(h->thread, NULL);
    mt_httpd_destroy(h->srv);
    mt_loop_destroy(h->loop);
    free(h);
}

static char g_url[128];
static const char *url_for(const char *path) {
    snprintf(g_url, sizeof(g_url), "http://127.0.0.1:%d%s", TEST_PORT, path);
    return g_url;
}

TEST fetches_plain_200(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    char *body = NULL;
    size_t len = 0;
    ASSERT_EQ(MT_OK, mt_sub_fetch_list(url_for("/list"), &body, &len));
    ASSERT_STR_EQ("example.com\nexample.org", body);
    ASSERT_EQ(strlen(body), len);
    free(body);
    harness_stop(h);
    PASS();
}

TEST fetches_empty_body(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    char *body = NULL;
    size_t len = (size_t)-1;
    ASSERT_EQ(MT_OK, mt_sub_fetch_list(url_for("/empty"), &body, &len));
    ASSERT(body != NULL);
    ASSERT_EQ(0u, len);
    ASSERT_STR_EQ("", body);
    free(body);
    harness_stop(h);
    PASS();
}

TEST follows_301_and_302(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    char *body = NULL;
    size_t len = 0;
    ASSERT_EQ(MT_OK, mt_sub_fetch_list(url_for("/redirect301"), &body, &len));
    ASSERT_STR_EQ("example.com\nexample.org", body);
    free(body);

    ASSERT_EQ(MT_OK, mt_sub_fetch_list(url_for("/redirect302"), &body, &len));
    ASSERT_STR_EQ("example.com\nexample.org", body);
    free(body);
    harness_stop(h);
    PASS();
}

TEST detects_redirect_loop(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    char *body = NULL;
    size_t len = 0;
    ASSERT_EQ(MT_ERR_INVAL, mt_sub_fetch_list(url_for("/loopA"), &body, &len));
    ASSERT(body == NULL);
    harness_stop(h);
    PASS();
}

TEST allows_exactly_five_redirects(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    char *body = NULL;
    size_t len = 0;
    ASSERT_EQ(MT_OK, mt_sub_fetch_list(url_for("/chain_ok0"), &body, &len));
    ASSERT_STR_EQ("example.com\nexample.org", body);
    free(body);
    harness_stop(h);
    PASS();
}

TEST rejects_six_redirects(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    char *body = NULL;
    size_t len = 0;
    ASSERT_EQ(MT_ERR_LIMIT, mt_sub_fetch_list(url_for("/chain_over0"), &body, &len));
    ASSERT(body == NULL);
    harness_stop(h);
    PASS();
}

TEST non_2xx_is_proto_error(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    char *body = NULL;
    size_t len = 0;
    ASSERT_EQ(MT_ERR_PROTO, mt_sub_fetch_list(url_for("/notfound"), &body, &len));
    ASSERT(body == NULL);
    harness_stop(h);
    PASS();
}

TEST oversized_body_is_limit_error(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    char *body = NULL;
    size_t len = 0;
    ASSERT_EQ(MT_ERR_LIMIT, mt_sub_fetch_list(url_for("/big"), &body, &len));
    ASSERT(body == NULL);
    harness_stop(h);
    PASS();
}

TEST unsupported_scheme_is_inval(void) {
    char *body = NULL;
    size_t len = 0;
    ASSERT_EQ(MT_ERR_INVAL, mt_sub_fetch_list("ftp://example.com/list.txt", &body, &len));
    ASSERT(body == NULL);
    PASS();
}

TEST malformed_url_is_inval(void) {
    char *body = NULL;
    size_t len = 0;
    ASSERT_EQ(MT_ERR_INVAL, mt_sub_fetch_list("not a url at all", &body, &len));
    ASSERT(body == NULL);
    PASS();
}

TEST connection_refused_is_io_error(void) {
    /* Nothing listens on this port -- exercises the network-failure path
     * distinctly from the protocol/limit/inval paths above. */
    char *body = NULL;
    size_t len = 0;
    ASSERT_EQ(MT_ERR_IO, mt_sub_fetch_list("http://127.0.0.1:1/list", &body, &len));
    ASSERT(body == NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    mt_sub_fetch_global_init();
    GREATEST_MAIN_BEGIN();
    RUN_TEST(fetches_plain_200);
    RUN_TEST(fetches_empty_body);
    RUN_TEST(follows_301_and_302);
    RUN_TEST(detects_redirect_loop);
    RUN_TEST(allows_exactly_five_redirects);
    RUN_TEST(rejects_six_redirects);
    RUN_TEST(non_2xx_is_proto_error);
    RUN_TEST(oversized_body_is_limit_error);
    RUN_TEST(unsupported_scheme_is_inval);
    RUN_TEST(malformed_url_is_inval);
    RUN_TEST(connection_refused_is_io_error);
    GREATEST_MAIN_END(); /* returns directly; process exit reclaims libcurl's
                          * global state, so mt_sub_fetch_global_cleanup()
                          * is skipped here (unreachable after MAIN_END,
                          * same as every other GREATEST_MAIN_END() call
                          * in this test suite). */
}
