/* IPTables engine — port of utils/iptables/iptables.go (registration,
 * GetCurrentRules parsing, Commit transcript assembly). See iptables.h for
 * the concurrency/ordering notes (D-17/D-19).
 */
#include "magitrickle/iptables.h"
#include "magitrickle/bytebuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- registration storage (insertion-ordered) -------------------------- */

typedef struct chain_reg {
    char *chain_name;
    mt_ipt_chain_t *chain;
} chain_reg_t;

typedef struct table_reg {
    char *table_name;
    chain_reg_t *chains;
    size_t n_chains, cap_chains;
} table_reg_t;

struct mt_ipt {
    table_reg_t *tables;
    size_t n_tables, cap_tables;
    mt_ipt_executable_t *exe;
    mt_cancel_t *cancel; /* borrowed, nullable */
};

mt_ipt_t *mt_ipt_new(mt_ipt_executable_t *exe) {
    mt_ipt_t *ipt = calloc(1, sizeof(*ipt));
    if (!ipt) { return NULL; }
    ipt->exe = exe;
    return ipt;
}

static void chain_reg_destroy(chain_reg_t *r) {
    free(r->chain_name);
    if (r->chain) { r->chain->ops->destroy(r->chain); }
}

static void table_reg_destroy(table_reg_t *t) {
    for (size_t i = 0; i < t->n_chains; i++) { chain_reg_destroy(&t->chains[i]); }
    free(t->chains);
    free(t->table_name);
}

void mt_ipt_free(mt_ipt_t *ipt) {
    if (!ipt) { return; }
    for (size_t i = 0; i < ipt->n_tables; i++) { table_reg_destroy(&ipt->tables[i]); }
    free(ipt->tables);
    mt_ipt_executable_free(ipt->exe);
    free(ipt);
}

mt_ipt_proto_t mt_ipt_proto(const mt_ipt_t *ipt) {
    return ipt->exe->ops->proto(ipt->exe);
}

static table_reg_t *find_table(mt_ipt_t *ipt, const char *name) {
    for (size_t i = 0; i < ipt->n_tables; i++) {
        if (strcmp(ipt->tables[i].table_name, name) == 0) { return &ipt->tables[i]; }
    }
    return NULL;
}

static table_reg_t *find_or_create_table(mt_ipt_t *ipt, const char *name) {
    table_reg_t *t = find_table(ipt, name);
    if (t) { return t; }

    if (ipt->n_tables + 1 > ipt->cap_tables) {
        size_t newcap = ipt->cap_tables == 0 ? 4 : ipt->cap_tables * 2;
        table_reg_t *tmp = realloc(ipt->tables, newcap * sizeof(*tmp));
        if (!tmp) { return NULL; }
        ipt->tables = tmp;
        ipt->cap_tables = newcap;
    }
    table_reg_t *nt = &ipt->tables[ipt->n_tables];
    memset(nt, 0, sizeof(*nt));
    nt->table_name = strdup(name);
    if (!nt->table_name) { return NULL; }
    ipt->n_tables++;
    return nt;
}

static chain_reg_t *find_chain_reg(table_reg_t *t, const char *name) {
    for (size_t i = 0; i < t->n_chains; i++) {
        if (strcmp(t->chains[i].chain_name, name) == 0) { return &t->chains[i]; }
    }
    return NULL;
}

