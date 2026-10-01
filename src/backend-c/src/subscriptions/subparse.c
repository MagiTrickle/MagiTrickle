#define PCRE2_CODE_UNIT_WIDTH 8

#include "magitrickle/subparse.h"
#include "magitrickle/id_pool.h"

#include "magitrickle/lookup.h"
#include "magitrickle/rand.h"

#include <ctype.h>
#include <stdio.h>
#include <pcre2.h>
#include <stdlib.h>
#include <string.h>

/* ---- validators (validate.go) ---- */

static bool all_chars_in(const char *s, const char *extra)
{
    for (const char *p = s; *p != '\0'; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.') {
            continue;
        }
        if (extra != NULL && strchr(extra, c) != NULL) {
            continue;
        }
        return false;
    }
    return true;
}

static bool is_valid_domain(const char *p)
{
    size_t len = strlen(p);
    if (len == 0) {
        return false;
    }
    if (p[0] == '.' || p[len - 1] == '.') {
        return false;
    }
    if (strstr(p, "..") != NULL) {
        return false;
    }
    return all_chars_in(p, NULL);
}

static bool is_valid_wildcard(const char *p)
{
    size_t len = strlen(p);
    if (len == 0) {
        return false;
    }
    if (p[0] == '.' || p[len - 1] == '.') {
        return false;
    }
    if (strstr(p, "..") != NULL || strstr(p, "**") != NULL) {
        return false;
    }
    return all_chars_in(p, "*?");
}

static int parse_dec(const char *s, size_t len)
{
    if (len == 0) {
        return -1;
    }
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        if (n > (999999999 - (s[i] - '0')) / 10) {
            return -1;
        }
        n = n * 10 + (s[i] - '0');
    }
    return n;
}

/* subnetRe: ^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?:\/(\d{1,2}))?$
 * with octets 0..255 and prefix 0..32. */
static bool is_valid_subnet(const char *p)
{
    const char *s = p;
    for (int octet = 0; octet < 4; octet++) {
        const char *start = s;
        while (*s >= '0' && *s <= '9') {
            s++;
        }
        size_t dlen = (size_t)(s - start);
        if (dlen < 1 || dlen > 3) {
            return false;
        }
        int v = parse_dec(start, dlen);
        if (v < 0 || v > 255) {
            return false;
        }
        if (octet < 3) {
            if (*s != '.') {
                return false;
            }
            s++;
        }
    }
    if (*s == '/') {
        s++;
        const char *start = s;
        while (*s >= '0' && *s <= '9') {
            s++;
        }
        size_t dlen = (size_t)(s - start);
        if (dlen < 1 || dlen > 2) {
            return false;
        }
        int v = parse_dec(start, dlen);
        if (v < 0 || v > 32) {
            return false;
        }
    }
    return *s == '\0';
}

static bool is_valid_ipv6_chars(const char *ip)
{
    if (strchr(ip, ':') == NULL) {
        return false;
    }
    for (const char *p = ip; *p != '\0'; p++) {
        char c = *p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F') || c == ':') {
            continue;
        }
        return false;
    }
    return true;
}

static bool is_valid_subnet6(const char *p)
{
    const char *slash = strchr(p, '/');
    if (slash == NULL) {
        return is_valid_ipv6_chars(p);
    }
    if (strchr(slash + 1, '/') != NULL) {
        return false;
    }
    int prefix = parse_dec(slash + 1, strlen(slash + 1));
    if (prefix < 0 || prefix > 128) {
        return false;
    }
    char head[256];
    size_t head_len = (size_t)(slash - p);
    if (head_len >= sizeof(head)) {
        return false;
    }
    memcpy(head, p, head_len);
    head[head_len] = '\0';
    return is_valid_ipv6_chars(head);
}

/* isValidRegex: Go uses regexp2.Compile(pattern, 0). We use PCRE2 —
 * the 3 known divergence classes are documented in decisions.md D-07. */
static bool is_valid_regex(const char *p)
{
    int errcode = 0;
    PCRE2_SIZE erroff = 0;
    pcre2_code *code =
        pcre2_compile((PCRE2_SPTR)p, PCRE2_ZERO_TERMINATED,
                      PCRE2_UTF | PCRE2_UCP, &errcode, &erroff, NULL);
    if (code == NULL) {
        return false;
    }
    pcre2_code_free(code);
    return true;
}

