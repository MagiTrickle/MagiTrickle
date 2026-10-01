#include "greatest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "magitrickle/lookup.h"
#include "magitrickle/subparse.h"
#include "magitrickle/rand.h"

mt_err_t __real_mt_random_bytes(uint8_t *buf, size_t len);
mt_err_t __wrap_mt_random_bytes(uint8_t *buf, size_t len);
static unsigned entropy_calls;
static int entropy_mode;
mt_err_t __wrap_mt_random_bytes(uint8_t *buf, size_t len) {
    entropy_calls++;
    if (entropy_mode == 1) { return MT_ERR_SYS; }
    if (entropy_mode == 2) { memset(buf, 0, len); return MT_OK; }
    return __real_mt_random_bytes(buf, len);
}

static char *list_of(size_t count, bool duplicates) {
    size_t cap = count * (duplicates ? 64u : 32u) + 1;
    char *list = malloc(cap);
    if (!list) { return NULL; }
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        int n = snprintf(list + used, cap - used, "10.%zu.%zu.1/32\r\n", i / 256, i % 256);
        if (n < 0 || (size_t)n >= cap - used) { free(list); return NULL; }
        used += (size_t)n;
        if (duplicates) {
            n = snprintf(list + used, cap - used, "10.%zu.%zu.1/32\n", i / 256, i % 256);
            if (n < 0 || (size_t)n >= cap - used) { free(list); return NULL; }
            used += (size_t)n;
        }
    }
    list[used] = '\0';
    return list;
}

TEST fifty_thousand_rules_parse_refresh_and_compare(void) {
    const size_t count = 50000;
    char *list = list_of(count, true); ASSERT(list);
    mt_sub_rule_t **rules = NULL; size_t n = 0;
    entropy_calls = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules(list, &rules, &n)); ASSERT_EQ(count, n);
    ASSERT(entropy_calls <= 256u); /* batches, not 50000 entropy device opens */
    ASSERT_STR_EQ("10.0.0.1/32", rules[0]->rule);
    ASSERT_STR_EQ("10.195.79.1/32", rules[count - 1]->rule);
    mt_lookup_t ids = {0};
    for (size_t i = 0; i < n; i++) {
        ASSERT(!mt_id_is_zero(rules[i]->id));
        bool inserted = false;
        ASSERT_EQ(MT_OK, mt_lookup_put(&ids, rules[i]->id.b, 4, i, &inserted));
        ASSERT(inserted); ASSERT(rules[i]->enable); ASSERT_STR_EQ("subnet", rules[i]->type);
    }
    mt_lookup_clear(&ids);
    rules[2]->enable = false;
    ASSERT_EQ(MT_OK, mt_strset(&rules[2]->type, "regex"));
    mt_sub_rule_t **refreshed = NULL; size_t nr = 0;
    ASSERT_EQ(MT_OK, mt_sub_refresh_rules(list, rules, n, &refreshed, &nr));
    ASSERT_EQ(n, nr); ASSERT(mt_sub_same_rules(rules, n, refreshed, nr));
    for (size_t i = 0; i < n; i++) { ASSERT(mt_id_equal(rules[i]->id, refreshed[i]->id)); }
    ASSERT(!refreshed[2]->enable); ASSERT_STR_EQ("regex", refreshed[2]->type);
    mt_sub_rule_t *tmp = refreshed[0]; refreshed[0] = refreshed[nr - 1]; refreshed[nr - 1] = tmp;
    ASSERT(mt_sub_same_rules(rules, n, refreshed, nr));
    refreshed[2]->enable = true;
    ASSERT(!mt_sub_same_rules(rules, n, refreshed, nr));
    mt_sub_rules_free(rules, n); mt_sub_rules_free(refreshed, nr); free(list); PASS();
}

TEST parser_bounds_and_cancellation_fail_without_partial_output(void) {
    mt_sub_rule_t **rules = NULL; size_t n = 123;
    atomic_bool cancel; atomic_init(&cancel, true);
    ASSERT_EQ(MT_ERR_CANCELED, mt_sub_parse_rules_cancel("example.com", &rules, &n, &cancel));
    ASSERT(!rules); ASSERT_EQ(0u, n);
    char *long_line = malloc(MT_SUB_MAX_LINE_BYTES + 2); ASSERT(long_line);
    memset(long_line, 'a', MT_SUB_MAX_LINE_BYTES + 1); long_line[MT_SUB_MAX_LINE_BYTES + 1] = '\0';
    ASSERT_EQ(MT_ERR_LIMIT, mt_sub_parse_rules(long_line, &rules, &n));
    ASSERT(!rules); ASSERT_EQ(0u, n); free(long_line);
    char *list = list_of(MT_SUB_MAX_RULES + 1, false); ASSERT(list);
    ASSERT_EQ(MT_ERR_LIMIT, mt_sub_parse_rules(list, &rules, &n));
    ASSERT(!rules); ASSERT_EQ(0u, n); free(list); PASS();
}

