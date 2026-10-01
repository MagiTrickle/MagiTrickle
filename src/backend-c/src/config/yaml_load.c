/* Config loader: libyaml document API + yaml.v2 typing semantics +
 * Go LoadConfig overlay behaviour (config.go). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#include "magitrickle/log.h"
#include "magitrickle/yamlio.h"
#include "yaml_scalar.h"

typedef yaml_node_t node_t;

static node_t *doc_root(yaml_document_t *doc)
{
    return yaml_document_get_root_node(doc);
}

static node_t *node_of(yaml_document_t *doc, yaml_node_item_t idx)
{
    return yaml_document_get_node(doc, idx);
}

static const char *scalar_text(const node_t *n)
{
    return (const char *)n->data.scalar.value;
}

static bool scalar_is_plain(const node_t *n)
{
    return n->data.scalar.style == YAML_PLAIN_SCALAR_STYLE;
}

/* A scalar node counts as "null" (absent) when it is plain and resolves
 * to null — mirrors yaml.v2 pointer-field behaviour. */
static bool node_is_null(const node_t *n)
{
    return n->type == YAML_SCALAR_NODE && scalar_is_plain(n) &&
           mt_yaml_resolve_plain(scalar_text(n)).kind == MT_SCALAR_NULL;
}

/* map lookup by key (scalar keys only) */
static node_t *map_get(yaml_document_t *doc, node_t *map, const char *key)
{
    if (map->type != YAML_MAPPING_NODE) {
        return NULL;
    }
    for (yaml_node_pair_t *p = map->data.mapping.pairs.start;
         p < map->data.mapping.pairs.top; p++) {
        node_t *k = node_of(doc, p->key);
        if (k != NULL && k->type == YAML_SCALAR_NODE &&
            strcmp(scalar_text(k), key) == 0) {
            return node_of(doc, p->value);
        }
    }
    return NULL;
}

/* ---- typed getters; every getter: node may be NULL/null -> "absent",
 * type mismatch -> MT_ERR_INVAL (like yaml.v2 unmarshal errors) ---- */

typedef enum getter_res {
    GET_ABSENT = 0,
    GET_OK,
    GET_ERR,
} getter_res_t;

static getter_res_t get_string(node_t *n, char **dst, mt_err_t *err)
{
    if (n == NULL || node_is_null(n)) {
        return GET_ABSENT;
    }
    if (n->type != YAML_SCALAR_NODE) {
        *err = MT_ERR_INVAL;
        return GET_ERR;
    }
    /* any scalar coerces to its literal text */
    if (mt_strset(dst, scalar_text(n)) != MT_OK) {
        *err = MT_ERR_NOMEM;
        return GET_ERR;
    }
    return GET_OK;
}

static getter_res_t get_bool(node_t *n, bool *dst, mt_err_t *err)
{
    if (n == NULL || node_is_null(n)) {
        return GET_ABSENT;
    }
    if (n->type != YAML_SCALAR_NODE || !scalar_is_plain(n)) {
        *err = MT_ERR_INVAL;
        return GET_ERR;
    }
    mt_scalar_value_t v = mt_yaml_resolve_plain(scalar_text(n));
    if (v.kind != MT_SCALAR_BOOL) {
        *err = MT_ERR_INVAL;
        return GET_ERR;
    }
    *dst = v.b;
    return GET_OK;
}

static getter_res_t get_uint64(node_t *n, uint64_t max, uint64_t *dst,
                               mt_err_t *err)
{
    if (n == NULL || node_is_null(n)) {
        return GET_ABSENT;
    }
    if (n->type != YAML_SCALAR_NODE || !scalar_is_plain(n)) {
        *err = MT_ERR_INVAL;
        return GET_ERR;
    }
    mt_scalar_value_t v = mt_yaml_resolve_plain(scalar_text(n));
    uint64_t value;
    if (v.kind == MT_SCALAR_INT) {
        if (v.is_uint) {
            value = v.u;
        } else if (v.i < 0) {
            *err = MT_ERR_INVAL;
            return GET_ERR;
        } else {
            value = (uint64_t)v.i;
        }
    } else if (v.kind == MT_SCALAR_FLOAT && v.f >= 0 &&
               v.f == (double)(uint64_t)v.f) {
        /* yaml.v2 accepts integral floats into uints (u3: 1.0) */
        value = (uint64_t)v.f;
    } else {
        *err = MT_ERR_INVAL;
        return GET_ERR;
    }
    if (value > max) {
        *err = MT_ERR_INVAL;
        return GET_ERR;
    }
    *dst = value;
    return GET_OK;
}

