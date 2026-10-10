/* See subscriptions_api.h. Port of the non-fetch slice of
 * api/v1/subscription_handlers.go and subscription_converters.go. */
#include "magitrickle/subscriptions_api.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cjson/cJSON.h>

#include "magitrickle/id.h"
#include "magitrickle/json.h"
#include "magitrickle/lookup.h"
#include "magitrickle/log.h"
#include "magitrickle/sub_fetch.h"
#include "magitrickle/subparse.h"

/* ---- small JSON request-parsing helpers (see groups.c for the same
 * pattern; duplicated rather than shared, matching how converters.go's
 * GroupFromReq/SubscriptionFromReq don't share helpers in Go either) --- */

static mt_err_t parse_optional_id(const cJSON *obj, const char *key, mt_id_t *out, bool *out_present) {
    *out_present = false;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item || cJSON_IsNull(item)) { return MT_OK; }
    if (!cJSON_IsString(item) || mt_id_parse(item->valuestring, out) != MT_OK) { return MT_ERR_INVAL; }
    *out_present = true;
    return MT_OK;
}

static const char *get_string(const cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

static void get_optional_bool(const cJSON *obj, const char *key, bool *out_val, bool *out_present) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    *out_present = item && cJSON_IsBool(item);
    if (*out_present) { *out_val = cJSON_IsTrue(item); }
}

/* ---- request body -> models (subscription_converters.go equivalents) ------- */

static mt_sub_rule_t *sub_rule_from_req(const cJSON *req, const mt_lookup_t *baseline) {
    mt_id_t id;
    bool has_id;
    bool found = false;
    if (parse_optional_id(req, "id", &id, &has_id) == MT_OK && has_id) {
        found = mt_lookup_get(baseline, id.b, sizeof(id.b), NULL);
    }
    mt_sub_rule_t *rule = mt_sub_rule_new();
    if (!rule) { return NULL; }
    rule->id = found ? id : mt_id_random();
    if (mt_strset(&rule->rule, get_string(req, "rule")) != MT_OK ||
        mt_strset(&rule->type, get_string(req, "type")) != MT_OK) {
        mt_sub_rule_free(rule);
        return NULL;
    }
    cJSON *enable_j = cJSON_GetObjectItemCaseSensitive(req, "enable");
    rule->enable = cJSON_IsBool(enable_j) && cJSON_IsTrue(enable_j);
    return rule;
}

/* Sparse overrides carry the old values as optimistic preconditions. An
 * automatic refresh must not turn an edit into an overwrite of a newer rule.
 * Work only on the detached replacement; failure cannot partially edit live state. */
static mt_err_t apply_rule_changes(const cJSON *changes, mt_subscription_t *sub,
                                    const char **err_msg) {
    if (!cJSON_IsArray(changes)) { *err_msg = "invalid ruleChanges"; return MT_ERR_INVAL; }
    if (!changes->child) { return MT_OK; }
    mt_lookup_t by_id = {0}, seen = {0};
    mt_err_t err = MT_OK;
    for (size_t i = 0; i < sub->n_rules; i++) {
        err = mt_lookup_put(&by_id, sub->rules[i]->id.b, 4, i, NULL);
        if (err != MT_OK) { break; }
    }
    const cJSON *change;
    cJSON_ArrayForEach(change, changes) {
        if (err != MT_OK) { break; }
        mt_id_t id; bool present; size_t index;
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(change, "type");
        const cJSON *enable = cJSON_GetObjectItemCaseSensitive(change, "enable");
        const cJSON *previous_type = cJSON_GetObjectItemCaseSensitive(change, "previousType");
        const cJSON *previous_enable = cJSON_GetObjectItemCaseSensitive(change, "previousEnable");
        const cJSON *pattern = cJSON_GetObjectItemCaseSensitive(change, "rule");
        if (parse_optional_id(change, "id", &id, &present) != MT_OK || !present ||
            !cJSON_IsString(type) || !cJSON_IsBool(enable) || !cJSON_IsString(previous_type) ||
            !cJSON_IsBool(previous_enable) || !cJSON_IsString(pattern) ||
            mt_lookup_get(&seen, id.b, 4, NULL)) {
            *err_msg = "invalid or duplicate rule change"; err = MT_ERR_INVAL; break;
        }
        if (!mt_lookup_get(&by_id, id.b, 4, &index)) {
            *err_msg = "subscription rules changed; reload before saving"; err = MT_ERR_STATE; break;
        }
        mt_sub_rule_t *rule = sub->rules[index];
        if (strcmp(rule->rule ? rule->rule : "", pattern->valuestring) != 0 ||
            strcmp(rule->type ? rule->type : "", previous_type->valuestring) != 0 ||
            rule->enable != (bool)cJSON_IsTrue(previous_enable)) {
            *err_msg = "subscription rules changed; reload before saving"; err = MT_ERR_STATE; break;
        }
        err = mt_lookup_put(&seen, id.b, 4, index, NULL);
        if (err == MT_OK) { err = mt_strset(&rule->type, type->valuestring); }
        if (err == MT_OK) { rule->enable = cJSON_IsTrue(enable); }
    }
    mt_lookup_clear(&by_id); mt_lookup_clear(&seen);
    return err;
}

