/* GET/PUT/POST /api/v1/subscriptions, DELETE
 * /api/v1/subscriptions/{subscriptionID}, POST
 * /api/v1/subscriptions/{subscriptionID}/sync, and GET
 * /api/v1/subscriptions/rules — port of api/v1/subscription_handlers.go
 * and subscription_converters.go (compatibility-contract.md §2's
 * subscriptions row). The sync/rules routes (Phase 7) are fetch-backed:
 * production uses mt_app_sync_subscription_async and mt_sub_fetcher_submit
 * with deferred HTTP responses; parsing/apply stay on the event loop.
 */
#ifndef MAGITRICKLE_SUBSCRIPTIONS_API_H
#define MAGITRICKLE_SUBSCRIPTIONS_API_H

#include "magitrickle/app.h"
#include "magitrickle/httpd.h"

typedef struct mt_subs_ctx {
    mt_app_t *app;
    mt_sub_fetcher_t *fetcher; /* production async I/O; NULL for synchronous embeddings */
    /* Same meaning as mt_groups_ctx_t's fields (groups.h). Note the
     * subscriptions handlers' ?save= default is the OPPOSITE of the
     * groups handlers': it saves unless save=false is explicit (matches
     * Go's `r.URL.Query().Get("save") != "false"`, vs. groups' `== "true"`
     * -- a real asymmetry already present in the Go handlers, not
     * introduced here). */
    const char *config_path;
    const char *config_version;
} mt_subs_ctx_t;

/* Registers GET/PUT/POST /api/v1/subscriptions and DELETE
 * /api/v1/subscriptions/{subscriptionID} on `h`. `ctx` must outlive the
 * server and every accepted fetch completion. Destroy the fetcher before
 * freeing ctx or its app. */
void mt_subs_register_routes(mt_httpd_t *h, mt_subs_ctx_t *ctx);

#endif /* MAGITRICKLE_SUBSCRIPTIONS_API_H */