static mt_err_t register_chain(mt_ipt_t *ipt, const char *table, const char *chain,
                               mt_ipt_chain_t *(*ctor)(void)) {
    table_reg_t *t = find_or_create_table(ipt, table);
    if (!t) { return MT_ERR_NOMEM; }

    mt_ipt_chain_t *newchain = ctor();
    if (!newchain) { return MT_ERR_NOMEM; }

    chain_reg_t *c = find_chain_reg(t, chain);
    if (c) {
        c->chain->ops->destroy(c->chain);
        c->chain = newchain;
        return MT_OK;
    }

    if (t->n_chains + 1 > t->cap_chains) {
        size_t newcap = t->cap_chains == 0 ? 4 : t->cap_chains * 2;
        chain_reg_t *tmp = realloc(t->chains, newcap * sizeof(*tmp));
        if (!tmp) {
            newchain->ops->destroy(newchain);
            return MT_ERR_NOMEM;
        }
        t->chains = tmp;
        t->cap_chains = newcap;
    }
    chain_reg_t *nc = &t->chains[t->n_chains];
    nc->chain_name = strdup(chain);
    if (!nc->chain_name) {
        newchain->ops->destroy(newchain);
        return MT_ERR_NOMEM;
    }
    nc->chain = newchain;
    t->n_chains++;
    return MT_OK;
}

mt_err_t mt_ipt_register_chain_delete(mt_ipt_t *ipt, const char *table, const char *chain) {
    return register_chain(ipt, table, chain, mt_ipt_chain_delete_new);
}
mt_err_t mt_ipt_register_chain_patch(mt_ipt_t *ipt, const char *table, const char *chain) {
    return register_chain(ipt, table, chain, mt_ipt_chain_patch_new);
}
mt_err_t mt_ipt_register_chain_override(mt_ipt_t *ipt, const char *table, const char *chain) {
    return register_chain(ipt, table, chain, mt_ipt_chain_override_new);
}

/* ---- Append/Insert/Delete dispatch -------------------------------------- */

static mt_err_t dispatch_rule_op(mt_ipt_t *ipt, const char *table, const char *chain,
                                 const char *const *args, size_t n_args, int rule_num,
                                 int op /* 0=append 1=insert 2=delete */) {
    table_reg_t *t = find_table(ipt, table);
    chain_reg_t *c = t ? find_chain_reg(t, chain) : NULL;
    if (!c) { return MT_ERR_STATE; /* Go: ErrChainNotInitialized */ }

    mt_ipt_rule_t *rule = mt_ipt_rule_new(args, n_args);
    if (!rule) { return MT_ERR_NOMEM; }

    mt_err_t err;
    switch (op) {
    case 0: err = c->chain->ops->append(c->chain, rule); break;
    case 1: err = c->chain->ops->insert(c->chain, rule_num, rule); break;
    default: err = c->chain->ops->remove(c->chain, rule); break;
    }
    mt_ipt_rule_free(rule);
    return err;
}

mt_err_t mt_ipt_append(mt_ipt_t *ipt, const char *table, const char *chain,
                       const char *const *args, size_t n_args) {
    return dispatch_rule_op(ipt, table, chain, args, n_args, 0, 0);
}
mt_err_t mt_ipt_insert(mt_ipt_t *ipt, const char *table, const char *chain, int rule_num,
                       const char *const *args, size_t n_args) {
    return dispatch_rule_op(ipt, table, chain, args, n_args, rule_num, 1);
}
mt_err_t mt_ipt_delete(mt_ipt_t *ipt, const char *table, const char *chain,
                       const char *const *args, size_t n_args) {
    return dispatch_rule_op(ipt, table, chain, args, n_args, 0, 2);
}

bool mt_ipt_err_is_chain_not_initialized(mt_err_t err) {
    return err == MT_ERR_STATE;
}

/* ---- GetCurrentRules: parse Executable.Save() output -------------------- */

typedef struct chain_builder {
    char *chain_name;
    mt_ipt_rule_t **rules;
    size_t n_rules, cap_rules;
} chain_builder_t;

typedef struct table_builder {
    char *table_name;
    chain_builder_t *chains;
    size_t n_chains, cap_chains;
} table_builder_t;

static chain_builder_t *builder_find_chain(table_builder_t *t, const char *name, size_t len) {
    for (size_t i = 0; i < t->n_chains; i++) {
        if (strlen(t->chains[i].chain_name) == len &&
            memcmp(t->chains[i].chain_name, name, len) == 0) {
            return &t->chains[i];
}
    }
    return NULL;
}

