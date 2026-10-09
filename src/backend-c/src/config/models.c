#include "magitrickle/models.h"

#include <stdlib.h>
#include <string.h>

mt_err_t mt_strset(char **dst, const char *src)
{
    char *copy = NULL;
    if (src != NULL) {
        copy = strdup(src);
        if (copy == NULL) {
            return MT_ERR_NOMEM;
        }
    }
    free(*dst);
    *dst = copy;
    return MT_OK;
}

/* ---- rule ---- */

mt_rule_t *mt_rule_new(void)
{
    return calloc(1, sizeof(mt_rule_t));
}

void mt_rule_free(mt_rule_t *r)
{
    if (r == NULL) {
        return;
    }
    free(r->name);
    free(r->type);
    free(r->rule);
    free(r);
}

/* ---- group ---- */

mt_group_t *mt_group_new(void)
{
    return calloc(1, sizeof(mt_group_t));
}

void mt_group_free(mt_group_t *g)
{
    if (g == NULL) {
        return;
    }
    for (size_t i = 0; i < g->n_rules; i++) {
        mt_rule_free(g->rules[i]);
    }
    free(g->rules);
    free(g->name);
    free(g->color);
    free(g->iface);
    free(g->profile);
    free(g);
}

static mt_err_t grow_array(void ***arr, size_t n)
{
    /* grow by doubling starting at 8 */
    if ((n & (n - 1)) == 0 && n >= 8) {
        void **na = realloc(*arr, n * 2 * sizeof(void *));
        if (na == NULL) {
            return MT_ERR_NOMEM;
        }
        *arr = na;
    } else if (n < 8) {
        if (*arr == NULL) {
            void **na = calloc(8, sizeof(void *));
            if (na == NULL) {
                return MT_ERR_NOMEM;
            }
            *arr = na;
        }
    }
    return MT_OK;
}

mt_err_t mt_group_add_rule(mt_group_t *g, mt_rule_t *r)
{
    mt_err_t err = grow_array((void ***)&g->rules, g->n_rules);
    if (err != MT_OK) {
        return err;
    }
    g->rules[g->n_rules++] = r;
    return MT_OK;
}

/* ---- subscription ---- */

mt_sub_rule_t *mt_sub_rule_new(void)
{
    return calloc(1, sizeof(mt_sub_rule_t));
}

void mt_sub_rule_free(mt_sub_rule_t *r)
{
    if (r == NULL) {
        return;
    }
    free(r->rule);
    free(r->type);
    free(r);
}

mt_subscription_t *mt_subscription_new(void)
{
    return calloc(1, sizeof(mt_subscription_t));
}

void mt_subscription_free(mt_subscription_t *s)
{
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < s->n_rules; i++) {
        mt_sub_rule_free(s->rules[i]);
    }
    free(s->rules);
    free(s->name);
    free(s->iface);
    free(s->profile);
    free(s->url);
    free(s);
}

mt_err_t mt_subscription_add_rule(mt_subscription_t *s, mt_sub_rule_t *r)
{
    mt_err_t err = grow_array((void ***)&s->rules, s->n_rules);
    if (err != MT_OK) {
        return err;
    }
    s->rules[s->n_rules++] = r;
    return MT_OK;
}

/* ---- app config ---- */

mt_err_t mt_app_config_init_defaults(mt_app_config_t *c)
{
    memset(c, 0, sizeof(*c));
    /* constant.DefaultAppConfig */
    c->http_web.enabled = true;
    c->http_web.auth.enabled = false;
    c->http_web.host.port = 8080;
    c->dns_proxy.host.port = 3553;
    c->dns_proxy.upstream.port = 53;
    c->dns_proxy.disable_remap53 = false;
    c->dns_proxy.disable_fake_ptr = false;
    c->dns_proxy.disable_drop_aaaa = false;
    c->dns_proxy.max_idle_conns = 10;
    c->dns_proxy.max_concurrent = 100;
    c->dns_proxy.timeout = 5000 * MT_DURATION_MS;
    c->netfilter.ipset.additional_ttl = 3600 * MT_DURATION_SEC;
    c->netfilter.disable_ipv4 = false;
    c->netfilter.disable_ipv6 = false;
    c->netfilter.start_mark_table_index = UINT32_C(0x4D616769); /* "Magi" */
    c->show_all_interfaces = false;

    mt_err_t err;
    if ((err = mt_strset(&c->http_web.host.address, "[::]")) != MT_OK ||
        (err = mt_strset(&c->http_web.skin, "default")) != MT_OK ||
        (err = mt_strset(&c->dns_proxy.host.address, "[::]")) != MT_OK ||
        (err = mt_strset(&c->dns_proxy.upstream.address, "127.0.0.1")) !=
            MT_OK ||
        (err = mt_strset(&c->netfilter.iptables.chain_prefix, "MT_")) !=
            MT_OK ||
        (err = mt_strset(&c->netfilter.ipset.table_prefix, "mt_")) != MT_OK ||
        (err = mt_strset(&c->log_level, "info")) != MT_OK) {
        mt_app_config_clear(c);
        return err;
    }

    c->link = calloc(1, sizeof(char *));
    if (c->link == NULL) {
        mt_app_config_clear(c);
        return MT_ERR_NOMEM;
    }
    c->link[0] = strdup("br0");
    if (c->link[0] == NULL) {
        mt_app_config_clear(c);
        return MT_ERR_NOMEM;
    }
    c->n_link = 1;
    return MT_OK;
}

