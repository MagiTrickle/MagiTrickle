/* JSON helpers on top of cJSON (see decisions.md D-05) — thin wrappers
 * matching the shape of api/utils/helpers.go (WriteJson/WriteError/
 * ReadJson) that the HTTP handler layer (Phase 6) builds on. cJSON is
 * dynamically linked (available in both OpenWrt and Entware feeds, per
 * dependencies.md), same pattern as libyaml/pcre2/libmnl.
 *
 * Byte-for-byte identity with Go's encoding/json output (e.g. its
 * default HTML-escaping of '<'/'>'/'&') is NOT a goal: the compatibility
 * contract for the API is verified by *structural* (parsed, normalized)
 * comparison, not raw bytes (migration-plan.md Phase 6: "same requests,
 * normalized comparison") — a JSON parser reads < and a literal '<'
 * identically, so this only affects wire bytes, never observable API
 * behaviour.
 */
#ifndef MAGITRICKLE_JSON_H
#define MAGITRICKLE_JSON_H

#include <cjson/cJSON.h>

/* Builds {"error": "<msg>"}, matching api/types.ErrorRes. Caller owns the
 * result (cJSON_Delete). msg is copied. */
cJSON *mt_json_error(const char *msg);

/* Compact (no whitespace) serialization, matching Go's default
 * json.Marshal formatting (no indentation). Caller frees with free().
 * Returns NULL on OOM. */
char *mt_json_dump(const cJSON *obj);

#endif /* MAGITRICKLE_JSON_H */