static getter_res_t get_duration(node_t *n, mt_duration_t *dst, mt_err_t *err)
{
    if (n == NULL || node_is_null(n)) {
        return GET_ABSENT;
    }
    if (n->type != YAML_SCALAR_NODE) {
        *err = MT_ERR_INVAL;
        return GET_ERR;
    }
    if (scalar_is_plain(n)) {
        mt_scalar_value_t v = mt_yaml_resolve_plain(scalar_text(n));
        if (v.kind == MT_SCALAR_INT) {
            *dst = v.is_uint ? (mt_duration_t)v.u : (mt_duration_t)v.i;
            return GET_OK;
        }
        if (v.kind != MT_SCALAR_STR) {
            *err = MT_ERR_INVAL;
            return GET_ERR;
        }
    }
    /* string (plain or quoted): time.ParseDuration */
    if (mt_duration_parse(scalar_text(n), dst) != MT_OK) {
        *err = MT_ERR_INVAL;
        return GET_ERR;
    }
    return GET_OK;
}

static getter_res_t get_id(node_t *n, mt_id_t *dst, mt_err_t *err)
{
    if (n == NULL || node_is_null(n)) {
        return GET_ABSENT;
    }
    if (n->type != YAML_SCALAR_NODE) {
        *err = MT_ERR_INVAL;
        return GET_ERR;
    }
    if (mt_id_parse(scalar_text(n), dst) != MT_OK) {
        *err = MT_ERR_INVAL;
        return GET_ERR;
    }
    return GET_OK;
}

static getter_res_t get_string_list(yaml_document_t *doc, node_t *n,
                                    char ***dst, size_t *dst_n,
                                    mt_err_t *err)
{
    if (n == NULL || node_is_null(n)) {
        return GET_ABSENT;
    }
    if (n->type != YAML_SEQUENCE_NODE) {
        *err = MT_ERR_INVAL;
        return GET_ERR;
    }
    size_t count = (size_t)(n->data.sequence.items.top -
                            n->data.sequence.items.start);
    char **list = calloc(count > 0 ? count : 1, sizeof(char *));
    if (list == NULL) {
        *err = MT_ERR_NOMEM;
        return GET_ERR;
    }
    size_t idx = 0;
    for (yaml_node_item_t *it = n->data.sequence.items.start;
         it < n->data.sequence.items.top; it++) {
        node_t *item = node_of(doc, *it);
        if (item == NULL || item->type != YAML_SCALAR_NODE) {
            for (size_t i = 0; i < idx; i++) {
                free(list[i]);
            }
            free(list);
            *err = MT_ERR_INVAL;
            return GET_ERR;
        }
        list[idx] = strdup(scalar_text(item));
        if (list[idx] == NULL) {
            for (size_t i = 0; i < idx; i++) {
                free(list[i]);
            }
            free(list);
            *err = MT_ERR_NOMEM;
            return GET_ERR;
        }
        idx++;
    }
    for (size_t i = 0; i < *dst_n; i++) {
        free((*dst)[i]);
    }
    free(*dst);
    *dst = list;
    *dst_n = idx;
    return GET_OK;
}

/* ---- section loaders ---- */

#define GET_OR_FAIL(expr)          \
    do {                           \
        if ((expr) == GET_ERR) {   \
            return err;            \
        }                          \
    } while (0)

static mt_err_t load_addr_port(yaml_document_t *doc, node_t *n, char **addr,
                               uint16_t *port)
{
    mt_err_t err = MT_OK;
    if (n == NULL || node_is_null(n)) {
        return MT_OK;
    }
    if (n->type != YAML_MAPPING_NODE) {
        return MT_ERR_INVAL;
    }
    GET_OR_FAIL(get_string(map_get(doc, n, "address"), addr, &err));
    uint64_t p;
    getter_res_t r = get_uint64(map_get(doc, n, "port"), UINT16_MAX, &p, &err);
    if (r == GET_ERR) {
        return err;
    }
    if (r == GET_OK) {
        *port = (uint16_t)p;
    }
    return MT_OK;
}

