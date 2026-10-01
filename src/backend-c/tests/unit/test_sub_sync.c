/* Tests for mt_app_sync_subscription_by_id/mt_app_sync_due_subscriptions
 * (subscriptions.go's SyncSubscriptionByID/SyncDueSubscriptions ports) --
 * against a real local mt_httpd_t server acting as the stub subscription-
 * list host, same background-loop-thread harness as test_sub_fetch.c. The
 * mt_app_t under test is never "running" (mt_app_set_running is never
 * called), so ruleset enable/sync are no-ops here -- this exercises the
 * fetch/refresh/rebuild-array/rollback logic, not netfilter.
 */
#include "greatest.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "magitrickle/app.h"
#include "magitrickle/dns_cache.h"
#include "magitrickle/httpd.h"
#include "magitrickle/loop.h"
#include "magitrickle/sub_fetch.h"

#define TEST_PORT 18110

typedef struct list_ud {
    const char *body;
} list_ud_t;

static void h_list(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    list_ud_t *l = ud;
    mt_http_res_write(res, 200, "text/plain", (const uint8_t *)l->body, strlen(l->body));
}

static void h_notfound(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    (void)ud;
    static const char body[] = "nope";
    mt_http_res_write(res, 404, "text/plain", (const uint8_t *)body, sizeof(body) - 1);
}

/* Alternates content on every request -- lets a test drive
 * mt_app_sync_due_subscriptions through more than one real "rules
 * actually changed" cycle for the SAME subscription (regression
 * coverage for the leaked-superseded-rules bug found by the Phase 7
 * fault-injection soak, decisions.md D-36: only visible under
 * `make sanitize`, since a normal run has no way to observe the leak
 * functionally). */
static bool g_toggle_state = false;
static void h_toggle(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    (void)ud;
    g_toggle_state = !g_toggle_state;
    const char *body = g_toggle_state ? "one.example\ntwo.example" : "three.example";
    mt_http_res_write(res, 200, "text/plain", (const uint8_t *)body, strlen(body));
}

typedef struct harness {
    mt_loop_t *loop;
    mt_httpd_t *srv;
    pthread_t thread;
    mt_config_t cfg;
    mt_cache_t *cache;
    mt_app_t *app;
} harness_t;

static void *loop_thread(void *ud) {
    harness_t *h = ud;
    mt_loop_run(h->loop);
    return NULL;
}

static list_ud_t g_list_a = {.body = "one.example\ntwo.example"};
static list_ud_t g_list_b = {.body = "three.example"};

static harness_t *harness_start(void) {
    harness_t *h = calloc(1, sizeof(*h));
    mt_config_init_defaults(&h->cfg);
    h->cache = mt_cache_create(0);

    if (mt_loop_create(&h->loop) != MT_OK) { return NULL; }
    if (mt_httpd_create(h->loop, &h->srv) != MT_OK) { return NULL; }
    mt_httpd_route(h->srv, "GET", "/a", h_list, &g_list_a);
    mt_httpd_route(h->srv, "GET", "/b", h_list, &g_list_b);
    mt_httpd_route(h->srv, "GET", "/404", h_notfound, NULL);
    mt_httpd_route(h->srv, "GET", "/toggle", h_toggle, NULL);
    if (mt_httpd_listen_tcp(h->srv, "127.0.0.1", TEST_PORT) != MT_OK) { return NULL; }
    pthread_create(&h->thread, NULL, loop_thread, h);

    mt_app_deps_t deps = {.cfg = &h->cfg, .cache = h->cache};
    h->app = mt_app_create(&deps);
    return h;
}

static void harness_stop(harness_t *h) {
    mt_loop_stop(h->loop);
    pthread_join(h->thread, NULL);
    mt_httpd_destroy(h->srv);
    mt_loop_destroy(h->loop);
    mt_app_destroy(h->app);
    mt_cache_destroy(h->cache);
    mt_config_clear(&h->cfg);
    free(h);
}

