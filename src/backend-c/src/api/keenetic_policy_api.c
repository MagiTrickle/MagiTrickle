/* Loop-owned policy API + one bounded, coalesced RCI worker. No HTTP or curl
 * operation runs on the DNS event loop. No route exists in non-Keenetic builds. */
#include "magitrickle/keenetic_policy_api.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <cjson/cJSON.h>
#include "magitrickle/log.h"

#define MAX_WAITERS 16u
#define REFRESH_MS 15000u

typedef struct {
    mt_http_deferred_t *response;
    bool save;
    mt_kn_policy_config_t desired, baseline;
} waiter_t;
struct mt_kn_policy_service {
    mt_loop_t *loop;
    mt_app_t *app;
    const char *config_path, *version; /* borrowed for service lifetime */
    mt_kn_policy_fetch_fn fetch;
    int notify[2], timer;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    bool thread_started, registered;
    atomic_bool stopping;
    bool requested, result_ready; /* under mu */
    mt_kn_policy_entry_t *result;
    size_t result_count;
    mt_err_t result_error;
    /* Only the loop accesses fields below. */
    bool inflight;
    waiter_t waiters[MAX_WAITERS];
    size_t n_waiters;
    mt_err_t last_error;
};

static cJSON *status_json(mt_kn_policy_service_t *s)
{
    mt_kn_policy_config_t c;
    bool ready, applied;
    mt_err_t apply_error;
    mt_app_policy_get(s->app, &c, &ready, &applied, &apply_error);
    cJSON *out = cJSON_CreateObject();
    cJSON *settings = out ? cJSON_AddObjectToObject(out, "settings") : NULL;
    cJSON *runtime = out ? cJSON_AddObjectToObject(out, "runtime") : NULL;
    if (!settings || !runtime || !cJSON_AddBoolToObject(settings, "enabled", c.enabled) ||
        !cJSON_AddStringToObject(settings, "policy_id", c.policy_id) ||
        !cJSON_AddStringToObject(settings, "mode", c.mode == MT_KN_POLICY_EXCLUDE ? "exclude" : "include")) {
        cJSON_Delete(out); return NULL;
    }
    const char *state = apply_error != MT_OK && apply_error != MT_ERR_CANCELED ? "error" :
                        !applied ? "applying" : !c.enabled ? "disabled" : ready ? "ready" : "unavailable";
    if (!cJSON_AddStringToObject(runtime, "state", state)) { cJSON_Delete(out); return NULL; }
    if (strcmp(state, "unavailable") == 0) {
        cJSON_AddStringToObject(runtime, "reason", s->last_error == MT_ERR_NOENT ? "POLICY_NOT_FOUND" : "RCI_UNAVAILABLE");
    }
    if (strcmp(state, "error") == 0) { cJSON_AddStringToObject(runtime, "reason", "NETFILTER_APPLY_FAILED"); }
    return out;
}
static cJSON *error_json(const char *code, const char *message)
{
    cJSON *out = cJSON_CreateObject();
    if (out && cJSON_AddStringToObject(out, "code", code) && cJSON_AddStringToObject(out, "error", message)) { return out; }
    cJSON_Delete(out); return NULL;
}
static const mt_kn_policy_entry_t *find_policy(const mt_kn_policy_entry_t *entries, size_t count, const char *id)
{
    for (size_t i = 0; i < count; i++) { if (strcmp(entries[i].id, id) == 0) { return &entries[i]; } }
    return NULL;
}
static void request_fetch(mt_kn_policy_service_t *s)
{
    if (s->inflight || atomic_load(&s->stopping)) { return; }
    s->inflight = true;
    pthread_mutex_lock(&s->mu);
    s->requested = true;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
}
static void *worker(void *ud)
{
    mt_kn_policy_service_t *s = ud;
    for (;;) {
        pthread_mutex_lock(&s->mu);
        while (!s->requested && !atomic_load(&s->stopping)) { pthread_cond_wait(&s->cv, &s->mu); }
        if (atomic_load(&s->stopping)) { pthread_mutex_unlock(&s->mu); break; }
        s->requested = false;
        pthread_mutex_unlock(&s->mu);
        mt_kn_policy_entry_t *entries = NULL;
        size_t count = 0;
        mt_err_t err = s->fetch(&entries, &count, &s->stopping);
        pthread_mutex_lock(&s->mu);
        s->result = entries; s->result_count = count; s->result_error = err; s->result_ready = true;
        pthread_mutex_unlock(&s->mu);
        ssize_t n;
        do { n = write(s->notify[1], "x", 1); } while (n < 0 && errno == EINTR);
    }
    return NULL;
}
static cJSON *apply_settings(mt_kn_policy_service_t *s, const mt_kn_policy_config_t *c,
                             const mt_kn_policy_entry_t *entry, int *status)
{
    mt_err_t err = mt_app_policy_set(s->app, c, entry && entry->available, entry ? entry->mark : 0);
    if (err == MT_ERR_INVAL) {
        *status = 400;
        return error_json("INVALID_POLICY_SETTINGS", "Policy settings or configured chain prefix are invalid");
    }
    /* Even a synchronous/test committer can fail after model publication.
     * Preserve the desired config and accurately report whether it was saved. */
    mt_err_t saved = s->config_path ? mt_app_save_config(s->app, s->config_path, s->version ? s->version : "") : MT_OK;
    cJSON *out = status_json(s);
    *status = 200;
    if (saved != MT_OK || err != MT_OK) {
        *status = 500;
        if (out) {
            cJSON_AddBoolToObject(out, "applied", true);
            cJSON_AddStringToObject(out, "code", saved != MT_OK ? "PERSISTENCE_FAILED" : "NETFILTER_APPLY_FAILED");
            cJSON_AddStringToObject(out, "error", saved != MT_OK ?
                "Failed to save configuration; settings are active only in memory. Retry saving." :
                "Failed to apply netfilter rules; check the system log and retry or disable the policy filter.");
        }
    }
    return out;
}
static void completed(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)loop; (void)events;
    mt_kn_policy_service_t *s = ud;
    char buf[64];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0 || (n < 0 && errno == EINTR)) { continue; }
        break;
    }
    pthread_mutex_lock(&s->mu);
    if (!s->result_ready) { pthread_mutex_unlock(&s->mu); return; }
    mt_kn_policy_entry_t *entries = s->result;
    size_t count = s->result_count;
    mt_err_t err = s->result_error;
    s->result = NULL; s->result_count = 0; s->result_ready = false;
    pthread_mutex_unlock(&s->mu);
    s->inflight = false;
    /* Reconcile the CURRENT setting, not the one that initiated the fetch.
     * Disabling during a fetch cannot be undone by its late completion. */
    mt_kn_policy_config_t current;
    mt_app_policy_get(s->app, &current, NULL, NULL, NULL);
    if (current.enabled) {
        const mt_kn_policy_entry_t *entry = err == MT_OK ? find_policy(entries, count, current.policy_id) : NULL;
        bool ready = entry && entry->available;
        mt_err_t next_error = err != MT_OK ? err : !entry ? MT_ERR_NOENT : !ready ? MT_ERR_AGAIN : MT_OK;
        if (s->last_error != next_error && next_error != MT_OK) {
            MT_WARN("Keenetic policy unavailable; MT scope is closed to new traffic: %s", mt_err_str(next_error));
        }
        s->last_error = next_error;
        (void)mt_app_policy_set(s->app, &current, ready, ready ? entry->mark : 0);
    }
    for (size_t i = 0; i < s->n_waiters; i++) {
        waiter_t *w = &s->waiters[i];
        cJSON *out = NULL;
        int status = 200;
        if (err != MT_OK) {
            status = 502;
            out = error_json("KEENETIC_RCI_UNAVAILABLE", "Cannot read Keenetic policies through the local RCI API");
        } else if (w->save) {
            mt_app_policy_get(s->app, &current, NULL, NULL, NULL);
            const mt_kn_policy_entry_t *entry = find_policy(entries, count, w->desired.policy_id);
            if (!mt_kn_policy_config_equal(&current, &w->baseline)) {
                status = 409; out = error_json("POLICY_SETTINGS_CHANGED", "Policy settings changed while loading; reload and retry");
            } else if (!entry || !entry->available) {
                status = 409;
                out = error_json(entry ? "POLICY_UNAVAILABLE" : "POLICY_NOT_FOUND",
                                 "The selected Keenetic policy is missing or has no usable mark");
            } else {
                s->last_error = MT_OK;
                out = apply_settings(s, &w->desired, entry, &status);
            }
        } else {
            out = cJSON_CreateObject();
            cJSON *array = out ? cJSON_AddArrayToObject(out, "policies") : NULL;
            if (!array) { cJSON_Delete(out); out = NULL; }
            for (size_t j = 0; out && j < count; j++) {
                cJSON *item = cJSON_CreateObject();
                if (!item || !cJSON_AddItemToArray(array, item)) { cJSON_Delete(item); cJSON_Delete(out); out = NULL; break; }
                if (!cJSON_AddStringToObject(item, "id", entries[j].id) ||
                    !cJSON_AddStringToObject(item, "name", entries[j].description) ||
                    !cJSON_AddBoolToObject(item, "available", entries[j].available)) {
                    cJSON_Delete(out); out = NULL; break;
                }
            }
        }
        if (out) { mt_http_deferred_json(w->response, status, out); }
        else { mt_http_deferred_error(w->response, 500, "out of memory"); }
    }
    s->n_waiters = 0;
    free(entries);
}
static void refresh(mt_loop_t *loop, void *ud)
{
    (void)loop;
    mt_kn_policy_service_t *s = ud;
    mt_kn_policy_config_t c;
    mt_app_policy_get(s->app, &c, NULL, NULL, NULL);
    if (c.enabled) { request_fetch(s); }
}
static void get_settings(mt_http_req_t *req, mt_http_res_t *res, void *ud)
{
    (void)req;
    cJSON *out = status_json(ud);
    if (out) { mt_http_res_write_json(res, 200, out); }
    else { mt_http_res_write_error(res, 500, "out of memory"); }
}
static void list_policies(mt_http_req_t *req, mt_http_res_t *res, void *ud)
{
    mt_kn_policy_service_t *s = ud;
    if (s->n_waiters == MAX_WAITERS) { mt_http_res_write_error(res, 503, "Policy discovery is busy; retry later"); return; }
    mt_http_deferred_t *response = mt_http_res_defer(req, res);
    if (!response) { mt_http_res_write_error(res, 500, "out of memory"); return; }
    s->waiters[s->n_waiters++] = (waiter_t){.response = response};
    request_fetch(s);
}
static bool parse_settings(mt_http_req_t *req, mt_kn_policy_config_t *config)
{
    size_t len;
    const uint8_t *body = mt_http_req_body(req, &len);
    if (!len || len > 4096 || memchr(body, 0, len)) { return false; }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)body, len, &end, false);
    while (end && end < (const char *)body + len && (*end == ' ' || *end == '\n' || *end == '\t' || *end == '\r')) { end++; }
    const cJSON *enable = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "policy_id");
    bool ok = cJSON_IsObject(root) && cJSON_GetArraySize(root) == 3 && end == (const char *)body + len &&
              cJSON_IsBool(enable) && cJSON_IsString(mode) && cJSON_IsString(id) &&
              strlen(id->valuestring) < sizeof(config->policy_id);
    if (ok) {
        config->enabled = cJSON_IsTrue(enable);
        if (strcmp(mode->valuestring, "include") == 0) { config->mode = MT_KN_POLICY_INCLUDE; }
        else if (strcmp(mode->valuestring, "exclude") == 0) { config->mode = MT_KN_POLICY_EXCLUDE; }
        else { ok = false; }
        strcpy(config->policy_id, id->valuestring);
        ok = ok && mt_kn_policy_config_valid(config);
    }
    cJSON_Delete(root);
    return ok;
}
static void put_settings(mt_http_req_t *req, mt_http_res_t *res, void *ud)
{
    mt_kn_policy_service_t *s = ud;
    mt_kn_policy_config_t desired = {0};
    if (!parse_settings(req, &desired)) {
        mt_http_res_write_error(res, 400, "Expected enabled (boolean), policy_id (string) and mode (include or exclude)"); return;
    }
    if (!desired.enabled) {
        /* Recovery switch never needs RCI and supersedes an in-flight enable. */
        size_t n = 0;
        for (size_t i = 0; i < s->n_waiters; i++) {
            if (s->waiters[i].save) { mt_http_deferred_error(s->waiters[i].response, 409, "Policy activation was canceled by disabling the filter"); }
            else { s->waiters[n++] = s->waiters[i]; }
        }
        s->n_waiters = n;
        int status;
        cJSON *out = apply_settings(s, &desired, NULL, &status);
        if (out) { mt_http_res_write_json(res, status, out); }
        else { mt_http_res_write_error(res, 500, "out of memory"); }
        return;
    }
    for (size_t i = 0; i < s->n_waiters; i++) {
        if (s->waiters[i].save) { mt_http_res_write_error(res, 409, "Another policy update is pending"); return; }
    }
    if (s->n_waiters == MAX_WAITERS) { mt_http_res_write_error(res, 503, "Policy discovery is busy; retry later"); return; }
    mt_http_deferred_t *response = mt_http_res_defer(req, res);
    if (!response) { mt_http_res_write_error(res, 500, "out of memory"); return; }
    waiter_t w = {.response = response, .save = true, .desired = desired};
    mt_app_policy_get(s->app, &w.baseline, NULL, NULL, NULL);
    s->waiters[s->n_waiters++] = w;
    request_fetch(s);
}
mt_err_t mt_kn_policy_register_routes(mt_httpd_t *httpd, mt_kn_policy_service_t *service)
{
    mt_err_t err = mt_httpd_route(httpd, "GET", "/api/v1/keenetic/policy", get_settings, service);
    if (err == MT_OK) { err = mt_httpd_route(httpd, "PUT", "/api/v1/keenetic/policy", put_settings, service); }
    if (err == MT_OK) { err = mt_httpd_route(httpd, "GET", "/api/v1/keenetic/policies", list_policies, service); }
    return err;
}
mt_err_t mt_kn_policy_service_create(mt_loop_t *loop, mt_app_t *app, const char *config_path,
                                      const char *version, mt_kn_policy_fetch_fn fetch,
                                      mt_kn_policy_service_t **out)
{
    *out = NULL;
    if (!loop || !app) { return MT_ERR_INVAL; }
    mt_kn_policy_service_t *s = calloc(1, sizeof(*s));
    if (!s) { return MT_ERR_NOMEM; }
    s->loop = loop; s->app = app; s->config_path = config_path; s->version = version;
    s->fetch = fetch ? fetch : mt_kn_policy_fetch;
    s->notify[0] = s->notify[1] = -1;
    atomic_init(&s->stopping, false);
    if (pthread_mutex_init(&s->mu, NULL) != 0) { free(s); return MT_ERR_SYS; }
    if (pthread_cond_init(&s->cv, NULL) != 0) { pthread_mutex_destroy(&s->mu); free(s); return MT_ERR_SYS; }
    if (pipe(s->notify) != 0) { mt_kn_policy_service_destroy(s); return MT_ERR_IO; }
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(s->notify[i], F_GETFL);
        if (flags < 0 || fcntl(s->notify[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(s->notify[i], F_SETFD, FD_CLOEXEC) < 0) {
            mt_kn_policy_service_destroy(s); return MT_ERR_IO;
        }
    }
    mt_err_t err = mt_loop_add_fd(loop, s->notify[0], EPOLLIN, completed, s);
    if (err != MT_OK) { mt_kn_policy_service_destroy(s); return err; }
    s->registered = true;
    if (pthread_create(&s->thread, NULL, worker, s) != 0) { mt_kn_policy_service_destroy(s); return MT_ERR_SYS; }
    s->thread_started = true;
    err = mt_loop_add_timer(loop, 1, REFRESH_MS, refresh, s, &s->timer);
    if (err != MT_OK) { mt_kn_policy_service_destroy(s); return err; }
    *out = s;
    return MT_OK;
}
void mt_kn_policy_service_destroy(mt_kn_policy_service_t *s)
{
    if (!s) { return; }
    atomic_store(&s->stopping, true);
    pthread_mutex_lock(&s->mu); pthread_cond_signal(&s->cv); pthread_mutex_unlock(&s->mu);
    if (s->thread_started) { pthread_join(s->thread, NULL); }
    if (s->timer) { mt_loop_del_timer(s->loop, s->timer); }
    if (s->registered) { mt_loop_del_fd(s->loop, s->notify[0]); }
    if (s->notify[0] >= 0) { close(s->notify[0]); }
    if (s->notify[1] >= 0) { close(s->notify[1]); }
    for (size_t i = 0; i < s->n_waiters; i++) { mt_http_deferred_error(s->waiters[i].response, 503, "Service is stopping"); }
    free(s->result);
    pthread_cond_destroy(&s->cv); pthread_mutex_destroy(&s->mu);
    free(s);
}