static mt_err_t load_app(yaml_document_t *doc, node_t *app,
                         mt_app_config_t *c)
{
    mt_err_t err = MT_OK;
    if (app->type != YAML_MAPPING_NODE) {
        return MT_ERR_INVAL;
    }

    node_t *hw = map_get(doc, app, "httpWeb");
    if (hw != NULL && !node_is_null(hw)) {
        if (hw->type != YAML_MAPPING_NODE) {
            return MT_ERR_INVAL;
        }
        GET_OR_FAIL(get_bool(map_get(doc, hw, "enabled"),
                             &c->http_web.enabled, &err));
        GET_OR_FAIL(get_string(map_get(doc, hw, "skin"), &c->http_web.skin,
                               &err));
        err = load_addr_port(doc, map_get(doc, hw, "host"),
                             &c->http_web.host.address,
                             &c->http_web.host.port);
        if (err != MT_OK) {
            return err;
        }
        node_t *auth = map_get(doc, hw, "auth");
        if (auth != NULL && !node_is_null(auth)) {
            if (auth->type != YAML_MAPPING_NODE) {
                return MT_ERR_INVAL;
            }
            GET_OR_FAIL(get_bool(map_get(doc, auth, "enabled"),
                                 &c->http_web.auth.enabled, &err));
        }
    }

    node_t *dp = map_get(doc, app, "dnsProxy");
    if (dp != NULL && !node_is_null(dp)) {
        if (dp->type != YAML_MAPPING_NODE) {
            return MT_ERR_INVAL;
        }
        err = load_addr_port(doc, map_get(doc, dp, "upstream"),
                             &c->dns_proxy.upstream.address,
                             &c->dns_proxy.upstream.port);
        if (err != MT_OK) {
            return err;
        }
        err = load_addr_port(doc, map_get(doc, dp, "host"),
                             &c->dns_proxy.host.address,
                             &c->dns_proxy.host.port);
        if (err != MT_OK) {
            return err;
        }
        GET_OR_FAIL(get_bool(map_get(doc, dp, "disableRemap53"),
                             &c->dns_proxy.disable_remap53, &err));
        GET_OR_FAIL(get_bool(map_get(doc, dp, "disableFakePTR"),
                             &c->dns_proxy.disable_fake_ptr, &err));
        GET_OR_FAIL(get_bool(map_get(doc, dp, "disableDropAAAA"),
                             &c->dns_proxy.disable_drop_aaaa, &err));
        GET_OR_FAIL(get_uint64(map_get(doc, dp, "maxIdleConns"), UINT64_MAX,
                               &c->dns_proxy.max_idle_conns, &err));
        GET_OR_FAIL(get_uint64(map_get(doc, dp, "maxConcurrent"), UINT64_MAX,
                               &c->dns_proxy.max_concurrent, &err));
        mt_duration_t t;
        getter_res_t r = get_duration(map_get(doc, dp, "timeout"), &t, &err);
        if (r == GET_ERR) {
            return err;
        }
        if (r == GET_OK) {
            /* legacy: value below 1ms is interpreted as milliseconds
             * (config.go TODO before 1.0.0) */
            if (t < MT_DURATION_MS) {
                t *= MT_DURATION_MS;
            }
            c->dns_proxy.timeout = t;
        }
    }

    node_t *nf = map_get(doc, app, "netfilter");
    if (nf != NULL && !node_is_null(nf)) {
        if (nf->type != YAML_MAPPING_NODE) {
            return MT_ERR_INVAL;
        }
        node_t *ipt = map_get(doc, nf, "iptables");
        if (ipt != NULL && !node_is_null(ipt)) {
            if (ipt->type != YAML_MAPPING_NODE) {
                return MT_ERR_INVAL;
            }
            GET_OR_FAIL(get_string(map_get(doc, ipt, "chainPrefix"),
                                   &c->netfilter.iptables.chain_prefix,
                                   &err));
        }
        node_t *ips = map_get(doc, nf, "ipset");
        if (ips != NULL && !node_is_null(ips)) {
            if (ips->type != YAML_MAPPING_NODE) {
                return MT_ERR_INVAL;
            }
            GET_OR_FAIL(get_string(map_get(doc, ips, "tablePrefix"),
                                   &c->netfilter.ipset.table_prefix, &err));
            mt_duration_t ttl;
            getter_res_t r =
                get_duration(map_get(doc, ips, "additionalTTL"), &ttl, &err);
            if (r == GET_ERR) {
                return err;
            }
            if (r == GET_OK) {
                /* legacy: below 1s means seconds */
                if (ttl < MT_DURATION_SEC) {
                    ttl *= MT_DURATION_SEC;
                }
                c->netfilter.ipset.additional_ttl = ttl;
            }
        }
        GET_OR_FAIL(get_bool(map_get(doc, nf, "disableIPv4"),
                             &c->netfilter.disable_ipv4, &err));
        GET_OR_FAIL(get_bool(map_get(doc, nf, "disableIPv6"),
                             &c->netfilter.disable_ipv6, &err));
        uint64_t idx;
        getter_res_t r = get_uint64(map_get(doc, nf, "startMarkTableIndex"),
                                    UINT32_MAX, &idx, &err);
        if (r == GET_ERR) {
            return err;
        }
        if (r == GET_OK) {
            c->netfilter.start_mark_table_index = (uint32_t)idx;
        }
    }

    GET_OR_FAIL(get_string_list(doc, map_get(doc, app, "link"), &c->link,
                                &c->n_link, &err));
    GET_OR_FAIL(get_bool(map_get(doc, app, "showAllInterfaces"),
                         &c->show_all_interfaces, &err));
    GET_OR_FAIL(get_string(map_get(doc, app, "logLevel"), &c->log_level,
                           &err));
    return MT_OK;
}

