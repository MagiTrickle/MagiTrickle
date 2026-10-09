/* Data models mirroring src/backend/models (Go).
 *
 * Ownership: every char* inside a model is owned by that model (strdup'd on
 * set, freed by the *_free function). Arrays own their elements. Models are
 * plain single-threaded data — concurrent access is the caller's problem
 * (the future core will publish immutable snapshots, decisions.md D-12).
 *
 * Contract notes (compatibility-contract.md §1):
 * - absent `enable` in YAML unmarshals to false (plain Go bool);
 * - rule types: domain | namespace | wildcard | regex | subnet | subnet6;
 * - group color is normalized at load: invalid -> "#ffffff", valid ->
 *   lowercased.
 */
#ifndef MAGITRICKLE_MODELS_H
#define MAGITRICKLE_MODELS_H

#include <stdbool.h>
#include <stdint.h>

#include "magitrickle/duration.h"
#include "magitrickle/err.h"
#include "magitrickle/id.h"

#define MT_RULE_DOMAIN    "domain"
#define MT_RULE_NAMESPACE "namespace"
#define MT_RULE_WILDCARD  "wildcard"
#define MT_RULE_REGEX     "regex"
#define MT_RULE_SUBNET    "subnet"
#define MT_RULE_SUBNET6   "subnet6"

/* Routing precedence shared by user groups and subscriptions. Larger values
 * win; an omitted field in a legacy config or create request uses its default. */
#define MT_PRIORITY_MIN 1
#define MT_PRIORITY_MAX 1000
#define MT_GROUP_DEFAULT_PRIORITY 300
#define MT_SUBSCRIPTION_DEFAULT_PRIORITY 100

typedef struct mt_rule {
    mt_id_t id;
    char *name;
    char *type;
    char *rule;
    bool enable;
} mt_rule_t;

typedef struct mt_group {
    mt_id_t id;
    char *name;
    char *color;
    char *iface; /* yaml key: interface; configured primary, never runtime active */
    char *profile; /* optional stable profile ID; authoritative when nonempty */
    bool enable;
    uint16_t priority;
    mt_rule_t **rules;
    size_t n_rules;
} mt_group_t;

typedef struct mt_sub_rule {
    mt_id_t id;
    char *rule;
    char *type;
    bool enable;
} mt_sub_rule_t;

typedef struct mt_subscription {
    mt_id_t id;
    char *name;
    char *iface;
    char *profile;
    bool enable;
    uint16_t priority;
    char *url;
    uint32_t interval;
    uint32_t last_update;
    uint32_t last_check; /* runtime only, never serialized */
    uint64_t revision;   /* runtime identity for stale fetch rejection */
    bool sync_pending;   /* at most one accepted fetch per incarnation */
    mt_sub_rule_t **rules;
    size_t n_rules;
} mt_subscription_t;

/* A reusable ordered failover chain. The final action is always blackhole.
 * No fixed count limit: profile/interface arrays grow as needed. */
typedef struct mt_profile {
    char *id;
    char *name;
    char **interfaces;
    size_t n_interfaces;
} mt_profile_t;

/* AppConfig with the exact defaults from Go constant.DefaultAppConfig. */
typedef struct mt_app_config {
    struct {
        bool enabled;
        struct {
            bool enabled;
        } auth;
        struct {
            char *address;
            uint16_t port;
        } host;
        char *skin;
    } http_web;
    struct {
        struct {
            char *address;
            uint16_t port;
        } host, upstream;
        bool disable_remap53;
        bool disable_fake_ptr;
        bool disable_drop_aaaa;
        uint64_t max_idle_conns;
        uint64_t max_concurrent;
        mt_duration_t timeout;
    } dns_proxy;
    struct {
        struct {
            char *chain_prefix;
        } iptables;
        struct {
            char *table_prefix;
            mt_duration_t additional_ttl;
        } ipset;
        bool disable_ipv4;
        bool disable_ipv6;
        uint32_t start_mark_table_index;
    } netfilter;
    char **link;
    size_t n_link;
    bool show_all_interfaces;
    char *log_level;
} mt_app_config_t;

/* Whole-application state produced by config load (defaults + overlay). */
typedef struct mt_config {
    mt_app_config_t app;
    mt_group_t **groups;
    size_t n_groups;
    bool groups_present; /* was the `groups` key present in YAML */
    mt_subscription_t **subscriptions;
    size_t n_subscriptions;
    bool subscriptions_present;
    mt_profile_t **profiles;
    size_t n_profiles;
    bool profiles_present;
} mt_config_t;

mt_rule_t *mt_rule_new(void);
void mt_rule_free(mt_rule_t *r);

mt_group_t *mt_group_new(void);
void mt_group_free(mt_group_t *g);
mt_err_t mt_group_add_rule(mt_group_t *g, mt_rule_t *r); /* takes ownership */

mt_sub_rule_t *mt_sub_rule_new(void);
void mt_sub_rule_free(mt_sub_rule_t *r);

mt_subscription_t *mt_subscription_new(void);
void mt_subscription_free(mt_subscription_t *s);
mt_err_t mt_subscription_add_rule(mt_subscription_t *s, mt_sub_rule_t *r);

mt_profile_t *mt_profile_new(void);
void mt_profile_free(mt_profile_t *p);
mt_err_t mt_profile_add_interface(mt_profile_t *p, const char *name);
mt_err_t mt_config_add_profile(mt_config_t *c, mt_profile_t *p); /* takes ownership on success */
void mt_config_clear_profiles(mt_config_t *c);
mt_err_t mt_config_clone_profiles(mt_config_t *dst, const mt_config_t *src);

/* Initialize with DefaultAppConfig values (allocates strings). */
mt_err_t mt_app_config_init_defaults(mt_app_config_t *c);
void mt_app_config_clear(mt_app_config_t *c);
/* Deep copy into an uninitialized destination. Cleans up on failure. */
mt_err_t mt_app_config_clone(mt_app_config_t *dst, const mt_app_config_t *src);

mt_err_t mt_config_init_defaults(mt_config_t *c);
void mt_config_clear(mt_config_t *c);
mt_err_t mt_config_add_group(mt_config_t *c, mt_group_t *g);
/* Frees c->groups[idx] and shifts the remaining pointers down (idx must
 * be < c->n_groups). */
void mt_config_remove_group_by_index(mt_config_t *c, size_t idx);
/* Frees every group and empties c->groups (does not touch subscriptions
 * or app config, unlike mt_config_clear). */
void mt_config_clear_groups(mt_config_t *c);
mt_err_t mt_config_add_subscription(mt_config_t *c, mt_subscription_t *s);
/* Frees c->subscriptions[idx] and shifts the remaining pointers down (idx
 * must be < c->n_subscriptions). */
void mt_config_remove_subscription_by_index(mt_config_t *c, size_t idx);
/* Frees every subscription and empties c->subscriptions (does not touch
 * groups or app config). */
void mt_config_clear_subscriptions(mt_config_t *c);

/* Group color normalization: valid #rrggbb (case-insensitive) is lowercased
 * in place; anything else becomes "#ffffff". Matches Go LoadConfig. */
mt_err_t mt_group_normalize_color(mt_group_t *g);

/* strdup that maps NULL input to NULL output and reports OOM. */
mt_err_t mt_strset(char **dst, const char *src);

#endif /* MAGITRICKLE_MODELS_H */
