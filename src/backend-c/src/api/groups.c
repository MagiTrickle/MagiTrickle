/* See groups.h. Port of api/v1/handlers.go's group+rule handlers and
 * api/v1/converters.go (compatibility-contract.md §2). */
#include "magitrickle/groups.h"

#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "magitrickle/id.h"
#include "magitrickle/id_pool.h"
#include "magitrickle/json.h"
#include "magitrickle/log.h"

/* ---- small JSON request-parsing helpers ------------------------------------ */

/* Optional "id" string field. *out_present is false when the key is absent
 * or JSON null (mirrors a Go *intID.ID decoding to a nil pointer in both
 * cases). Returns MT_ERR_INVAL when the key is present with some other
 * type, or a string that isn't exactly 8 hex chars (mirrors Go's
 * intID.ID.UnmarshalText failing at JSON-decode time, before any handler
 * logic runs). */
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

/* Optional bool field (Go *bool semantics: *out_present false when
 * absent/null). Used only for GroupReq.Enable -- RuleReq.Enable is a plain
 * (non-pointer) bool in Go, handled separately in fill_rule_fields. */
static void get_optional_bool(const cJSON *obj, const char *key, bool *out_val, bool *out_present) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    *out_present = item && cJSON_IsBool(item);
    if (*out_present) { *out_val = cJSON_IsTrue(item); }
}

/* ---- request body -> models (converters.go equivalents) -------------------- */

static mt_err_t fill_rule_fields(mt_rule_t *rule, const cJSON *req) {
    if (mt_strset(&rule->name, get_string(req, "name")) != MT_OK) { return MT_ERR_NOMEM; }
    if (mt_strset(&rule->type, get_string(req, "type")) != MT_OK) { return MT_ERR_NOMEM; }
    if (mt_strset(&rule->rule, get_string(req, "rule")) != MT_OK) { return MT_ERR_NOMEM; }
    /* RuleReq.Enable is a plain bool in Go -- absent/wrong-type/null all
     * decode to its zero value, false. */
    cJSON *enable_j = cJSON_GetObjectItemCaseSensitive(req, "enable");
    rule->enable = cJSON_IsBool(enable_j) && cJSON_IsTrue(enable_j);
    return MT_OK;
}

/* Lenient reuse, matches Go's RuleFromReq: an "id" that matches one of
 * baseline_rules keeps that identity; any other case (absent id, or an id
 * that matches nothing in baseline) silently gets a fresh random id -- Go
 * does not treat an unmatched id here as an error. Used for CreateRule,
 * PutRule's body-less fields (id ignored entirely there, see below), and
 * GroupReq's nested "rules" array. Returns NULL on OOM. */
static mt_err_t index_rules(mt_lookup_t *index, mt_rule_t **rules, size_t n) {
    for (size_t i = 0; i < n; i++) {
        mt_err_t err = mt_lookup_put(index, rules[i]->id.b, sizeof(rules[i]->id.b), i, NULL);
        if (err != MT_OK) { return err; }
    }
    return MT_OK;
}

static mt_err_t rule_from_index(const cJSON *req, const mt_lookup_t *baseline,
                                 mt_lookup_t *used, mt_id_pool_t *pool, bool strict,
                                 mt_rule_t **out) {
    *out = NULL;
    mt_id_t id; bool has_id;
    mt_err_t err = parse_optional_id(req, "id", &id, &has_id);
    if (strict && err != MT_OK) { return err; }
    bool found = err == MT_OK && has_id && mt_lookup_get(baseline, id.b, sizeof(id.b), NULL);
    if (strict && has_id && !found) { return MT_ERR_NOENT; }
    if (!found) {
        err = mt_id_pool_unique(used, pool, &id);
        if (err != MT_OK) { return err; }
    }
    mt_rule_t *rule = mt_rule_new();
    if (!rule) { return MT_ERR_NOMEM; }
    rule->id = id;
    err = fill_rule_fields(rule, req);
    if (err != MT_OK) { mt_rule_free(rule); return err; }
    *out = rule;
    return MT_OK;
}

/* Compact metadata/small rule edits retain all untouched server-side rules.
 * Optimistic per-field preconditions reject stale edits, never fall back to
 * replacing the live group with an empty rules array. */
