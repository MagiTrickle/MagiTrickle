/* Regression for f7d4ecd: an owned override vanished after Save(), so the
 * next Restore() tried -F MT_DNSOR without declaring it first. This is a
 * rootless transport test, not a claim of testing Keenetic's kernel.
 * Unlike fake_iptables.c, this transport rejects -F/-A on missing chains
 * and implements the implicit flush caused by a :CHAIN declaration.
 */
#include "magitrickle/iptables.h"
#include "magitrickle/bytebuf.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RULES_CAP 8u
#define RULE_CAP 256u

typedef struct probe {
    mt_ipt_executable_t base;
    mt_ipt_proto_t proto;
    const char *name;
    bool exists, disappear_after_save;
    unsigned saves, restores, declarations, injected;
    mt_err_t restore_error;
    char rules[RULES_CAP][RULE_CAP];
    size_t count;
} probe_t;

static mt_err_t probe_save(mt_ipt_executable_t *self, uint8_t **out, size_t *len)
{
    probe_t *p = (probe_t *)self;
    mt_bytebuf_t b;
    mt_bytebuf_init(&b);
    mt_err_t err = mt_bytebuf_append_str(&b, "*nat\n");
    char line[RULE_CAP + 80u];
    if (p->exists && err == MT_OK) {
        (void)snprintf(line, sizeof(line), ":%s - [0:0]\n", p->name);
        err = mt_bytebuf_append_str(&b, line);
        for (size_t i = 0; i < p->count && err == MT_OK; i++) {
            (void)snprintf(line, sizeof(line), "-A %s %s\n", p->name, p->rules[i]);
            err = mt_bytebuf_append_str(&b, line);
        }
    }
    if (err == MT_OK) { err = mt_bytebuf_append_str(&b, "COMMIT\n"); }
    if (err != MT_OK) { mt_bytebuf_free(&b); return err; }
    *out = b.data;
    *len = b.len;
    p->saves++;
    /* Deterministic concurrent firmware update AFTER taking the snapshot. */
    if (p->disappear_after_save) {
        p->exists = false;
        p->count = 0;
        p->disappear_after_save = false;
        p->injected++;
    }
    return MT_OK;
}

static mt_err_t probe_restore(mt_ipt_executable_t *self, const uint8_t *data, size_t len)
{
    probe_t *p = (probe_t *)self;
    p->restores++;
    if (p->restore_error != MT_OK) { return p->restore_error; }
    char *script = malloc(len + 1u);
    if (!script) { return MT_ERR_NOMEM; }
    memcpy(script, data, len);
    script[len] = '\0';
    mt_err_t err = MT_OK;
    char declaration[96];
    (void)snprintf(declaration, sizeof(declaration), ":%s - [0:0]", p->name);
    char *state = NULL;
    for (char *line = strtok_r(script, "\n", &state); line;
         line = strtok_r(NULL, "\n", &state)) {
        if (strcmp(line, "*nat") == 0 || strcmp(line, "COMMIT") == 0) { continue; }
        if (strcmp(line, declaration) == 0) {
            p->exists = true;
            p->count = 0; /* --noflush does not protect re-declared user chains. */
            p->declarations++;
            continue;
        }
        const size_t name_len = strlen(p->name);
        if (strlen(line) < 3u + name_len || line[0] != '-' || line[2] != ' ' ||
            strncmp(line + 3, p->name, name_len) != 0 ||
            (line[3u + name_len] != '\0' && line[3u + name_len] != ' ')) {
            err = MT_ERR_PROTO;
            break;
        }
        if (!p->exists) {
            fprintf(stderr, "restore rejected: missing chain for %s\n", line);
            err = MT_ERR_NOENT;
            break;
        }
        char *rule = line + 3u + name_len;
        if (*rule == ' ') { rule++; }
        switch (line[1]) {
        case 'F': p->count = 0; break;
        case 'X':
            if (p->count) { err = MT_ERR_STATE; }
            else { p->exists = false; }
            break;
        case 'A':
            if (p->count == RULES_CAP || strlen(rule) >= RULE_CAP) { err = MT_ERR_LIMIT; }
            else { strcpy(p->rules[p->count++], rule); }
            break;
        case 'D': {
            size_t i = 0;
            while (i < p->count && strcmp(p->rules[i], rule) != 0) { i++; }
            if (i == p->count) { err = MT_ERR_NOENT; break; }
            for (; i + 1u < p->count; i++) { memcpy(p->rules[i], p->rules[i + 1u], RULE_CAP); }
            p->count--;
            break;
        }
        default: err = MT_ERR_PROTO; break;
        }
        if (err != MT_OK) { break; }
    }
    free(script);
    return err;
}

static mt_ipt_proto_t probe_proto(mt_ipt_executable_t *self)
{
    return ((probe_t *)self)->proto;
}

static void probe_destroy(mt_ipt_executable_t *self) { free(self); }

