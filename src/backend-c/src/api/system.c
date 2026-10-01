/* See system.h. Port of handlers.go's ListInterfaces/SaveConfig/
 * NetfilterDHook. */
#include "magitrickle/system.h"

#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "magitrickle/log.h"

static void must_route(mt_httpd_t *h, const char *method, const char *pattern, mt_http_handler_fn fn,
                       void *ud) {
    if (mt_httpd_route(h, method, pattern, fn, ud) != MT_OK) {
        MT_ERROR("failed to register route %s %s", method, pattern);
    }
}

static void handle_list_interfaces(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    mt_system_ctx_t *ctx = ud;

    mt_iface_info_t *ifaces = NULL;
    size_t n = 0;
    mt_err_t err = mt_app_list_interfaces(ctx->app, &ifaces, &n);
    if (err != MT_OK) {
        mt_http_res_write_error(res, 500, mt_err_str(err));
        return;
    }

    cJSON *out = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(out, "interfaces");
    cJSON *blackhole = cJSON_CreateObject();
    cJSON_AddStringToObject(blackhole, "id", "blackhole");
    cJSON_AddItemToArray(arr, blackhole);
    for (size_t i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", ifaces[i].id);
        /* InterfaceRes.Name has `omitempty`; ifaces[i].name is always ""
         * here (Keenetic-RCI friendly names are entware_kn-only, deferred
         * to Phase 8), so it's simply never added -- matches Go. */
        if (ifaces[i].name[0] != '\0') { cJSON_AddStringToObject(item, "name", ifaces[i].name); }
        cJSON_AddItemToArray(arr, item);
    }
    free(ifaces);
    mt_http_res_write_json(res, 200, out);
}

static void handle_save_config(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    mt_system_ctx_t *ctx = ud;
    if (!ctx->config_path) {
        mt_http_res_write(res, 200, NULL, NULL, 0);
        return;
    }
    mt_err_t err =
        mt_app_save_config(ctx->app, ctx->config_path, ctx->config_version ? ctx->config_version : "");
    if (err != MT_OK) {
        mt_http_res_write_error(res, 500, mt_err_str(err));
        return;
    }
    mt_http_res_write(res, 200, NULL, NULL, 0);
}

static void handle_netfilterd_hook(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_system_ctx_t *ctx = ud;
    size_t body_len;
    const uint8_t *body = mt_http_req_body(req, &body_len);
    cJSON *json = body_len > 0 ? cJSON_ParseWithLength((const char *)body, body_len) : NULL;
    if (!json) {
        mt_http_res_write_error(res, 400, "failed to parse request");
        return;
    }
    cJSON *type_j = cJSON_GetObjectItemCaseSensitive(json, "type");
    cJSON *table_j = cJSON_GetObjectItemCaseSensitive(json, "table");
    MT_DEBUG("received netfilter.d event: type=%s table=%s",
            cJSON_IsString(type_j) ? type_j->valuestring : "",
            cJSON_IsString(table_j) ? table_j->valuestring : "");
    cJSON_Delete(json);

    /* Never turned into an HTTP error response: the firmware fires this
     * hook while it is still rewriting tables and has nothing to do with
     * a failure. Where a committer runs (Keenetic `_kn`), the call only
     * queues a rebuild and cannot fail at all -- see
     * mt_app_force_commit_iptables. */
    mt_err_t err = mt_app_force_commit_iptables(ctx->app);
    if (err != MT_OK) { MT_ERROR("error fixing iptables after netfilter.d: %s", mt_err_str(err)); }
    mt_http_res_write(res, 200, NULL, NULL, 0);
}

void mt_system_register_routes(mt_httpd_t *h, mt_system_ctx_t *ctx) {
    must_route(h, "GET", "/api/v1/system/interfaces", handle_list_interfaces, ctx);
    must_route(h, "POST", "/api/v1/system/config/save", handle_save_config, ctx);
    must_route(h, "POST", "/api/v1/system/hooks/netfilterd", handle_netfilterd_hook, ctx);
}