void mt_app_config_clear(mt_app_config_t *c)
{
    free(c->http_web.host.address);
    free(c->http_web.skin);
    free(c->dns_proxy.host.address);
    free(c->dns_proxy.upstream.address);
    free(c->netfilter.iptables.chain_prefix);
    free(c->netfilter.ipset.table_prefix);
    for (size_t i = 0; i < c->n_link; i++) {
        free(c->link[i]);
    }
    free(c->link);
    free(c->log_level);
    memset(c, 0, sizeof(*c));
}

mt_err_t mt_app_config_clone(mt_app_config_t *dst, const mt_app_config_t *src)
{
    *dst = *src;
    dst->http_web.host.address = NULL;
    dst->http_web.skin = NULL;
    dst->dns_proxy.host.address = NULL;
    dst->dns_proxy.upstream.address = NULL;
    dst->netfilter.iptables.chain_prefix = NULL;
    dst->netfilter.ipset.table_prefix = NULL;
    dst->log_level = NULL;
    dst->link = NULL;
    dst->n_link = 0;
    mt_err_t err;
    if ((err = mt_strset(&dst->http_web.host.address, src->http_web.host.address)) != MT_OK ||
        (err = mt_strset(&dst->http_web.skin, src->http_web.skin)) != MT_OK ||
        (err = mt_strset(&dst->dns_proxy.host.address, src->dns_proxy.host.address)) != MT_OK ||
        (err = mt_strset(&dst->dns_proxy.upstream.address, src->dns_proxy.upstream.address)) != MT_OK ||
        (err = mt_strset(&dst->netfilter.iptables.chain_prefix, src->netfilter.iptables.chain_prefix)) != MT_OK ||
        (err = mt_strset(&dst->netfilter.ipset.table_prefix, src->netfilter.ipset.table_prefix)) != MT_OK ||
        (err = mt_strset(&dst->log_level, src->log_level)) != MT_OK) {
        mt_app_config_clear(dst);
        return err;
    }
    if (src->n_link > 0) {
        dst->link = calloc(src->n_link, sizeof(*dst->link));
        if (!dst->link) { mt_app_config_clear(dst); return MT_ERR_NOMEM; }
        for (size_t i = 0; i < src->n_link; i++) {
            err = mt_strset(&dst->link[i], src->link[i]);
            if (err != MT_OK) { mt_app_config_clear(dst); return err; }
            dst->n_link++;
        }
    }
    return MT_OK;
}

/* ---- config ---- */

mt_err_t mt_config_init_defaults(mt_config_t *c)
{
    memset(c, 0, sizeof(*c));
    return mt_app_config_init_defaults(&c->app);
}

void mt_config_clear(mt_config_t *c)
{
    mt_app_config_clear(&c->app);
    mt_config_clear_profiles(c);
    for (size_t i = 0; i < c->n_groups; i++) {
        mt_group_free(c->groups[i]);
    }
    free(c->groups);
    for (size_t i = 0; i < c->n_subscriptions; i++) {
        mt_subscription_free(c->subscriptions[i]);
    }
    free(c->subscriptions);
    memset(c, 0, sizeof(*c));
}

mt_err_t mt_config_add_group(mt_config_t *c, mt_group_t *g)
{
    mt_err_t err = grow_array((void ***)&c->groups, c->n_groups);
    if (err != MT_OK) {
        return err;
    }
    c->groups[c->n_groups++] = g;
    return MT_OK;
}

void mt_config_remove_group_by_index(mt_config_t *c, size_t idx)
{
    mt_group_free(c->groups[idx]);
    for (size_t i = idx; i + 1 < c->n_groups; i++) {
        c->groups[i] = c->groups[i + 1];
    }
    c->n_groups--;
}

void mt_config_clear_groups(mt_config_t *c)
{
    for (size_t i = 0; i < c->n_groups; i++) {
        mt_group_free(c->groups[i]);
    }
    free(c->groups);
    c->groups = NULL;
    c->n_groups = 0;
}