static chain_builder_t *builder_find_or_create_chain(table_builder_t *t, const char *name,
                                                     size_t len) {
    chain_builder_t *c = builder_find_chain(t, name, len);
    if (c) { return c; }

    if (t->n_chains + 1 > t->cap_chains) {
        size_t newcap = t->cap_chains == 0 ? 4 : t->cap_chains * 2;
        chain_builder_t *tmp = realloc(t->chains, newcap * sizeof(*tmp));
        if (!tmp) { return NULL; }
        t->chains = tmp;
        t->cap_chains = newcap;
    }
    chain_builder_t *nc = &t->chains[t->n_chains];
    nc->chain_name = strndup(name, len);
    if (!nc->chain_name) { return NULL; }
    /* Non-NULL sentinel array even with zero rules, so callers can tell
     * "chain declared, zero rules" (non-NULL) from "chain absent"
     * (find_chain returns NULL entirely) -- see iptables.h contract. */
    nc->rules = calloc(1, sizeof(mt_ipt_rule_t *));
    if (!nc->rules) { return NULL; }
    nc->n_rules = 0;
    nc->cap_rules = 1;
    t->n_chains++;
    return nc;
}

static mt_err_t chain_builder_add_rule(chain_builder_t *c, mt_ipt_rule_t *rule) {
    if (c->n_rules + 1 > c->cap_rules) {
        size_t newcap = c->cap_rules == 0 ? 4 : c->cap_rules * 2;
        mt_ipt_rule_t **tmp = realloc(c->rules, newcap * sizeof(*tmp));
        if (!tmp) { return MT_ERR_NOMEM; }
        c->rules = tmp;
        c->cap_rules = newcap;
    }
    c->rules[c->n_rules++] = rule;
    return MT_OK;
}

static table_builder_t *tb_find(table_builder_t *tables, size_t n, const char *name, size_t len) {
    for (size_t i = 0; i < n; i++) {
        if (strlen(tables[i].table_name) == len && memcmp(tables[i].table_name, name, len) == 0) {
            return &tables[i];
}
    }
    return NULL;
}

static table_builder_t *tb_find_or_create(table_builder_t **tables, size_t *n, size_t *cap,
                                          const char *name, size_t len) {
    table_builder_t *t = tb_find(*tables, *n, name, len);
    if (t) { return t; }

    if (*n + 1 > *cap) {
        size_t newcap = *cap == 0 ? 4 : *cap * 2;
        table_builder_t *tmp = realloc(*tables, newcap * sizeof(*tmp));
        if (!tmp) { return NULL; }
        *tables = tmp;
        *cap = newcap;
    }
    table_builder_t *nt = &(*tables)[*n];
    memset(nt, 0, sizeof(*nt));
    nt->table_name = strndup(name, len);
    if (!nt->table_name) { return NULL; }
    (*n)++;
    return nt;
}

/* Frees inner content (table_name/chains/rules) for tables[start,end)
 * without touching the `tables` array pointer itself -- needed because
 * `tables` may be freed separately at a different point than the range
 * of entries still owned by the builder (see the transplant-OOM path in
 * mt_ipt_get_current_rules: freeing `tables + start` directly would be
 * undefined behaviour, since that is not a pointer malloc/realloc ever
 * returned). */
static void builders_free_range(table_builder_t *tables, size_t start, size_t end) {
    for (size_t ti = start; ti < end; ti++) {
        table_builder_t *t = &tables[ti];
        for (size_t ci = 0; ci < t->n_chains; ci++) {
            chain_builder_t *c = &t->chains[ci];
            for (size_t ri = 0; ri < c->n_rules; ri++) { mt_ipt_rule_free(c->rules[ri]); }
            free(c->rules);
            free(c->chain_name);
        }
        free(t->chains);
        free(t->table_name);
    }
}

static void builders_free(table_builder_t *tables, size_t n) {
    builders_free_range(tables, 0, n);
    free(tables);
}