static char g_url[128];
static const char *url_for(const char *path) {
    snprintf(g_url, sizeof(g_url), "http://127.0.0.1:%d%s", TEST_PORT, path);
    return g_url;
}

TEST first_sync_fetches_and_marks_changed(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    mt_subscription_t *sub = mt_subscription_new();
    sub->id = mt_id_random();
    mt_strset(&sub->iface, "eth0");
    mt_strset(&sub->url, url_for("/a"));
    sub->enable = true;
    ASSERT_EQ(MT_OK, mt_app_add_subscription(h->app, sub));
    mt_id_t id = sub->id;

    bool changed = false;
    ASSERT_EQ(MT_OK, mt_app_sync_subscription_by_id(h->app, id, 1000, NULL, &changed));
    ASSERT(changed);

    const mt_subscription_t *cur = mt_app_find_subscription_by_id(h->app, id);
    ASSERT(cur != NULL);
    ASSERT_EQ(2u, cur->n_rules);
    ASSERT_STR_EQ("one.example", cur->rules[0]->rule);
    ASSERT_STR_EQ("two.example", cur->rules[1]->rule);
    ASSERT_EQ((uint32_t)1000, cur->last_update);
    ASSERT_EQ((uint32_t)1000, cur->last_check);

    harness_stop(h);
    PASS();
}

TEST resync_same_content_is_unchanged_but_bumps_last_check(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    mt_subscription_t *sub = mt_subscription_new();
    sub->id = mt_id_random();
    mt_strset(&sub->iface, "eth0");
    mt_strset(&sub->url, url_for("/a"));
    sub->enable = true;
    ASSERT_EQ(MT_OK, mt_app_add_subscription(h->app, sub));
    mt_id_t id = sub->id;

    bool changed = true;
    ASSERT_EQ(MT_OK, mt_app_sync_subscription_by_id(h->app, id, 1000, NULL, &changed));
    ASSERT(changed);

    changed = true;
    ASSERT_EQ(MT_OK, mt_app_sync_subscription_by_id(h->app, id, 2000, NULL, &changed));
    ASSERT_FALSE(changed);

    const mt_subscription_t *cur = mt_app_find_subscription_by_id(h->app, id);
    ASSERT_EQ((uint32_t)1000, cur->last_update); /* unchanged */
    ASSERT_EQ((uint32_t)2000, cur->last_check);  /* always bumped */
    ASSERT_EQ(2u, cur->n_rules);

    harness_stop(h);
    PASS();
}

TEST resync_different_content_updates_rules_and_last_update(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    mt_subscription_t *sub = mt_subscription_new();
    sub->id = mt_id_random();
    mt_strset(&sub->iface, "eth0");
    mt_strset(&sub->url, url_for("/a"));
    sub->enable = true;
    ASSERT_EQ(MT_OK, mt_app_add_subscription(h->app, sub));
    mt_id_t id = sub->id;

    bool changed = false;
    ASSERT_EQ(MT_OK, mt_app_sync_subscription_by_id(h->app, id, 1000, NULL, &changed));
    ASSERT(changed);

    /* url_override points at a different list -> both url and rules change */
    changed = false;
    ASSERT_EQ(MT_OK, mt_app_sync_subscription_by_id(h->app, id, 2000, url_for("/b"), &changed));
    ASSERT(changed);

    const mt_subscription_t *cur = mt_app_find_subscription_by_id(h->app, id);
    ASSERT_EQ(1u, cur->n_rules);
    ASSERT_STR_EQ("three.example", cur->rules[0]->rule);
    ASSERT_STR_EQ(url_for("/b"), cur->url);
    ASSERT_EQ((uint32_t)2000, cur->last_update);
    ASSERT_EQ((uint32_t)2000, cur->last_check);

    harness_stop(h);
    PASS();
}

TEST unknown_id_is_noent(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    bool changed = false;
    mt_id_t bogus = mt_id_random();
    ASSERT_EQ(MT_ERR_NOENT, mt_app_sync_subscription_by_id(h->app, bogus, 1000, NULL, &changed));
    ASSERT_FALSE(changed);
    harness_stop(h);
    PASS();
}