mt_err_t mt_config_add_subscription(mt_config_t *c, mt_subscription_t *s)
{
    mt_err_t err = grow_array((void ***)&c->subscriptions, c->n_subscriptions);
    if (err != MT_OK) {
        return err;
    }
    c->subscriptions[c->n_subscriptions++] = s;
    return MT_OK;
}

void mt_config_remove_subscription_by_index(mt_config_t *c, size_t idx)
{
    mt_subscription_free(c->subscriptions[idx]);
    for (size_t i = idx; i + 1 < c->n_subscriptions; i++) {
        c->subscriptions[i] = c->subscriptions[i + 1];
    }
    c->n_subscriptions--;
}

void mt_config_clear_subscriptions(mt_config_t *c)
{
    for (size_t i = 0; i < c->n_subscriptions; i++) {
        mt_subscription_free(c->subscriptions[i]);
    }
    free(c->subscriptions);
    c->subscriptions = NULL;
    c->n_subscriptions = 0;
}

/* ---- color normalization (Go: regexp2 `^#[0-9a-f]{6}$` IgnoreCase) ---- */

static bool is_hex_lower_or_upper(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

mt_err_t mt_group_normalize_color(mt_group_t *g)
{
    const char *c = g->color;
    bool valid = c != NULL && strlen(c) == 7 && c[0] == '#';
    if (valid) {
        for (int i = 1; i < 7; i++) {
            if (!is_hex_lower_or_upper(c[i])) {
                valid = false;
                break;
            }
        }
    }
    if (!valid) {
        return mt_strset(&g->color, "#ffffff");
    }
    for (int i = 1; i < 7; i++) {
        if (g->color[i] >= 'A' && g->color[i] <= 'F') {
            g->color[i] = (char)(g->color[i] - 'A' + 'a');
        }
    }
    return MT_OK;
}

/* ---- routing profiles ---- */

mt_profile_t *mt_profile_new(void)
{
    return calloc(1, sizeof(mt_profile_t));
}

void mt_profile_free(mt_profile_t *p)
{
    if (!p) { return; }
    free(p->id);
    free(p->name);
    for (size_t i = 0; i < p->n_interfaces; i++) { free(p->interfaces[i]); }
    free(p->interfaces);
    free(p);
}

mt_err_t mt_profile_add_interface(mt_profile_t *p, const char *name)
{
    if (!p || !name) { return MT_ERR_INVAL; }
    if (p->n_interfaces >= SIZE_MAX / (2 * sizeof(char *))) { return MT_ERR_LIMIT; }
    char *copy = strdup(name);
    if (!copy) { return MT_ERR_NOMEM; }
    mt_err_t err = grow_array((void ***)&p->interfaces, p->n_interfaces);
    if (err != MT_OK) { free(copy); return err; }
    p->interfaces[p->n_interfaces++] = copy;
    return MT_OK;
}

mt_err_t mt_config_add_profile(mt_config_t *c, mt_profile_t *p)
{
    if (!c || !p) { return MT_ERR_INVAL; }
    if (c->n_profiles >= SIZE_MAX / (2 * sizeof(void *))) { return MT_ERR_LIMIT; }
    mt_err_t err = grow_array((void ***)&c->profiles, c->n_profiles);
    if (err != MT_OK) { return err; }
    c->profiles[c->n_profiles++] = p;
    return MT_OK;
}

void mt_config_clear_profiles(mt_config_t *c)
{
    for (size_t i = 0; i < c->n_profiles; i++) { mt_profile_free(c->profiles[i]); }
    free(c->profiles);
    c->profiles = NULL;
    c->n_profiles = 0;
}

mt_err_t mt_config_clone_profiles(mt_config_t *dst, const mt_config_t *src)
{
    if (!dst || !src || dst->profiles || dst->n_profiles) { return MT_ERR_INVAL; }
    mt_err_t err = MT_OK;
    for (size_t i = 0; i < src->n_profiles; i++) {
        const mt_profile_t *s = src->profiles[i];
        mt_profile_t *p = mt_profile_new();
        if (!p) { err = MT_ERR_NOMEM; break; }
        err = mt_strset(&p->id, s->id);
        if (err == MT_OK) { err = mt_strset(&p->name, s->name); }
        for (size_t j = 0; err == MT_OK && j < s->n_interfaces; j++) {
            err = mt_profile_add_interface(p, s->interfaces[j]);
        }
        if (err == MT_OK) { err = mt_config_add_profile(dst, p); }
        if (err != MT_OK) { mt_profile_free(p); break; }
    }
    if (err != MT_OK) { mt_config_clear_profiles(dst); }
    return err;
}
