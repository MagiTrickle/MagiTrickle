#define PCRE2_CODE_UNIT_WIDTH 8

#include "magitrickle/subparse.h"

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
        n = n * 10 + (s[i] - '0');
        if (n > 999999999) {
            return -1;
        }
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

/* ---- tokenizer + dedup (parse.go ParseRules) ---- */

typedef struct seen_key {
    char *key;
    struct seen_key *next;
} seen_key_t;

static bool seen_add(seen_key_t **head, const char *type, const char *text)
{
    size_t klen = strlen(type) + 1 + strlen(text) + 1;
    char *key = malloc(klen);
    if (key == NULL) {
        return false; /* treat OOM as "seen" to fail closed */
    }
    snprintf(key, klen, "%s|%s", type, text);
    for (seen_key_t *k = *head; k != NULL; k = k->next) {
        if (strcmp(k->key, key) == 0) {
            free(key);
            return false;
        }
    }
    seen_key_t *node = malloc(sizeof(*node));
    if (node == NULL) {
        free(key);
        return false;
    }
    node->key = key;
    node->next = *head;
    *head = node;
    return true;
}

static void seen_clear(seen_key_t *head)
{
    while (head != NULL) {
        seen_key_t *next = head->next;
        free(head->key);
        free(head);
        head = next;
    }
}

/* Go strings.TrimSpace (ASCII whitespace; unicode spaces are not expected
 * in subscription lists — divergence would be caught by the differential
 * corpus). */
static char *trim(char *s)
{
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

typedef struct id_set {
    mt_id_t *ids;
    size_t len;
    size_t cap;
} id_set_t;

static bool id_set_contains(const id_set_t *set, mt_id_t id)
{
    for (size_t i = 0; i < set->len; i++) {
        if (mt_id_equal(set->ids[i], id)) {
            return true;
        }
    }
    return false;
}

static mt_err_t id_set_add(id_set_t *set, mt_id_t id)
{
    if (set->len == set->cap) {
        size_t cap = set->cap == 0 ? 16 : set->cap * 2;
        mt_id_t *grown = realloc(set->ids, cap * sizeof(mt_id_t));
        if (grown == NULL) {
            return MT_ERR_NOMEM;
        }
        set->ids = grown;
        set->cap = cap;
    }
    set->ids[set->len++] = id;
    return MT_OK;
}

static mt_id_t next_unique_id(id_set_t *used)
{
    for (;;) {
        mt_id_t candidate = mt_id_random();
        if (!id_set_contains(used, candidate)) {
            (void)id_set_add(used, candidate);
            return candidate;
        }
    }
}

static void free_rule_array(mt_sub_rule_t **rules, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        mt_sub_rule_free(rules[i]);
    }
    free(rules);
}

mt_err_t mt_sub_parse_rules(const char *list, mt_sub_rule_t ***out_rules,
                            size_t *out_n)
{
    char *copy = strdup(list);
    if (copy == NULL) {
        return MT_ERR_NOMEM;
    }

    mt_sub_rule_t **rules = NULL;
    size_t n = 0, cap = 0;
    seen_key_t *seen = NULL;
    id_set_t used = {NULL, 0, 0};
    mt_err_t err = MT_OK;

    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, "\n\r,", &saveptr); tok != NULL;
         tok = strtok_r(NULL, "\n\r,", &saveptr)) {
        char *line = trim(tok);
        if (*line == '\0' || line[0] == '#') {
            continue;
        }
        const char *type = mt_sub_detect_type(line);
        if (!seen_add(&seen, type, line)) {
            continue;
        }
        mt_sub_rule_t *rule = mt_sub_rule_new();
        if (rule == NULL) {
            err = MT_ERR_NOMEM;
            goto out;
        }
        rule->id = next_unique_id(&used);
        rule->enable = true;
        if ((err = mt_strset(&rule->rule, line)) != MT_OK ||
            (err = mt_strset(&rule->type, type)) != MT_OK) {
            mt_sub_rule_free(rule);
            goto out;
        }
        if (n == cap) {
            cap = cap == 0 ? 16 : cap * 2;
            mt_sub_rule_t **grown = realloc(rules, cap * sizeof(*rules));
            if (grown == NULL) {
                mt_sub_rule_free(rule);
                err = MT_ERR_NOMEM;
                goto out;
            }
            rules = grown;
        }
        rules[n++] = rule;
    }

out:
    seen_clear(seen);
    free(used.ids);
    free(copy);
    if (err != MT_OK) {
        free_rule_array(rules, n);
        return err;
    }
    *out_rules = rules;
    *out_n = n;
    return MT_OK;
}