/* Whitespace (space/tab) tokenizer over one line; returns owned array of
 * (offset,len) pairs into the caller's buffer (no copies here). */
typedef struct field_span {
    size_t off, len;
} field_span_t;

static mt_err_t split_fields(const uint8_t *line, size_t line_len, field_span_t **out,
                             size_t *out_n) {
    field_span_t *fields = NULL;
    size_t n = 0, cap = 0;
    long start = -1;

    for (size_t i = 0; i < line_len; i++) {
        uint8_t b = line[i];
        if (b == ' ' || b == '\t') {
            if (start >= 0) {
                if (n + 1 > cap) {
                    size_t newcap = cap == 0 ? 8 : cap * 2;
                    field_span_t *tmp = realloc(fields, newcap * sizeof(*tmp));
                    if (!tmp) {
                        free(fields);
                        return MT_ERR_NOMEM;
                    }
                    fields = tmp;
                    cap = newcap;
                }
                fields[n].off = (size_t)start;
                fields[n].len = i - (size_t)start;
                n++;
                start = -1;
            }
        } else if (start < 0) {
            start = (long)i;
        }
    }
    if (start >= 0) {
        if (n + 1 > cap) {
            field_span_t *tmp = realloc(fields, (n + 1) * sizeof(*tmp));
            if (!tmp) {
                free(fields);
                return MT_ERR_NOMEM;
            }
            fields = tmp;
        }
        fields[n].off = (size_t)start;
        fields[n].len = line_len - (size_t)start;
        n++;
    }

    *out = fields;
    *out_n = n;
    return MT_OK;
}