/* Builds a *new* mt_subscription_t from a SubscriptionReq JSON body --
 * matches Go's SubscriptionFromReq(req, existing). existing == NULL
 * mirrors Go's existing==nil (brand new subscription); existing != NULL
 * supplies the id/LastUpdate/LastCheck to seed and the baseline rules for
 * lenient nested rule-id reuse. Note Interval is NEVER seeded from
 * existing in Go (only ever taken from the request, defaulting to 0) --
 * faithfully NOT copied from existing here either, even though that
 * looks asymmetric next to LastUpdate/LastCheck. */
static mt_err_t subscription_from_req(mt_app_t *app, const cJSON *req, const mt_subscription_t *existing,
                                      mt_subscription_t **out, const char **err_msg) {
    mt_id_t req_id;
    bool has_req_id;
    if (parse_optional_id(req, "id", &req_id, &has_req_id) != MT_OK) {
        *err_msg = "invalid subscription id";
        return MT_ERR_INVAL;
    }
    if (existing && has_req_id && !mt_id_equal(existing->id, req_id)) {
        *err_msg = "subscription ID mismatch";
        return MT_ERR_INVAL;
    }

    uint16_t priority = existing ? existing->priority : MT_SUBSCRIPTION_DEFAULT_PRIORITY;
    if (mt_json_parse_priority(req, &priority) != MT_OK) {
        *err_msg = "priority must be an integer between 1 and 999";
        return MT_ERR_INVAL;
    }

    mt_subscription_t *sub = mt_subscription_new();
    if (!sub) { return MT_ERR_NOMEM; }
    sub->priority = priority;
    if (existing) {
        sub->id = existing->id;
        sub->last_update = existing->last_update;
        sub->last_check = existing->last_check;
    } else {
        sub->id = has_req_id ? req_id : mt_id_random();
    }

    mt_err_t err = mt_strset(&sub->name, get_string(req, "name"));
    if (err == MT_OK) { err = mt_strset(&sub->iface, get_string(req, "interface")); }
    const cJSON *profile = cJSON_GetObjectItemCaseSensitive(req, "profile");
    if (profile && !cJSON_IsNull(profile) && !cJSON_IsString(profile)) { err = MT_ERR_INVAL; }
    if (err == MT_OK) { err = mt_strset(&sub->profile, get_string(req, "profile")); }
    if (err == MT_OK) { err = mt_app_normalize_route(app, sub->profile, &sub->iface); }
    if (err != MT_OK) { *err_msg = "invalid or missing routing profile"; }
    if (err == MT_OK) { err = mt_strset(&sub->url, get_string(req, "url")); }
    if (err != MT_OK) {
        mt_subscription_free(sub);
        return err;
    }

    bool enable_present, enable_val;
    get_optional_bool(req, "enable", &enable_val, &enable_present);
    sub->enable = enable_present ? enable_val : true;

    cJSON *interval_j = cJSON_GetObjectItemCaseSensitive(req, "interval");
    if (cJSON_IsNumber(interval_j)) { sub->interval = (uint32_t)interval_j->valuedouble; }
    cJSON *last_update_j = cJSON_GetObjectItemCaseSensitive(req, "lastUpdate");
    if (cJSON_IsNumber(last_update_j)) { sub->last_update = (uint32_t)last_update_j->valuedouble; }

    cJSON *rules_j = cJSON_GetObjectItemCaseSensitive(req, "rules");
    if (rules_j && !cJSON_IsNull(rules_j)) {
        if (!cJSON_IsArray(rules_j)) {
            mt_subscription_free(sub);
            *err_msg = "invalid rules";
            return MT_ERR_INVAL;
        }
        mt_sub_rule_t **baseline = existing ? existing->rules : NULL;
        size_t n_baseline = existing ? existing->n_rules : 0;
        if ((size_t)cJSON_GetArraySize(rules_j) > MT_SUB_MAX_RULES) {
            mt_subscription_free(sub); *err_msg = "too many subscription rules"; return MT_ERR_LIMIT;
        }
        mt_lookup_t baseline_ids = {0};
        for (size_t i = 0; i < n_baseline && err == MT_OK; i++) {
            err = mt_lookup_put(&baseline_ids, baseline[i]->id.b, 4, i, NULL);
        }
        const cJSON *item;
        cJSON_ArrayForEach(item, rules_j) {
            if (err != MT_OK) { break; }
            mt_sub_rule_t *r = sub_rule_from_req(item, &baseline_ids);
            err = r ? mt_subscription_add_rule(sub, r) : MT_ERR_NOMEM;
            if (err != MT_OK) { mt_sub_rule_free(r); }
        }
        mt_lookup_clear(&baseline_ids);
        if (err != MT_OK) { mt_subscription_free(sub); return err; }
    } else if (existing) {
        for (size_t i = 0; i < existing->n_rules; i++) {
            mt_sub_rule_t *r = mt_sub_rule_new();
            mt_err_t e = r ? MT_OK : MT_ERR_NOMEM;
            if (e == MT_OK) { r->id = existing->rules[i]->id; }
            if (e == MT_OK) { e = mt_strset(&r->rule, existing->rules[i]->rule); }
            if (e == MT_OK) { e = mt_strset(&r->type, existing->rules[i]->type); }
            if (e == MT_OK) { r->enable = existing->rules[i]->enable; }
            if (e != MT_OK || mt_subscription_add_rule(sub, r) != MT_OK) {
                mt_sub_rule_free(r);
                mt_subscription_free(sub);
                return e != MT_OK ? e : MT_ERR_NOMEM;
            }
        }
    }
    cJSON *changes = cJSON_GetObjectItemCaseSensitive(req, "ruleChanges");
    if (changes) {
        if (!existing) {
            mt_subscription_free(sub); *err_msg = "subscription changed; reload before saving";
            return MT_ERR_STATE;
        }
        if (rules_j && !cJSON_IsNull(rules_j)) {
            mt_subscription_free(sub); *err_msg = "rules and ruleChanges are mutually exclusive";
            return MT_ERR_INVAL;
        }
        err = apply_rule_changes(changes, sub, err_msg);
        if (err != MT_OK) { mt_subscription_free(sub); return err; }
    }
    *out = sub;
    return MT_OK;
}