mt_err_t mt_sub_refresh_rules(const char *list, mt_sub_rule_t **existing,
                              size_t n_existing, mt_sub_rule_t ***out_rules,
                              size_t *out_n)
{
    mt_sub_rule_t **parsed = NULL;
    size_t n = 0;
    mt_err_t err = mt_sub_parse_rules(list, &parsed, &n);
    if (err != MT_OK) {
        return err;
    }
    if (n == 0) {
        *out_rules = parsed;
        *out_n = 0;
        return MT_OK;
    }

    id_set_t used = {NULL, 0, 0};

    for (size_t i = 0; i < n; i++) {
        mt_sub_rule_t *rule = parsed[i];
        /* find first existing with same non-empty text (Go builds a map
         * keeping the FIRST occurrence per text) */
        mt_sub_rule_t *current = NULL;
        for (size_t j = 0; j < n_existing; j++) {
            if (existing[j] != NULL && existing[j]->rule != NULL &&
                existing[j]->rule[0] != '\0' &&
                strcmp(existing[j]->rule, rule->rule) == 0) {
                current = existing[j];
                break;
            }
        }
        if (current != NULL) {
            rule->id = current->id;
            rule->enable = current->enable;
            if (current->type != NULL && current->type[0] != '\0') {
                if ((err = mt_strset(&rule->type, current->type)) != MT_OK) {
                    break;
                }
            }
        }
        if (mt_id_is_zero(rule->id)) {
            rule->id = next_unique_id(&used);
            continue;
        }
        if (id_set_contains(&used, rule->id)) {
            rule->id = next_unique_id(&used);
            continue;
        }
        if ((err = id_set_add(&used, rule->id)) != MT_OK) {
            break;
        }
    }

    free(used.ids);
    if (err != MT_OK) {
        free_rule_array(parsed, n);
        return err;
    }
    *out_rules = parsed;
    *out_n = n;
    return MT_OK;
}

/* ---- sameRules ---- */

typedef struct rule_state {
    char *key;
    mt_id_t id;
    bool enable;
    int count;
    struct rule_state *next;
} rule_state_t;

static char *same_rules_key(const mt_sub_rule_t *rule)
{
    const char *type =
        (rule->type != NULL && rule->type[0] != '\0')
            ? rule->type
            : mt_sub_detect_type(rule->rule != NULL ? rule->rule : "");
    const char *text = rule->rule != NULL ? rule->rule : "";
    size_t len = strlen(type) + 1 + strlen(text) + 1;
    char *key = malloc(len);
    if (key != NULL) {
        snprintf(key, len, "%s|%s", type, text);
    }
    return key;
}

bool mt_sub_same_rules(mt_sub_rule_t **left, size_t n_left,
                       mt_sub_rule_t **right, size_t n_right)
{
    if (n_left != n_right) {
        return false;
    }
    if (n_left == 0) {
        return true;
    }

    rule_state_t *states = NULL;
    bool result = true;
    size_t left_nil = 0, right_nil = 0;

    for (size_t i = 0; i < n_left && result; i++) {
        if (left[i] == NULL) {
            left_nil++;
            continue;
        }
        char *key = same_rules_key(left[i]);
        if (key == NULL) {
            result = false;
            break;
        }
        rule_state_t *st = states;
        while (st != NULL && strcmp(st->key, key) != 0) {
            st = st->next;
        }
        if (st != NULL) {
            free(key);
            if (st->count > 0 && (!mt_id_equal(st->id, left[i]->id) ||
                                  st->enable != left[i]->enable)) {
                result = false;
                break;
            }
        } else {
            st = calloc(1, sizeof(*st));
            if (st == NULL) {
                free(key);
                result = false;
                break;
            }
            st->key = key;
            st->next = states;
            states = st;
        }
        st->id = left[i]->id;
        st->enable = left[i]->enable;
        st->count++;
    }

    for (size_t i = 0; i < n_right && result; i++) {
        if (right[i] == NULL) {
            right_nil++;
            continue;
        }
        char *key = same_rules_key(right[i]);
        if (key == NULL) {
            result = false;
            break;
        }
        rule_state_t *st = states;
        while (st != NULL && strcmp(st->key, key) != 0) {
            st = st->next;
        }
        free(key);
        if (st == NULL || st->count == 0) {
            result = false;
            break;
        }
        if (!mt_id_equal(st->id, right[i]->id) ||
            st->enable != right[i]->enable) {
            result = false;
            break;
        }
        st->count--;
    }

    if (result) {
        for (rule_state_t *st = states; st != NULL; st = st->next) {
            if (st->count != 0) {
                result = false;
                break;
            }
        }
        if (left_nil != right_nil) {
            result = false;
        }
    }

    while (states != NULL) {
        rule_state_t *next = states->next;
        free(states->key);
        free(states);
        states = next;
    }
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
