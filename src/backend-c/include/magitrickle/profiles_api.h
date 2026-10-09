#ifndef MAGITRICKLE_PROFILES_API_H
#define MAGITRICKLE_PROFILES_API_H

#include "magitrickle/system.h"

/* Protected by the server's existing API authentication middleware.
 * GET /api/v1/profiles, PUT /api/v1/profiles (save by default). */
void mt_profiles_register_routes(mt_httpd_t *httpd, mt_system_ctx_t *ctx);

#endif