mt_err_t mt_ipt_get_current_rules(mt_ipt_t *ipt, mt_ipt_rules_snapshot_t **out) {
    *out = NULL;

    uint8_t *data = NULL;
    size_t data_len = 0;
    mt_err_t err = ipt->exe->ops->save(ipt->exe, &data, &data_len);
    if (err != MT_OK) { return err; }

    table_builder_t *tables = NULL;
    size_t n_tables = 0, cap_tables = 0;
    table_builder_t *cur_table = NULL;

    size_t line_start = 0;
    for (size_t i = 0; i <= data_len && err == MT_OK; i++) {
        if (i < data_len && data[i] != '\n') { continue; }

        const uint8_t *line = data + line_start;
        size_t line_len = i - line_start;
        line_start = i + 1;

        if (line_len == 0) { continue; }

        switch (line[0]) {
        case '*': {
            cur_table = tb_find_or_create(&tables, &n_tables, &cap_tables,
                                          (const char *)line + 1, line_len - 1);
            if (!cur_table) { err = MT_ERR_NOMEM; }
            break;
        }
        case ':': {
            size_t name_len;
            const uint8_t *body = line + 1;
            size_t body_len = line_len - 1;
            size_t space_idx = body_len;
            for (size_t k = 0; k < body_len; k++) {
                if (body[k] == ' ') {
                    space_idx = k;
                    break;
                }
            }
            name_len = space_idx;
            if (name_len == 0) {
                err = MT_ERR_PROTO;
                break;
            }
            if (!cur_table) {
                err = MT_ERR_PROTO; /* chain decl outside of table (hardened
                                      * vs. Go's nil-map panic on malformed
                                      * input; never hit by real iptables-
                                      * save output — see decisions.md). */
                break;
            }
            if (!builder_find_or_create_chain(cur_table, (const char *)body, name_len)) {
                err = MT_ERR_NOMEM;
}
            break;
        }
        case '-': {
            if (!cur_table) {
                err = MT_ERR_PROTO;
                break;
            }
            field_span_t *fields = NULL;
            size_t n_fields = 0;
            err = split_fields(line, line_len, &fields, &n_fields);
            if (err != MT_OK) { break; }
            if (n_fields < 2) {
                err = MT_ERR_PROTO;
                free(fields);
                break;
            }
            if (fields[0].len < 2) {
                err = MT_ERR_PROTO;
                free(fields);
                break;
            }
            uint8_t op = line[fields[0].off + 1];
            chain_builder_t *cb = builder_find_or_create_chain(
                cur_table, (const char *)line + fields[1].off, fields[1].len);
            if (!cb) {
                err = MT_ERR_NOMEM;
                free(fields);
                break;
            }
            if (op == 'A') {
                size_t n_parts = n_fields - 2;
                char **parts = NULL;
                if (n_parts > 0) {
                    parts = calloc(n_parts, sizeof(char *));
                    if (!parts) {
                        err = MT_ERR_NOMEM;
                        free(fields);
                        break;
                    }
                    for (size_t k = 0; k < n_parts; k++) {
                        parts[k] = strndup((const char *)line + fields[k + 2].off,
                                           fields[k + 2].len);
                        if (!parts[k]) {
                            for (size_t j = 0; j < k; j++) { free(parts[j]); }
                            free(parts);
                            parts = NULL;
                            err = MT_ERR_NOMEM;
                            break;
                        }
                    }
                }
                if (err == MT_OK) {
                    mt_ipt_rule_t *rule = mt_ipt_rule_new((const char *const *)parts, n_parts);
                    for (size_t k = 0; k < n_parts; k++) { free(parts[k]); }
                    free(parts);
                    if (!rule) {
                        err = MT_ERR_NOMEM;
                    } else {
                        err = chain_builder_add_rule(cb, rule);
                        if (err != MT_OK) { mt_ipt_rule_free(rule); }
                    }
                }
            } else {
                err = MT_ERR_PROTO; /* unknown iptables command in Save() output */
            }
            free(fields);
            break;
        }
        case '#':
            break; /* comment */
        case 'C':
            cur_table = NULL; /* COMMIT */
            break;
        default:
            err = MT_ERR_PROTO;
            break;
        }
    }

    free(data);

    if (err != MT_OK) {
        builders_free(tables, n_tables);
        return err;
    }

    /* Transplant builders -> public snapshot shape. */
    mt_ipt_rules_snapshot_t *snap = calloc(1, sizeof(*snap));
    if (!snap) {
        builders_free(tables, n_tables);
        return MT_ERR_NOMEM;
    }
    if (n_tables > 0) {
        snap->tables = calloc(n_tables, sizeof(*snap->tables));
        if (!snap->tables) {
            free(snap);
            builders_free(tables, n_tables);
            return MT_ERR_NOMEM;
        }
    }
    snap->n_tables = n_tables;

    for (size_t ti = 0; ti < n_tables; ti++) {
        table_builder_t *tb = &tables[ti];
        mt_ipt_table_rules_t *dst = &snap->tables[ti];
        dst->table_name = tb->table_name;
        dst->n_chains = tb->n_chains;
        if (tb->n_chains > 0) {
            dst->chains = calloc(tb->n_chains, sizeof(*dst->chains));
            if (!dst->chains) {
                /* OOM mid-transplant. snap->tables[0,ti) are fully
                 * transplanted (their table_name/chains ownership moved
                 * out of `tables`), so restricting snap->n_tables to ti
                 * before freeing it reclaims exactly that range. Index ti
                 * itself only had dst->table_name aliased (not yet fully
                 * owned -- its chains alloc just failed), so the matching
                 * builder entry at tables[ti] still legitimately owns
                 * table_name and must free it via builders_free_range,
                 * which also covers the untouched [ti+1, n_tables) tail.
                 * `tables` itself is a single allocation, freed once here
                 * (never via a pointer offset -- see builders_free_range's
                 * comment). */
                snap->n_tables = ti;
                mt_ipt_rules_snapshot_free(snap);
                builders_free_range(tables, ti, n_tables);
                free(tables);
                return MT_ERR_NOMEM;
            }
        } else {
            dst->chains = NULL;
        }
        for (size_t ci = 0; ci < tb->n_chains; ci++) {
            chain_builder_t *cb = &tb->chains[ci];
            dst->chains[ci].chain_name = cb->chain_name;
            dst->chains[ci].rules = cb->rules;
            dst->chains[ci].n_rules = cb->n_rules;
        }
        free(tb->chains); /* shell only: fields moved above */
    }
    free(tables);

    *out = snap;
    return MT_OK;
}