static mt_err_t load_rule(yaml_document_t *doc, node_t *n, mt_rule_t *r)
{
    mt_err_t err = MT_OK;
    if (n->type != YAML_MAPPING_NODE) {
        return MT_ERR_INVAL;
    }
    GET_OR_FAIL(get_id(map_get(doc, n, "id"), &r->id, &err));
    GET_OR_FAIL(get_string(map_get(doc, n, "name"), &r->name, &err));
    GET_OR_FAIL(get_string(map_get(doc, n, "type"), &r->type, &err));
    GET_OR_FAIL(get_string(map_get(doc, n, "rule"), &r->rule, &err));
    GET_OR_FAIL(get_bool(map_get(doc, n, "enable"), &r->enable, &err));
    if (r->name == NULL) {
        err = mt_strset(&r->name, "");
        if (err != MT_OK) {
            return err;
        }
    }
    if (r->type == NULL && (err = mt_strset(&r->type, "")) != MT_OK) {
        return err;
    }
    if (r->rule == NULL && (err = mt_strset(&r->rule, "")) != MT_OK) {
        return err;
    }
    return MT_OK;
}

static mt_err_t load_group(yaml_document_t *doc, node_t *n, mt_group_t *g)
{
    mt_err_t err = MT_OK;
    if (n->type != YAML_MAPPING_NODE) {
        return MT_ERR_INVAL;
    }
    GET_OR_FAIL(get_id(map_get(doc, n, "id"), &g->id, &err));
    GET_OR_FAIL(get_string(map_get(doc, n, "name"), &g->name, &err));
    GET_OR_FAIL(get_string(map_get(doc, n, "color"), &g->color, &err));
    GET_OR_FAIL(get_string(map_get(doc, n, "interface"), &g->iface, &err));
    GET_OR_FAIL(get_bool(map_get(doc, n, "enable"), &g->enable, &err));
    if (g->name == NULL && (err = mt_strset(&g->name, "")) != MT_OK) {
        return err;
    }
    if (g->iface == NULL && (err = mt_strset(&g->iface, "")) != MT_OK) {
        return err;
    }

    node_t *rules = map_get(doc, n, "rules");
    if (rules != NULL && !node_is_null(rules)) {
        if (rules->type != YAML_SEQUENCE_NODE) {
            return MT_ERR_INVAL;
        }
        for (yaml_node_item_t *it = rules->data.sequence.items.start;
             it < rules->data.sequence.items.top; it++) {
            node_t *item = node_of(doc, *it);
            if (item == NULL) {
                return MT_ERR_INVAL;
            }
            mt_rule_t *rule = mt_rule_new();
            if (rule == NULL) {
                return MT_ERR_NOMEM;
            }
            err = load_rule(doc, item, rule);
            if (err == MT_OK) {
                err = mt_group_add_rule(g, rule);
            }
            if (err != MT_OK) {
                mt_rule_free(rule);
                return err;
            }
        }
    }
    return MT_OK;
}

