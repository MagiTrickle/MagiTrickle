/* GET/PUT/POST/DELETE /api/v1/groups[...]  and the nested
 * /api/v1/groups/{groupID}/rules[...] routes — port of api/v1/handlers.go's
 * group+rule handlers and api/v1/converters.go (compatibility-contract.md
 * §2's groups/rules row). DTO JSON is built/parsed directly with cJSON
 * inside groups.c (mirrors converters.go living in the same Go package as
 * handlers.go) — nothing outside this module needs the wire shape.
 */
#ifndef MAGITRICKLE_GROUPS_H
#define MAGITRICKLE_GROUPS_H

#include "magitrickle/app.h"
#include "magitrickle/httpd.h"

typedef struct mt_groups_ctx {
    mt_app_t *app;
    /* Used when a handler's ?save=true is set, mirroring Go's
     * h.app.SaveConfig() (which has the path/version baked into the App
     * struct already). NULL config_path makes save=true a no-op, which
     * lets tests exercise the handlers without a real file on disk --
     * production wiring (main.c) always supplies both. */
    const char *config_path;
    const char *config_version;
} mt_groups_ctx_t;

/* Registers GET/PUT/POST /api/v1/groups, GET/PUT/DELETE
 * /api/v1/groups/{groupID}, and the nested GET/PUT/POST
 * /api/v1/groups/{groupID}/rules + GET/PUT/DELETE
 * /api/v1/groups/{groupID}/rules/{ruleID} routes on `h`. `ctx` must
 * outlive the server. */
void mt_groups_register_routes(mt_httpd_t *h, mt_groups_ctx_t *ctx);

#endif /* MAGITRICKLE_GROUPS_H */
