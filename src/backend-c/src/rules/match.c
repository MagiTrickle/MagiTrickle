#define PCRE2_CODE_UNIT_WIDTH 8

#include "magitrickle/match.h"

#include <pcre2.h>
#include <stdlib.h>
#include <string.h>

#include "magitrickle/log.h"

/* PCRE2 hardening limits (decisions.md D-07). */
#define MT_REGEX_MATCH_LIMIT 1000000
#define MT_REGEX_DEPTH_LIMIT 10000

typedef enum rule_kind {
    RK_DOMAIN,
    RK_NAMESPACE,
    RK_WILDCARD,
    RK_REGEX,
    RK_NEVER, /* subnet/subnet6/unknown: IsMatch == false */
} rule_kind_t;

struct mt_rule_matcher {
    rule_kind_t kind;
    char *rule;
    size_t rule_len;
    pcre2_code *re;               /* RK_REGEX only, NULL if compile failed */
    pcre2_match_data *md;
    pcre2_match_context *mctx;
    bool compile_ok;
};

static rule_kind_t kind_of(const char *type)
{
    if (strcmp(type, MT_RULE_DOMAIN) == 0) {
        return RK_DOMAIN;
    }
    if (strcmp(type, MT_RULE_NAMESPACE) == 0) {
        return RK_NAMESPACE;
    }
    if (strcmp(type, MT_RULE_WILDCARD) == 0) {
        return RK_WILDCARD;
    }
    if (strcmp(type, MT_RULE_REGEX) == 0) {
        return RK_REGEX;
    }
    return RK_NEVER;
}

mt_rule_matcher_t *mt_rule_matcher_new(const char *type, const char *rule)
{
    mt_rule_matcher_t *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    m->kind = kind_of(type);
    m->rule = strdup(rule);
    if (m->rule == NULL) {
        free(m);
        return NULL;
    }
    m->rule_len = strlen(m->rule);
    m->compile_ok = true;

    if (m->kind == RK_REGEX) {
        int errcode = 0;
        PCRE2_SIZE erroff = 0;
        m->re = pcre2_compile((PCRE2_SPTR)rule, PCRE2_ZERO_TERMINATED,
                              PCRE2_CASELESS | PCRE2_UTF | PCRE2_UCP,
                              &errcode, &erroff, NULL);
        if (m->re == NULL) {
            PCRE2_UCHAR msg[128];
            pcre2_get_error_message(errcode, msg, sizeof(msg));
            MT_ERROR("regex compile failed: %s (pattern %s at %zu)",
                     (char *)msg, rule, (size_t)erroff);
            m->compile_ok = false;
        } else {
            m->md = pcre2_match_data_create_from_pattern(m->re, NULL);
            m->mctx = pcre2_match_context_create(NULL);
            if (m->md == NULL || m->mctx == NULL) {
                mt_rule_matcher_free(m);
                return NULL;
            }
            pcre2_set_match_limit(m->mctx, MT_REGEX_MATCH_LIMIT);
            pcre2_set_depth_limit(m->mctx, MT_REGEX_DEPTH_LIMIT);
        }
    }
    return m;
}

void mt_rule_matcher_free(mt_rule_matcher_t *m)
{
    if (m == NULL) {
        return;
    }
    if (m->md != NULL) {
        pcre2_match_data_free(m->md);
    }
    if (m->mctx != NULL) {
        pcre2_match_context_free(m->mctx);
    }
    if (m->re != NULL) {
        pcre2_code_free(m->re);
    }
    free(m->rule);
    free(m);
}

bool mt_rule_matcher_ok(const mt_rule_matcher_t *m)
{
    return m->compile_ok;
}

static bool namespace_match(const char *rule, size_t rule_len,
                            const char *domain, size_t domain_len)
{
    /* Go: domainName == rule || (domainLen >= ruleLen+1 &&
     *     domainName[domainLen-ruleLen-1] == '.' && suffix == rule) */
    if (domain_len == rule_len && memcmp(domain, rule, rule_len) == 0) {
        return true;
    }
    if (domain_len < rule_len + 1) {
        return false;
    }
    return domain[domain_len - rule_len - 1] == '.' &&
           memcmp(domain + domain_len - rule_len, rule, rule_len) == 0;
}