static mt_err_t apply_group_rule_changes(const cJSON *changes, mt_group_t *group) {
    if (!cJSON_IsArray(changes)) { return MT_ERR_INVAL; }
    mt_lookup_t index = {0}, seen = {0};
    mt_err_t err = index_rules(&index, group->rules, group->n_rules);
    const cJSON *change;
    cJSON_ArrayForEach(change, changes) {
        if (err != MT_OK) { break; }
        mt_id_t id; bool has_id; size_t i;
        if (parse_optional_id(change, "id", &id, &has_id) != MT_OK || !has_id ||
            mt_lookup_get(&seen, id.b, sizeof(id.b), NULL) ||
            !mt_lookup_get(&index, id.b, sizeof(id.b), &i)) { err = MT_ERR_STATE; break; }
        err = mt_lookup_put(&seen, id.b, sizeof(id.b), i, NULL);
        if (err != MT_OK) { break; }
        mt_rule_t *rule = group->rules[i];
        const cJSON *previous = cJSON_GetObjectItemCaseSensitive(change, "previous");
        const cJSON *enable = cJSON_GetObjectItemCaseSensitive(previous, "enable");
        const char *keys[] = {"name", "type", "rule"};
        const char *values[] = {rule->name, rule->type, rule->rule};
        for (size_t k = 0; k < 3; k++) {
            const cJSON *item = cJSON_GetObjectItemCaseSensitive(previous, keys[k]);
            const cJSON *next = cJSON_GetObjectItemCaseSensitive(change, keys[k]);
            if (!cJSON_IsString(item) || !cJSON_IsString(next) ||
                strcmp(item->valuestring, values[k] ? values[k] : "") != 0) { err = MT_ERR_STATE; }
        }
        if (!cJSON_IsBool(enable) || cJSON_IsTrue(enable) != rule->enable ||
            !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(change, "enable"))) { err = MT_ERR_STATE; }
        if (err == MT_OK) { err = fill_rule_fields(rule, change); }
    }
    mt_lookup_clear(&index); mt_lookup_clear(&seen);
    return err;
}

/* Builds a *new* mt_group_t from a GroupReq JSON body -- matches Go's
 * GroupFromReq(req, existing). existing == NULL mirrors Go's existing==nil
 * (brand new group: an "id" in the body, if any, is used as-is with no
 * conflict check here -- mt_app_add_group catches duplicates later);
 * existing != NULL supplies both the id to validate against (mismatch ->
 * MT_ERR_INVAL "group ID mismatch") and the baseline rules for lenient
 * nested rule-id reuse when the body's own "rules" key is absent (Go
 * mutates `existing` in place; callers here get an independent object back
 * and must transplant it onto the live one themselves -- see
 * group_move_into, used by handle_put_group). */
