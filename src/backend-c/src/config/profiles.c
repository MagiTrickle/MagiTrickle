#include "magitrickle/profiles.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "magitrickle/lookup.h"

const mt_profile_t *mt_profile_find(const mt_config_t *cfg, const char *id)
{
    if (!cfg || !id || !*id) { return NULL; }
    for (size_t i = 0; i < cfg->n_profiles; i++) {
        const mt_profile_t *p = cfg->profiles[i];
        if (p && p->id && strcmp(p->id, id) == 0) { return p; }
    }
    return NULL;
}

bool mt_profile_has_interface(const mt_profile_t *p, const char *iface)
{
    if (!p || !iface) { return false; }
    for (size_t i = 0; i < p->n_interfaces; i++) {
        if (strcmp(p->interfaces[i], iface) == 0) { return true; }
    }
    return false;
}

static bool valid_id(const char *id)
{
    if (!id || !*id || strlen(id) > 64) { return false; }
    for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) { return false; }
    }
    return true;
}

static bool valid_interface(const char *name)
{
    /* Linux IFNAMSIZ includes NUL. Restrict to literal names safe for
     * iptables-restore; '+' would mean a wildcard rather than one link. */
    if (!name || !*name || strlen(name) >= 16 ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
        strcmp(name, MT_PROFILE_TERMINAL) == 0) { return false; }
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' ||
              *p == '.' || *p == ':')) { return false; }
    }
    return true;
}

static bool valid_name(const char *name)
{
    if (!name || !*name || strlen(name) > 256) { return false; }
    bool nonspace = false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 32 || *p == 127) { return false; }
        if (!isspace(*p)) { nonspace = true; }
    }
    return nonspace;
}

static mt_err_t invalid(char *message, size_t size, mt_err_t err,
                        const char *text, const char *id)
{
    if (message && size) { snprintf(message, size, "%s: %s", text, id ? id : ""); }
    return err;
}

mt_err_t mt_profiles_validate(const mt_config_t *cfg, char *message, size_t size)
{
    if (message && size) { message[0] = '\0'; }
    if (!cfg) { return MT_ERR_INVAL; }
    mt_lookup_t ids = {0};
    mt_err_t err = MT_OK;
    for (size_t i = 0; i < cfg->n_profiles; i++) {
        const mt_profile_t *p = cfg->profiles[i];
        if (!p || !valid_id(p->id) || !valid_name(p->name) || !p->n_interfaces || !p->interfaces) {
            err = invalid(message, size, MT_ERR_INVAL, "Invalid or empty profile", p ? p->id : NULL);
            break;
        }
        bool inserted = false;
        err = mt_lookup_put(&ids, p->id, strlen(p->id), i, &inserted);
        if (err != MT_OK) { break; }
        if (!inserted) { err = invalid(message, size, MT_ERR_EXIST, "Duplicate profile ID", p->id); break; }
        mt_lookup_t interfaces = {0};
        for (size_t j = 0; j < p->n_interfaces; j++) {
            if (!valid_interface(p->interfaces[j])) {
                err = invalid(message, size, MT_ERR_INVAL, "Invalid interface in profile", p->id); break;
            }
            err = mt_lookup_put(&interfaces, p->interfaces[j], strlen(p->interfaces[j]), j, &inserted);
            if (err != MT_OK) { break; }
            if (!inserted) { err = invalid(message, size, MT_ERR_INVAL, "Duplicate interface in profile", p->id); break; }
        }
        mt_lookup_clear(&interfaces);
        if (err != MT_OK) { break; }
    }
    for (size_t i = 0; err == MT_OK && i < cfg->n_groups; i++) {
        const char *id = cfg->groups[i]->profile;
        if (id && *id && !mt_lookup_get(&ids, id, strlen(id), NULL)) {
            err = invalid(message, size, MT_ERR_NOENT, "Group references missing profile", id);
        }
    }
    for (size_t i = 0; err == MT_OK && i < cfg->n_subscriptions; i++) {
        const char *id = cfg->subscriptions[i]->profile;
        if (id && *id && !mt_lookup_get(&ids, id, strlen(id), NULL)) {
            err = invalid(message, size, MT_ERR_NOENT, "Subscription references missing profile", id);
        }
    }
    mt_lookup_clear(&ids);
    return err;
}

mt_err_t mt_route_primary(const mt_config_t *cfg, const char *profile,
                         const char *iface, const char **primary)
{
    if (!primary) { return MT_ERR_INVAL; }
    *primary = iface ? iface : "";
    if (!profile || !*profile) { return MT_OK; }
    const mt_profile_t *p = mt_profile_find(cfg, profile);
    if (!p) { return MT_ERR_NOENT; }
    if (!p->n_interfaces || !p->interfaces[0] || !*p->interfaces[0]) { return MT_ERR_INVAL; }
    *primary = p->interfaces[0];
    return MT_OK;
}

mt_err_t mt_route_normalize(const mt_config_t *cfg, const char *profile, char **iface)
{
    if (!iface) { return MT_ERR_INVAL; }
    const char *primary;
    mt_err_t err = mt_route_primary(cfg, profile, *iface, &primary);
    if (err != MT_OK) { return err; }
    if (*iface && strcmp(*iface, primary) == 0) { return MT_OK; }
    return mt_strset(iface, primary);
}

mt_err_t mt_profiles_normalize(mt_config_t *cfg)
{
    mt_err_t err = mt_profiles_validate(cfg, NULL, 0);
    if (err != MT_OK) { return err; }
    for (size_t i = 0; i < cfg->n_groups; i++) {
        err = mt_route_normalize(cfg, cfg->groups[i]->profile, &cfg->groups[i]->iface);
        if (err != MT_OK) { return err; }
    }
    for (size_t i = 0; i < cfg->n_subscriptions; i++) {
        err = mt_route_normalize(cfg, cfg->subscriptions[i]->profile, &cfg->subscriptions[i]->iface);
        if (err != MT_OK) { return err; }
    }
    return MT_OK;
}
