/* Regression for the 2026-10-07 MT_DNSOR retry loop.
 *
 * The ordinary fake accepts a jump to a missing chain and deletion of a
 * referenced chain. Validate candidate state BEFORE applying a fake restore:
 * otherwise a broken intermediate cleanup can look like successful recovery.
 * The same tests can use the real save/restore backend in a disposable network
 * namespace (MT_TEST_REAL_IPTABLES=1); CI runs both legacy and nft frontends.
 */
#include "greatest.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fake_iptables.h"
#include "magitrickle/app.h"
#include "magitrickle/netfilter_cleaner.h"
#include "magitrickle/nfcommit.h"

/* Faults apply to one selected call, never to all future attempts. */
typedef struct {
    mt_ipt_executable_t base;
    mt_ipt_executable_t *inner;
    mt_cancel_t *cancel;
    const char *prefix;
    unsigned saves, restores, invalid_batches;
    unsigned fail_save, fail_restore, cancel_after_restore;
    mt_err_t failure;
    bool real;
} gate_t;

static const char *jump_target(const mt_ipt_rule_t *rule) {
    for (size_t i = 0; i + 1 < rule->n_parts; i++) {
        const char *p = rule->parts[i];
        if (strcmp(p, "-j") == 0 || strcmp(p, "--jump") == 0 ||
            strcmp(p, "-g") == 0 || strcmp(p, "--goto") == 0) {
            return rule->parts[i + 1];
        }
    }
    return NULL;
}

static mt_err_t validate_references(mt_ipt_t *view, const char *prefix) {
    mt_ipt_rules_snapshot_t *snap = NULL;
    mt_err_t err = mt_ipt_get_current_rules(view, &snap);
    if (err != MT_OK) { return err; }
    for (size_t ti = 0; ti < snap->n_tables && err == MT_OK; ti++) {
        const mt_ipt_table_rules_t *table = &snap->tables[ti];
        for (size_t ci = 0; ci < table->n_chains && err == MT_OK; ci++) {
            const mt_ipt_chain_rules_t *chain = &table->chains[ci];
            for (size_t ri = 0; ri < chain->n_rules; ri++) {
                const char *target = jump_target(chain->rules[ri]);
                if (target && strncmp(target, prefix, strlen(prefix)) == 0 &&
                    !mt_ipt_table_rules_find_chain(table, target)) {
                    fprintf(stderr, "invalid restore: %s/%s -> missing %s\n",
                            table->table_name, chain->chain_name, target);
                    err = MT_ERR_AGAIN;
                    break;
                }
            }
        }
    }
    mt_ipt_rules_snapshot_free(snap);
    return err;
}

static mt_err_t checked_fake_restore(gate_t *g, const uint8_t *data, size_t len) {
    mt_fake_ipt_t *candidate = mt_fake_ipt_new(g->inner->ops->proto(g->inner));
    if (!candidate) { return MT_ERR_NOMEM; }
    mt_ipt_executable_t *exe = mt_fake_ipt_as_executable(candidate);
    mt_ipt_t *view = mt_ipt_new(exe);
    if (!view) { mt_ipt_executable_free(exe); return MT_ERR_NOMEM; }
    uint8_t *saved = NULL;
    size_t saved_len = 0;
    mt_err_t err = g->inner->ops->save(g->inner, &saved, &saved_len);
    if (err == MT_OK) { err = exe->ops->restore(exe, saved, saved_len); }
    free(saved);
    if (err == MT_OK) { err = exe->ops->restore(exe, data, len); }
    if (err == MT_OK) { err = validate_references(view, g->prefix); }
    mt_ipt_free(view);
    if (err != MT_OK) { g->invalid_batches++; return err; }
    /* The checked fake models one atomic restore. Actual per-table commit
     * ordering/partial application is additionally checked by the real backend. */
    return g->inner->ops->restore(g->inner, data, len);
}

static mt_err_t gate_save(mt_ipt_executable_t *self, uint8_t **out, size_t *len) {
    gate_t *g = (gate_t *)self;
    if (++g->saves == g->fail_save) { return g->failure; }
    return g->inner->ops->save(g->inner, out, len);
}

static mt_err_t gate_restore(mt_ipt_executable_t *self, const uint8_t *data, size_t len) {
    gate_t *g = (gate_t *)self;
    if (++g->restores == g->fail_restore) { return g->failure; }
    mt_err_t err = g->real ? g->inner->ops->restore(g->inner, data, len)
                         : checked_fake_restore(g, data, len);
    if (err == MT_OK && g->restores == g->cancel_after_restore) {
        mt_cancel_raise(g->cancel);
    }
    return err;
}

static mt_ipt_proto_t gate_proto(mt_ipt_executable_t *self) {
    gate_t *g = (gate_t *)self;
    return g->inner->ops->proto(g->inner);
}