static mt_err_t group_from_req(mt_app_t *app, const cJSON *req, const mt_group_t *existing, mt_group_t **out,
                               const char **err_msg) {
    mt_id_t req_id;
    bool has_req_id;
    if (parse_optional_id(req, "id", &req_id, &has_req_id) != MT_OK) {
        *err_msg = "invalid group id";
        return MT_ERR_INVAL;
    }
    if (existing && has_req_id && !mt_id_equal(existing->id, req_id)) {
        *err_msg = "group ID mismatch";
        return MT_ERR_INVAL;
    }

    uint16_t priority = existing ? existing->priority : MT_GROUP_DEFAULT_PRIORITY;
    if (mt_json_parse_priority(req, &priority) != MT_OK) {
        *err_msg = "priority must be an integer between 1 and 999";
        return MT_ERR_INVAL;
    }

    mt_group_t *group = mt_group_new();
    if (!group) { return MT_ERR_NOMEM; }
    group->id = existing ? existing->id : (has_req_id ? req_id : mt_id_random());
    group->priority = priority;

    mt_err_t err = mt_strset(&group->name, get_string(req, "name"));
    if (err == MT_OK) { err = mt_strset(&group->color, get_string(req, "color")); }
    if (err == MT_OK) { err = mt_strset(&group->iface, get_string(req, "interface")); }
    const cJSON *profile = cJSON_GetObjectItemCaseSensitive(req, "profile");
    if (profile && !cJSON_IsNull(profile) && !cJSON_IsString(profile)) { err = MT_ERR_INVAL; }
    if (err == MT_OK) { err = mt_strset(&group->profile, get_string(req, "profile")); }
    if (err == MT_OK) { err = mt_app_normalize_route(app, group->profile, &group->iface); }
    if (err != MT_OK) { *err_msg = "invalid or missing routing profile"; }
    if (err == MT_OK) { err = mt_group_normalize_color(group); }
    if (err != MT_OK) {
        mt_group_free(group);
        return err;
    }

    bool enable_present, enable_val;
    get_optional_bool(req, "enable", &enable_val, &enable_present);
    group->enable = enable_present ? enable_val : true;

    cJSON *rules_j = cJSON_GetObjectItemCaseSensitive(req, "rules");
    if (rules_j && !cJSON_IsNull(rules_j)) {
        if (!cJSON_IsArray(rules_j)) {
            mt_group_free(group);
            *err_msg = "invalid rules";
            return MT_ERR_INVAL;
        }
        mt_lookup_t baseline = {0}, used = {0}; mt_id_pool_t pool = {0};
        err = existing ? index_rules(&baseline, existing->rules, existing->n_rules) : MT_OK;
        if (err == MT_OK && existing) { err = index_rules(&used, existing->rules, existing->n_rules); }
        const cJSON *item;
        cJSON_ArrayForEach(item, rules_j) {
            if (err != MT_OK) { break; }
            mt_rule_t *r = NULL;
            err = rule_from_index(item, &baseline, &used, &pool, false, &r);
            if (err == MT_OK) { err = mt_group_add_rule(group, r); }
            if (err != MT_OK) { mt_rule_free(r); break; }
        }
        mt_lookup_clear(&baseline); mt_lookup_clear(&used);
        if (err != MT_OK) { mt_group_free(group); return err; }
    } else if (existing) {
        /* "rules" key absent -> Go leaves existing.Rules untouched. We
         * return an independent object, so deep-copy existing's rules
         * rather than aliasing them. */
        for (size_t i = 0; i < existing->n_rules; i++) {
            mt_rule_t *r = mt_rule_new();
            mt_err_t e = r ? MT_OK : MT_ERR_NOMEM;
            if (e == MT_OK) { r->id = existing->rules[i]->id; }
            if (e == MT_OK) { e = mt_strset(&r->name, existing->rules[i]->name); }
            if (e == MT_OK) { e = mt_strset(&r->type, existing->rules[i]->type); }
            if (e == MT_OK) { e = mt_strset(&r->rule, existing->rules[i]->rule); }
            if (e == MT_OK) { r->enable = existing->rules[i]->enable; }
            if (e != MT_OK || mt_group_add_rule(group, r) != MT_OK) {
                mt_rule_free(r);
                mt_group_free(group);
                return e != MT_OK ? e : MT_ERR_NOMEM;
            }
        }
    }
    const cJSON *changes = cJSON_GetObjectItemCaseSensitive(req, "ruleChanges");
    if (changes) {
        if (!existing || (rules_j && !cJSON_IsNull(rules_j))) {
            mt_group_free(group); *err_msg = "group changed; reload before saving";
            return MT_ERR_STATE;
        }
        err = apply_group_rule_changes(changes, group);
        if (err != MT_OK) {
            mt_group_free(group); *err_msg = "group changed; reload before saving"; return err;
        }
    }
    *out = group;
    return MT_OK;
}

/* Moves name/color/iface/enable/priority/rules from `from` into `into` in place
 * (freeing into's old contents first), preserving into's address/identity
 * and its original id. Frees the now-empty `from` shell with plain free()
 * -- NOT mt_group_free, which would double-free the pointers just moved. */
static void group_move_into(mt_group_t *into, mt_group_t *from) {
    free(into->name);
    into->name = from->name;
    free(into->color);
    into->color = from->color;
    free(into->iface);
    into->iface = from->iface;
    free(into->profile);
    into->profile = from->profile;
    into->enable = from->enable;
    into->priority = from->priority;
    for (size_t i = 0; i < into->n_rules; i++) { mt_rule_free(into->rules[i]); }
    free(into->rules);
    into->rules = from->rules;
    into->n_rules = from->n_rules;
    free(from);
}

/* ---- models -> response JSON ------------------------------------------------ */