static const mt_ipt_executable_ops_t ops = {
    .save = probe_save, .restore = probe_restore, .proto = probe_proto,
    .destroy = probe_destroy,
};

typedef enum scenario {
    DISAPPEARED_OVERRIDE, ABSENT_OVERRIDE, UNCHANGED_OVERRIDE, EXISTING_PATCH,
    OVERRIDE_TO_PATCH, PATCH_TO_OVERRIDE, OVERRIDE_TO_DELETE, RESTORE_ERROR,
} scenario_t;

static bool run_case(scenario_t scenario, mt_ipt_proto_t proto)
{
    probe_t *p = calloc(1, sizeof(*p));
    if (!p) { return false; }
    p->base.ops = &ops;
    p->proto = proto;
    /* Metadata, not the MT_ prefix, decides whether a chain is fully owned. */
    p->name = proto == MT_IPT_PROTO_IPV4 ? "MT_DNSOR" : "XY_DNSOR";
    p->exists = true;
    strcpy(p->rules[0], "-j RETURN");
    strcpy(p->rules[1], "-j DROP");
    p->count = 2;
    mt_ipt_t *ipt = mt_ipt_new(&p->base);
    if (!ipt) { free(p); return false; }
    const char *keep[] = {"-j", "RETURN"}, *remove[] = {"-j", "DROP"};
    bool ok = true;
#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL case=%d proto=%d line=%d: %s\n", \
                (int)scenario, (int)proto, __LINE__, #expression); \
        ok = false; goto done; \
    } \
} while (0)

    if (scenario == EXISTING_PATCH || scenario == OVERRIDE_TO_PATCH) {
        if (scenario == OVERRIDE_TO_PATCH) {
            CHECK(mt_ipt_register_chain_override(ipt, "nat", p->name) == MT_OK);
        }
        CHECK(mt_ipt_register_chain_patch(ipt, "nat", p->name) == MT_OK);
        CHECK(mt_ipt_delete(ipt, "nat", p->name, remove, 2) == MT_OK);
    } else if (scenario == OVERRIDE_TO_DELETE) {
        CHECK(mt_ipt_register_chain_override(ipt, "nat", p->name) == MT_OK);
        CHECK(mt_ipt_register_chain_delete(ipt, "nat", p->name) == MT_OK);
    } else {
        if (scenario == PATCH_TO_OVERRIDE) {
            CHECK(mt_ipt_register_chain_patch(ipt, "nat", p->name) == MT_OK);
        }
        CHECK(mt_ipt_register_chain_override(ipt, "nat", p->name) == MT_OK);
        CHECK(mt_ipt_append(ipt, "nat", p->name, keep, 2) == MT_OK);
        p->disappear_after_save = scenario == DISAPPEARED_OVERRIDE || scenario == PATCH_TO_OVERRIDE;
        if (scenario == ABSENT_OVERRIDE) { p->exists = false; p->count = 0; }
        if (scenario == UNCHANGED_OVERRIDE) { p->count = 1; }
    }
    if (scenario == RESTORE_ERROR) {
        p->restore_error = MT_ERR_IO;
        CHECK(mt_ipt_commit(ipt) == MT_ERR_IO);
        CHECK(p->restores == 1);
        goto done;
    }
    CHECK(mt_ipt_commit(ipt) == MT_OK);
    CHECK(p->saves == 1);
    if (scenario == OVERRIDE_TO_DELETE) {
        CHECK(!p->exists && p->declarations == 0);
    } else {
        CHECK(p->exists && p->count == 1 && strcmp(p->rules[0], "-j RETURN") == 0);
        if (scenario == UNCHANGED_OVERRIDE) {
            CHECK(p->restores == 0);
        } else if (scenario == EXISTING_PATCH || scenario == OVERRIDE_TO_PATCH) {
            CHECK(p->declarations == 0);
        } else {
            CHECK(p->declarations == 1);
        }
        if (scenario == DISAPPEARED_OVERRIDE || scenario == PATCH_TO_OVERRIDE) {
            CHECK(p->injected == 1);
            unsigned writes = p->restores;
            CHECK(mt_ipt_commit(ipt) == MT_OK);
            CHECK(p->restores == writes); /* Stable state remains a no-op. */
        }
    }
done:
    mt_ipt_free(ipt);
#undef CHECK
    return ok;
}

int main(void)
{
    unsigned passed = 0, failed = 0;
    for (int proto = MT_IPT_PROTO_IPV4; proto <= MT_IPT_PROTO_IPV6; proto++) {
        for (int test = DISAPPEARED_OVERRIDE; test <= RESTORE_ERROR; test++) {
            bool ok = run_case((scenario_t)test, (mt_ipt_proto_t)proto);
            printf("%s case=%d proto=%d\n", ok ? "PASS" : "FAIL", test, proto);
            if (ok) { passed++; } else { failed++; }
        }
    }
    printf("Restore snapshot race: %u passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
