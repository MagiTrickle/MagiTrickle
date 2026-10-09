#include "greatest.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "magitrickle/failover.h"

typedef struct fake {
    bool ready[2][128];
    int races, probe_error, replace_error, block_error;
    size_t installed[2], probes, replaces, blocks;
} fake_t;
static mt_err_t probe(void *ud, const char *name, int family, mt_route_candidate_t *c, bool *ready) {
    fake_t *f = ud; f->probes++;
    if (f->probe_error) { return MT_ERR_IO; }
    unsigned index = (unsigned)strtoul(name, NULL, 10);
    if (index >= 128) { return MT_ERR_INVAL; }
    c->ifindex = (int)index;
    *ready = f->ready[family == AF_INET6][index]; return MT_OK;
}
static mt_err_t replace(void *ud, int family, const mt_route_candidate_t *c, bool *unavailable) {
    fake_t *f = ud; f->replaces++;
    if (f->replace_error) { return MT_ERR_IO; }
    *unavailable = f->races > 0;
    if (*unavailable) { f->races--; return MT_OK; }
    f->installed[family == AF_INET6] = (size_t)c->ifindex; return MT_OK;
}
static mt_err_t block(void *ud, int family) {
    fake_t *f = ud; f->blocks++;
    if (f->block_error) { return MT_ERR_IO; }
    f->installed[family == AF_INET6] = SIZE_MAX; return MT_OK;
}
static const mt_failover_ops_t ops = {probe, replace, block};
static const char *names[] = {"0", "1", "2"};

TEST ordered_failover_and_automatic_failback(void) {
    fake_t f = {0}; size_t selected;
    for (size_t i = 0; i < 3; i++) { f.ready[0][i] = true; }
    ASSERT_EQ(MT_OK, mt_failover_reconcile(names, 3, AF_INET, &ops, &f, &selected));
    ASSERT_EQ((size_t)0, selected);
    f.ready[0][0] = false;
    ASSERT_EQ(MT_OK, mt_failover_reconcile(names, 3, AF_INET, &ops, &f, &selected));
    ASSERT_EQ((size_t)1, selected);
    f.ready[0][1] = false;
    ASSERT_EQ(MT_OK, mt_failover_reconcile(names, 3, AF_INET, &ops, &f, &selected));
    ASSERT_EQ((size_t)2, selected);
    f.ready[0][0] = true;
    ASSERT_EQ(MT_OK, mt_failover_reconcile(names, 3, AF_INET, &ops, &f, &selected));
    ASSERT_EQ((size_t)0, selected);
    ASSERT_EQ((size_t)0, f.blocks); PASS();
}
TEST all_down_blocks_then_recovers(void) {
    fake_t f = {0}; size_t selected;
    ASSERT_EQ(MT_OK, mt_failover_reconcile(names, 3, AF_INET, &ops, &f, &selected));
    ASSERT_EQ(SIZE_MAX, selected); ASSERT_EQ((size_t)1, f.blocks);
    f.ready[0][2] = true;
    ASSERT_EQ(MT_OK, mt_failover_reconcile(names, 3, AF_INET, &ops, &f, &selected));
    ASSERT_EQ((size_t)2, selected); PASS();
}
TEST families_are_independent_and_repeated_events_repair_routes(void) {
    fake_t f = {0}; size_t selected;
    f.ready[0][0] = true; f.ready[1][1] = true;
    ASSERT_EQ(MT_OK, mt_failover_reconcile(names, 3, AF_INET, &ops, &f, &selected));
    ASSERT_EQ(MT_OK, mt_failover_reconcile(names, 3, AF_INET6, &ops, &f, &selected));
    ASSERT_EQ((size_t)0, f.installed[0]); ASSERT_EQ((size_t)1, f.installed[1]);
    f.installed[0] = SIZE_MAX; /* kernel removed route between events */
    ASSERT_EQ(MT_OK, mt_failover_reconcile(names, 3, AF_INET, &ops, &f, &selected));
    ASSERT_EQ((size_t)0, f.installed[0]); ASSERT_EQ((size_t)3, f.replaces); PASS();
}
TEST disappeared_during_install_skips_to_next(void) {
    fake_t f = {0}; size_t selected;
    for (size_t i = 0; i < 3; i++) { f.ready[0][i] = true; }
    f.races = 2;
    ASSERT_EQ(MT_OK, mt_failover_reconcile(names, 3, AF_INET, &ops, &f, &selected));
    ASSERT_EQ((size_t)2, selected); ASSERT_EQ((size_t)3, f.replaces); PASS();
}
TEST unexpected_errors_fail_closed_and_are_not_hidden(void) {
    fake_t f = {0}; size_t selected; f.ready[0][0] = true;
    f.probe_error = 1;
    ASSERT_EQ(MT_ERR_IO, mt_failover_reconcile(names, 3, AF_INET, &ops, &f, &selected));
    ASSERT_EQ(SIZE_MAX, f.installed[0]);
    f.probe_error = 0; f.replace_error = 1;
    ASSERT_EQ(MT_ERR_IO, mt_failover_reconcile(names, 3, AF_INET, &ops, &f, &selected));
    ASSERT_EQ(SIZE_MAX, f.installed[0]);
    f.replace_error = 0; f.ready[0][0] = false; f.block_error = 1;
    ASSERT_EQ(MT_ERR_IO, mt_failover_reconcile(names, 3, AF_INET, &ops, &f, &selected));
    ASSERT_EQ((size_t)3, f.blocks); PASS();
}
TEST long_chain_and_reordering(void) {
    fake_t f = {0}; size_t selected; char storage[128][8]; const char *many[128];
    for (size_t i = 0; i < 128; i++) { snprintf(storage[i], 8, "%u", (unsigned)i); many[i] = storage[i]; }
    f.ready[0][127] = true;
    ASSERT_EQ(MT_OK, mt_failover_reconcile(many, 128, AF_INET, &ops, &f, &selected));
    ASSERT_EQ((size_t)127, selected);
    const char *reordered[] = {"127", "0"};
    ASSERT_EQ(MT_OK, mt_failover_reconcile(reordered, 2, AF_INET, &ops, &f, &selected));
    ASSERT_EQ((size_t)0, selected); ASSERT_EQ((size_t)127, f.installed[0]); PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(ordered_failover_and_automatic_failback);
    RUN_TEST(all_down_blocks_then_recovers);
    RUN_TEST(families_are_independent_and_repeated_events_repair_routes);
    RUN_TEST(disappeared_during_install_skips_to_next);
    RUN_TEST(unexpected_errors_fail_closed_and_are_not_hidden);
    RUN_TEST(long_chain_and_reordering);
    GREATEST_MAIN_END();
}
