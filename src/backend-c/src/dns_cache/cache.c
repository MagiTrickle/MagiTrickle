/* Port of src/backend/utils/recordsCache/records.go. See dns_cache.h for
 * the behavioural contract. Two file-local chained hash tables (domains,
 * reverse-alias) — small enough that a shared generic map would not pay
 * for itself; see AGENTS.md guidance against premature abstraction.
 */
#include "magitrickle/dns_cache.h"

#include <stdlib.h>
#include <string.h>

#define DOM_BUCKETS 4096u
#define REV_BUCKETS 1024u

static uint64_t fnv1a(const char *s)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)s; *p != 0; p++) {
        h ^= *p;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

/* ---- domain table: domain -> (addresses[], alias, alias_deadline) ---- */

typedef struct domain_node {
    char *domain;
    mt_cache_addr_t *addrs;
    size_t n_addrs;
    size_t cap_addrs;
    char *alias; /* NULL if none */
    int64_t alias_deadline;
    struct domain_node *next;
} domain_node_t;

/* ---- reverse table: alias -> [domains pointing at it] ---- */

typedef struct rev_node {
    char *alias;
    char **domains;
    size_t n_domains;
    size_t cap_domains;
    struct rev_node *next;
} rev_node_t;

struct mt_cache {
    domain_node_t *dom_buckets[DOM_BUCKETS];
    rev_node_t *rev_buckets[REV_BUCKETS];
    size_t n_domains;
    size_t max_domains;
    uint64_t dropped;
};

mt_cache_t *mt_cache_create(size_t max_domains)
{
    mt_cache_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->max_domains =
        max_domains > 0 ? max_domains : MT_CACHE_DEFAULT_MAX_DOMAINS;
    return c;
}

static void domain_node_free(domain_node_t *n)
{
    free(n->domain);
    free(n->addrs);
    free(n->alias);
    free(n);
}

static void rev_node_free(rev_node_t *n)
{
    for (size_t i = 0; i < n->n_domains; i++) {
        free(n->domains[i]);
    }
    free(n->domains);
    free(n->alias);
    free(n);
}

void mt_cache_destroy(mt_cache_t *c)
{
    if (c == NULL) {
        return;
    }
    for (size_t i = 0; i < DOM_BUCKETS; i++) {
        domain_node_t *n = c->dom_buckets[i];
        while (n != NULL) {
            domain_node_t *next = n->next;
            domain_node_free(n);
            n = next;
        }
    }
    for (size_t i = 0; i < REV_BUCKETS; i++) {
        rev_node_t *n = c->rev_buckets[i];
        while (n != NULL) {
            rev_node_t *next = n->next;
            rev_node_free(n);
            n = next;
        }
    }
    free(c);
}

static domain_node_t *dom_find(mt_cache_t *c, const char *domain)
{
    uint64_t h = fnv1a(domain) % DOM_BUCKETS;
    for (domain_node_t *n = c->dom_buckets[h]; n != NULL; n = n->next) {
        if (strcmp(n->domain, domain) == 0) {
            return n;
        }
    }
    return NULL;
}

/* Creates the node if absent; returns NULL if absent AND at capacity
 * (caller must count it as dropped). */
static domain_node_t *dom_find_or_create(mt_cache_t *c, const char *domain)
{
    domain_node_t *n = dom_find(c, domain);
    if (n != NULL) {
        return n;
    }
    if (c->n_domains >= c->max_domains) {
        return NULL;
    }
    n = calloc(1, sizeof(*n));
    if (n == NULL) {
        return NULL;
    }
    n->domain = strdup(domain);
    if (n->domain == NULL) {
        free(n);
        return NULL;
    }
    uint64_t h = fnv1a(domain) % DOM_BUCKETS;
    n->next = c->dom_buckets[h];
    c->dom_buckets[h] = n;
    c->n_domains++;
    return n;
}

/* Removes and frees a node with no addresses and no alias (assumes the
 * caller already verified emptiness). */
static void dom_remove_if_empty(mt_cache_t *c, domain_node_t *n)
{
    if (n->n_addrs != 0 || n->alias != NULL) {
        return;
    }
    uint64_t h = fnv1a(n->domain) % DOM_BUCKETS;
    domain_node_t **pp = &c->dom_buckets[h];
    while (*pp != NULL) {
        if (*pp == n) {
            *pp = n->next;
            domain_node_free(n);
            c->n_domains--;
            return;
        }
        pp = &(*pp)->next;
    }
}

static rev_node_t *rev_find(mt_cache_t *c, const char *alias)
{
    uint64_t h = fnv1a(alias) % REV_BUCKETS;
    for (rev_node_t *n = c->rev_buckets[h]; n != NULL; n = n->next) {
        if (strcmp(n->alias, alias) == 0) {
            return n;
        }
    }
    return NULL;
}

static mt_err_t rev_add(mt_cache_t *c, const char *alias, const char *domain)
{
    rev_node_t *n = rev_find(c, alias);
    if (n == NULL) {
        n = calloc(1, sizeof(*n));
        if (n == NULL) {
            return MT_ERR_NOMEM;
        }
        n->alias = strdup(alias);
        if (n->alias == NULL) {
            free(n);
            return MT_ERR_NOMEM;
        }
        uint64_t h = fnv1a(alias) % REV_BUCKETS;
        n->next = c->rev_buckets[h];
        c->rev_buckets[h] = n;
    }
    if (n->n_domains == n->cap_domains) {
        size_t cap = n->cap_domains == 0 ? 4 : n->cap_domains * 2;
        char **grown = realloc(n->domains, cap * sizeof(char *));
        if (grown == NULL) {
            return MT_ERR_NOMEM;
        }
        n->domains = grown;
        n->cap_domains = cap;
    }
    char *copy = strdup(domain);
    if (copy == NULL) {
        return MT_ERR_NOMEM;
    }
    n->domains[n->n_domains++] = copy;
    return MT_OK;
}

/* Removes domain from reverse[alias]; deletes the rev_node if it becomes
 * empty (matches Go's delete(reverseAliases, alias)). Order not
 * preserved (swap-with-last), matching Go's removeReverseAlias. */
static void rev_remove(mt_cache_t *c, const char *alias, const char *domain)
{
    rev_node_t *n = rev_find(c, alias);
    if (n == NULL) {
        return;
    }
    for (size_t i = 0; i < n->n_domains; i++) {
        if (strcmp(n->domains[i], domain) == 0) {
            free(n->domains[i]);
            n->domains[i] = n->domains[n->n_domains - 1];
            n->n_domains--;
            break;
        }
    }
    if (n->n_domains == 0) {
        uint64_t h = fnv1a(alias) % REV_BUCKETS;
        rev_node_t **pp = &c->rev_buckets[h];
        while (*pp != NULL) {
            if (*pp == n) {
                *pp = n->next;
                rev_node_free(n);
                return;
            }
            pp = &(*pp)->next;
        }
    }
}

void mt_cache_add_address(mt_cache_t *c, const char *domain,
                          const uint8_t *addr, uint8_t addr_len,
                          uint32_t ttl_seconds, int64_t now)
{
    domain_node_t *n = dom_find_or_create(c, domain);
    if (n == NULL) {
        c->dropped++;
        return;
    }
    int64_t deadline = now + (int64_t)ttl_seconds;
    for (size_t i = 0; i < n->n_addrs; i++) {
        if (n->addrs[i].addr_len == addr_len &&
            memcmp(n->addrs[i].addr, addr, addr_len) == 0) {
            n->addrs[i].deadline = deadline;
            return;
        }
    }
    if (n->n_addrs == n->cap_addrs) {
        size_t cap = n->cap_addrs == 0 ? 4 : n->cap_addrs * 2;
        mt_cache_addr_t *grown = realloc(n->addrs, cap * sizeof(*grown));
        if (grown == NULL) {
            return; /* OOM: silently drop this update, like a failed
                     * best-effort cache write; no crash (spec §19). */
        }
        n->addrs = grown;
        n->cap_addrs = cap;
    }
    mt_cache_addr_t *slot = &n->addrs[n->n_addrs++];
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->addr, addr, addr_len);
    slot->addr_len = addr_len;
    slot->deadline = deadline;
}

