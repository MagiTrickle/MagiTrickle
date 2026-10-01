/* iptables save/restore batching engine — port of utils/iptables (Go).
 *
 * Mirrors the Go design exactly: rules are staged into named "chains" of
 * one of three kinds (patch/override/delete), and Commit() asks each
 * registered chain to compile itself against the *current* on-system rule
 * table (parsed from `iptables-save`), then writes one iptables-restore
 * transcript and executes it in a single batch. See chain-patch.c,
 * chain-override.c, chain-delete.c for the per-kind compile semantics
 * (ported byte-for-byte from chain-patch.go/chain-override.go/chain-delete.go).
 *
 * Concurrency: unlike the Go type (which guards every method with a mutex
 * because multiple goroutines share one *IPTables), this engine has no
 * internal locking. Per decisions.md D-17/D-19, all netfilter mutation in
 * the C backend happens on a single thread (the loop thread, or a single
 * dedicated worker thread that owns fork/exec+netlink calls and to which
 * work is marshalled via mt_loop_post) — never concurrently. Callers must
 * not share one mt_ipt_t across threads without external synchronization.
 *
 * Table/chain iteration order: Go stores `rules map[string]map[string]chain`
 * and iterates it in Go's randomized map order every Commit(); this port
 * uses insertion-ordered arrays instead. This is not an observable
 * behavioural difference: within one priority bucket a given chain's own
 * compiled commands always stay contiguous (see engine.c), and the
 * relative order between *different* chains/tables in the restore
 * transcript is inherently insignificant to iptables-restore (distinct
 * named chains/tables are independent; only within-chain command order
 * matters, and that is preserved). See decisions.md D-19.
 */
#ifndef MAGITRICKLE_IPTABLES_H
#define MAGITRICKLE_IPTABLES_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "magitrickle/cancel.h"
#include "magitrickle/err.h"

typedef enum mt_ipt_proto {
    MT_IPT_PROTO_IPV4 = 0,
    MT_IPT_PROTO_IPV6 = 1,
} mt_ipt_proto_t;

/* A rule is an ordered list of argv-style string parts, e.g.
 * {"-o", "eth0", "-m", "set", "--match-set", "mt_g1_4", "dst", "-j", "ACCEPT"}.
 * All parts are owned (strdup'd) copies. */
typedef struct mt_ipt_rule {
    char **parts;
    size_t n_parts;
} mt_ipt_rule_t;

/* Deep-copies args[0..n_args). Returns NULL on OOM. */
mt_ipt_rule_t *mt_ipt_rule_new(const char *const *args, size_t n_args);
mt_ipt_rule_t *mt_ipt_rule_clone(const mt_ipt_rule_t *r);
void mt_ipt_rule_free(mt_ipt_rule_t *r);
bool mt_ipt_rule_equal(const mt_ipt_rule_t *a, const mt_ipt_rule_t *b);
/* Space-joined "part1 part2 ...". Caller frees. NULL on OOM. */
char *mt_ipt_rule_string(const mt_ipt_rule_t *r);
bool mt_ipt_rule_contains(const mt_ipt_rule_t *r, const char *substr);

/* ---- Executable backend (Save/Restore transport) ---------------------- */

typedef struct mt_ipt_executable mt_ipt_executable_t;

typedef struct mt_ipt_executable_ops {
    /* *out is malloc'd, *out_len its length (not NUL-terminated). */
    mt_err_t (*save)(mt_ipt_executable_t *self, uint8_t **out, size_t *out_len);
    mt_err_t (*restore)(mt_ipt_executable_t *self, const uint8_t *data, size_t len);
    mt_ipt_proto_t (*proto)(mt_ipt_executable_t *self);
    void (*destroy)(mt_ipt_executable_t *self);
    /* Optional (may be NULL): hands the backend a token to abort an
     * in-flight transfer on. The real backend polls it alongside the
     * child's pipes and kills the child when it is raised; backends with
     * nothing to interrupt (the test fake) leave this NULL. */
    void (*set_cancel)(mt_ipt_executable_t *self, mt_cancel_t *cancel);
} mt_ipt_executable_ops_t;