const mt_ipt_table_rules_t *mt_ipt_rules_snapshot_find_table(const mt_ipt_rules_snapshot_t *snap,
                                                              const char *table_name) {
    if (!snap) { return NULL; }
    for (size_t i = 0; i < snap->n_tables; i++) {
        if (strcmp(snap->tables[i].table_name, table_name) == 0) { return &snap->tables[i]; }
    }
    return NULL;
}

const mt_ipt_chain_rules_t *mt_ipt_table_rules_find_chain(const mt_ipt_table_rules_t *table,
                                                          const char *chain_name) {
    if (!table) { return NULL; }
    for (size_t i = 0; i < table->n_chains; i++) {
        if (strcmp(table->chains[i].chain_name, chain_name) == 0) { return &table->chains[i]; }
    }
    return NULL;
}

void mt_ipt_rules_snapshot_free(mt_ipt_rules_snapshot_t *snap) {
    if (!snap) { return; }
    for (size_t ti = 0; ti < snap->n_tables; ti++) {
        mt_ipt_table_rules_t *t = &snap->tables[ti];
        for (size_t ci = 0; ci < t->n_chains; ci++) {
            mt_ipt_chain_rules_t *c = &t->chains[ci];
            for (size_t ri = 0; ri < c->n_rules; ri++) { mt_ipt_rule_free(c->rules[ri]); }
            free(c->rules);
            free(c->chain_name);
        }
        free(t->chains);
        free(t->table_name);
    }
    free(snap->tables);
    free(snap);
}

/* ---- Commit: compile every registered chain, assemble one restore ------ */
/* transcript grouped by priority, execute it.                             */

typedef struct prio_bucket {
    int8_t priority;
    mt_ipt_command_t *cmds;
    size_t n, cap;
} prio_bucket_t;

static prio_bucket_t *bucket_find_or_create(prio_bucket_t **buckets, size_t *n, size_t *cap,
                                            int8_t priority) {
    for (size_t i = 0; i < *n; i++) {
        if ((*buckets)[i].priority == priority) { return &(*buckets)[i]; }
    }
    if (*n + 1 > *cap) {
        size_t newcap = *cap == 0 ? 4 : *cap * 2;
        prio_bucket_t *tmp = realloc(*buckets, newcap * sizeof(*tmp));
        if (!tmp) { return NULL; }
        *buckets = tmp;
        *cap = newcap;
    }
    prio_bucket_t *b = &(*buckets)[*n];
    b->priority = priority;
    b->cmds = NULL;
    b->n = 0;
    b->cap = 0;
    (*n)++;
    return b;
}

static mt_err_t bucket_append_range(prio_bucket_t *b, mt_ipt_command_t *src, size_t n_src) {
    if (b->n + n_src > b->cap) {
        size_t newcap = b->cap == 0 ? 8 : b->cap;
        while (newcap < b->n + n_src) { newcap *= 2; }
        mt_ipt_command_t *tmp = realloc(b->cmds, newcap * sizeof(*tmp));
        if (!tmp) { return MT_ERR_NOMEM; }
        b->cmds = tmp;
        b->cap = newcap;
    }
    memcpy(b->cmds + b->n, src, n_src * sizeof(*src));
    b->n += n_src;
    return MT_OK;
}

static int bucket_cmp(const void *a, const void *b) {
    int8_t pa = ((const prio_bucket_t *)a)->priority;
    int8_t pb = ((const prio_bucket_t *)b)->priority;
    return (pa > pb) - (pa < pb);
}

static void buckets_free(prio_bucket_t *buckets, size_t n) {
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < buckets[i].n; j++) {
            free(buckets[i].cmds[j].chain);
            mt_ipt_rule_free(buckets[i].cmds[j].rule);
        }
        free(buckets[i].cmds);
    }
    free(buckets);
}