const char *mt_sub_detect_type(const char *pattern)
{
    /* Go re-trims here (callers already trim, harmless to repeat). */
    if (is_valid_subnet6(pattern)) {
        return MT_RULE_SUBNET6;
    }
    if (is_valid_subnet(pattern)) {
        return MT_RULE_SUBNET;
    }
    if (is_valid_domain(pattern)) {
        /* namespace validator == domain validator, namespace checked
         * first in Go, so plain domains always become "namespace" */
        return MT_RULE_NAMESPACE;
    }
    if (is_valid_regex(pattern)) {
        return MT_RULE_REGEX;
    }
    if (is_valid_wildcard(pattern)) {
        return MT_RULE_WILDCARD;
    }
    return "";
}

/* ---- tokenizer + indexed dedup / ID allocation --------------------------- */

void mt_sub_rules_free(mt_sub_rule_t **rules, size_t n) {
    for (size_t i = 0; i < n; i++) { mt_sub_rule_free(rules[i]); }
    free(rules);
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) { s++; }
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) { s[--len] = '\0'; }
    return s;
}

static bool canceled(const atomic_bool *cancel) {
    return cancel && atomic_load(cancel);
}

mt_err_t mt_sub_parse_rules_cancel(const char *list, mt_sub_rule_t ***out_rules,
                                   size_t *out_n, const atomic_bool *cancel) {
    *out_rules = NULL; *out_n = 0;
    if (!list) { return MT_ERR_INVAL; }
    if (canceled(cancel)) { return MT_ERR_CANCELED; }
    char *copy = strdup(list);
    if (!copy) { return MT_ERR_NOMEM; }
    mt_sub_rule_t **rules = NULL;
    size_t n = 0, cap = 0;
    mt_lookup_t seen = {0}, used = {0};
    mt_id_pool_t pool = {0};
    mt_err_t err = MT_OK;
    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, "\n\r,", &saveptr); tok;
         tok = strtok_r(NULL, "\n\r,", &saveptr)) {
        if (canceled(cancel)) { err = MT_ERR_CANCELED; break; }
        char *line = trim(tok);
        if (!*line || *line == '#') { continue; }
        size_t len = strlen(line);
        if (len > MT_SUB_MAX_LINE_BYTES) { err = MT_ERR_LIMIT; break; }
        /* Detection is deterministic for identical text. Indexing text before
         * detection is equivalent to the old (detected-type, text) key and also
         * avoids compiling duplicate regexes. Preserve first occurrence order. */
        if (mt_lookup_get(&seen, line, len, NULL)) { continue; }
        if (n == MT_SUB_MAX_RULES) { err = MT_ERR_LIMIT; break; }
        err = mt_lookup_put(&seen, line, len, n, NULL);
        if (err != MT_OK) { break; }
        const char *type = mt_sub_detect_type(line);
        mt_sub_rule_t *r = mt_sub_rule_new();
        if (!r) { err = MT_ERR_NOMEM; break; }
        r->enable = true;
        err = mt_id_pool_unique(&used, &pool, &r->id);
        if (err == MT_OK) { err = mt_strset(&r->rule, line); }
        if (err == MT_OK) { err = mt_strset(&r->type, type); }
        if (err != MT_OK) { mt_sub_rule_free(r); break; }
        if (n == cap) {
            size_t next = cap ? cap * 2 : 16;
            mt_sub_rule_t **grown = realloc(rules, next * sizeof(*rules));
            if (!grown) { mt_sub_rule_free(r); err = MT_ERR_NOMEM; break; }
            rules = grown; cap = next;
        }
        rules[n++] = r;
    }
    if (err == MT_OK && canceled(cancel)) { err = MT_ERR_CANCELED; }
    mt_lookup_clear(&seen); mt_lookup_clear(&used); free(copy);
    if (err != MT_OK) { mt_sub_rules_free(rules, n); return err; }
    *out_rules = rules; *out_n = n;
    return MT_OK;
}

mt_err_t mt_sub_parse_rules(const char *list, mt_sub_rule_t ***out_rules, size_t *out_n) {
    return mt_sub_parse_rules_cancel(list, out_rules, out_n, NULL);
}

mt_err_t mt_sub_rules_fix_ids(mt_sub_rule_t **rules, size_t n) {
    mt_lookup_t used = {0}; mt_id_pool_t pool = {0};
    mt_err_t err = MT_OK;
    for (size_t i = 0; i < n; i++) {
        if (!rules[i]) { continue; }
        mt_id_t *id = &rules[i]->id;
        if (mt_id_is_zero(*id) || mt_lookup_get(&used, id->b, sizeof(id->b), NULL)) {
            err = mt_id_pool_unique(&used, &pool, id);
        } else { err = mt_lookup_put(&used, id->b, sizeof(id->b), i, NULL); }
        if (err != MT_OK) { break; }
    }
    mt_lookup_clear(&used);
    return err;
}