static mt_err_t load_sub_rule(yaml_document_t *doc, node_t *n,
                              mt_sub_rule_t *r)
{
    mt_err_t err = MT_OK;
    if (n->type != YAML_MAPPING_NODE) {
        return MT_ERR_INVAL;
    }
    GET_OR_FAIL(get_id(map_get(doc, n, "id"), &r->id, &err));
    GET_OR_FAIL(get_string(map_get(doc, n, "rule"), &r->rule, &err));
    GET_OR_FAIL(get_string(map_get(doc, n, "type"), &r->type, &err));
    GET_OR_FAIL(get_bool(map_get(doc, n, "enable"), &r->enable, &err));
    if (r->rule == NULL && (err = mt_strset(&r->rule, "")) != MT_OK) {
        return err;
    }
    if (r->type == NULL && (err = mt_strset(&r->type, "")) != MT_OK) {
        return err;
    }
    return MT_OK;
}

static mt_err_t load_subscription(yaml_document_t *doc, node_t *n,
                                  mt_subscription_t *s)
{
    mt_err_t err = MT_OK;
    if (n->type != YAML_MAPPING_NODE) {
        return MT_ERR_INVAL;
    }
    GET_OR_FAIL(get_id(map_get(doc, n, "id"), &s->id, &err));
    GET_OR_FAIL(get_string(map_get(doc, n, "name"), &s->name, &err));
    GET_OR_FAIL(get_string(map_get(doc, n, "interface"), &s->iface, &err));
    GET_OR_FAIL(get_bool(map_get(doc, n, "enable"), &s->enable, &err));
    GET_OR_FAIL(get_string(map_get(doc, n, "url"), &s->url, &err));
    uint64_t v;
    getter_res_t r = get_uint64(map_get(doc, n, "interval"), UINT32_MAX, &v,
                                &err);
    if (r == GET_ERR) {
        return err;
    }
    if (r == GET_OK) {
        s->interval = (uint32_t)v;
    }
    r = get_uint64(map_get(doc, n, "last_update"), UINT32_MAX, &v, &err);
    if (r == GET_ERR) {
        return err;
    }
    if (r == GET_OK) {
        s->last_update = (uint32_t)v;
    }
    if (s->name == NULL && (err = mt_strset(&s->name, "")) != MT_OK) {
        return err;
    }
    if (s->iface == NULL && (err = mt_strset(&s->iface, "")) != MT_OK) {
        return err;
    }
    if (s->url == NULL && (err = mt_strset(&s->url, "")) != MT_OK) {
        return err;
    }

    node_t *rules = map_get(doc, n, "rules");
    if (rules != NULL && !node_is_null(rules)) {
        if (rules->type != YAML_SEQUENCE_NODE) {
            return MT_ERR_INVAL;
        }
        for (yaml_node_item_t *it = rules->data.sequence.items.start;
             it < rules->data.sequence.items.top; it++) {
            node_t *item = node_of(doc, *it);
            if (item == NULL) {
                return MT_ERR_INVAL;
            }
            mt_sub_rule_t *rule = mt_sub_rule_new();
            if (rule == NULL) {
                return MT_ERR_NOMEM;
            }
            err = load_sub_rule(doc, item, rule);
            if (err == MT_OK) {
                err = mt_subscription_add_rule(s, rule);
            }
            if (err != MT_OK) {
                mt_sub_rule_free(rule);
                return err;
            }
        }
    }
    return MT_OK;
}

static bool group_ids_conflict(mt_config_t *cfg, mt_group_t *g)
{
    for (size_t i = 0; i < cfg->n_groups; i++) {
        if (mt_id_equal(cfg->groups[i]->id, g->id)) {
            return true;
        }
    }
    for (size_t i = 0; i < g->n_rules; i++) {
        for (size_t j = i + 1; j < g->n_rules; j++) {
            if (mt_id_equal(g->rules[i]->id, g->rules[j]->id)) {
                return true;
            }
        }
    }
    return false;
}