bool mt_rule_matcher_match(mt_rule_matcher_t *m, const char *domain)
{
    size_t domain_len = strlen(domain);
    switch (m->kind) {
    case RK_DOMAIN:
        return domain_len == m->rule_len &&
               memcmp(domain, m->rule, domain_len) == 0;
    case RK_NAMESPACE:
        return namespace_match(m->rule, m->rule_len, domain, domain_len);
    case RK_WILDCARD:
        return mt_wildcard_match(m->rule, domain);
    case RK_REGEX: {
        if (m->re == NULL) {
            return false;
        }
        int rc = pcre2_match(m->re, (PCRE2_SPTR)domain, domain_len, 0, 0,
                             m->md, m->mctx);
        return rc >= 0;
    }
    case RK_NEVER:
        return false;
    }
    return false;
}

/* ---------------- per-group index ---------------- */

/* exact-domain open-addressing hash set (FNV-1a) */
typedef struct exact_set {
    char **slots;
    size_t cap; /* power of two */
    size_t len;
} exact_set_t;

static uint64_t fnv1a(const char *s)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)s; *p != 0; p++) {
        h ^= *p;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static mt_err_t exact_set_insert(exact_set_t *set, const char *s);

static mt_err_t exact_set_grow(exact_set_t *set)
{
    size_t new_cap = set->cap == 0 ? 16 : set->cap * 2;
    char **old_slots = set->slots;
    size_t old_cap = set->cap;

    set->slots = calloc(new_cap, sizeof(char *));
    if (set->slots == NULL) {
        set->slots = old_slots;
        return MT_ERR_NOMEM;
    }
    set->cap = new_cap;
    set->len = 0;
    for (size_t i = 0; i < old_cap; i++) {
        if (old_slots[i] != NULL) {
            /* re-insert existing pointer (no re-dup) */
            uint64_t h = fnv1a(old_slots[i]);
            size_t idx = (size_t)h & (set->cap - 1);
            while (set->slots[idx] != NULL) {
                idx = (idx + 1) & (set->cap - 1);
            }
            set->slots[idx] = old_slots[i];
            set->len++;
        }
    }
    free(old_slots);
    return MT_OK;
}

static mt_err_t exact_set_insert(exact_set_t *set, const char *s)
{
    if (set->cap == 0 || set->len * 4 >= set->cap * 3) {
        mt_err_t err = exact_set_grow(set);
        if (err != MT_OK) {
            return err;
        }
    }
    uint64_t h = fnv1a(s);
    size_t idx = (size_t)h & (set->cap - 1);
    while (set->slots[idx] != NULL) {
        if (strcmp(set->slots[idx], s) == 0) {
            return MT_OK; /* dedup */
        }
        idx = (idx + 1) & (set->cap - 1);
    }
    set->slots[idx] = strdup(s);
    if (set->slots[idx] == NULL) {
        return MT_ERR_NOMEM;
    }
    set->len++;
    return MT_OK;
}

static bool exact_set_contains(const exact_set_t *set, const char *s)
{
    if (set->cap == 0) {
        return false;
    }
    uint64_t h = fnv1a(s);
    size_t idx = (size_t)h & (set->cap - 1);
    while (set->slots[idx] != NULL) {
        if (strcmp(set->slots[idx], s) == 0) {
            return true;
        }
        idx = (idx + 1) & (set->cap - 1);
    }
    return false;
}

static void exact_set_clear(exact_set_t *set)
{
    for (size_t i = 0; i < set->cap; i++) {
        free(set->slots[i]);
    }
    free(set->slots);
    memset(set, 0, sizeof(*set));
}

/* reversed-label trie for namespace rules:
 * "example.com" is stored as com -> example; a lookup walks the domain's
 * labels right-to-left and matches when it passes through a terminal node
 * at a label boundary. Also covers the exact-match and leading-dot cases
 * of namespace_match. */
typedef struct trie_node {
    char *label;
    struct trie_node **children;
    size_t n_children;
    bool terminal;
} trie_node_t;

typedef struct ns_trie {
    trie_node_t root;
    bool has_empty_rule; /* namespace rule "" (matches domains ending in '.')*/
} ns_trie_t;

static trie_node_t *trie_child(trie_node_t *node, const char *label,
                               size_t len, bool create)
{
    for (size_t i = 0; i < node->n_children; i++) {
        if (strlen(node->children[i]->label) == len &&
            memcmp(node->children[i]->label, label, len) == 0) {
            return node->children[i];
        }
    }
    if (!create) {
        return NULL;
    }
    trie_node_t *child = calloc(1, sizeof(*child));
    if (child == NULL) {
        return NULL;
    }
    child->label = strndup(label, len);
    if (child->label == NULL) {
        free(child);
        return NULL;
    }
    trie_node_t **grown = realloc(
        node->children, (node->n_children + 1) * sizeof(trie_node_t *));
    if (grown == NULL) {
        free(child->label);
        free(child);
        return NULL;
    }
    node->children = grown;
    node->children[node->n_children++] = child;
    return child;
}

static void trie_node_clear(trie_node_t *node)
{
    for (size_t i = 0; i < node->n_children; i++) {
        trie_node_clear(node->children[i]);
        free(node->children[i]);
    }
    free(node->children);
    free(node->label);
}

static mt_err_t ns_trie_insert(ns_trie_t *t, const char *rule)
{
    size_t len = strlen(rule);
    if (len == 0) {
        t->has_empty_rule = true;
        return MT_OK;
    }
    /* walk labels right to left */
    trie_node_t *node = &t->root;
    const char *end = rule + len;
    while (end > rule) {
        const char *start = end;
        while (start > rule && start[-1] != '.') {
            start--;
        }
        node = trie_child(node, start, (size_t)(end - start), true);
        if (node == NULL) {
            return MT_ERR_NOMEM;
        }
        end = start > rule ? start - 1 : rule;
    }
    node->terminal = true;
    return MT_OK;
}

static bool ns_trie_match(const ns_trie_t *t, const char *domain)
{
    size_t len = strlen(domain);
    if (t->has_empty_rule) {
        /* namespace rule "": matches "" or any domain ending with '.' */
        if (len == 0 || domain[len - 1] == '.') {
            return true;
        }
    }
    const trie_node_t *node = &t->root;
    const char *end = domain + len;
    while (end > domain) {
        const char *start = end;
        while (start > domain && start[-1] != '.') {
            start--;
        }
        const trie_node_t *next = NULL;
        size_t lab_len = (size_t)(end - start);
        for (size_t i = 0; i < node->n_children; i++) {
            if (strlen(node->children[i]->label) == lab_len &&
                memcmp(node->children[i]->label, start, lab_len) == 0) {
                next = node->children[i];
                break;
            }
        }
        if (next == NULL) {
            return false;
        }
        if (next->terminal) {
            /* matched full rule; must be exact or at a dot boundary
             * (start == domain covers exact and ".rule" via empty label) */
            return true;
        }
        node = next;
        end = start > domain ? start - 1 : domain;
    }
    return false;
}

struct mt_matcher {
    exact_set_t exact;
    ns_trie_t ns;
    mt_rule_matcher_t **scan; /* wildcard + regex */
    size_t n_scan;
};

mt_matcher_t *mt_matcher_new(void)
{
    return calloc(1, sizeof(mt_matcher_t));
}

void mt_matcher_free(mt_matcher_t *m)
{
    if (m == NULL) {
        return;
    }
    exact_set_clear(&m->exact);
    trie_node_clear(&m->ns.root);
    for (size_t i = 0; i < m->n_scan; i++) {
        mt_rule_matcher_free(m->scan[i]);
    }
    free(m->scan);
    free(m);
}

mt_err_t mt_matcher_add(mt_matcher_t *m, const char *type, const char *rule)
{
    rule_kind_t kind = kind_of(type);
    switch (kind) {
    case RK_DOMAIN:
        return exact_set_insert(&m->exact, rule);
    case RK_NAMESPACE:
        return ns_trie_insert(&m->ns, rule);
    case RK_WILDCARD:
    case RK_REGEX: {
        mt_rule_matcher_t *rm = mt_rule_matcher_new(type, rule);
        if (rm == NULL) {
            return MT_ERR_NOMEM;
        }
        mt_rule_matcher_t **grown =
            realloc(m->scan, (m->n_scan + 1) * sizeof(*grown));
        if (grown == NULL) {
            mt_rule_matcher_free(rm);
            return MT_ERR_NOMEM;
        }
        m->scan = grown;
        m->scan[m->n_scan++] = rm;
        return MT_OK;
    }
    case RK_NEVER:
        return MT_OK; /* subnet/subnet6 never participate in DNS matching */
    }
    return MT_OK;
}

bool mt_matcher_match(mt_matcher_t *m, const char *domain)
{
    if (exact_set_contains(&m->exact, domain)) {
        return true;
    }
    if (ns_trie_match(&m->ns, domain)) {
        return true;
    }
    for (size_t i = 0; i < m->n_scan; i++) {
        if (mt_rule_matcher_match(m->scan[i], domain)) {
            return true;
        }
    }
    return false;
}
