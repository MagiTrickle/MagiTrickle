/* See fake_iptables.h. Port of executable-fake.go's FakeIPTables. */
#include "fake_iptables.h"

#include "magitrickle/bytebuf.h"

#include <stdlib.h>
#include <string.h>

typedef struct fake_chain {
    char *name;
    mt_ipt_rule_t **rules;
    size_t n, cap;
} fake_chain_t;

typedef struct fake_table {
    char *name;
    fake_chain_t *chains;
    size_t n_chains, cap_chains;
} fake_table_t;

struct mt_fake_ipt {
    mt_ipt_executable_t base;
    mt_ipt_proto_t proto;
    fake_table_t *tables;
    size_t n_tables, cap_tables;
};

static fake_table_t *ft_find(mt_fake_ipt_t *f, const char *name) {
    for (size_t i = 0; i < f->n_tables; i++) {
        if (strcmp(f->tables[i].name, name) == 0) { return &f->tables[i]; }
}
    return NULL;
}

static fake_table_t *ft_find_or_create(mt_fake_ipt_t *f, const char *name, size_t name_len) {
    for (size_t i = 0; i < f->n_tables; i++) {
        if (strlen(f->tables[i].name) == name_len &&
            memcmp(f->tables[i].name, name, name_len) == 0) {
            return &f->tables[i];
}
    }
    if (f->n_tables + 1 > f->cap_tables) {
        size_t newcap = f->cap_tables == 0 ? 4 : f->cap_tables * 2;
        fake_table_t *tmp = realloc(f->tables, newcap * sizeof(*tmp));
        if (!tmp) { return NULL; }
        f->tables = tmp;
        f->cap_tables = newcap;
    }
    fake_table_t *nt = &f->tables[f->n_tables];
    memset(nt, 0, sizeof(*nt));
    nt->name = strndup(name, name_len);
    if (!nt->name) { return NULL; }
    f->n_tables++;
    return nt;
}

static fake_chain_t *fc_find(fake_table_t *t, const char *name) {
    for (size_t i = 0; i < t->n_chains; i++) {
        if (strcmp(t->chains[i].name, name) == 0) { return &t->chains[i]; }
}
    return NULL;
}

static fake_chain_t *fc_find_or_create(fake_table_t *t, const char *name, size_t name_len) {
    for (size_t i = 0; i < t->n_chains; i++) {
        if (strlen(t->chains[i].name) == name_len &&
            memcmp(t->chains[i].name, name, name_len) == 0) {
            return &t->chains[i];
}
    }
    if (t->n_chains + 1 > t->cap_chains) {
        size_t newcap = t->cap_chains == 0 ? 4 : t->cap_chains * 2;
        fake_chain_t *tmp = realloc(t->chains, newcap * sizeof(*tmp));
        if (!tmp) { return NULL; }
        t->chains = tmp;
        t->cap_chains = newcap;
    }
    fake_chain_t *nc = &t->chains[t->n_chains];
    memset(nc, 0, sizeof(*nc));
    nc->name = strndup(name, name_len);
    if (!nc->name) { return NULL; }
    t->n_chains++;
    return nc;
}

static mt_err_t fc_append_owned(fake_chain_t *c, mt_ipt_rule_t *r) {
    if (c->n + 1 > c->cap) {
        size_t newcap = c->cap == 0 ? 4 : c->cap * 2;
        mt_ipt_rule_t **tmp = realloc(c->rules, newcap * sizeof(*tmp));
        if (!tmp) { return MT_ERR_NOMEM; }
        c->rules = tmp;
        c->cap = newcap;
    }
    c->rules[c->n++] = r;
    return MT_OK;
}

static void fc_clear(fake_chain_t *c) {
    for (size_t i = 0; i < c->n; i++) { mt_ipt_rule_free(c->rules[i]); }
    c->n = 0;
}

/* ---- public test-seeding/inspection API --------------------------------- */

mt_err_t mt_fake_ipt_set_initial_rules(mt_fake_ipt_t *f, const char *table, const char *chain,
                                       const char *const *const *rules, const size_t *rule_lens,
                                       size_t n_rules) {
    fake_table_t *t = ft_find_or_create(f, table, strlen(table));
    if (!t) { return MT_ERR_NOMEM; }
    fake_chain_t *c = fc_find_or_create(t, chain, strlen(chain));
    if (!c) { return MT_ERR_NOMEM; }
    fc_clear(c);

    for (size_t i = 0; i < n_rules; i++) {
        mt_ipt_rule_t *r = mt_ipt_rule_new(rules[i], rule_lens[i]);
        if (!r) { return MT_ERR_NOMEM; }
        mt_err_t err = fc_append_owned(c, r);
        if (err != MT_OK) {
            mt_ipt_rule_free(r);
            return err;
        }
    }
    return MT_OK;
}