/* Silent ID-uniqueness fixup -- matches Go's ensureUniqueSubscriptionIDs/
 * ensureUniqueSubscriptionRuleIDs exactly: despite the `error` return
 * type in Go, neither function can actually fail; a zero or duplicate id
 * (checked against every earlier entry only, same as Go's incrementally-
 * built `dup` map) is silently replaced with a fresh random one. Not a
 * validation step. */
static void ensure_unique_subscription_ids(mt_subscription_t **subs, size_t n) {
    for (size_t i = 0; i < n; i++) {
        bool exists = mt_id_is_zero(subs[i]->id);
        for (size_t j = 0; !exists && j < i; j++) {
            if (mt_id_equal(subs[j]->id, subs[i]->id)) { exists = true; }
        }
        while (exists) {
            subs[i]->id = mt_id_random();
            exists = false;
            for (size_t j = 0; j < i; j++) {
                if (mt_id_equal(subs[j]->id, subs[i]->id)) {
                    exists = true;
                    break;
                }
            }
        }
    }
}

static mt_err_t ensure_unique_subscription_rule_ids(mt_subscription_t *sub) {
    return mt_sub_rules_fix_ids(sub->rules, sub->n_rules);
}

/* ---- models -> response JSON ------------------------------------------------ */

static cJSON *sub_rule_to_json(const mt_sub_rule_t *r) {
    char id_buf[MT_ID_STR_LEN];
    mt_id_format(r->id, id_buf);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", id_buf);
    cJSON_AddStringToObject(obj, "rule", r->rule ? r->rule : "");
    cJSON_AddStringToObject(obj, "type", r->type ? r->type : "");
    cJSON_AddBoolToObject(obj, "enable", r->enable);
    return obj;
}

static cJSON *sub_rules_to_json_array(mt_sub_rule_t **rules, size_t n) {
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) { cJSON_AddItemToArray(arr, sub_rule_to_json(rules[i])); }
    return arr;
}

/* Always includes "rules" (matches RespFromSubscription(sub, true), the
 * only call site Go's GetSubscriptions ever uses -- there is no
 * with_rules query param and no single-subscription GET endpoint). */
