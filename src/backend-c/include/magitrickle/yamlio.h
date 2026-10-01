/* YAML config load/save with go-yaml-v2 parity (compatibility-contract §1).
 *
 * Load: overlay-present-fields-onto-defaults semantics, yaml.v2 scalar
 * typing rules (quoted numerics are strings, `yes`/`On` are bools, bare
 * ints into durations are nanoseconds, legacy <1ms/<1s normalization),
 * `configVersion` must start with "0.", duplicate group/rule IDs fail,
 * group colors normalized. Unknown keys ignored.
 *
 * Save: byte-identical to Go SaveConfig for the same state (key order,
 * scalar styles, duration strings, flow [] for empty lists) — guarded by
 * the differential suite. Atomic write: tmp file + fsync + rename +
 * directory fsync, mode 0600.
 */
#ifndef MAGITRICKLE_YAMLIO_H
#define MAGITRICKLE_YAMLIO_H

#include <stddef.h>

#include "magitrickle/err.h"
#include "magitrickle/models.h"

/* Parse YAML text into cfg (which must already hold defaults via
 * mt_config_init_defaults). On error the config may be partially
 * modified — mirroring Go's LoadConfig, callers treat it as poisoned. */
mt_err_t mt_config_load_buffer(mt_config_t *cfg, const char *buf, size_t len);

/* Convenience: read whole file. Missing file == MT_ERR_NOENT (Go treats
 * it as "keep defaults" — that decision is the caller's). */
mt_err_t mt_config_load_file(mt_config_t *cfg, const char *path);

/* Serialize the full tree exactly like Go SaveConfig (configVersion is
 * passed in — Go writes constant.Version). Returns a malloc'd buffer. */
mt_err_t mt_config_save_buffer(const mt_config_t *cfg, const char *version,
                               char **out, size_t *out_len);

/* Atomic write to path (0600). */
mt_err_t mt_config_save_file(const mt_config_t *cfg, const char *version,
                             const char *path);

#endif /* MAGITRICKLE_YAMLIO_H */