static void gate_cancel(mt_ipt_executable_t *self, mt_cancel_t *cancel) {
    gate_t *g = (gate_t *)self;
    g->cancel = cancel;
    if (g->inner->ops->set_cancel) { g->inner->ops->set_cancel(g->inner, cancel); }
}

static void gate_destroy(mt_ipt_executable_t *self) {
    gate_t *g = (gate_t *)self;
    mt_ipt_executable_free(g->inner);
    free(g);
}

static const mt_ipt_executable_ops_t gate_ops = {
    .save = gate_save, .restore = gate_restore, .proto = gate_proto,
    .destroy = gate_destroy, .set_cancel = gate_cancel,
};

static bool real_backend;
static struct {
    mt_config_t cfg;
    mt_cache_t *cache;
    mt_app_t *app;
    mt_port_remap_t *remap;
    gate_t *g[2];
    mt_ipt_t *ipt[2];
    const char *prefix;
    char name[32];
} fx;

static mt_err_t raw_restore(gate_t *g, const char *text) {
    return g->inner->ops->restore(g->inner, (const uint8_t *)text, strlen(text));
}

static void setup(unsigned families, const char *prefix) {
    memset(&fx, 0, sizeof(fx));
    fx.prefix = prefix;
    snprintf(fx.name, sizeof(fx.name), "%sDNSOR", prefix);
    assert(mt_config_init_defaults(&fx.cfg) == MT_OK);
    free(fx.cfg.app.netfilter.iptables.chain_prefix);
    fx.cfg.app.netfilter.iptables.chain_prefix = strdup(prefix);
    assert(fx.cfg.app.netfilter.iptables.chain_prefix);
    fx.cache = mt_cache_create(0);
    assert(fx.cache);

    for (unsigned i = 0; i < 2; i++) {
        if ((families & (1u << i)) == 0) { continue; }
        /* main() refuses real mode in the host's network namespace. */
        if (real_backend) {
            assert(system(i == 0 ? "iptables -t nat -F && iptables -t nat -X"
                                 : "ip6tables -t nat -F && ip6tables -t nat -X") == 0);
        }
        gate_t *g = calloc(1, sizeof(*g));
        assert(g);
        g->base.ops = &gate_ops;
        g->prefix = prefix;
        g->real = real_backend;
        g->failure = MT_ERR_AGAIN;
        mt_ipt_proto_t proto = i == 0 ? MT_IPT_PROTO_IPV4 : MT_IPT_PROTO_IPV6;
        if (real_backend) {
            g->inner = mt_ipt_executable_real_new(proto);
        } else {
            mt_fake_ipt_t *f = mt_fake_ipt_new(proto);
            assert(f);
            g->inner = mt_fake_ipt_as_executable(f);
        }
        assert(g->inner);
        fx.g[i] = g;
        fx.ipt[i] = mt_ipt_new(&g->base);
        assert(fx.ipt[i]);
        assert(raw_restore(g, "*nat\n:PREROUTING - [0:0]\n:FOREIGN_KEEP - [0:0]\n"
                              "-A FOREIGN_KEEP -j RETURN\n"
                              "-A PREROUTING -p udp --dport 65000 -j FOREIGN_KEEP\nCOMMIT\n") == MT_OK);
    }
    assert(mt_netfilter_register_base_chains(fx.ipt[0], fx.ipt[1]) == MT_OK);
    mt_app_deps_t deps = {.cfg = &fx.cfg, .cache = fx.cache,
                          .ipt4 = fx.ipt[0], .ipt6 = fx.ipt[1]};
    fx.app = mt_app_create(&deps);
    assert(fx.app);
    const mt_remap_addr_t addresses[] = {
        {.family = AF_INET, .ip = {192, 0, 2, 1}, .iplen = 4},
        {.family = AF_INET6, .ip = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0, 1}, .iplen = 16},
    };
    fx.remap = mt_port_remap_new(prefix, 53, 3553, addresses, 2, fx.ipt[0], fx.ipt[1]);
    assert(fx.remap);
    assert(mt_port_remap_enable(fx.remap) == MT_OK);
    mt_app_set_port_remap(fx.app, fx.remap);
}

static void teardown(void *unused) {
    (void)unused;
    if (fx.remap) { (void)mt_port_remap_disable(fx.remap); }
    mt_app_destroy(fx.app);
    mt_port_remap_free(fx.remap);
    for (unsigned i = 0; i < 2; i++) { mt_ipt_free(fx.ipt[i]); }
    mt_cache_destroy(fx.cache);
    mt_config_clear(&fx.cfg);
    memset(&fx, 0, sizeof(fx));
}