static cJSON *subscription_to_json(const mt_subscription_t *s) {
    char id_buf[MT_ID_STR_LEN];
    mt_id_format(s->id, id_buf);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", id_buf);
    cJSON_AddStringToObject(obj, "name", s->name ? s->name : "");
    cJSON_AddStringToObject(obj, "interface", s->iface ? s->iface : "");
    if (s->profile && *s->profile) { cJSON_AddStringToObject(obj, "profile", s->profile); }
    cJSON_AddBoolToObject(obj, "enable", s->enable);
    cJSON_AddNumberToObject(obj, "priority", s->priority);
    cJSON_AddStringToObject(obj, "url", s->url ? s->url : "");
    cJSON_AddNumberToObject(obj, "interval", s->interval);
    cJSON_AddNumberToObject(obj, "lastUpdate", s->last_update);
    cJSON_AddItemToObject(obj, "rules", sub_rules_to_json_array(s->rules, s->n_rules));
    return obj;
}

static cJSON *status_ok_json(void) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "status", "ok");
    return obj;
}

/* ---- HTTP glue --------------------------------------------------------------- */

static void must_route(mt_httpd_t *h, const char *method, const char *pattern, mt_http_handler_fn fn,
                       void *ud) {
    if (mt_httpd_route(h, method, pattern, fn, ud) != MT_OK) {
        MT_ERROR("failed to register route %s %s", method, pattern);
    }
}

/* Subscriptions default to SAVING unless ?save=false is explicit --
 * matches Go's `r.URL.Query().Get("save") != "false"`, the opposite
 * default of the groups handlers' `== "true"` (see subscriptions_api.h). */
static mt_err_t maybe_save(mt_subs_ctx_t *ctx, mt_http_req_t *req) {
    const char *save = mt_http_req_query(req, "save");
    if ((save && strcmp(save, "false") == 0) || !ctx->config_path) { return MT_OK; }
    mt_err_t err = mt_app_save_config(ctx->app, ctx->config_path,
                                     ctx->config_version ? ctx->config_version : "");
    if (err != MT_OK) { MT_ERROR("failed to save config file: %s", mt_err_str(err)); }
    return err;
}

static cJSON *parse_body_json(mt_http_req_t *req) {
    size_t body_len;
    const uint8_t *body = mt_http_req_body(req, &body_len);
    return body_len > 0 ? cJSON_ParseWithLength((const char *)body, body_len) : NULL;
}

static void handle_get_subscriptions(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    mt_subs_ctx_t *ctx = ud;
    cJSON *out = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(out, "subscriptions");
    size_t n = mt_app_subscription_count(ctx->app);
    for (size_t i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, subscription_to_json(mt_app_subscription_at(ctx->app, i)));
    }
    mt_http_res_write_json(res, 200, out);
}