void mt_cache_add_alias(mt_cache_t *c, const char *domain, const char *alias,
                        uint32_t ttl_seconds, int64_t now)
{
    if (strcmp(domain, alias) == 0) {
        return; /* self-alias ignored, like Go */
    }
    domain_node_t *n = dom_find_or_create(c, domain);
    if (n == NULL) {
        c->dropped++;
        return;
    }
    if (n->alias != NULL) {
        rev_remove(c, n->alias, domain);
        free(n->alias);
        n->alias = NULL;
    }
    n->alias = strdup(alias);
    if (n->alias == NULL) {
        dom_remove_if_empty(c, n);
        return;
    }
    n->alias_deadline = now + (int64_t)ttl_seconds;
    if (rev_add(c, alias, domain) != MT_OK) {
        /* best-effort: alias entry itself is set even if the reverse
         * index insert failed (OOM) — matches "never crash on a single
         * client/record" (spec §19); GetAliases just won't discover this
         * edge until memory frees up and a future update retries. */
    }
}

mt_err_t mt_cache_get_addresses(mt_cache_t *c, const char *domain,
                                int64_t now, mt_cache_addr_t **out,
                                size_t *out_n)
{
    *out = NULL;
    *out_n = 0;

    /* cycle guard: track visited domain names (small linear list; alias
     * chains are short in practice) */
    char *seen[64];
    size_t n_seen = 0;
    seen[n_seen++] = (char *)domain;

    const char *current = domain;
    for (;;) {
        domain_node_t *n = dom_find(c, current);
        if (n != NULL && n->n_addrs > 0) {
            mt_cache_addr_t *valid = malloc(n->n_addrs * sizeof(*valid));
            if (valid == NULL) {
                return MT_ERR_NOMEM;
            }
            size_t n_valid = 0;
            for (size_t i = 0; i < n->n_addrs; i++) {
                if (n->addrs[i].deadline > now) {
                    valid[n_valid++] = n->addrs[i];
                }
            }
            if (n_valid > 0) {
                *out = valid;
                *out_n = n_valid;
                return MT_OK;
            }
            free(valid);
        }
        if (n == NULL || n->alias == NULL || n->alias_deadline <= now) {
            return MT_OK; /* nothing found; *out stays NULL */
        }
        if (n_seen >= sizeof(seen) / sizeof(seen[0])) {
            return MT_OK; /* pathologically long chain: treat as no match */
        }
        bool cycle = false;
        for (size_t i = 0; i < n_seen; i++) {
            if (strcmp(seen[i], n->alias) == 0) {
                cycle = true;
                break;
            }
        }
        if (cycle) {
            return MT_OK;
        }
        seen[n_seen++] = n->alias;
        current = n->alias;
    }
}