static cJSON *rule_to_json(const mt_rule_t *r) {
    char id_buf[MT_ID_STR_LEN];
    mt_id_format(r->id, id_buf);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", id_buf);
    cJSON_AddStringToObject(obj, "name", r->name ? r->name : "");
    cJSON_AddStringToObject(obj, "type", r->type ? r->type : "");
    cJSON_AddStringToObject(obj, "rule", r->rule ? r->rule : "");
    cJSON_AddBoolToObject(obj, "enable", r->enable);
    return obj;
}

static cJSON *rules_to_json_array(mt_rule_t **rules, size_t n) {
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) { cJSON_AddItemToArray(arr, rule_to_json(rules[i])); }
    return arr;
}

/* {"rules": [...]} -- matches RespFromRules, which always sets a non-nil
 * (possibly empty) slice pointer, so "rules" is always present. */
static cJSON *wrap_rules(mt_rule_t **rules, size_t n) {
    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "rules", rules_to_json_array(rules, n));
    return out;
}

static cJSON *group_to_json(const mt_group_t *g, bool with_rules) {
    char id_buf[MT_ID_STR_LEN];
    mt_id_format(g->id, id_buf);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", id_buf);
    cJSON_AddStringToObject(obj, "name", g->name ? g->name : "");
    cJSON_AddStringToObject(obj, "color", g->color ? g->color : "");
    cJSON_AddStringToObject(obj, "interface", g->iface ? g->iface : "");
    if (g->profile && *g->profile) { cJSON_AddStringToObject(obj, "profile", g->profile); }
    cJSON_AddBoolToObject(obj, "enable", g->enable);
    cJSON_AddNumberToObject(obj, "priority", g->priority);
    /* "rules" key omitted unless with_rules, matching GroupRes.RulesRes's
     * `omitempty` (a nil slice pointer when withRules is false). */
    if (with_rules) { cJSON_AddItemToObject(obj, "rules", rules_to_json_array(g->rules, g->n_rules)); }
    return obj;
}

/* ---- HTTP glue --------------------------------------------------------------- */

static void must_route(mt_httpd_t *h, const char *method, const char *pattern, mt_http_handler_fn fn,
                       void *ud) {
    if (mt_httpd_route(h, method, pattern, fn, ud) != MT_OK) {
        MT_ERROR("failed to register route %s %s", method, pattern);
    }
}

static mt_err_t save_if_requested(mt_groups_ctx_t *ctx, mt_http_req_t *req) {
    if (!mt_http_req_query_is_true(req, "save") || !ctx->config_path) { return MT_OK; }
    mt_err_t err = mt_app_save_config(ctx->app, ctx->config_path, ctx->config_version ? ctx->config_version : "");
    if (err != MT_OK) { MT_ERROR("failed to save config file: %s", mt_err_str(err)); }
    return err;
}

static void maybe_save(mt_groups_ctx_t *ctx, mt_http_req_t *req, mt_http_res_t *res) {
    if (save_if_requested(ctx, req) != MT_OK) {
        mt_http_res_write_error(res, 500, "failed to save config file; changes are not persisted");
    }
}

/* Called after every in-place group/rule edit in this file (the handlers
 * that go through mt_ruleset_group_mut rather than one of mt_app_t's own
 * mutation functions, which already republish internally) -- otherwise
 * an HTTP-created rule would update netfilter (via mt_app_sync_group)
 * but never actually become resolvable over DNS. Found as a real gap in
 * Phase 7 while wiring subscription rule sets into the same snapshot;
 * see decisions.md and app.h's mt_app_republish_dns_snapshot doc. */
static void republish_dns_snapshot(mt_groups_ctx_t *ctx) {
    mt_err_t err = mt_app_republish_dns_snapshot(ctx->app);
    if (err != MT_OK) { MT_ERROR("failed to republish DNS-matching snapshot: %s", mt_err_str(err)); }
}

static bool resolve_group(mt_groups_ctx_t *ctx, mt_http_req_t *req, mt_http_res_t *res, mt_ruleset_t **out) {
    const char *id_str = mt_http_req_param(req, "groupID");
    mt_id_t id;
    if (!id_str || mt_id_parse(id_str, &id) != MT_OK) {
        mt_http_res_write_error(res, 400, "invalid group id");
        return false;
    }
    mt_ruleset_t *rs = mt_app_find_group_by_id(ctx->app, id);
    if (!rs) {
        mt_http_res_write_error(res, 404, "group not exist");
        return false;
    }
    *out = rs;
    return true;
}