bool mt_fake_ipt_get_rules(mt_fake_ipt_t *f, const char *table, const char *chain,
                          mt_ipt_rule_t *const **out_rules, size_t *out_n) {
    fake_table_t *t = ft_find(f, table);
    fake_chain_t *c = t ? fc_find(t, chain) : NULL;
    if (!c) {
        *out_rules = NULL;
        *out_n = 0;
        return false;
    }
    *out_rules = c->rules;
    *out_n = c->n;
    return true;
}

bool mt_fake_ipt_chain_exists(mt_fake_ipt_t *f, const char *table, const char *chain) {
    fake_table_t *t = ft_find(f, table);
    return t ? fc_find(t, chain) != NULL : false;
}

/* ---- Save(): sorted-by-name serialization, mirrors FakeIPTables.Save ---- */

static int name_cmp_table(const void *a, const void *b) {
    const fake_table_t *ta = *(const fake_table_t *const *)a;
    const fake_table_t *tb = *(const fake_table_t *const *)b;
    return strcmp(ta->name, tb->name);
}

static int name_cmp_chain(const void *a, const void *b) {
    const fake_chain_t *ca = *(const fake_chain_t *const *)a;
    const fake_chain_t *cb = *(const fake_chain_t *const *)b;
    return strcmp(ca->name, cb->name);
}

static mt_err_t write_rule_parts(mt_bytebuf_t *buf, const mt_ipt_rule_t *r) {
    mt_err_t err = MT_OK;
    for (size_t i = 0; i < r->n_parts && err == MT_OK; i++) {
        err = mt_bytebuf_append_byte(buf, ' ');
        if (err == MT_OK) { err = mt_bytebuf_append_str(buf, r->parts[i]); }
    }
    return err;
}

static mt_err_t fake_save(mt_ipt_executable_t *self, uint8_t **out, size_t *out_len) {
    mt_fake_ipt_t *f = (mt_fake_ipt_t *)self;
    mt_bytebuf_t buf;
    mt_bytebuf_init(&buf);
    mt_err_t err = MT_OK;

    fake_table_t **tables = NULL;
    if (f->n_tables > 0) {
        tables = malloc(f->n_tables * sizeof(*tables));
        if (!tables) { return MT_ERR_NOMEM; }
        for (size_t i = 0; i < f->n_tables; i++) { tables[i] = &f->tables[i]; }
        qsort(tables, f->n_tables, sizeof(*tables), name_cmp_table);
    }

    for (size_t ti = 0; ti < f->n_tables && err == MT_OK; ti++) {
        fake_table_t *t = tables[ti];
        err = mt_bytebuf_append_byte(&buf, '*');
        if (err == MT_OK) { err = mt_bytebuf_append_str(&buf, t->name); }
        if (err == MT_OK) { err = mt_bytebuf_append_byte(&buf, '\n'); }
        if (err != MT_OK) { break; }

        fake_chain_t **chains = NULL;
        if (t->n_chains > 0) {
            chains = malloc(t->n_chains * sizeof(*chains));
            if (!chains) {
                err = MT_ERR_NOMEM;
                break;
            }
            for (size_t i = 0; i < t->n_chains; i++) { chains[i] = &t->chains[i]; }
            qsort(chains, t->n_chains, sizeof(*chains), name_cmp_chain);
        }

        for (size_t ci = 0; ci < t->n_chains && err == MT_OK; ci++) {
            err = mt_bytebuf_append_byte(&buf, ':');
            if (err == MT_OK) { err = mt_bytebuf_append_str(&buf, chains[ci]->name); }
            if (err == MT_OK) { err = mt_bytebuf_append_str(&buf, " - [0:0]\n"); }
        }
        for (size_t ci = 0; ci < t->n_chains && err == MT_OK; ci++) {
            fake_chain_t *c = chains[ci];
            for (size_t ri = 0; ri < c->n && err == MT_OK; ri++) {
                err = mt_bytebuf_append_str(&buf, "-A ");
                if (err == MT_OK) { err = mt_bytebuf_append_str(&buf, c->name); }
                if (err == MT_OK) { err = write_rule_parts(&buf, c->rules[ri]); }
                if (err == MT_OK) { err = mt_bytebuf_append_byte(&buf, '\n'); }
            }
        }
        free(chains);
        if (err == MT_OK) { err = mt_bytebuf_append_str(&buf, "COMMIT\n"); }
    }
    free(tables);

    if (err != MT_OK) {
        mt_bytebuf_free(&buf);
        return err;
    }
    *out = buf.data;
    *out_len = buf.len;
    return MT_OK;
}