typedef struct str_list {
    char **items;
    size_t n;
    size_t cap;
} str_list_t;

static mt_err_t str_list_push(str_list_t *l, const char *s)
{
    if (l->n == l->cap) {
        size_t cap = l->cap == 0 ? 8 : l->cap * 2;
        char **grown = realloc(l->items, cap * sizeof(char *));
        if (grown == NULL) {
            return MT_ERR_NOMEM;
        }
        l->items = grown;
        l->cap = cap;
    }
    char *copy = strdup(s);
    if (copy == NULL) {
        return MT_ERR_NOMEM;
    }
    l->items[l->n++] = copy;
    return MT_OK;
}

static bool str_list_contains(const str_list_t *l, const char *s)
{
    for (size_t i = 0; i < l->n; i++) {
        if (strcmp(l->items[i], s) == 0) {
            return true;
        }
    }
    return false;
}

mt_err_t mt_cache_get_aliases(mt_cache_t *c, const char *domain, char ***out,
                              size_t *out_n)
{
    str_list_t result = {0};
    str_list_t queue = {0}; /* BFS queue, not freed of its string copies
                              * (shares ownership conceptually with result
                              * via duplicate strdup — simplicity over a
                              * shared-pointer scheme for a small list). */
    mt_err_t err;
    if ((err = str_list_push(&result, domain)) != MT_OK ||
        (err = str_list_push(&queue, domain)) != MT_OK) {
        goto fail;
    }

    size_t head = 0;
    while (head < queue.n) {
        const char *cur = queue.items[head++];
        rev_node_t *rn = rev_find(c, cur);
        if (rn == NULL) {
            continue;
        }
        for (size_t i = 0; i < rn->n_domains; i++) {
            const char *pointing = rn->domains[i];
            if (str_list_contains(&result, pointing)) {
                continue;
            }
            if ((err = str_list_push(&result, pointing)) != MT_OK ||
                (err = str_list_push(&queue, pointing)) != MT_OK) {
                goto fail;
            }
        }
    }

    mt_cache_free_strings(queue.items, queue.n);
    *out = result.items;
    *out_n = result.n;
    return MT_OK;

fail:
    mt_cache_free_strings(result.items, result.n);
    mt_cache_free_strings(queue.items, queue.n);
    *out = NULL;
    *out_n = 0;
    return err;
}

mt_err_t mt_cache_list_known_domains(mt_cache_t *c, char ***out,
                                     size_t *out_n)
{
    str_list_t result = {0};
    mt_err_t err;
    for (size_t i = 0; i < DOM_BUCKETS; i++) {
        for (domain_node_t *n = c->dom_buckets[i]; n != NULL; n = n->next) {
            if ((err = str_list_push(&result, n->domain)) != MT_OK) {
                mt_cache_free_strings(result.items, result.n);
                *out = NULL;
                *out_n = 0;
                return err;
            }
        }
    }
    *out = result.items;
    *out_n = result.n;
    return MT_OK;
}

void mt_cache_free_strings(char **strs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(strs[i]);
    }
    free(strs);
}

void mt_cache_cleanup(mt_cache_t *c, int64_t now)
{
    for (size_t b = 0; b < DOM_BUCKETS; b++) {
        domain_node_t *n = c->dom_buckets[b];
        domain_node_t *prev = NULL;
        while (n != NULL) {
            domain_node_t *next = n->next;

            /* compact expired addresses */
            size_t kept = 0;
            for (size_t i = 0; i < n->n_addrs; i++) {
                if (n->addrs[i].deadline > now) {
                    n->addrs[kept++] = n->addrs[i];
                }
            }
            n->n_addrs = kept;

            /* expire alias */
            if (n->alias != NULL && n->alias_deadline <= now) {
                rev_remove(c, n->alias, n->domain);
                free(n->alias);
                n->alias = NULL;
            }

            if (n->n_addrs == 0 && n->alias == NULL) {
                if (prev == NULL) {
                    c->dom_buckets[b] = next;
                } else {
                    prev->next = next;
                }
                domain_node_free(n);
                c->n_domains--;
            } else {
                prev = n;
            }
            n = next;
        }
    }
}

size_t mt_cache_domain_count(const mt_cache_t *c)
{
    return c->n_domains;
}

uint64_t mt_cache_dropped(const mt_cache_t *c)
{
    return c->dropped;
}
