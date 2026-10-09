#include "magitrickle/profiles_api.h"

#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "magitrickle/log.h"
#include "magitrickle/profiles.h"
#include "magitrickle/rand.h"

static mt_err_t profile_from_json(const cJSON *json, mt_profile_t **out)
{
    *out = NULL;
    if (!cJSON_IsObject(json)) { return MT_ERR_INVAL; }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
    const cJSON *interfaces = cJSON_GetObjectItemCaseSensitive(json, "interfaces");
    const cJSON *terminal = cJSON_GetObjectItemCaseSensitive(json, "on_unavailable");
    if ((id && !cJSON_IsString(id)) || !cJSON_IsString(name) || !cJSON_IsArray(interfaces) ||
        (terminal && (!cJSON_IsString(terminal) || strcmp(terminal->valuestring, MT_PROFILE_TERMINAL) != 0))) {
        return MT_ERR_INVAL;
    }
    mt_profile_t *p = mt_profile_new();
    if (!p) { return MT_ERR_NOMEM; }
    char generated[MT_ID_STR_LEN];
    mt_err_t err = MT_OK;
    if (!id) {
        mt_id_t value = {{0, 0, 0, 0}};
        err = mt_random_bytes(value.b, sizeof(value.b));
        if (err == MT_OK) { mt_id_format(value, generated); }
    }
    if (err == MT_OK) { err = mt_strset(&p->id, id ? id->valuestring : generated); }
    if (err == MT_OK) { err = mt_strset(&p->name, name->valuestring); }
    const cJSON *item;
    cJSON_ArrayForEach(item, interfaces) {
        if (err != MT_OK) { break; }
        if (!cJSON_IsString(item)) { err = MT_ERR_INVAL; break; }
        err = mt_profile_add_interface(p, item->valuestring);
    }
    if (err != MT_OK) { mt_profile_free(p); return err; }
    *out = p;
    return MT_OK;
}

static cJSON *profiles_response(mt_app_t *app)
{
    cJSON *out = cJSON_CreateObject();
    cJSON *array = out ? cJSON_AddArrayToObject(out, "profiles") : NULL;
    if (!array) { cJSON_Delete(out); return NULL; }
    for (size_t i = 0; i < mt_app_profile_count(app); i++) {
        const mt_profile_t *p = mt_app_profile_at(app, i);
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(array, item)) { cJSON_Delete(item); goto oom; }
        if (!cJSON_AddStringToObject(item, "id", p->id) ||
            !cJSON_AddStringToObject(item, "name", p->name) ||
            !cJSON_AddStringToObject(item, "on_unavailable", MT_PROFILE_TERMINAL)) { goto oom; }
        cJSON *interfaces = cJSON_AddArrayToObject(item, "interfaces");
        if (!interfaces) { goto oom; }
        for (size_t j = 0; j < p->n_interfaces; j++) {
            cJSON *value = cJSON_CreateString(p->interfaces[j]);
            if (!value || !cJSON_AddItemToArray(interfaces, value)) { cJSON_Delete(value); goto oom; }
        }
        size_t groups = 0, subscriptions = 0;
        for (size_t j = 0; j < mt_app_user_group_count(app); j++) {
            const mt_group_t *g = mt_ruleset_group(mt_app_user_group_at(app, j));
            if (g->profile && strcmp(g->profile, p->id) == 0) { groups++; }
        }
        for (size_t j = 0; j < mt_app_subscription_count(app); j++) {
            const mt_subscription_t *s = mt_app_subscription_at(app, j);
            if (s->profile && strcmp(s->profile, p->id) == 0) { subscriptions++; }
        }
        cJSON *usage = cJSON_AddObjectToObject(item, "usage");
        if (!usage || !cJSON_AddNumberToObject(usage, "groups", (double)groups) ||
            !cJSON_AddNumberToObject(usage, "subscriptions", (double)subscriptions)) { goto oom; }
    }
    return out;
oom:
    cJSON_Delete(out);
    return NULL;
}

static void handle_get_profiles(mt_http_req_t *req, mt_http_res_t *res, void *ud)
{
    (void)req;
    mt_system_ctx_t *ctx = ud;
    cJSON *out = profiles_response(ctx->app);
    if (!out) { mt_http_res_write_error(res, 500, "out of memory"); return; }
    mt_http_res_write_json(res, 200, out);
}

static void handle_put_profiles(mt_http_req_t *req, mt_http_res_t *res, void *ud)
{
    mt_system_ctx_t *ctx = ud;
    size_t size;
    const uint8_t *body = mt_http_req_body(req, &size);
    cJSON *json = size ? cJSON_ParseWithLength((const char *)body, size) : NULL;
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(json, "profiles");
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(json);
        mt_http_res_write_error(res, 400, "profiles must be an array");
        return;
    }
    mt_config_t incoming = {0};
    mt_err_t err = MT_OK;
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        mt_profile_t *p = NULL;
        err = profile_from_json(item, &p);
        if (err == MT_OK) { err = mt_config_add_profile(&incoming, p); }
        if (err != MT_OK) { mt_profile_free(p); break; }
    }
    cJSON_Delete(json);
    char message[256] = "Invalid profile: check its ID, name, interfaces and terminal action";
    if (err == MT_OK) { err = mt_app_replace_profiles(ctx->app, &incoming, message, sizeof(message)); }
    mt_config_clear_profiles(&incoming);
    if (err != MT_OK) {
        int status = err == MT_ERR_NOENT || err == MT_ERR_EXIST ? 409 : err == MT_ERR_INVAL ? 400 : 500;
        mt_http_res_write_error(res, status, *message ? message : mt_err_str(err));
        return;
    }
    cJSON *out = profiles_response(ctx->app);
    if (!out) { mt_http_res_write_error(res, 500, "out of memory; changes are active only in memory"); return; }
    const char *save = mt_http_req_query(req, "save");
    if (ctx->config_path && (!save || strcmp(save, "false") != 0)) {
        err = mt_app_save_config(ctx->app, ctx->config_path,
                                 ctx->config_version ? ctx->config_version : "");
    }
    if (err != MT_OK) {
        cJSON_AddStringToObject(out, "error", "failed to save config file; changes are active only in memory");
        cJSON_AddStringToObject(out, "code", "PERSISTENCE_FAILED");
        cJSON_AddBoolToObject(out, "applied", true);
        mt_http_res_write_json(res, 500, out);
        return;
    }
    mt_http_res_write_json(res, 200, out);
}

void mt_profiles_register_routes(mt_httpd_t *httpd, mt_system_ctx_t *ctx)
{
    if (mt_httpd_route(httpd, "GET", "/api/v1/profiles", handle_get_profiles, ctx) != MT_OK ||
        mt_httpd_route(httpd, "PUT", "/api/v1/profiles", handle_put_profiles, ctx) != MT_OK) {
        MT_ERROR("failed to register profile API routes");
    }
}