static void handle_put_subscriptions(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_subs_ctx_t *ctx = ud;
    cJSON *json = parse_body_json(req);
    if (!json) {
        mt_http_res_write_error(res, 400, "failed to parse request");
        return;
    }
    cJSON *subs_j = cJSON_GetObjectItemCaseSensitive(json, "subscriptions");
    if (!subs_j || cJSON_IsNull(subs_j) || !cJSON_IsArray(subs_j)) {
        cJSON_Delete(json);
        mt_http_res_write_error(res, 400, "no subscriptions in request");
        return;
    }

    int n = cJSON_GetArraySize(subs_j);
    mt_subscription_t **new_subs = n > 0 ? calloc((size_t)n, sizeof(*new_subs)) : NULL;
    if (n > 0 && !new_subs) {
        cJSON_Delete(json);
        mt_http_res_write_error(res, 500, "out of memory");
        return;
    }
    for (int i = 0; i < n; i++) {
        cJSON *sub_req = cJSON_GetArrayItem(subs_j, i);
        const char *url = get_string(sub_req, "url");
        if (url[0] == '\0') {
            cJSON_Delete(json);
            for (int j = 0; j < i; j++) { mt_subscription_free(new_subs[j]); }
            free(new_subs);
            mt_http_res_write_error(res, 400, "subscription url is required");
            return;
        }
        mt_id_t wanted_id;
        bool has_id;
        const mt_subscription_t *existing = NULL;
        if (parse_optional_id(sub_req, "id", &wanted_id, &has_id) == MT_OK && has_id) {
            existing = mt_app_find_subscription_by_id(ctx->app, wanted_id);
        }
        const char *err_msg = "invalid subscription";
        mt_err_t err = subscription_from_req(ctx->app, sub_req, existing, &new_subs[i], &err_msg);
        if (err != MT_OK) {
            cJSON_Delete(json);
            for (int j = 0; j < i; j++) { mt_subscription_free(new_subs[j]); }
            free(new_subs);
            mt_http_res_write_error(res, err == MT_ERR_STATE ? 409 : err == MT_ERR_LIMIT ? 413 : 400, err_msg);
            return;
        }
    }
    cJSON_Delete(json);

    ensure_unique_subscription_ids(new_subs, (size_t)n);
    for (int i = 0; i < n; i++) {
        mt_err_t id_err = ensure_unique_subscription_rule_ids(new_subs[i]);
        if (id_err != MT_OK) {
            for (int j = 0; j < n; j++) { mt_subscription_free(new_subs[j]); }
            free(new_subs);
            mt_http_res_write_error(res, 500, mt_err_str(id_err)); return;
        }
    }

    mt_err_t err = mt_app_replace_subscriptions(ctx->app, new_subs, (size_t)n); /* always takes ownership */
    if (err != MT_OK) {
        mt_http_res_write_error(res, 500, mt_err_str(err));
        return;
    }
    if (maybe_save(ctx, req) != MT_OK) {
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "error", "failed to save config file; changes are active only in memory");
        cJSON_AddStringToObject(out, "code", "PERSISTENCE_FAILED");
        cJSON_AddBoolToObject(out, "applied", true);
        cJSON *arr = cJSON_AddArrayToObject(out, "subscriptions");
        for (size_t i = 0; i < mt_app_subscription_count(ctx->app); i++) {
            cJSON_AddItemToArray(arr, subscription_to_json(mt_app_subscription_at(ctx->app, i)));
        }
        mt_http_res_write_json(res, 500, out);
        return;
    }
    mt_http_res_write_json(res, 200, status_ok_json());
}

typedef struct pending_create {
    mt_subs_ctx_t *ctx;
    mt_subscription_t *sub;
    mt_http_deferred_t *response;
    bool save;
} pending_create_t;

static void create_fetched(void *ud, mt_err_t err, mt_sub_rule_t **rules, size_t n) {
    pending_create_t *p = ud;
    if (err != MT_OK) {
        mt_sub_rules_free(rules, n);
        mt_http_deferred_error(p->response, err == MT_ERR_UPSTREAM ? 502 : err == MT_ERR_LIMIT ? 413 : 500,
                              err == MT_ERR_UPSTREAM ? "subscription fetch failed" : mt_err_str(err));
        mt_subscription_free(p->sub); free(p); return;
    }
    p->sub->rules = rules; p->sub->n_rules = n;
    p->sub->last_check = p->sub->last_update = (uint32_t)time(NULL);
    mt_id_t id = p->sub->id;
    err = mt_app_add_subscription(p->ctx->app, p->sub); /* always takes ownership */
    p->sub = NULL;
    if (err != MT_OK) {
        mt_http_deferred_error(p->response, err == MT_ERR_EXIST ? 409 : 500, mt_err_str(err));
        free(p); return;
    }
    if (p->save && p->ctx->config_path) {
        err = mt_app_save_config(p->ctx->app, p->ctx->config_path,
                                p->ctx->config_version ? p->ctx->config_version : "");
        if (err != MT_OK) {
            bool found;
            mt_err_t rollback = mt_app_remove_subscription_by_id(p->ctx->app, id, &found);
            MT_ERROR("failed to save created subscription: %s; rollback=%s", mt_err_str(err), mt_err_str(rollback));
            mt_http_deferred_error(p->response, 500, rollback == MT_OK ?
                "failed to save config file; subscription was not added" :
                "failed to save config file; rollback failed; reload current state");
            free(p); return;
        }
    }
    const mt_subscription_t *sub = mt_app_find_subscription_by_id(p->ctx->app, id);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "subscription", subscription_to_json(sub));
    mt_http_deferred_json(p->response, 200, out);
    free(p);
}

/* ?fetch=true creates from URL+settings; no client round-trip of the rule array.
 * Detached work is committed only after fetch/parse succeeds. */