mt_err_t mt_sub_reconcile_rules(mt_sub_rule_t **parsed, size_t n,
                                 mt_sub_rule_t **existing, size_t n_existing) {
    mt_lookup_t by_text = {0};
    mt_err_t err = MT_OK;
    for (size_t i = 0; i < n_existing; i++) {
        const mt_sub_rule_t *r = existing[i];
        if (!r || !r->rule || !*r->rule) { continue; }
        err = mt_lookup_put(&by_text, r->rule, strlen(r->rule), i, NULL);
        if (err != MT_OK) { break; }
    }
    for (size_t i = 0; i < n && err == MT_OK; i++) {
        mt_sub_rule_t *r = parsed[i]; size_t j;
        if (!r || !r->rule || !mt_lookup_get(&by_text, r->rule, strlen(r->rule), &j)) { continue; }
        const mt_sub_rule_t *old = existing[j];
        r->id = old->id; r->enable = old->enable;
        if (old->type && *old->type) { err = mt_strset(&r->type, old->type); }
    }
    mt_lookup_clear(&by_text);
    if (err == MT_OK) { err = mt_sub_rules_fix_ids(parsed, n); }
    return err;
}

mt_err_t mt_sub_refresh_rules(const char *list, mt_sub_rule_t **existing,
                              size_t n_existing, mt_sub_rule_t ***out_rules, size_t *out_n) {
    *out_rules = NULL; *out_n = 0;
    mt_sub_rule_t **parsed = NULL; size_t n = 0;
    mt_err_t err = mt_sub_parse_rules(list, &parsed, &n);
    if (err == MT_OK) { err = mt_sub_reconcile_rules(parsed, n, existing, n_existing); }
    if (err != MT_OK) { mt_sub_rules_free(parsed, n); return err; }
    *out_rules = parsed; *out_n = n;
    return MT_OK;
}

/* ---- order-insensitive multiset compare; preserve legacy duplicate rules --- */
typedef struct rule_state { mt_id_t id; bool enable; size_t count; } rule_state_t;

static char *same_rules_key(const mt_sub_rule_t *rule) {
    const char *text = rule->rule ? rule->rule : "";
    const char *type = rule->type && *rule->type ? rule->type : mt_sub_detect_type(text);
    size_t len = strlen(type) + 1 + strlen(text) + 1;
    char *key = malloc(len);
    if (key) { snprintf(key, len, "%s|%s", type, text); }
    return key;
}

bool mt_sub_same_rules(mt_sub_rule_t **left, size_t n_left,
                       mt_sub_rule_t **right, size_t n_right) {
    if (n_left != n_right) { return false; }
    if (!n_left) { return true; }
    rule_state_t *states = calloc(n_left, sizeof(*states));
    if (!states) { return false; }
    mt_lookup_t map = {0};
    size_t left_nil = 0, right_nil = 0;
    bool result = true;
    for (size_t i = 0; i < n_left && result; i++) {
        if (!left[i]) { left_nil++; continue; }
        char *key = same_rules_key(left[i]);
        if (!key) { result = false; break; }
        size_t j = i;
        if (!mt_lookup_get(&map, key, strlen(key), &j)) {
            if (mt_lookup_put(&map, key, strlen(key), i, NULL) != MT_OK) { result = false; }
        }
        free(key);
        rule_state_t *st = &states[j];
        if (st->count && (!mt_id_equal(st->id, left[i]->id) || st->enable != left[i]->enable)) {
            result = false; break;
        }
        st->id = left[i]->id; st->enable = left[i]->enable; st->count++;
    }
    for (size_t i = 0; i < n_right && result; i++) {
        if (!right[i]) { right_nil++; continue; }
        char *key = same_rules_key(right[i]); size_t j;
        if (!key) { result = false; break; }
        bool found = mt_lookup_get(&map, key, strlen(key), &j);
        free(key);
        if (!found || !states[j].count || !mt_id_equal(states[j].id, right[i]->id) ||
            states[j].enable != right[i]->enable) { result = false; break; }
        states[j].count--;
    }
    if (left_nil != right_nil) { result = false; }
    for (size_t i = 0; i < n_left && result; i++) { if (states[i].count) { result = false; } }
    mt_lookup_clear(&map); free(states);
    return result;
}

bool mt_sub_is_due(const mt_subscription_t *sub, int64_t now_unix)
{
    if (sub == NULL || !sub->enable || sub->url == NULL ||
        sub->url[0] == '\0' || sub->interval == 0) {
        return false;
    }
    uint32_t last_check = sub->last_check;
    if (last_check == 0) {
        last_check = sub->last_update;
    }
    if (last_check > 0 &&
        (uint64_t)now_unix < (uint64_t)last_check + (uint64_t)sub->interval) {
        return false;
    }
    return true;
}
