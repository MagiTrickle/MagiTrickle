/* Profile validation/resolution is shared by YAML, HTTP and runtime.
 * Definitions and references are configuration, not connection state. */
#ifndef MAGITRICKLE_PROFILES_H
#define MAGITRICKLE_PROFILES_H

#include "magitrickle/models.h"

#define MT_PROFILE_TERMINAL "blackhole"

const mt_profile_t *mt_profile_find(const mt_config_t *cfg, const char *id);
/* Validates definitions, duplicates and all group/subscription references.
 * Does not require interfaces to exist right now: absent links are backups
 * waiting to appear. `message` is an optional caller-owned error buffer. */
mt_err_t mt_profiles_validate(const mt_config_t *cfg, char *message, size_t size);
mt_err_t mt_route_primary(const mt_config_t *cfg, const char *profile,
                         const char *iface, const char **primary);
mt_err_t mt_route_normalize(const mt_config_t *cfg, const char *profile, char **iface);
/* Normalizes compatibility shadows only, never used by failover events. */
mt_err_t mt_profiles_normalize(mt_config_t *cfg);
bool mt_profile_has_interface(const mt_profile_t *p, const char *iface);

#endif
