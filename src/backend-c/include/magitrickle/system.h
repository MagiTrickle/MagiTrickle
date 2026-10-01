/* GET /api/v1/system/interfaces, POST /api/v1/system/config/save, POST
 * /api/v1/system/hooks/netfilterd — port of the remaining handlers in
 * api/v1/handlers.go not covered by groups.h (compatibility-contract.md
 * §2's system row).
 */
#ifndef MAGITRICKLE_SYSTEM_H
#define MAGITRICKLE_SYSTEM_H

#include "magitrickle/app.h"
#include "magitrickle/httpd.h"

typedef struct mt_system_ctx {
    mt_app_t *app;
    /* Same meaning as mt_groups_ctx_t's fields (groups.h): used for
     * SaveConfig; NULL config_path makes the handler a no-op write
     * (still 200), keeping the module testable without a real file. */
    const char *config_path;
    const char *config_version;
} mt_system_ctx_t;

/* Registers GET /api/v1/system/interfaces, POST
 * /api/v1/system/config/save, and POST /api/v1/system/hooks/netfilterd
 * on `h`. `ctx` must outlive the server. */
void mt_system_register_routes(mt_httpd_t *h, mt_system_ctx_t *ctx);

#endif /* MAGITRICKLE_SYSTEM_H */