static bool resolve_rule(mt_ruleset_t *rs, mt_http_req_t *req, mt_http_res_t *res, size_t *out_idx) {
    const char *id_str = mt_http_req_param(req, "ruleID");
    mt_id_t id;
    if (!id_str || mt_id_parse(id_str, &id) != MT_OK) {
        mt_http_res_write_error(res, 400, "invalid rule id");
        return false;
    }
    const mt_group_t *g = mt_ruleset_group(rs);
    for (size_t i = 0; i < g->n_rules; i++) {
        if (mt_id_equal(g->rules[i]->id, id)) {
            *out_idx = i;
            return true;
        }
    }
    mt_http_res_write_error(res, 404, "rule not exist");
    return false;
}

static cJSON *parse_body_json(mt_http_req_t *req) {
    size_t body_len;
    const uint8_t *body = mt_http_req_body(req, &body_len);
    return body_len > 0 ? cJSON_ParseWithLength((const char *)body, body_len) : NULL;
}

/* ---- /api/v1/groups ---------------------------------------------------------- */

static void handle_get_groups(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_groups_ctx_t *ctx = ud;
    bool with_rules = mt_http_req_query_is_true(req, "with_rules");
    cJSON *out = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(out, "groups");
    size_t n = mt_app_user_group_count(ctx->app);
    for (size_t i = 0; i < n; i++) {
        const mt_group_t *g = mt_ruleset_group(mt_app_user_group_at(ctx->app, i));
        cJSON_AddItemToArray(arr, group_to_json(g, with_rules));
    }
    mt_http_res_write_json(res, 200, out);
}

