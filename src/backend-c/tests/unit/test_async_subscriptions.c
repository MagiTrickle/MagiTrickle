#include "greatest.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "magitrickle/app.h"
#include "magitrickle/httpd.h"
#include "magitrickle/sub_fetch.h"

#define ASYNC_PORT 18903
#define ASYNC_URL "http://127.0.0.1:18903/list"

typedef struct fixture {
    mt_loop_t *loop;
    mt_httpd_t *server;
    mt_sub_fetcher_t *fetcher;
    mt_config_t cfg;
    mt_app_t *app;
    unsigned ticks, completed, expected, delayed_replies;
    mt_err_t error;
    bool changed;
} fixture_t;
static void stop_timeout(mt_loop_t *loop, void *ud) { (void)ud; mt_loop_stop(loop); }
static void tick(mt_loop_t *loop, void *ud) { (void)loop; ((fixture_t *)ud)->ticks++; }
static void list_handler(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req; (void)ud;
    static const char text[] = "one.example\ntwo.example\n";
    mt_http_res_write(res, 200, "text/plain", (const uint8_t *)text, sizeof(text) - 1);
}
typedef struct slow_response { mt_http_deferred_t *response; fixture_t *f; } slow_response_t;
static void delayed_reply(mt_loop_t *loop, void *ud) {
    (void)loop; slow_response_t *s = ud;
    s->f->delayed_replies++;
    mt_http_deferred_json(s->response, 200, cJSON_CreateString("ready"));
    free(s);
}
static void slow_handler(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    fixture_t *f = ud;
    slow_response_t *s = calloc(1, sizeof(*s));
    if (!s) { mt_http_res_write_error(res, 500, "out of memory"); return; }
    s->f = f; s->response = mt_http_res_defer(req, res);
    if (!s->response || mt_loop_add_timer(f->loop, 60, 0, delayed_reply, s, NULL) != MT_OK) {
        mt_http_res_cancel_defer(res); free(s); mt_http_res_write_error(res, 500, "timer failed");
    }
}
static bool setup(fixture_t *f) {
    memset(f, 0, sizeof(*f)); f->expected = 1;
    if (mt_config_init_defaults(&f->cfg) != MT_OK) { return false; }
    mt_app_deps_t deps = {.cfg = &f->cfg};
    f->app = mt_app_create(&deps);
    if (!f->app || mt_loop_create(&f->loop) != MT_OK ||
        mt_httpd_create(f->loop, &f->server) != MT_OK ||
        mt_httpd_listen_tcp(f->server, "127.0.0.1", ASYNC_PORT) != MT_OK ||
        mt_sub_fetcher_create(f->loop, &f->fetcher) != MT_OK) { return false; }
    mt_httpd_route(f->server, "GET", "/list", list_handler, f);
    mt_httpd_route(f->server, "GET", "/slow", slow_handler, f);
    mt_loop_add_timer(f->loop, 5, 5, tick, f, NULL);
    mt_loop_add_timer(f->loop, 2000, 0, stop_timeout, NULL, NULL);
    return true;
}
static void cleanup(fixture_t *f) {
    mt_sub_fetcher_destroy(f->fetcher);
    mt_httpd_destroy(f->server);
    mt_app_destroy(f->app); mt_config_clear(&f->cfg); mt_loop_destroy(f->loop);
}
static mt_subscription_t *subscription(unsigned id) {
    mt_subscription_t *s = mt_subscription_new();
    if (!s) { return NULL; }
    s->id.b[3] = (uint8_t)id; s->enable = true; s->interval = 1;
    mt_strset(&s->name, "async"); mt_strset(&s->iface, ""); mt_strset(&s->url, ASYNC_URL);
    return s;
}
static void raw_done(void *ud, mt_err_t err, const char *body, size_t len) {
    fixture_t *f = ud; f->error = err; f->completed++;
    f->changed = body && len > 0;
    mt_loop_stop(f->loop);
}
static void sync_done(void *ud, mt_id_t id, mt_err_t err, bool changed) {
    (void)id; fixture_t *f = ud;
    if (err != MT_OK) { f->error = err; }
    f->changed = f->changed || changed;
    f->completed++;
    if (f->completed == f->expected) { mt_loop_stop(f->loop); }
}
TEST slow_fetch_does_not_block_the_loop_serving_its_own_upstream(void) {
    fixture_t f; ASSERT(setup(&f));
    ASSERT_EQ(MT_OK, mt_sub_fetcher_submit(f.fetcher, "http://127.0.0.1:18903/slow", raw_done, &f));
    ASSERT_EQ(MT_OK, mt_loop_run(f.loop));
    ASSERT_EQ(1u, f.completed); ASSERT_EQ(MT_OK, f.error); ASSERT(f.changed);
    ASSERT(f.ticks >= 2); ASSERT_EQ(1u, f.delayed_replies);
    cleanup(&f); PASS();
}
TEST async_sync_applies_on_the_loop_and_rejects_duplicate_inflight_sync(void) {
    fixture_t f; ASSERT(setup(&f));
    mt_subscription_t *s = subscription(1); ASSERT(s); mt_id_t id = s->id;
    ASSERT_EQ(MT_OK, mt_app_add_subscription(f.app, s));
    ASSERT_EQ(MT_OK, mt_app_sync_subscription_async(f.app, f.fetcher, id, 100, NULL, sync_done, &f));
    ASSERT_EQ(MT_ERR_STATE, mt_app_sync_subscription_async(f.app, f.fetcher, id, 100, NULL, sync_done, &f));
    bool changed = false;
    ASSERT_EQ(MT_ERR_STATE, mt_app_sync_subscription_by_id(f.app, id, 100, NULL, &changed));
    ASSERT_EQ(MT_OK, mt_loop_run(f.loop));
    ASSERT_EQ(1u, f.completed); ASSERT_EQ(MT_OK, f.error); ASSERT(f.changed);
    s = f.cfg.subscriptions[0]; ASSERT_EQ(2u, s->n_rules); ASSERT_EQ(100u, s->last_check);
    ASSERT(!s->sync_pending); ASSERT_STR_EQ("one.example", s->rules[0]->rule);
    cleanup(&f); PASS();
}
TEST reload_or_replace_during_fetch_cannot_overwrite_new_subscription_state(void) {
    fixture_t f; ASSERT(setup(&f));
    mt_subscription_t *s = subscription(1); ASSERT(s); mt_id_t id = s->id;
    ASSERT_EQ(MT_OK, mt_app_add_subscription(f.app, s));
    ASSERT_EQ(MT_OK, mt_app_sync_subscription_async(f.app, f.fetcher, id, 100, NULL, sync_done, &f));
    mt_subscription_t **replacement = calloc(1, sizeof(*replacement)); ASSERT(replacement);
    replacement[0] = subscription(1); ASSERT(replacement[0]);
    mt_strset(&replacement[0]->url, "http://127.0.0.1:18903/edited");
    ASSERT_EQ(MT_OK, mt_app_replace_subscriptions(f.app, replacement, 1));
    ASSERT_EQ(MT_OK, mt_loop_run(f.loop));
    ASSERT_EQ(1u, f.completed); ASSERT_EQ(MT_ERR_STATE, f.error); ASSERT(!f.changed);
    ASSERT_EQ(0u, f.cfg.subscriptions[0]->n_rules);
    ASSERT_STR_EQ("http://127.0.0.1:18903/edited", f.cfg.subscriptions[0]->url);
    ASSERT(!f.cfg.subscriptions[0]->sync_pending);
    cleanup(&f); PASS();
}
TEST multiple_due_subscriptions_complete_independently(void) {
    fixture_t f; ASSERT(setup(&f)); f.expected = 2;
    ASSERT_EQ(MT_OK, mt_app_add_subscription(f.app, subscription(1)));
    ASSERT_EQ(MT_OK, mt_app_add_subscription(f.app, subscription(2)));
    ASSERT_EQ(MT_OK, mt_app_sync_due_subscriptions_async(f.app, f.fetcher, 100, sync_done, &f));
    ASSERT_EQ(MT_OK, mt_loop_run(f.loop));
    ASSERT_EQ(2u, f.completed); ASSERT_EQ(MT_OK, f.error);
    for (size_t i = 0; i < 2; i++) {
        ASSERT_EQ(2u, f.cfg.subscriptions[i]->n_rules); ASSERT(!f.cfg.subscriptions[i]->sync_pending);
    }
    cleanup(&f); PASS();
}
struct cancel_count { unsigned n; unsigned wrong; };
static void canceled(void *ud, mt_err_t err, const char *body, size_t len) {
    (void)body; (void)len; struct cancel_count *c = ud; c->n++;
    if (err != MT_ERR_CANCELED) { c->wrong++; }
}
TEST bounded_queue_and_shutdown_complete_every_accepted_job_exactly_once(void) {
    mt_loop_t *loop; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    mt_sub_fetcher_t *fetcher; ASSERT_EQ(MT_OK, mt_sub_fetcher_create(loop, &fetcher));
    struct cancel_count count = {0};
    for (unsigned i = 0; i < 32; i++) {
        ASSERT_EQ(MT_OK, mt_sub_fetcher_submit(fetcher, "http://127.0.0.1:9/", canceled, &count));
    }
    ASSERT_EQ(MT_ERR_LIMIT, mt_sub_fetcher_submit(fetcher, "http://127.0.0.1:9/", canceled, &count));
    mt_sub_fetcher_destroy(fetcher);
    ASSERT_EQ(32u, count.n); ASSERT_EQ(0u, count.wrong);
    mt_loop_destroy(loop); PASS();
}
static void destroy_server(mt_loop_t *loop, void *ud) {
    (void)loop; fixture_t *f = ud; mt_httpd_destroy(f->server); f->server = NULL;
}
TEST delayed_response_is_safe_after_server_and_connection_are_destroyed(void) {
    fixture_t f; ASSERT(setup(&f));
    int fd = socket(AF_INET, SOCK_STREAM, 0); ASSERT(fd >= 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(ASYNC_PORT)};
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    ASSERT_EQ(0, connect(fd, (struct sockaddr *)&sa, sizeof(sa)));
    const char request[] = "GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ((ssize_t)sizeof(request) - 1, send(fd, request, sizeof(request) - 1, 0));
    ASSERT_EQ(MT_OK, mt_loop_add_timer(f.loop, 20, 0, destroy_server, &f, NULL));
    ASSERT_EQ(MT_OK, mt_loop_add_timer(f.loop, 120, 0, stop_timeout, NULL, NULL));
    ASSERT_EQ(MT_OK, mt_loop_run(f.loop)); ASSERT_EQ(1u, f.delayed_replies);
    close(fd); cleanup(&f); PASS();
}
TEST saturated_due_queue_resumes_at_unserved_subscriptions(void) {
    fixture_t f; ASSERT(setup(&f));
    for (unsigned i = 1; i <= 40; i++) {
        ASSERT_EQ(MT_OK, mt_app_add_subscription(f.app, subscription(i)));
    }
    ASSERT_EQ(MT_ERR_LIMIT, mt_app_sync_due_subscriptions_async(f.app, f.fetcher, 100, sync_done, &f));
    ASSERT(f.cfg.subscriptions[0]->sync_pending);
    ASSERT(!f.cfg.subscriptions[32]->sync_pending);
    mt_sub_fetcher_destroy(f.fetcher); f.fetcher = NULL;
    ASSERT_EQ(MT_OK, mt_sub_fetcher_create(f.loop, &f.fetcher));
    ASSERT_EQ(MT_ERR_LIMIT, mt_app_sync_due_subscriptions_async(f.app, f.fetcher, 100, sync_done, &f));
    ASSERT(f.cfg.subscriptions[32]->sync_pending);
    ASSERT(f.cfg.subscriptions[39]->sync_pending);
    cleanup(&f); PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN(); mt_sub_fetch_global_init();
    RUN_TEST(slow_fetch_does_not_block_the_loop_serving_its_own_upstream);
    RUN_TEST(async_sync_applies_on_the_loop_and_rejects_duplicate_inflight_sync);
    RUN_TEST(reload_or_replace_during_fetch_cannot_overwrite_new_subscription_state);
    RUN_TEST(multiple_due_subscriptions_complete_independently);
    RUN_TEST(saturated_due_queue_resumes_at_unserved_subscriptions);
    RUN_TEST(bounded_queue_and_shutdown_complete_every_accepted_job_exactly_once);
    RUN_TEST(delayed_response_is_safe_after_server_and_connection_are_destroyed);
    mt_sub_fetch_global_cleanup(); GREATEST_MAIN_END();
}