static void begin_create(mt_subs_ctx_t *ctx, mt_subscription_t *sub,
                          mt_http_req_t *req, mt_http_res_t *res) {
    if (!ctx->fetcher) {
        mt_subscription_free(sub);
        mt_http_res_write_error(res, 503, "subscription worker unavailable"); return;
    }
    pending_create_t *p = calloc(1, sizeof(*p));
    if (!p) { mt_subscription_free(sub); mt_http_res_write_error(res, 500, "out of memory"); return; }
    p->ctx = ctx; p->sub = sub;
    const char *save = mt_http_req_query(req, "save");
    p->save = !(save && strcmp(save, "false") == 0);
    p->response = mt_http_res_defer(req, res);
    mt_err_t err = p->response ? mt_sub_fetcher_submit_rules(ctx->fetcher, sub->url, create_fetched, p)
                               : MT_ERR_NOMEM;
    if (err != MT_OK) {
        mt_http_res_cancel_defer(res);
        mt_subscription_free(sub); free(p);
        mt_http_res_write_error(res, err == MT_ERR_LIMIT ? 503 : 500, mt_err_str(err));
    }
}

static void handle_create_subscription(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_subs_ctx_t *ctx = ud;
    cJSON *json = parse_body_json(req);
    if (!json) {
        mt_http_res_write_error(res, 400, "failed to parse request");
        return;
    }
    const char *url = get_string(json, "url");
    if (url[0] == '\0') {
        cJSON_Delete(json);
        mt_http_res_write_error(res, 400, "subscription url is required");
        return;
    }

    const char *fetch = mt_http_req_query(req, "fetch");
    bool fetch_rules = fetch && strcmp(fetch, "true") == 0;
    const cJSON *client_rules = cJSON_GetObjectItemCaseSensitive(json, "rules");
    if (fetch_rules && client_rules && !cJSON_IsNull(client_rules)) {
        cJSON_Delete(json); mt_http_res_write_error(res, 400, "fetch=true does not accept rules"); return;
    }
    mt_subscription_t *sub = NULL;
    const char *err_msg = "invalid subscription";
    mt_err_t err = subscription_from_req(ctx->app, json, NULL, &sub, &err_msg);
    cJSON_Delete(json);
    if (err != MT_OK) {
        mt_http_res_write_error(res, 400, err_msg);
        return;
    }
    if (fetch_rules) { begin_create(ctx, sub, req, res); return; }
    err = ensure_unique_subscription_rule_ids(sub);
    if (err != MT_OK) { mt_subscription_free(sub); mt_http_res_write_error(res, 500, mt_err_str(err)); return; }

    err = mt_app_add_subscription(ctx->app, sub); /* always takes ownership */
    if (err != MT_OK) {
        int status = err == MT_ERR_EXIST ? 409 : 500;
        mt_http_res_write_error(res, status, mt_err_str(err));
        return;
    }
    if (maybe_save(ctx, req) != MT_OK) {
        mt_http_res_write_error(res, 500, "failed to save config file; changes are active only in memory");
        return;
    }
    mt_http_res_write_json(res, 200, status_ok_json());
}

static void handle_delete_subscription(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_subs_ctx_t *ctx = ud;
    const char *id_str = mt_http_req_param(req, "subscriptionID");
    mt_id_t id;
    if (!id_str || mt_id_parse(id_str, &id) != MT_OK) {
        mt_http_res_write_error(res, 400, "invalid subscription id");
        return;
    }
    bool found = false;
    mt_err_t err = mt_app_remove_subscription_by_id(ctx->app, id, &found);
    if (err != MT_OK) {
        mt_http_res_write_error(res, 500, mt_err_str(err));
        return;
    }
    if (!found) {
        mt_http_res_write_error(res, 404, "subscription not found");
        return;
    }
    if (maybe_save(ctx, req) != MT_OK) {
        mt_http_res_write_error(res, 500, "failed to save config file; changes are active only in memory");
        return;
    }
    mt_http_res_write_json(res, 200, status_ok_json());
}

static int sync_status(mt_err_t err) {
    switch (err) {
    case MT_ERR_NOENT: return 404;
    case MT_ERR_INVAL: return 400;
    case MT_ERR_UPSTREAM: return 502;
    case MT_ERR_STATE: return 409;
    case MT_ERR_LIMIT: return 503;
    default: return 500;
    }
}

static const char *sync_message(mt_err_t err) {
    switch (err) {
    case MT_ERR_NOENT: return "subscription not found";
    case MT_ERR_INVAL: return "subscription invalid";
    case MT_ERR_UPSTREAM: return "subscription fetch failed";
    case MT_ERR_STATE: return "subscription changed or sync already pending";
    default: return mt_err_str(err);
    }
}

typedef struct pending_sync {
    mt_subs_ctx_t *ctx;
    mt_http_deferred_t *response;
    bool save;
} pending_sync_t;