static void handle_put_groups(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_groups_ctx_t *ctx = ud;
    cJSON *json = parse_body_json(req);
    if (!json) {
        mt_http_res_write_error(res, 400, "failed to parse request");
        return;
    }
    cJSON *groups_j = cJSON_GetObjectItemCaseSensitive(json, "groups");
    if (!groups_j || cJSON_IsNull(groups_j) || !cJSON_IsArray(groups_j)) {
        cJSON_Delete(json);
        mt_http_res_write_error(res, 400, "no groups in request");
        return;
    }

    int n_req = cJSON_GetArraySize(groups_j);
    mt_group_t **new_groups = n_req > 0 ? calloc((size_t)n_req, sizeof(*new_groups)) : NULL;
    if (n_req > 0 && !new_groups) {
        cJSON_Delete(json);
        mt_http_res_write_error(res, 500, "out of memory");
        return;
    }
    int i = 0;
    /* Traverse once, bounded by the capacity validated above. */
    for (cJSON *group_req = groups_j->child; group_req && i < n_req; group_req = group_req->next) {
        mt_id_t wanted_id;
        bool has_id;
        const mt_group_t *existing = NULL;
        if (parse_optional_id(group_req, "id", &wanted_id, &has_id) == MT_OK && has_id) {
            mt_ruleset_t *rs = mt_app_find_group_by_id(ctx->app, wanted_id);
            if (rs) { existing = mt_ruleset_group(rs); }
        }
        const char *err_msg = "invalid group";
        mt_err_t err = group_from_req(ctx->app, group_req, existing, &new_groups[i], &err_msg);
        if (err != MT_OK) {
            cJSON_Delete(json);
            for (int j = 0; j < i; j++) { mt_group_free(new_groups[j]); }
            free(new_groups);
            mt_http_res_write_error(res, err == MT_ERR_STATE ? 409 : 400, err_msg);
            return;
        }
        i++;
    }
    cJSON_Delete(json);

    mt_err_t err = mt_app_replace_groups(ctx->app, new_groups, (size_t)n_req);
    if (err != MT_OK) {
        mt_http_res_write_error(res, err == MT_ERR_EXIST ? 409 : 500, mt_err_str(err));
        return;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(out, "groups");
    for (size_t k = 0; k < mt_app_user_group_count(ctx->app); k++) {
        cJSON_AddItemToArray(arr, group_to_json(mt_ruleset_group(mt_app_user_group_at(ctx->app, k)), true));
    }
    if (save_if_requested(ctx, req) != MT_OK) {
        /* The canonical response is also the next optimistic edit baseline.
         * Keep it on disk failure, especially IDs generated by a full import. */
        cJSON_AddStringToObject(out, "error", "failed to save config file; changes are active only in memory");
        cJSON_AddStringToObject(out, "code", "PERSISTENCE_FAILED");
        cJSON_AddBoolToObject(out, "applied", true);
        mt_http_res_write_json(res, 500, out);
        return;
    }
    mt_http_res_write_json(res, 200, out);
}

static void handle_create_group(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_groups_ctx_t *ctx = ud;
    cJSON *json = parse_body_json(req);
    if (!json) {
        mt_http_res_write_error(res, 400, "failed to parse request");
        return;
    }
    mt_group_t *group = NULL;
    const char *err_msg = "invalid group";
    mt_err_t err = group_from_req(ctx->app, json, NULL, &group, &err_msg);
    cJSON_Delete(json);
    if (err != MT_OK) {
        mt_http_res_write_error(res, 400, err_msg);
        return;
    }

    err = mt_app_add_group(ctx->app, group); /* always takes ownership */
    if (err != MT_OK) {
        mt_http_res_write_error(res, err == MT_ERR_EXIST ? 409 : 500, mt_err_str(err));
        return;
    }
    mt_http_res_write_json(res, 200, group_to_json(group, true)); /* still valid: no free on success */
    maybe_save(ctx, req, res);
}

static void handle_get_group(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_groups_ctx_t *ctx = ud;
    mt_ruleset_t *rs;
    if (!resolve_group(ctx, req, res, &rs)) { return; }
    bool with_rules = mt_http_req_query_is_true(req, "with_rules");
    mt_http_res_write_json(res, 200, group_to_json(mt_ruleset_group(rs), with_rules));
}

static void handle_put_group(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_groups_ctx_t *ctx = ud;
    mt_ruleset_t *rs;
    if (!resolve_group(ctx, req, res, &rs)) { return; }

    cJSON *json = parse_body_json(req);
    if (!json) {
        mt_http_res_write_error(res, 400, "failed to parse request");
        return;
    }

    bool was_enabled = mt_ruleset_runtime_enabled(rs);

    mt_group_t *live = mt_ruleset_group_mut(rs);
    mt_group_t *built = NULL;
    const char *err_msg = "invalid group";
    mt_err_t err = group_from_req(ctx->app, json, live, &built, &err_msg);
    cJSON_Delete(json);
    if (err != MT_OK) {
        mt_http_res_write_error(res, err == MT_ERR_STATE ? 409 : 400, err_msg);
        return;
    }
    if (was_enabled) {
        err = mt_ruleset_disable(rs);
        if (err != MT_OK) { mt_group_free(built); mt_http_res_write_error(res, 500, mt_err_str(err)); return; }
    }
    group_move_into(live, built);
    republish_dns_snapshot(ctx);

    if (was_enabled) {
        err = mt_ruleset_enable(rs);
        if (err == MT_OK) { err = mt_app_sync_group(ctx->app, rs); }
        if (err != MT_OK) {
            mt_http_res_write_error(res, 500, mt_err_str(err));
            return;
        }
    }

    mt_http_res_write_json(res, 200, group_to_json(live, true));
    maybe_save(ctx, req, res);
}

static void handle_delete_group(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_groups_ctx_t *ctx = ud;
    mt_ruleset_t *rs;
    if (!resolve_group(ctx, req, res, &rs)) { return; }
    if (mt_ruleset_runtime_enabled(rs)) {
        mt_err_t err = mt_ruleset_disable(rs);
        if (err != MT_OK) {
            mt_http_res_write_error(res, 500, mt_err_str(err));
            return;
        }
    }
    mt_id_t id = mt_ruleset_group(rs)->id;
    mt_app_remove_group_by_id(ctx->app, id);
    mt_http_res_write(res, 200, NULL, NULL, 0);
    maybe_save(ctx, req, res);
}

/* ---- /api/v1/groups/{groupID}/rules ------------------------------------------ */

static void handle_get_rules(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_groups_ctx_t *ctx = ud;
    mt_ruleset_t *rs;
    if (!resolve_group(ctx, req, res, &rs)) { return; }
    const mt_group_t *g = mt_ruleset_group(rs);
    mt_http_res_write_json(res, 200, wrap_rules(g->rules, g->n_rules));
}

static void handle_put_rules(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_groups_ctx_t *ctx = ud;
    mt_ruleset_t *rs;
    if (!resolve_group(ctx, req, res, &rs)) { return; }

    cJSON *json = parse_body_json(req);
    if (!json) {
        mt_http_res_write_error(res, 400, "failed to parse request");
        return;
    }
    cJSON *rules_j = cJSON_GetObjectItemCaseSensitive(json, "rules");
    if (!rules_j || cJSON_IsNull(rules_j) || !cJSON_IsArray(rules_j)) {
        cJSON_Delete(json);
        mt_http_res_write_error(res, 400, "no rules in request");
        return;
    }

    mt_group_t *group = mt_ruleset_group_mut(rs);
    mt_group_t built = {0};
    mt_lookup_t baseline = {0}, used = {0}; mt_id_pool_t pool = {0};
    mt_err_t err = index_rules(&baseline, group->rules, group->n_rules);
    if (err == MT_OK) { err = index_rules(&used, group->rules, group->n_rules); }
    const cJSON *item;
    cJSON_ArrayForEach(item, rules_j) {
        if (err != MT_OK) { break; }
        mt_rule_t *rule = NULL;
        err = rule_from_index(item, &baseline, &used, &pool, true, &rule);
        if (err == MT_OK) { err = mt_group_add_rule(&built, rule); }
        if (err != MT_OK) { mt_rule_free(rule); break; }
    }
    mt_lookup_clear(&baseline); mt_lookup_clear(&used); cJSON_Delete(json);
    if (err != MT_OK) {
        for (size_t k = 0; k < built.n_rules; k++) { mt_rule_free(built.rules[k]); }
        free(built.rules);
        mt_http_res_write_error(res, err == MT_ERR_NOENT ? 404 : 400,
                                 err == MT_ERR_NOENT ? "rule not found" : "invalid rule");
        return;
    }
    for (size_t k = 0; k < group->n_rules; k++) { mt_rule_free(group->rules[k]); }
    free(group->rules);
    group->rules = built.rules;
    group->n_rules = built.n_rules;
    republish_dns_snapshot(ctx);

    if (mt_ruleset_runtime_enabled(rs)) {
        err = mt_app_sync_group(ctx->app, rs);
        if (err != MT_OK) {
            mt_http_res_write_error(res, 500, mt_err_str(err));
            return;
        }
    }

    mt_http_res_write_json(res, 200, wrap_rules(group->rules, group->n_rules));
    maybe_save(ctx, req, res);
}

static void handle_create_rule(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_groups_ctx_t *ctx = ud;
    mt_ruleset_t *rs;
    if (!resolve_group(ctx, req, res, &rs)) { return; }

    cJSON *json = parse_body_json(req);
    if (!json) {
        mt_http_res_write_error(res, 400, "failed to parse request");
        return;
    }
    mt_group_t *group = mt_ruleset_group_mut(rs);
    mt_lookup_t baseline = {0}, used = {0}; mt_id_pool_t pool = {0};
    mt_rule_t *rule = NULL;
    mt_err_t build_err = index_rules(&baseline, group->rules, group->n_rules);
    if (build_err == MT_OK) { build_err = index_rules(&used, group->rules, group->n_rules); }
    if (build_err == MT_OK) { build_err = rule_from_index(json, &baseline, &used, &pool, false, &rule); }
    mt_lookup_clear(&baseline); mt_lookup_clear(&used); cJSON_Delete(json);
    if (build_err != MT_OK) {
        mt_http_res_write_error(res, 500, "out of memory");
        return;
    }
    if (mt_group_add_rule(group, rule) != MT_OK) {
        mt_rule_free(rule);
        mt_http_res_write_error(res, 500, "out of memory");
        return;
    }
    republish_dns_snapshot(ctx);

    if (mt_ruleset_runtime_enabled(rs)) {
        mt_err_t err = mt_app_sync_group(ctx->app, rs);
        if (err != MT_OK) {
            mt_http_res_write_error(res, 500, mt_err_str(err));
            return;
        }
    }

    mt_http_res_write_json(res, 200, rule_to_json(rule));
    maybe_save(ctx, req, res);
}

static void handle_get_rule(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_groups_ctx_t *ctx = ud;
    mt_ruleset_t *rs;
    if (!resolve_group(ctx, req, res, &rs)) { return; }
    size_t idx;
    if (!resolve_rule(rs, req, res, &idx)) { return; }
    mt_http_res_write_json(res, 200, rule_to_json(mt_ruleset_group(rs)->rules[idx]));
}

static void handle_put_rule(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_groups_ctx_t *ctx = ud;
    mt_ruleset_t *rs;
    if (!resolve_group(ctx, req, res, &rs)) { return; }
    size_t idx;
    if (!resolve_rule(rs, req, res, &idx)) { return; }

    cJSON *json = parse_body_json(req);
    if (!json) {
        mt_http_res_write_error(res, 400, "failed to parse request");
        return;
    }
    /* Matches Go's PutRule exactly: the resolved rule's fields are
     * overwritten in place; the body's own "id" (if any) is never read --
     * the rule's identity always stays the path-resolved one. */
    mt_rule_t *rule = mt_ruleset_group_mut(rs)->rules[idx];
    mt_err_t err = fill_rule_fields(rule, json);
    cJSON_Delete(json);
    if (err != MT_OK) {
        mt_http_res_write_error(res, 500, "out of memory");
        return;
    }
    republish_dns_snapshot(ctx);

    if (mt_ruleset_runtime_enabled(rs)) {
        err = mt_app_sync_group(ctx->app, rs);
        if (err != MT_OK) {
            mt_http_res_write_error(res, 500, mt_err_str(err));
            return;
        }
    }

    mt_http_res_write_json(res, 200, rule_to_json(rule));
    maybe_save(ctx, req, res);
}

static void handle_delete_rule(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_groups_ctx_t *ctx = ud;
    mt_ruleset_t *rs;
    if (!resolve_group(ctx, req, res, &rs)) { return; }
    size_t idx;
    if (!resolve_rule(rs, req, res, &idx)) { return; }

    mt_group_t *group = mt_ruleset_group_mut(rs);
    mt_rule_free(group->rules[idx]);
    for (size_t i = idx; i + 1 < group->n_rules; i++) { group->rules[i] = group->rules[i + 1]; }
    group->n_rules--;
    republish_dns_snapshot(ctx);

    if (mt_ruleset_runtime_enabled(rs)) {
        mt_err_t err = mt_app_sync_group(ctx->app, rs);
        if (err != MT_OK) {
            mt_http_res_write_error(res, 500, mt_err_str(err));
            return;
        }
    }

    mt_http_res_write(res, 200, NULL, NULL, 0);
    maybe_save(ctx, req, res);
}

/* ---- registration -------------------------------------------------------------- */

void mt_groups_register_routes(mt_httpd_t *h, mt_groups_ctx_t *ctx) {
    must_route(h, "GET", "/api/v1/groups", handle_get_groups, ctx);
    must_route(h, "PUT", "/api/v1/groups", handle_put_groups, ctx);
    must_route(h, "POST", "/api/v1/groups", handle_create_group, ctx);
    must_route(h, "GET", "/api/v1/groups/{groupID}", handle_get_group, ctx);
    must_route(h, "PUT", "/api/v1/groups/{groupID}", handle_put_group, ctx);
    must_route(h, "DELETE", "/api/v1/groups/{groupID}", handle_delete_group, ctx);
    must_route(h, "GET", "/api/v1/groups/{groupID}/rules", handle_get_rules, ctx);
    must_route(h, "PUT", "/api/v1/groups/{groupID}/rules", handle_put_rules, ctx);
    must_route(h, "POST", "/api/v1/groups/{groupID}/rules", handle_create_rule, ctx);
    must_route(h, "GET", "/api/v1/groups/{groupID}/rules/{ruleID}", handle_get_rule, ctx);
    must_route(h, "PUT", "/api/v1/groups/{groupID}/rules/{ruleID}", handle_put_rule, ctx);
    must_route(h, "DELETE", "/api/v1/groups/{groupID}/rules/{ruleID}", handle_delete_rule, ctx);
}