TEST empty_url_and_no_override_is_inval(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    mt_subscription_t *sub = mt_subscription_new();
    sub->id = mt_id_random();
    mt_strset(&sub->iface, "eth0");
    sub->enable = true; /* url left NULL */
    ASSERT_EQ(MT_OK, mt_app_add_subscription(h->app, sub));
    mt_id_t id = sub->id;

    bool changed = true;
    ASSERT_EQ(MT_ERR_INVAL, mt_app_sync_subscription_by_id(h->app, id, 1000, NULL, &changed));
    ASSERT_FALSE(changed);
    harness_stop(h);
    PASS();
}

TEST fetch_failure_is_upstream_error(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    mt_subscription_t *sub = mt_subscription_new();
    sub->id = mt_id_random();
    mt_strset(&sub->iface, "eth0");
    mt_strset(&sub->url, url_for("/404"));
    sub->enable = true;
    ASSERT_EQ(MT_OK, mt_app_add_subscription(h->app, sub));
    mt_id_t id = sub->id;

    bool changed = true;
    ASSERT_EQ(MT_ERR_UPSTREAM, mt_app_sync_subscription_by_id(h->app, id, 1000, NULL, &changed));
    ASSERT_FALSE(changed);

    /* subscription left untouched by the failed attempt */
    const mt_subscription_t *cur = mt_app_find_subscription_by_id(h->app, id);
    ASSERT_EQ(0u, cur->n_rules);
    ASSERT_EQ((uint32_t)0, cur->last_check);

    harness_stop(h);
    PASS();
}

TEST due_subscriptions_are_fetched_and_others_skipped(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);

    mt_subscription_t *due = mt_subscription_new();
    due->id = mt_id_random();
    mt_strset(&due->iface, "eth0");
    mt_strset(&due->url, url_for("/a"));
    due->enable = true;
    due->interval = 60;
    ASSERT_EQ(MT_OK, mt_app_add_subscription(h->app, due));
    mt_id_t due_id = due->id;

    mt_subscription_t *not_due = mt_subscription_new();
    not_due->id = mt_id_random();
    mt_strset(&not_due->iface, "eth0");
    mt_strset(&not_due->url, url_for("/b"));
    not_due->enable = true;
    not_due->interval = 3600;
    not_due->last_check = 950; /* now(1000) - lastCheck(950) = 50s < 3600s interval */
    ASSERT_EQ(MT_OK, mt_app_add_subscription(h->app, not_due));
    mt_id_t not_due_id = not_due->id;

    bool any_changed = false;
    ASSERT_EQ(MT_OK, mt_app_sync_due_subscriptions(h->app, 1000, &any_changed));
    ASSERT(any_changed);

    const mt_subscription_t *due_cur = mt_app_find_subscription_by_id(h->app, due_id);
    ASSERT_EQ(2u, due_cur->n_rules);
    ASSERT_EQ((uint32_t)1000, due_cur->last_check);
    ASSERT_EQ((uint32_t)1000, due_cur->last_update);

    const mt_subscription_t *not_due_cur = mt_app_find_subscription_by_id(h->app, not_due_id);
    ASSERT_EQ(0u, not_due_cur->n_rules); /* never fetched */
    ASSERT_EQ((uint32_t)950, not_due_cur->last_check); /* untouched */

    harness_stop(h);
    PASS();
}