/* Coherent firmware states only: never delete a chain while retaining a jump
 * into it. A missing jump with a surviving chain is enough to trigger the bug. */
static mt_err_t remove_jump(unsigned family, bool remove_chain) {
    char text[256];
    if (remove_chain) {
        snprintf(text, sizeof(text), "*nat\n-D PREROUTING -j %s\n-F %s\n-X %s\nCOMMIT\n",
                 fx.name, fx.name, fx.name);
    } else {
        snprintf(text, sizeof(text), "*nat\n-D PREROUTING -j %s\nCOMMIT\n", fx.name);
    }
    return raw_restore(fx.g[family], text);
}

static bool has_arg(const mt_ipt_rule_t *rule, const char *arg) {
    for (size_t i = 0; i < rule->n_parts; i++) {
        if (strcmp(rule->parts[i], arg) == 0) { return true; }
    }
    return false;
}

TEST assert_recovered(void) {
    for (unsigned i = 0; i < 2; i++) {
        if (!fx.ipt[i]) { continue; }
        mt_ipt_rules_snapshot_t *snap = NULL;
        ASSERT_EQ(MT_OK, mt_ipt_get_current_rules(fx.ipt[i], &snap));
        const mt_ipt_table_rules_t *nat = mt_ipt_rules_snapshot_find_table(snap, "nat");
        const mt_ipt_chain_rules_t *dns = mt_ipt_table_rules_find_chain(nat, fx.name);
        const mt_ipt_chain_rules_t *pre = mt_ipt_table_rules_find_chain(nat, "PREROUTING");
        const mt_ipt_chain_rules_t *foreign = mt_ipt_table_rules_find_chain(nat, "FOREIGN_KEEP");
        bool good = dns && dns->n_rules == 2 && pre && foreign && foreign->n_rules == 1;
        size_t ours = 0, theirs = 0;
        if (pre) {
            for (size_t r = 0; r < pre->n_rules; r++) {
                const char *target = jump_target(pre->rules[r]);
                if (!target) { continue; }
                if (strcmp(target, fx.name) == 0) { ours++; }
                if (strcmp(target, "FOREIGN_KEEP") == 0) { theirs++; }
            }
        }
        if (dns) {
            for (size_t r = 0; r < dns->n_rules; r++) {
                good = good && has_arg(dns->rules[r], "53") &&
                       has_arg(dns->rules[r], ":3553") && has_arg(dns->rules[r], "DNAT");
            }
        }
        if (foreign && foreign->n_rules == 1) { good = good && has_arg(foreign->rules[0], "RETURN"); }
        mt_ipt_rules_snapshot_free(snap);
        ASSERT(good);
        ASSERT_EQ(1u, ours);
        ASSERT_EQ(1u, theirs);
        ASSERT_EQ(0u, fx.g[i]->invalid_batches);
    }
    PASS();
}

TEST missing_jump(unsigned family) {
    setup(1u << family, "MT_");
    ASSERT_EQ(MT_OK, remove_jump(family, false));
    ASSERT_EQ(MT_OK, mt_app_rebuild_netfilter(fx.app, NULL));
    return assert_recovered();
}

/* Exact observed shape: cleanup leaves a DELETE registration, then firmware
 * removes the chain too. The next attempt must not emit an orphan MT_DNSOR jump. */
TEST failed_cleanup_then_firmware_wipe(unsigned family) {
    setup(1u << family, "MT_");
    ASSERT_EQ(MT_OK, remove_jump(family, false));
    fx.g[family]->fail_restore = fx.g[family]->restores + 1;
    ASSERT_EQ(MT_ERR_AGAIN, mt_app_rebuild_netfilter(fx.app, NULL));
    char text[160];
    snprintf(text, sizeof(text), "*nat\n-F %s\n-X %s\nCOMMIT\n", fx.name, fx.name);
    ASSERT_EQ(MT_OK, raw_restore(fx.g[family], text));
    ASSERT_EQ(MT_OK, mt_app_rebuild_netfilter(fx.app, NULL));
    return assert_recovered();
}

TEST repeated_recovery_and_foreign_rules(void) {
    setup(3, "MT_");
    for (unsigned n = 0; n < 8; n++) {
        ASSERT_EQ(MT_OK, remove_jump(0, (n % 2) != 0));
        ASSERT_EQ(MT_OK, remove_jump(1, (n % 2) == 0));
        ASSERT_EQ(MT_OK, mt_app_rebuild_netfilter(fx.app, NULL));
        ASSERT_EQ(GREATEST_TEST_RES_PASS, assert_recovered());
    }
    return assert_recovered();
}

