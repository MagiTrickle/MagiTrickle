#include "greatest.h"

#include "magitrickle/lifecycle.h"

static int destroy_order[32];
static int destroy_count;

static void record_destroy(void *ctx)
{
    destroy_order[destroy_count++] = *(int *)ctx;
}

TEST fail_unwinds_in_reverse_order(void)
{
    destroy_count = 0;
    mt_lifecycle_t lc;
    mt_lc_init(&lc);

    int a = 1, b = 2, c = 3;
    ASSERT_EQ(MT_OK, mt_lc_push(&lc, record_destroy, &a, "a"));
    ASSERT_EQ(MT_OK, mt_lc_push(&lc, record_destroy, &b, "b"));
    ASSERT_EQ(MT_OK, mt_lc_push(&lc, record_destroy, &c, "c"));

    mt_lc_fail(&lc);
    ASSERT_EQ(3, destroy_count);
    ASSERT_EQ(3, destroy_order[0]);
    ASSERT_EQ(2, destroy_order[1]);
    ASSERT_EQ(1, destroy_order[2]);
    /* reusable after fail */
    ASSERT_EQ(0u, (unsigned)lc.len);
    PASS();
}

TEST commit_runs_nothing(void)
{
    destroy_count = 0;
    mt_lifecycle_t lc;
    mt_lc_init(&lc);
    int a = 1;
    ASSERT_EQ(MT_OK, mt_lc_push(&lc, record_destroy, &a, "a"));
    mt_lc_commit(&lc);
    ASSERT_EQ(0, destroy_count);
    PASS();
}

TEST grows_past_initial_capacity(void)
{
    destroy_count = 0;
    mt_lifecycle_t lc;
    mt_lc_init(&lc);
    static int vals[20];
    for (int i = 0; i < 20; i++) {
        vals[i] = i;
        ASSERT_EQ(MT_OK, mt_lc_push(&lc, record_destroy, &vals[i], "x"));
    }
    mt_lc_fail(&lc);
    ASSERT_EQ(20, destroy_count);
    ASSERT_EQ(19, destroy_order[0]);
    ASSERT_EQ(0, destroy_order[19]);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(fail_unwinds_in_reverse_order);
    RUN_TEST(commit_runs_nothing);
    RUN_TEST(grows_past_initial_capacity);
    GREATEST_MAIN_END();
}