TEST due_but_unchanged_bumps_last_check_without_rebuild_flag(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    mt_subscription_t *sub = mt_subscription_new();
    sub->id = mt_id_random();
    mt_strset(&sub->iface, "eth0");
    mt_strset(&sub->url, url_for("/a"));
    sub->enable = true;
    sub->interval = 60;
    ASSERT_EQ(MT_OK, mt_app_add_subscription(h->app, sub));
    mt_id_t id = sub->id;

    /* Prime it with the same content it will refetch, via a manual sync. */
    bool changed = false;
    ASSERT_EQ(MT_OK, mt_app_sync_subscription_by_id(h->app, id, 500, NULL, &changed));
    ASSERT(changed);

    /* Not due yet at t=520 (interval 60, last_check 500). */
    bool any_changed = true;
    ASSERT_EQ(MT_OK, mt_app_sync_due_subscriptions(h->app, 520, &any_changed));
    ASSERT_FALSE(any_changed);

    /* Due at t=600; content on the server is unchanged -> no rules change,
     * but last_check still advances. */
    any_changed = true;
    ASSERT_EQ(MT_OK, mt_app_sync_due_subscriptions(h->app, 600, &any_changed));
    ASSERT_FALSE(any_changed);

    const mt_subscription_t *cur = mt_app_find_subscription_by_id(h->app, id);
    ASSERT_EQ((uint32_t)600, cur->last_check);
    ASSERT_EQ((uint32_t)500, cur->last_update);

    harness_stop(h);
    PASS();
}

/* Regression test for the leaked-superseded-rules bug (decisions.md
 * D-36): drives mt_app_sync_due_subscriptions through TWO real
 * content-change cycles for the same subscription. Functionally this
 * always passed (the bug only orphaned the old mt_sub_rule_t array,
 * never corrupted the live one) -- its value is as `make sanitize`
 * coverage: before the D-36 fix, this exact sequence leaked one
 * mt_sub_rule_t (with its strdup'd rule/type strings) per changed
 * cycle. */
TEST due_subscriptions_repeated_content_changes_leak_nothing(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    mt_subscription_t *sub = mt_subscription_new();
    sub->id = mt_id_random();
    mt_strset(&sub->iface, "eth0");
    mt_strset(&sub->url, url_for("/toggle"));
    sub->enable = true;
    sub->interval = 1;
    ASSERT_EQ(MT_OK, mt_app_add_subscription(h->app, sub));
    mt_id_t id = sub->id;

    bool any_changed = false;
    ASSERT_EQ(MT_OK, mt_app_sync_due_subscriptions(h->app, 1000, &any_changed));
    ASSERT(any_changed);
    const mt_subscription_t *cur = mt_app_find_subscription_by_id(h->app, id);
    ASSERT_EQ(2u, cur->n_rules); /* toggled to "one.example\ntwo.example" */

    any_changed = false;
    ASSERT_EQ(MT_OK, mt_app_sync_due_subscriptions(h->app, 1002, &any_changed));
    ASSERT(any_changed);
    cur = mt_app_find_subscription_by_id(h->app, id);
    ASSERT_EQ(1u, cur->n_rules); /* toggled back to "three.example" */
    ASSERT_STR_EQ("three.example", cur->rules[0]->rule);

    harness_stop(h);
    PASS();
}

TEST no_due_subscriptions_is_a_noop(void) {
    harness_t *h = harness_start();
    ASSERT(h != NULL);
    bool any_changed = true;
    ASSERT_EQ(MT_OK, mt_app_sync_due_subscriptions(h->app, 1000, &any_changed));
    ASSERT_FALSE(any_changed);
    harness_stop(h);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    mt_sub_fetch_global_init();
    GREATEST_MAIN_BEGIN();
    RUN_TEST(first_sync_fetches_and_marks_changed);
    RUN_TEST(resync_same_content_is_unchanged_but_bumps_last_check);
    RUN_TEST(resync_different_content_updates_rules_and_last_update);
    RUN_TEST(unknown_id_is_noent);
    RUN_TEST(empty_url_and_no_override_is_inval);
    RUN_TEST(fetch_failure_is_upstream_error);
    RUN_TEST(due_subscriptions_are_fetched_and_others_skipped);
    RUN_TEST(due_but_unchanged_bumps_last_check_without_rebuild_flag);
    RUN_TEST(due_subscriptions_repeated_content_changes_leak_nothing);
    RUN_TEST(no_due_subscriptions_is_a_noop);
    GREATEST_MAIN_END();
}