mt_err_t mt_config_load_buffer(mt_config_t *cfg, const char *buf, size_t len)
{
    yaml_parser_t parser;
    yaml_document_t doc;
    if (!yaml_parser_initialize(&parser)) {
        return MT_ERR_NOMEM;
    }
    yaml_parser_set_input_string(&parser, (const unsigned char *)buf, len);
    if (!yaml_parser_load(&parser, &doc)) {
        yaml_parser_delete(&parser);
        return MT_ERR_PROTO; /* malformed YAML */
    }
    yaml_parser_delete(&parser);

    mt_err_t err = MT_OK;
    node_t *root = doc_root(&doc);

    /* empty document -> zero config -> version check fails, like Go */
    const char *version = "";
    char *version_owned = NULL;
    node_t *version_node = NULL;
    if (root != NULL) {
        if (root->type != YAML_MAPPING_NODE) {
            err = MT_ERR_INVAL;
            goto out;
        }
        version_node = map_get(&doc, root, "configVersion");
    }
    if (version_node != NULL) {
        getter_res_t r = get_string(version_node, &version_owned, &err);
        if (r == GET_ERR) {
            goto out;
        }
        if (r == GET_OK) {
            version = version_owned;
        }
    }
    if (strncmp(version, "0.", 2) != 0) {
        err = MT_ERR_STATE; /* ErrConfigUnsupportedVersion */
        goto out;
    }

    node_t *app = root != NULL ? map_get(&doc, root, "app") : NULL;
    if (app != NULL && !node_is_null(app)) {
        err = load_app(&doc, app, &cfg->app);
        if (err != MT_OK) {
            goto out;
        }
    }

    node_t *groups = root != NULL ? map_get(&doc, root, "groups") : NULL;
    if (groups != NULL && !node_is_null(groups)) {
        if (groups->type != YAML_SEQUENCE_NODE) {
            err = MT_ERR_INVAL;
            goto out;
        }
        /* replace existing groups */
        for (size_t i = 0; i < cfg->n_groups; i++) {
            mt_group_free(cfg->groups[i]);
        }
        cfg->n_groups = 0;
        cfg->groups_present = true;
        for (yaml_node_item_t *it = groups->data.sequence.items.start;
             it < groups->data.sequence.items.top; it++) {
            node_t *item = node_of(&doc, *it);
            if (item == NULL) {
                err = MT_ERR_INVAL;
                goto out;
            }
            mt_group_t *g = mt_group_new();
            if (g == NULL) {
                err = MT_ERR_NOMEM;
                goto out;
            }
            err = load_group(&doc, item, g);
            if (err == MT_OK) {
                err = mt_group_normalize_color(g);
            }
            if (err == MT_OK && group_ids_conflict(cfg, g)) {
                err = MT_ERR_EXIST; /* group/rule id conflict */
            }
            if (err == MT_OK) {
                err = mt_config_add_group(cfg, g);
            }
            if (err != MT_OK) {
                mt_group_free(g);
                goto out;
            }
        }
    }

    node_t *subs = root != NULL ? map_get(&doc, root, "subscriptions") : NULL;
    /* Go: present -> replace, absent -> clear (asymmetry with groups,
     * documented in compatibility-contract §1 / phase-0-report) */
    for (size_t i = 0; i < cfg->n_subscriptions; i++) {
        mt_subscription_free(cfg->subscriptions[i]);
    }
    cfg->n_subscriptions = 0;
    if (subs != NULL && !node_is_null(subs)) {
        if (subs->type != YAML_SEQUENCE_NODE) {
            err = MT_ERR_INVAL;
            goto out;
        }
        cfg->subscriptions_present = true;
        for (yaml_node_item_t *it = subs->data.sequence.items.start;
             it < subs->data.sequence.items.top; it++) {
            node_t *item = node_of(&doc, *it);
            if (item == NULL) {
                err = MT_ERR_INVAL;
                goto out;
            }
            mt_subscription_t *s = mt_subscription_new();
            if (s == NULL) {
                err = MT_ERR_NOMEM;
                goto out;
            }
            err = load_subscription(&doc, item, s);
            if (err == MT_OK) {
                err = mt_config_add_subscription(cfg, s);
            }
            if (err != MT_OK) {
                mt_subscription_free(s);
                goto out;
            }
        }
    }

out:
    free(version_owned);
    yaml_document_delete(&doc);
    return err;
}

mt_err_t mt_config_load_file(mt_config_t *cfg, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return MT_ERR_NOENT;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return MT_ERR_IO;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return MT_ERR_IO;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return MT_ERR_IO;
    }
    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return MT_ERR_NOMEM;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return MT_ERR_IO;
    }
    buf[size] = '\0';
    mt_err_t err = mt_config_load_buffer(cfg, buf, (size_t)size);
    free(buf);
    return err;
}
