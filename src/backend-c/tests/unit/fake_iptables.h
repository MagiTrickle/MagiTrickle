/* Test-only in-memory Executable backend — port of
 * utils/iptables/executable-fake.go (the `//go:build testing` file). Not
 * part of the production binary; only test binaries link fake_iptables.c
 * (see Makefile TEST_SUPPORT_SRCS).
 */
#ifndef MT_TEST_FAKE_IPTABLES_H
#define MT_TEST_FAKE_IPTABLES_H

#include <stdbool.h>
#include <stddef.h>

#include "magitrickle/iptables.h"

typedef struct mt_fake_ipt mt_fake_ipt_t;

mt_fake_ipt_t *mt_fake_ipt_new(mt_ipt_proto_t proto);
/* mt_ipt_new()/mt_ipt_free() takes ownership from here on -- do not call
 * mt_fake_ipt_* accessors after the owning mt_ipt_t is freed. */
mt_ipt_executable_t *mt_fake_ipt_as_executable(mt_fake_ipt_t *f);

/* Seeds a chain's initial rules (mirrors Go's SetInitialRules). `rules[i]`
 * is an argv-style array of `rule_lens[i]` string parts. */
mt_err_t mt_fake_ipt_set_initial_rules(mt_fake_ipt_t *f, const char *table, const char *chain,
                                       const char *const *const *rules, const size_t *rule_lens,
                                       size_t n_rules);

/* Borrowed view into current rules for `table`/`chain`; NULL + 0 if the
 * chain doesn't exist. Valid until the next mutating call. */
bool mt_fake_ipt_get_rules(mt_fake_ipt_t *f, const char *table, const char *chain,
                          mt_ipt_rule_t *const **out_rules, size_t *out_n);
bool mt_fake_ipt_chain_exists(mt_fake_ipt_t *f, const char *table, const char *chain);

/* Drops every table and chain, standing in for another writer replacing
 * the whole ruleset (what Keenetic firmware does before it runs the
 * netfilter.d hook). */
void mt_fake_ipt_reset(mt_fake_ipt_t *f);

#endif /* MT_TEST_FAKE_IPTABLES_H */