TEST refresh_preserves_first_existing_and_repairs_duplicate_ids(void) {
    mt_sub_rule_t **old = NULL, **fresh = NULL; size_t n = 0, nf = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules("one.example\ntwo.example", &old, &n));
    old[1]->id = old[0]->id;
    ASSERT_EQ(MT_OK, mt_sub_refresh_rules("one.example\ntwo.example\nthree.example", old, n, &fresh, &nf));
    ASSERT_EQ(3u, nf); ASSERT(mt_id_equal(fresh[0]->id, old[0]->id));
    ASSERT(!mt_id_equal(fresh[0]->id, fresh[1]->id)); ASSERT(!mt_id_is_zero(fresh[1]->id));
    ASSERT(mt_id_equal(old[0]->id, old[1]->id)); /* baseline is never mutated */
    mt_sub_rules_free(fresh, nf);
    ASSERT_EQ(MT_OK, mt_strset(&old[1]->rule, "one.example")); old[0]->enable = false;
    ASSERT_EQ(MT_OK, mt_sub_refresh_rules("one.example", old, n, &fresh, &nf));
    ASSERT_EQ(1u, nf); ASSERT(!fresh[0]->enable); ASSERT(mt_id_equal(fresh[0]->id, old[0]->id));
    mt_sub_rules_free(fresh, nf); mt_sub_rules_free(old, n); PASS();
}

TEST lookup_binary_keys_growth_and_first_value_wins(void) {
    mt_lookup_t map = {0};
    ASSERT(!mt_lookup_get(&map, NULL, 0, NULL));
    ASSERT_EQ(MT_OK, mt_lookup_put(&map, NULL, 0, 99, NULL));
    for (uint32_t i = 0; i < 50000; i++) {
        bool inserted = false;
        ASSERT_EQ(MT_OK, mt_lookup_put(&map, &i, sizeof(i), (size_t)i + 1, &inserted)); ASSERT(inserted);
    }
    for (uint32_t i = 0; i < 50000; i++) {
        size_t value = 0; bool inserted = true;
        ASSERT_EQ(MT_OK, mt_lookup_put(&map, &i, sizeof(i), 0, &inserted)); ASSERT(!inserted);
        ASSERT(mt_lookup_get(&map, &i, sizeof(i), &value)); ASSERT_EQ((size_t)i + 1, value);
    }
    size_t value; ASSERT(mt_lookup_get(&map, NULL, 0, &value)); ASSERT_EQ(99u, value);
    mt_lookup_clear(&map); ASSERT_EQ(0u, map.len); mt_lookup_clear(&map); PASS();
}

TEST entropy_failure_and_repeated_zero_ids_terminate_cleanly(void) {
    mt_sub_rule_t **rules = NULL; size_t n = 0;
    entropy_mode = 1;
    mt_err_t failed = mt_sub_parse_rules("one.example\ntwo.example", &rules, &n);
    entropy_mode = 0;
    ASSERT_EQ(MT_ERR_SYS, failed); ASSERT(!rules); ASSERT_EQ(0u, n);
    entropy_mode = 2;
    failed = mt_sub_parse_rules("one.example", &rules, &n);
    entropy_mode = 0;
    ASSERT_EQ(MT_ERR_SYS, failed); ASSERT(!rules); ASSERT_EQ(0u, n);
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(entropy_failure_and_repeated_zero_ids_terminate_cleanly);
    RUN_TEST(fifty_thousand_rules_parse_refresh_and_compare);
    RUN_TEST(parser_bounds_and_cancellation_fail_without_partial_output);
    RUN_TEST(refresh_preserves_first_existing_and_repairs_duplicate_ids);
    RUN_TEST(lookup_binary_keys_growth_and_first_value_wins);
    GREATEST_MAIN_END();
}