/* ---- Restore(): apply a transcript's A/I/D/F/X operations --------------- */

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
        field_span_t *tmp = realloc(fields, (n + 1) * sizeof(*tmp));
        if (!tmp) {
            free(fields);
            return MT_ERR_NOMEM;
        }
        fields = tmp;
        fields[n].off = (size_t)start;
        fields[n].len = line_len - (size_t)start;
        n++;
    }
    *out = fields;
    *out_n = n;
    return MT_OK;
}

static mt_err_t rule_from_fields(const uint8_t *line, const field_span_t *fields,
                                 size_t start_field, size_t n_fields, mt_ipt_rule_t **out) {
    size_t n_parts = n_fields - start_field;
    char **parts = NULL;
    if (n_parts > 0) {
        parts = calloc(n_parts, sizeof(char *));
        if (!parts) { return MT_ERR_NOMEM; }
        for (size_t k = 0; k < n_parts; k++) {
            const field_span_t *fs = &fields[start_field + k];
            parts[k] = strndup((const char *)line + fs->off, fs->len);
            if (!parts[k]) {
                for (size_t j = 0; j < k; j++) { free(parts[j]); }
                free(parts);
                return MT_ERR_NOMEM;
            }
        }
    }
    mt_ipt_rule_t *r = mt_ipt_rule_new((const char *const *)parts, n_parts);
    for (size_t k = 0; k < n_parts; k++) { free(parts[k]); }
    free(parts);
    if (!r) { return MT_ERR_NOMEM; }
    *out = r;
    return MT_OK;
}

static mt_err_t fake_restore(mt_ipt_executable_t *self, const uint8_t *data, size_t len) {
    mt_fake_ipt_t *f = (mt_fake_ipt_t *)self;
    fake_table_t *cur_table = NULL;
    mt_err_t err = MT_OK;

    size_t line_start = 0;
    for (size_t i = 0; i <= len && err == MT_OK; i++) {
        if (i < len && data[i] != '\n') { continue; }
        const uint8_t *line = data + line_start;
        size_t line_len = i - line_start;
        line_start = i + 1;
        if (line_len == 0) { continue; }

        switch (line[0]) {
        case '*':
            cur_table = ft_find_or_create(f, (const char *)line + 1, line_len - 1);
            if (!cur_table) { err = MT_ERR_NOMEM; }
            break;

        case ':': {
            field_span_t *fields = NULL;
            size_t n_fields = 0;
            err = split_fields(line + 1, line_len - 1, &fields, &n_fields);
            if (err != MT_OK) { break; }
            if (n_fields == 0) {
                err = MT_ERR_PROTO;
                free(fields);
                break;
            }
            if (!cur_table) {
                err = MT_ERR_PROTO;
                free(fields);
                break;
            }
            if (!fc_find_or_create(cur_table, (const char *)(line + 1) + fields[0].off,
                                   fields[0].len)) {
                err = MT_ERR_NOMEM;
}
            free(fields);
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
            if (n_fields < 2 || fields[0].len < 2) {
                err = MT_ERR_PROTO;
                free(fields);
                break;
            }
            const char *chain_name = (const char *)line + fields[1].off;
            size_t chain_name_len = fields[1].len;
            uint8_t op = line[fields[0].off + 1];

            switch (op) {
            case 'A': {
                fake_chain_t *c = fc_find_or_create(cur_table, chain_name, chain_name_len);
                if (!c) {
                    err = MT_ERR_NOMEM;
                    break;
                }
                mt_ipt_rule_t *r;
                err = rule_from_fields(line, fields, 2, n_fields, &r);
                if (err == MT_OK) {
                    err = fc_append_owned(c, r);
                    if (err != MT_OK) { mt_ipt_rule_free(r); }
                }
                break;
            }
            case 'I': {
                if (n_fields < 3) {
                    err = MT_ERR_PROTO;
                    break;
                }
                char posbuf[32];
                size_t poslen = fields[2].len < sizeof(posbuf) - 1 ? fields[2].len
                                                                   : sizeof(posbuf) - 1;
                memcpy(posbuf, line + fields[2].off, poslen);
                posbuf[poslen] = '\0';
                char *endp = NULL;
                long pos = strtol(posbuf, &endp, 10);
                if (endp == posbuf || *endp != '\0' || pos < 1) {
                    err = MT_ERR_PROTO;
                    break;
                }
                fake_chain_t *c = fc_find_or_create(cur_table, chain_name, chain_name_len);
                if (!c) {
                    err = MT_ERR_NOMEM;
                    break;
                }
                mt_ipt_rule_t *r;
                err = rule_from_fields(line, fields, 3, n_fields, &r);
                if (err != MT_OK) { break; }

                size_t insert_idx = (size_t)pos - 1;
                if (insert_idx > c->n) { insert_idx = c->n; }

                if (c->n + 1 > c->cap) {
                    size_t newcap = c->cap == 0 ? 4 : c->cap * 2;
                    mt_ipt_rule_t **tmp = realloc(c->rules, newcap * sizeof(*tmp));
                    if (!tmp) {
                        mt_ipt_rule_free(r);
                        err = MT_ERR_NOMEM;
                        break;
                    }
                    c->rules = tmp;
                    c->cap = newcap;
                }
                for (size_t k = c->n; k > insert_idx; k--) { c->rules[k] = c->rules[k - 1]; }
                c->rules[insert_idx] = r;
                c->n++;
                break;
            }
            case 'D': {
                /* chain_name is a slice into `line`, not NUL-terminated,
                 * so this can't use fc_find (strcmp-based); scan by
                 * length like the other builders in this file. */
                fake_chain_t *c = NULL;
                for (size_t ci = 0; ci < cur_table->n_chains; ci++) {
                    if (strlen(cur_table->chains[ci].name) == chain_name_len &&
                        memcmp(cur_table->chains[ci].name, chain_name, chain_name_len) == 0) {
                        c = &cur_table->chains[ci];
                        break;
                    }
                }
                mt_ipt_rule_t *want;
                err = rule_from_fields(line, fields, 2, n_fields, &want);
                if (err != MT_OK) { break; }

                bool found = false;
                if (c) {
                    for (size_t ri = 0; ri < c->n; ri++) {
                        if (!mt_ipt_rule_equal(c->rules[ri], want)) { continue; }
                        mt_ipt_rule_free(c->rules[ri]);
                        for (size_t k = ri; k + 1 < c->n; k++) { c->rules[k] = c->rules[k + 1]; }
                        c->n--;
                        found = true;
                        break;
                    }
                }
                mt_ipt_rule_free(want);
                if (!found) { err = MT_ERR_NOENT; }
                break;
            }
            case 'F': {
                fake_chain_t *c = fc_find_or_create(cur_table, chain_name, chain_name_len);
                if (!c) {
                    err = MT_ERR_NOMEM;
                    break;
                }
                fc_clear(c);
                break;
            }
            case 'X': {
                fake_chain_t *c = NULL;
                size_t idx = 0;
                for (size_t ci = 0; ci < cur_table->n_chains; ci++) {
                    if (strlen(cur_table->chains[ci].name) == chain_name_len &&
                        memcmp(cur_table->chains[ci].name, chain_name, chain_name_len) == 0) {
                        c = &cur_table->chains[ci];
                        idx = ci;
                        break;
                    }
                }
                if (c) {
                    if (c->n != 0) {
                        err = MT_ERR_STATE; /* "cannot delete non-empty chain" */
                        break;
                    }
                    free(c->name);
                    free(c->rules);
                    for (size_t k = idx; k + 1 < cur_table->n_chains; k++) {
                        cur_table->chains[k] = cur_table->chains[k + 1];
}
                    cur_table->n_chains--;
                }
                break;
            }
            default:
                err = MT_ERR_PROTO;
                break;
            }
            free(fields);
            break;
        }

        case '#':
            break;
        case 'C':
            cur_table = NULL;
            break;
        default:
            err = MT_ERR_PROTO;
            break;
        }
    }

    return err;
}