struct mt_ipt_executable {
    const mt_ipt_executable_ops_t *ops;
};

/* Real backend: forks iptables-save / iptables-restore --noflush (or the
 * ip6tables-* variants for MT_IPT_PROTO_IPV6). Uses execvp with a fixed
 * argv array — no shell involved, no command injection surface. */
mt_ipt_executable_t *mt_ipt_executable_real_new(mt_ipt_proto_t proto);

static inline void mt_ipt_executable_free(mt_ipt_executable_t *exe) {
    if (exe) { exe->ops->destroy(exe); }
}

/* ---- Chain (patch / override / delete) --------------------------------- */

typedef enum mt_ipt_option {
    MT_IPT_OP_APPEND = 0,
    MT_IPT_OP_DELETE,
    MT_IPT_OP_INSERT,
    MT_IPT_OP_FLUSH,
    MT_IPT_OP_DELETE_CHAIN,
} mt_ipt_option_t;

typedef struct mt_ipt_command {
    mt_ipt_option_t option;
    char *chain;        /* owned copy of the chain name */
    int rule_num;       /* only meaningful for MT_IPT_OP_INSERT */
    mt_ipt_rule_t *rule; /* owned; NULL for FLUSH/DELETE_CHAIN */
} mt_ipt_command_t;

void mt_ipt_command_list_free(mt_ipt_command_t *cmds, size_t n);

typedef struct mt_ipt_chain mt_ipt_chain_t;

typedef struct mt_ipt_chain_ops {
    /* existing: rules currently on the system for this chain (from
     * iptables-save), or existing==NULL/n_existing==0 meaning the chain
     * does not exist yet. On success, out_cmds/out_n describe the
     * commands to emit (caller frees via mt_ipt_command_list_free);
     * *out_priority matches Go's per-kind constant (patch=0, override=-128,
     * delete=127) and decides emission order across chains in Commit(). */
    mt_err_t (*compile)(mt_ipt_chain_t *self, const char *chain_name,
                         mt_ipt_rule_t *const *existing, size_t n_existing,
                         mt_ipt_command_t **out_cmds, size_t *out_n,
                         int8_t *out_priority);
    mt_err_t (*append)(mt_ipt_chain_t *self, const mt_ipt_rule_t *rule);
    mt_err_t (*insert)(mt_ipt_chain_t *self, int rule_num, const mt_ipt_rule_t *rule);
    mt_err_t (*remove)(mt_ipt_chain_t *self, const mt_ipt_rule_t *rule); /* "Delete" in Go */
    void (*destroy)(mt_ipt_chain_t *self);
} mt_ipt_chain_ops_t;

struct mt_ipt_chain {
    const mt_ipt_chain_ops_t *ops;
};

mt_ipt_chain_t *mt_ipt_chain_patch_new(void);
mt_ipt_chain_t *mt_ipt_chain_override_new(void);
mt_ipt_chain_t *mt_ipt_chain_delete_new(void);

/* ---- IPTables engine ---------------------------------------------------- */

typedef struct mt_ipt mt_ipt_t;

/* Takes ownership of exe (freed by mt_ipt_free). */
mt_ipt_t *mt_ipt_new(mt_ipt_executable_t *exe);
void mt_ipt_free(mt_ipt_t *ipt);
mt_ipt_proto_t mt_ipt_proto(const mt_ipt_t *ipt);

/* Attaches a cancellation token (borrowed, nullable to detach), making
 * commits abortable: mt_ipt_commit checks it before reading the current
 * table and again before writing, and the executable backend kills an
 * already-running iptables-save/restore when it is raised. An aborted
 * commit returns MT_ERR_CANCELED and leaves the staged rules untouched,
 * so the caller can simply build again from scratch. */