static void sync_finished(void *ud, mt_id_t id, mt_err_t err, bool changed) {
    pending_sync_t *p = ud;
    if (err != MT_OK) { mt_http_deferred_error(p->response, sync_status(err), sync_message(err)); free(p); return; }
    /* Saving even an unchanged manual sync permits retry after a prior disk error. */
    (void)changed;
    if (p->save && p->ctx->config_path) {
        err = mt_app_save_config(p->ctx->app, p->ctx->config_path,
                                p->ctx->config_version ? p->ctx->config_version : "");
        if (err != MT_OK) {
            MT_ERROR("failed to save config file: %s", mt_err_str(err));
            mt_http_deferred_error(p->response, 500, "failed to save config file; changes are active only in memory");
            free(p); return;
        }
    }
    const mt_subscription_t *sub = mt_app_find_subscription_by_id(p->ctx->app, id);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "rules", sub_rules_to_json_array(sub->rules, sub->n_rules));
    cJSON_AddNumberToObject(out, "lastUpdate", sub->last_update);
    cJSON_AddStringToObject(out, "url", sub->url ? sub->url : "");
    mt_http_deferred_json(p->response, 200, out);
    free(p);
}

static cJSON *preview_json(mt_sub_rule_t **rules, size_t n, bool summary) {
    cJSON *out = cJSON_CreateObject();
    if (!summary) { cJSON_AddItemToObject(out, "rules", sub_rules_to_json_array(rules, n)); return out; }
    cJSON_AddNumberToObject(out, "count", (double)n);
    cJSON *types = cJSON_AddObjectToObject(out, "types");
    for (size_t i = 0; i < n; i++) {
        const char *type = rules[i]->type ? rules[i]->type : "";
        cJSON *count = cJSON_GetObjectItemCaseSensitive(types, type);
        if (count) { cJSON_SetNumberValue(count, count->valuedouble + 1); }
        else { cJSON_AddNumberToObject(types, type, 1); }
    }
    return out;
}

typedef struct pending_preview { mt_http_deferred_t *response; bool summary; } pending_preview_t;

static void preview_fetched(void *ud, mt_err_t err, mt_sub_rule_t **rules, size_t n) {
    pending_preview_t *p = ud;
    if (err != MT_OK) {
        mt_http_deferred_error(p->response, err == MT_ERR_UPSTREAM ? 502 : err == MT_ERR_LIMIT ? 413 : 500,
                              err == MT_ERR_UPSTREAM ? "subscription fetch failed" : mt_err_str(err));
    } else { mt_http_deferred_json(p->response, 200, preview_json(rules, n, p->summary)); }
    mt_sub_rules_free(rules, n); free(p);
}

/* POST /api/v1/subscriptions/{subscriptionID}/sync -- fetch+refresh a
 * single subscription's rules (SyncSubscription). Body is an optional
 * {"url": "..."} override; matches Go's json.Decoder-ignoring-EOF
 * behavior for a missing/empty body (no override, sub->url used as-is).
 * Saves only when the sync actually changed something AND ?save!=false
 * -- unlike maybe_save()'s unconditional-on-success save used by the
 * other handlers, matching SyncSubscription's `if changed && save !=
 * "false"` gate exactly. */
