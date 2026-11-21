/* Read-only Keenetic RCI integration. No CLI processes or private HTTP client.
 * Compiled only for Entware _kn, using the existing libcurl/cJSON dependencies. */
#ifndef MAGITRICKLE_KEENETIC_RCI_H
#define MAGITRICKLE_KEENETIC_RCI_H
#ifdef MT_ENTWARE_KN
#include <stddef.h>
#include <stdatomic.h>
#include "magitrickle/keenetic_ifstate.h"
#define MT_KN_RCI_BASE_URL "http://127.0.0.1:79"
/* No proxy inheritance or redirects. 500ms connect/2s total timeout per call;
 * caller supplies a body limit and frees the successful response. Outputs are
 * empty on any error. Only worker threads call this synchronous primitive. */
mt_err_t mt_kn_rci_request(const char *url, const char *post, size_t limit,
                            const atomic_bool *stop, char **body, size_t *length);
/* RCI response order matches request order; the firmware does not echo IDs. */
char *mt_kn_build_system_name_request(const mt_kn_ifinfo_t *items, size_t count);
#endif
#endif
