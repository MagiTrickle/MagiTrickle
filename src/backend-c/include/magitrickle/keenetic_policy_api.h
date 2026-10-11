#ifndef MAGITRICKLE_KEENETIC_POLICY_API_H
#define MAGITRICKLE_KEENETIC_POLICY_API_H
#ifdef MT_ENTWARE_KN
#include "magitrickle/app.h"
#include "magitrickle/httpd.h"
#include "magitrickle/loop.h"
#include "magitrickle/keenetic_policy_rci.h"
typedef struct mt_kn_policy_service mt_kn_policy_service_t;
/* Detached transport injectable for unit tests, never controllable by HTTP. */
typedef mt_err_t (*mt_kn_policy_fetch_fn)(mt_kn_policy_entry_t **entries, size_t *count,
                                         const atomic_bool *stop);
mt_err_t mt_kn_policy_service_create(mt_loop_t *loop, mt_app_t *app, const char *config_path,
                                      const char *version, mt_kn_policy_fetch_fn fetch,
                                      mt_kn_policy_service_t **out);
void mt_kn_policy_service_destroy(mt_kn_policy_service_t *service);
mt_err_t mt_kn_policy_register_routes(mt_httpd_t *httpd, mt_kn_policy_service_t *service);
#endif
#endif