static void handle_sync_subscription(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_subs_ctx_t *ctx = ud;
    const char *id_str = mt_http_req_param(req, "subscriptionID");
    mt_id_t id;
    if (!id_str || mt_id_parse(id_str, &id) != MT_OK) {
        mt_http_res_write_error(res, 400, "invalid subscription id");
        return;
    }

    const char *url_override = NULL;
    cJSON *json = NULL;
    size_t body_len;
    const uint8_t *body = mt_http_req_body(req, &body_len);
    if (body_len > 0) {
        json = cJSON_ParseWithLength((const char *)body, body_len);
        if (!json) {
            mt_http_res_write_error(res, 400, "failed to parse request");
            return;
        }
        const char *url = get_string(json, "url");
        if (url[0] != '\0') { url_override = url; }
    }

    if (ctx->fetcher) {
        pending_sync_t *pending = calloc(1, sizeof(*pending));
        if (!pending) {
            cJSON_Delete(json);
            mt_http_res_write_error(res, 500, "out of memory");
            return;
        }
        pending->ctx = ctx;
        const char *save = mt_http_req_query(req, "save");
        pending->save = !(save && strcmp(save, "false") == 0);
        pending->response = mt_http_res_defer(req, res);
        if (!pending->response) {
            free(pending); cJSON_Delete(json);
            mt_http_res_write_error(res, 500, "out of memory");
            return;
        }
        mt_err_t err = mt_app_sync_subscription_async(ctx->app, ctx->fetcher, id,
            (int64_t)time(NULL), url_override, sync_finished, pending);
        cJSON_Delete(json);
        if (err != MT_OK) {
            mt_http_res_cancel_defer(res);
            free(pending);
            mt_http_res_write_error(res, sync_status(err), sync_message(err));
        }
        return;
    }

    bool changed = false;
    mt_err_t err = mt_app_sync_subscription_by_id(ctx->app, id, (int64_t)time(NULL), url_override, &changed);
    cJSON_Delete(json);
    if (err != MT_OK) {
        int status = 500;
        const char *msg = mt_err_str(err);
        if (err == MT_ERR_NOENT) {
            status = 404;
            msg = "subscription not found";
        } else if (err == MT_ERR_INVAL) {
            status = 400;
            msg = "subscription invalid";
        } else if (err == MT_ERR_UPSTREAM) {
            status = 502;
            msg = "subscription fetch failed";
        }
        mt_http_res_write_error(res, status, msg);
        return;
    }

    if (maybe_save(ctx, req) != MT_OK) {
        mt_http_res_write_error(res, 500, "failed to save config file; changes are active only in memory"); return;
    }
    const mt_subscription_t *sub = mt_app_find_subscription_by_id(ctx->app, id);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "rules", sub_rules_to_json_array(sub->rules, sub->n_rules));
    cJSON_AddNumberToObject(out, "lastUpdate", sub->last_update);
    cJSON_AddStringToObject(out, "url", sub->url ? sub->url : "");
    mt_http_res_write_json(res, 200, out);


}

/* GET /api/v1/subscriptions/rules?url= -- fetch+parse a list without
 * persisting anything (GetSubscriptionRules): a preview endpoint for the
 * frontend's "add subscription" flow. */
static void handle_get_subscription_rules(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_subs_ctx_t *ctx = ud;
    const char *url = mt_http_req_query(req, "url");
    if (!url || url[0] == '\0') {
        mt_http_res_write_error(res, 400, "subscription url is required");
        return;
    }

    const char *mode = mt_http_req_query(req, "summary");
    bool summary = mode && strcmp(mode, "true") == 0;
    if (ctx->fetcher) {
        pending_preview_t *p = calloc(1, sizeof(*p));
        if (!p) { mt_http_res_write_error(res, 500, "out of memory"); return; }
        p->summary = summary; p->response = mt_http_res_defer(req, res);
        mt_err_t err = p->response ? mt_sub_fetcher_submit_rules(ctx->fetcher, url, preview_fetched, p)
                                   : MT_ERR_NOMEM;
        if (err != MT_OK) {
            mt_http_res_cancel_defer(res); free(p);
            mt_http_res_write_error(res, sync_status(err), mt_err_str(err));
        }
        return;
    }
    char *body = NULL;
    size_t body_len = 0;
    mt_err_t ferr = mt_sub_fetch_list(url, &body, &body_len);
    if (ferr != MT_OK) {
        MT_ERROR("failed to fetch subscription list: %s", mt_err_str(ferr));
        mt_http_res_write_error(res, 502, "subscription fetch failed");
        return;
    }

    mt_sub_rule_t **rules = NULL;
    size_t n_rules = 0;
    mt_err_t perr = mt_sub_parse_rules(body, &rules, &n_rules);
    free(body);
    if (perr != MT_OK) {
        mt_http_res_write_error(res, 500, mt_err_str(perr));
        return;
    }

    cJSON *out = preview_json(rules, n_rules, summary);
    mt_http_res_write_json(res, 200, out);

    for (size_t i = 0; i < n_rules; i++) { mt_sub_rule_free(rules[i]); }
    free(rules);
}

void mt_subs_register_routes(mt_httpd_t *h, mt_subs_ctx_t *ctx) {
    must_route(h, "GET", "/api/v1/subscriptions", handle_get_subscriptions, ctx);
    must_route(h, "PUT", "/api/v1/subscriptions", handle_put_subscriptions, ctx);
    must_route(h, "POST", "/api/v1/subscriptions", handle_create_subscription, ctx);
    must_route(h, "GET", "/api/v1/subscriptions/rules", handle_get_subscription_rules, ctx);
    must_route(h, "DELETE", "/api/v1/subscriptions/{subscriptionID}", handle_delete_subscription, ctx);
    must_route(h, "POST", "/api/v1/subscriptions/{subscriptionID}/sync", handle_sync_subscription, ctx);
}