TEST retry_after_apply_failure(unsigned family) {
    setup(3, "MT_");
    /* First restore cleans; the second writes desired state. For IPv6 this
     * also exercises IPv4 having committed while IPv6 has not. */
    fx.g[family]->failure = MT_ERR_IO;
    fx.g[family]->fail_restore = fx.g[family]->restores + 2;
    ASSERT_EQ(MT_ERR_IO, mt_app_rebuild_netfilter(fx.app, NULL));
    ASSERT_EQ(MT_OK, mt_app_rebuild_netfilter(fx.app, NULL));
    return assert_recovered();
}

TEST retry_after_snapshot_failure(void) {
    setup(3, "MT_");
    fx.g[1]->failure = MT_ERR_IO;
    fx.g[1]->fail_save = fx.g[1]->saves + 1;
    ASSERT_EQ(MT_ERR_IO, mt_app_rebuild_netfilter(fx.app, NULL));
    ASSERT_EQ(MT_OK, mt_app_rebuild_netfilter(fx.app, NULL));
    return assert_recovered();
}

TEST cancel_after_cleanup_then_retry(void) {
    setup(3, "MT_");
    mt_cancel_t *cancel = mt_cancel_new();
    ASSERT(cancel);
    fx.g[0]->cancel_after_restore = fx.g[0]->restores + 1;
    mt_err_t aborted = mt_app_rebuild_netfilter(fx.app, cancel);
    bool raised = mt_cancel_raised(cancel);
    mt_cancel_clear(cancel);
    mt_err_t retried = mt_app_rebuild_netfilter(fx.app, cancel);
    mt_cancel_free(cancel);
    ASSERT_EQ(MT_ERR_CANCELED, aborted);
    ASSERT(raised);
    ASSERT_EQ(MT_OK, retried);
    return assert_recovered();
}

TEST custom_prefix_recovery(void) {
    setup(3, "XY_");
    ASSERT_EQ(MT_OK, remove_jump(0, false));
    ASSERT_EQ(MT_OK, remove_jump(1, false));
    ASSERT_EQ(MT_OK, mt_app_rebuild_netfilter(fx.app, NULL));
    return assert_recovered();
}

typedef struct {
    mt_app_t *app;
    atomic_bool done;
} completion_t;

static mt_err_t rebuild_callback(void *ud, mt_cancel_t *cancel) {
    completion_t *c = ud;
    mt_err_t err = mt_app_rebuild_netfilter(c->app, cancel);
    if (err == MT_OK) { atomic_store(&c->done, true); }
    return err;
}

TEST event_burst_eventually_recovers(void) {
    setup(3, "MT_");
    ASSERT_EQ(MT_OK, remove_jump(0, false));
    fx.g[0]->fail_restore = fx.g[0]->restores + 1;
    completion_t c = {.app = fx.app};
    atomic_init(&c.done, false);
    mt_nfcommit_t *committer = mt_nfcommit_new(rebuild_callback, &c);
    ASSERT(committer);
    mt_nfcommit_set_delays_for_test(committer, 1, 4);
    mt_err_t started = mt_nfcommit_start(committer);
    if (started == MT_OK) {
        for (unsigned i = 0; i < 64; i++) { mt_nfcommit_request(committer); }
        for (unsigned i = 0; i < 1600 && !atomic_load(&c.done); i++) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 5000000L};
            nanosleep(&delay, NULL);
        }
    }
    mt_nfcommit_free(committer); /* join before inspecting the backend */
    ASSERT_EQ(MT_OK, started);
    ASSERT(atomic_load(&c.done));
    return assert_recovered();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    real_backend = getenv("MT_TEST_REAL_IPTABLES") != NULL;
    if (real_backend) {
        struct stat self, init;
        if (geteuid() != 0 || stat("/proc/self/ns/net", &self) != 0 ||
            stat("/proc/1/ns/net", &init) != 0 ||
            (self.st_dev == init.st_dev && self.st_ino == init.st_ino)) {
            fputs("Refusing real firewall tests outside a separate root network namespace.\n", stderr);
            return 2;
        }
    }
    GREATEST_MAIN_BEGIN();
    SET_TEARDOWN(teardown, NULL);
    RUN_TESTp(missing_jump, 0);
    RUN_TESTp(missing_jump, 1);
    RUN_TESTp(failed_cleanup_then_firmware_wipe, 0);
    RUN_TESTp(failed_cleanup_then_firmware_wipe, 1);
    RUN_TEST(repeated_recovery_and_foreign_rules);
    RUN_TESTp(retry_after_apply_failure, 0);
    RUN_TESTp(retry_after_apply_failure, 1);
    RUN_TEST(retry_after_snapshot_failure);
    RUN_TEST(cancel_after_cleanup_then_retry);
    RUN_TEST(custom_prefix_recovery);
    RUN_TEST(event_burst_eventually_recovers);
    GREATEST_MAIN_END();
}