static mt_err_t write_rule_string(mt_bytebuf_t *buf, const mt_ipt_rule_t *rule) {
    mt_err_t err = MT_OK;
    for (size_t i = 0; i < rule->n_parts && err == MT_OK; i++) {
        if (i > 0) { err = mt_bytebuf_append_byte(buf, ' '); }
        if (err == MT_OK) { err = mt_bytebuf_append_str(buf, rule->parts[i]); }
    }
    return err;
}

static mt_err_t write_command(mt_bytebuf_t *buf, const mt_ipt_command_t *cmd) {
    mt_err_t err = MT_OK;
    switch (cmd->option) {
    case MT_IPT_OP_APPEND:
        err = mt_bytebuf_append_str(buf, "-A ");
        if (err == MT_OK) { err = mt_bytebuf_append_str(buf, cmd->chain); }
        if (err == MT_OK && cmd->rule && cmd->rule->n_parts > 0) {
            err = mt_bytebuf_append_byte(buf, ' ');
            if (err == MT_OK) { err = write_rule_string(buf, cmd->rule); }
        }
        if (err == MT_OK) { err = mt_bytebuf_append_byte(buf, '\n'); }
        break;
    case MT_IPT_OP_DELETE:
        err = mt_bytebuf_append_str(buf, "-D ");
        if (err == MT_OK) { err = mt_bytebuf_append_str(buf, cmd->chain); }
        if (err == MT_OK && cmd->rule && cmd->rule->n_parts > 0) {
            err = mt_bytebuf_append_byte(buf, ' ');
            if (err == MT_OK) { err = write_rule_string(buf, cmd->rule); }
        }
        if (err == MT_OK) { err = mt_bytebuf_append_byte(buf, '\n'); }
        break;
    case MT_IPT_OP_INSERT: {
        char num[32];
        int written = snprintf(num, sizeof(num), "%d", cmd->rule_num);
        if (written < 0 || (size_t)written >= sizeof(num)) { return MT_ERR_INVAL; }
        err = mt_bytebuf_append_str(buf, "-I ");
        if (err == MT_OK) { err = mt_bytebuf_append_str(buf, cmd->chain); }
        if (err == MT_OK) { err = mt_bytebuf_append_byte(buf, ' '); }
        if (err == MT_OK) { err = mt_bytebuf_append_str(buf, num); }
        if (err == MT_OK && cmd->rule && cmd->rule->n_parts > 0) {
            err = mt_bytebuf_append_byte(buf, ' ');
            if (err == MT_OK) { err = write_rule_string(buf, cmd->rule); }
        }
        if (err == MT_OK) { err = mt_bytebuf_append_byte(buf, '\n'); }
        break;
    }
    case MT_IPT_OP_FLUSH:
        err = mt_bytebuf_append_str(buf, "-F ");
        if (err == MT_OK) { err = mt_bytebuf_append_str(buf, cmd->chain); }
        if (err == MT_OK) { err = mt_bytebuf_append_byte(buf, '\n'); }
        break;
    case MT_IPT_OP_DELETE_CHAIN:
        err = mt_bytebuf_append_str(buf, "-X ");
        if (err == MT_OK) { err = mt_bytebuf_append_str(buf, cmd->chain); }
        if (err == MT_OK) { err = mt_bytebuf_append_byte(buf, '\n'); }
        break;
    }
    return err;
}

void mt_ipt_set_cancel(mt_ipt_t *ipt, mt_cancel_t *cancel) {
    ipt->cancel = cancel;
    if (ipt->exe->ops->set_cancel) { ipt->exe->ops->set_cancel(ipt->exe, cancel); }
}