void mt_ipt_set_cancel(mt_ipt_t *ipt, mt_cancel_t *cancel);

/* Registering a table/chain twice replaces the previous registration
 * (matches Go: rules[table][chainName] = &chainX{}), discarding any
 * staged-but-uncommitted rules for the old registration. */
mt_err_t mt_ipt_register_chain_delete(mt_ipt_t *ipt, const char *table, const char *chain);
mt_err_t mt_ipt_register_chain_patch(mt_ipt_t *ipt, const char *table, const char *chain);
mt_err_t mt_ipt_register_chain_override(mt_ipt_t *ipt, const char *table, const char *chain);

/* MT_ERR_STATE (mirrors Go's ErrChainNotInitialized) when table/chain was
 * never registered via one of the RegisterChain* calls above. */
mt_err_t mt_ipt_append(mt_ipt_t *ipt, const char *table, const char *chain,
                       const char *const *args, size_t n_args);
mt_err_t mt_ipt_insert(mt_ipt_t *ipt, const char *table, const char *chain,
                       int rule_num, const char *const *args, size_t n_args);
mt_err_t mt_ipt_delete(mt_ipt_t *ipt, const char *table, const char *chain,
                       const char *const *args, size_t n_args);
/* True exactly when the most recent Append/Insert/Delete call above
 * returned MT_ERR_STATE for "chain not initialized" (vs. some other
 * MT_ERR_STATE-shaped failure) -- lets callers replicate Go's
 * errors.Is(err, ErrChainNotInitialized) check (iptables-cleaner.go). */
bool mt_ipt_err_is_chain_not_initialized(mt_err_t err);

/* ---- GetCurrentRules (Save() output, parsed) --------------------------- */

typedef struct mt_ipt_chain_rules {
    char *chain_name;
    mt_ipt_rule_t **rules; /* NULL/0 = chain declared but empty; the chain
                             * being ABSENT from a table is represented by
                             * it simply not appearing in .chains[] at all
                             * (mirrors Go's curTable[chainName]==nil vs
                             * curTable[chainName]==[]Rule{}). */
    size_t n_rules;
} mt_ipt_chain_rules_t;

typedef struct mt_ipt_table_rules {
    char *table_name;
    mt_ipt_chain_rules_t *chains;
    size_t n_chains;
} mt_ipt_table_rules_t;

typedef struct mt_ipt_rules_snapshot {
    mt_ipt_table_rules_t *tables;
    size_t n_tables;
} mt_ipt_rules_snapshot_t;

/* Finds a table by name, or NULL. */
const mt_ipt_table_rules_t *mt_ipt_rules_snapshot_find_table(
    const mt_ipt_rules_snapshot_t *snap, const char *table_name);
/* Finds a chain within a table, or NULL if the chain was never declared
 * (distinct from a declared-but-empty chain, which returns non-NULL with
 * n_rules==0). */
const mt_ipt_chain_rules_t *mt_ipt_table_rules_find_chain(
    const mt_ipt_table_rules_t *table, const char *chain_name);

mt_err_t mt_ipt_get_current_rules(mt_ipt_t *ipt, mt_ipt_rules_snapshot_t **out);
void mt_ipt_rules_snapshot_free(mt_ipt_rules_snapshot_t *snap);

/* Builds the restore transcript from all registered chains' Compile()
 * output (grouped and ordered by priority, see engine.c) and executes it
 * via the Executable backend. No-op (does not even call Save/Restore) if
 * every chain compiles to zero commands, matching Go's `buf.Len() == 0`
 * early return.
 *
 * MT_ERR_CANCELED when an attached cancellation token was raised (see
 * mt_ipt_set_cancel); MT_ERR_AGAIN when the write failed because someone
 * else changed the table underneath us (see executable_real.c) -- both
 * mean "nothing durable happened, build it again", not "give up". */
mt_err_t mt_ipt_commit(mt_ipt_t *ipt);

#endif /* MAGITRICKLE_IPTABLES_H */
