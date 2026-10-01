/* Keenetic RCI interface-alias lookup — port of Go's
 * internal/interfaces/keenetic_router_specific.go
 * (KeeneticRouterSpecificAPI.GetIfaceAliases).
 *
 * Keenetic firmware exposes a local JSON-RPC-ish endpoint ("RCI") on
 * http://127.0.0.1:79 that knows both the user-facing name of an
 * interface ("Home VPN") and the kernel name it is bound to ("nwg0").
 * The WebUI's interface picker shows the former; getifaddrs only ever
 * sees the latter. This module bridges the two, exactly as Go did.
 *
 * Two RCI calls, matching Go:
 *   1. GET  /rci/show/interface  -> object keyed by RCI interface id,
 *      each value carrying "description" and "interface-name".
 *   2. POST /rci/  with one batch array element per interface id, each
 *      {"show":{"interface":{"name":ID,"details":"yes",
 *      "system-name":"yes"}}}; the response is an array matched back to
 *      the request array *positionally* (RCI does not echo the id).
 *
 * Alias selection, byte-for-byte the same rules as Go: prefer the
 * trimmed "description", fall back to the trimmed "interface-name",
 * and skip the entry entirely when the result is empty or identical to
 * the system name (nothing to show the user that they don't already
 * see).
 *
 * Built on every platform, but mt_kn_get_iface_aliases() only performs
 * the RCI calls under -DMT_ENTWARE_KN; elsewhere it yields an empty set
 * without touching the network, mirroring Go's DummyRouterSpecificAPI
 * and its !entware_kn build tag. */
#ifndef MAGITRICKLE_KEENETIC_RCI_H
#define MAGITRICKLE_KEENETIC_RCI_H

#include <stddef.h>

#include "magitrickle/err.h"

/* Default RCI endpoint, as in Go's keeneticRCIBaseURL. */
#define MT_KN_RCI_BASE_URL "http://127.0.0.1:79"

/* Go's keeneticRCITimeout (2s), applied per RCI call. */
#define MT_KN_RCI_TIMEOUT_SECONDS 2L

/* Hard cap on an RCI response body. Go had no cap; this is a local,
 * trusted endpoint, but an unbounded read into a router's small RAM is
 * a worse failure mode than giving up on aliases, so cap and fail. */
#define MT_KN_RCI_MAX_BODY_BYTES (1024 * 1024)

/* One resolved alias: the kernel interface name getifaddrs reports, and
 * the human-facing name to show for it. Both are fixed-size and
 * truncate rather than allocate -- `system_name` is bounded by IFNAMSIZ
 * anyway, and `alias` matches mt_iface_info_t.name's own width, so a
 * longer description would be truncated at the destination regardless. */
typedef struct mt_kn_alias {
    char system_name[16]; /* IFNAMSIZ */
    char alias[64];       /* == sizeof(mt_iface_info_t.name) */
} mt_kn_alias_t;

typedef struct mt_kn_aliases {
    mt_kn_alias_t *items;
    size_t n;
} mt_kn_aliases_t;

/* Fetches system-name -> alias for every RCI interface.
 *
 * On success *out owns an array the caller frees with
 * mt_kn_aliases_free(); an empty result (n == 0, items == NULL) is a
 * success, not an error -- a router with nothing worth aliasing is
 * normal.
 *
 * Returns MT_ERR_UPSTREAM when RCI is unreachable or answers non-200,
 * MT_ERR_PROTO on a malformed body, MT_ERR_NOMEM on allocation
 * failure. Callers are expected to treat any failure as "no aliases"
 * and carry on, the way Go's interfaces.List logged and continued. */
mt_err_t mt_kn_get_iface_aliases(mt_kn_aliases_t *out);

/* Same, against an explicit base URL (no trailing slash required).
 * Exists so tests can point at a local stub server, mirroring the
 * BaseURL field Go's test overrode. Performs the RCI calls on every
 * platform, including non-Keenetic ones -- unlike
 * mt_kn_get_iface_aliases(), which is compiled out unless
 * MT_ENTWARE_KN. */
mt_err_t mt_kn_get_iface_aliases_from(const char *base_url, mt_kn_aliases_t *out);

/* Returns the alias for `system_name`, or NULL when there is none.
 * NULL/empty `aliases` is fine and yields NULL. */
const char *mt_kn_aliases_lookup(const mt_kn_aliases_t *aliases, const char *system_name);

/* Frees *aliases and zeroes it. Safe on a zeroed/NULL struct. */
void mt_kn_aliases_free(mt_kn_aliases_t *aliases);

/* ---- parsing internals, exposed for unit tests ------------------------------- */

/* One interface as RCI describes it, before alias selection. */
typedef struct mt_kn_iface_meta {
    char id[64];             /* RCI interface id, e.g. "Wireguard0" */
    char description[64];    /* user-set label, may be absent */
    char interface_name[64]; /* RCI's own label, fallback for description */
    char system_name[16];    /* kernel name; filled by the second call */
} mt_kn_iface_meta_t;

/* Parses GET /rci/show/interface. Object members whose value is not an
 * object are skipped, matching Go's per-entry json.Unmarshal error
 * `continue`. Caller frees *out. */
mt_err_t mt_kn_parse_interface_list(const char *json, mt_kn_iface_meta_t **out, size_t *out_n);

/* Builds the POST /rci/ batch body for `metas`. Caller frees the
 * returned string. Returns NULL on allocation failure. */
char *mt_kn_build_system_name_request(const mt_kn_iface_meta_t *metas, size_t n);

/* Parses the POST /rci/ response and fills `metas[i].system_name`
 * positionally. Extra response elements are ignored; missing ones leave
 * system_name empty -- both mirror Go's bounds-checked loop. */
mt_err_t mt_kn_parse_system_names(const char *json, mt_kn_iface_meta_t *metas, size_t n);

/* Applies the description/interface-name/skip rules to produce the
 * final map. Caller frees *out with mt_kn_aliases_free(). */
mt_err_t mt_kn_build_aliases(const mt_kn_iface_meta_t *metas, size_t n, mt_kn_aliases_t *out);

#endif /* MAGITRICKLE_KEENETIC_RCI_H */