static mt_ipt_proto_t fake_proto(mt_ipt_executable_t *self) {
    return ((mt_fake_ipt_t *)self)->proto;
}

static void fake_clear(mt_fake_ipt_t *f) {
    for (size_t ti = 0; ti < f->n_tables; ti++) {
        fake_table_t *t = &f->tables[ti];
        for (size_t ci = 0; ci < t->n_chains; ci++) {
            fake_chain_t *c = &t->chains[ci];
            for (size_t ri = 0; ri < c->n; ri++) { mt_ipt_rule_free(c->rules[ri]); }
            free(c->rules);
            free(c->name);
        }
        free(t->chains);
        free(t->name);
    }
    free(f->tables);
    f->tables = NULL;
    f->n_tables = 0;
    f->cap_tables = 0;
}

void mt_fake_ipt_reset(mt_fake_ipt_t *f) {
    if (f) { fake_clear(f); }
}

static void fake_destroy(mt_ipt_executable_t *self) {
    mt_fake_ipt_t *f = (mt_fake_ipt_t *)self;
    if (!f) { return; }
    fake_clear(f);
    free(f);
}

static const mt_ipt_executable_ops_t k_fake_ops = {
    .save = fake_save,
    .restore = fake_restore,
    .proto = fake_proto,
    .destroy = fake_destroy,
};

mt_fake_ipt_t *mt_fake_ipt_new(mt_ipt_proto_t proto) {
    mt_fake_ipt_t *f = calloc(1, sizeof(*f));
    if (!f) { return NULL; }
    f->base.ops = &k_fake_ops;
    f->proto = proto;
    return f;
}

mt_ipt_executable_t *mt_fake_ipt_as_executable(mt_fake_ipt_t *f) {
    return &f->base;
}