mt_err_t mt_ipt_commit(mt_ipt_t *ipt) {
    /* Checked here as well as inside the executable so an abort that
     * lands between the two transfers still stops the write: reading the
     * table we are about to diff against is pointless once we know the
     * result is going to be thrown away. */
    if (mt_cancel_raised(ipt->cancel)) { return MT_ERR_CANCELED; }

    mt_ipt_rules_snapshot_t *cur = NULL;
    mt_err_t err = mt_ipt_get_current_rules(ipt, &cur);
    if (err != MT_OK) { return err; }

    mt_bytebuf_t buf;
    mt_bytebuf_init(&buf);

    for (size_t ti = 0; ti < ipt->n_tables && err == MT_OK; ti++) {
        table_reg_t *t = &ipt->tables[ti];
        const mt_ipt_table_rules_t *cur_table = mt_ipt_rules_snapshot_find_table(cur, t->table_name);

        prio_bucket_t *buckets = NULL;
        size_t n_buckets = 0, cap_buckets = 0;
        bool table_written = false;

        for (size_t ci = 0; ci < t->n_chains && err == MT_OK; ci++) {
            chain_reg_t *c = &t->chains[ci];
            const mt_ipt_chain_rules_t *cur_chain =
                cur_table ? mt_ipt_table_rules_find_chain(cur_table, c->chain_name) : NULL;
            mt_ipt_rule_t *const *existing = cur_chain ? cur_chain->rules : NULL;
            size_t n_existing = cur_chain ? cur_chain->n_rules : 0;

            mt_ipt_command_t *cmds = NULL;
            size_t n_cmds = 0;
            int8_t priority = 0;
            err = c->chain->ops->compile(c->chain, c->chain_name, existing, n_existing, &cmds,
                                         &n_cmds, &priority);
            if (err != MT_OK) { break; }
            if (n_cmds == 0) { continue; }

            if (!table_written) {
                err = mt_bytebuf_append_byte(&buf, '*');
                if (err == MT_OK) { err = mt_bytebuf_append_str(&buf, t->table_name); }
                if (err == MT_OK) { err = mt_bytebuf_append_byte(&buf, '\n'); }
                if (err != MT_OK) {
                    mt_ipt_command_list_free(cmds, n_cmds);
                    break;
                }
                table_written = true;
            }
            err = mt_bytebuf_append_byte(&buf, ':');
            if (err == MT_OK) { err = mt_bytebuf_append_str(&buf, c->chain_name); }
            if (err == MT_OK) { err = mt_bytebuf_append_str(&buf, " - [0:0]\n"); }
            if (err != MT_OK) {
                mt_ipt_command_list_free(cmds, n_cmds);
                break;
            }

            prio_bucket_t *bucket = bucket_find_or_create(&buckets, &n_buckets, &cap_buckets, priority);
            if (!bucket) {
                mt_ipt_command_list_free(cmds, n_cmds);
                err = MT_ERR_NOMEM;
                break;
            }
            err = bucket_append_range(bucket, cmds, n_cmds);
            free(cmds); /* shallow: ownership of chain/rule moved into bucket */
        }

        if (err == MT_OK && table_written) {
            qsort(buckets, n_buckets, sizeof(*buckets), bucket_cmp);
            for (size_t bi = 0; bi < n_buckets && err == MT_OK; bi++) {
                for (size_t ci = 0; ci < buckets[bi].n && err == MT_OK; ci++) {
                    err = write_command(&buf, &buckets[bi].cmds[ci]);
                }
            }
            if (err == MT_OK) { err = mt_bytebuf_append_str(&buf, "COMMIT\n"); }
        }

        buckets_free(buckets, n_buckets);
    }

    mt_ipt_rules_snapshot_free(cur);

    if (err != MT_OK) {
        mt_bytebuf_free(&buf);
        return err;
    }

    if (buf.len == 0) {
        mt_bytebuf_free(&buf);
        return MT_OK;
    }

    if (mt_cancel_raised(ipt->cancel)) {
        mt_bytebuf_free(&buf);
        return MT_ERR_CANCELED;
    }

    err = ipt->exe->ops->restore(ipt->exe, buf.data, buf.len);
    mt_bytebuf_free(&buf);
    return err;
}
