#include "greatest.h"
#include <string.h>
#include "magitrickle/lookup.h"

TEST copied_keys_survive_growth_and_duplicate_insertion(void) {
    mt_lookup_t map = {0};
    size_t value = 99;
    ASSERT_FALSE(mt_lookup_get(&map, "missing", 7, &value));
    ASSERT_EQ((size_t)99, value);
    for (uint32_t key = 0; key < 50000; key++) {
        bool inserted = false;
        ASSERT_EQ(MT_OK, mt_lookup_put(&map, &key, sizeof(key), key, &inserted));
        ASSERT(inserted);
    }
    ASSERT_EQ((size_t)50000, map.len);
    for (uint32_t key = 0; key < 50000; key++) {
        bool inserted = true;
        ASSERT(mt_lookup_get(&map, &key, sizeof(key), &value));
        ASSERT_EQ((size_t)key, value);
        ASSERT_EQ(MT_OK, mt_lookup_put(&map, &key, sizeof(key), 99999, &inserted));
        ASSERT_FALSE(inserted);
        ASSERT(mt_lookup_get(&map, &key, sizeof(key), &value));
        ASSERT_EQ((size_t)key, value);
    }
    ASSERT_EQ((size_t)50000, map.len);
    mt_lookup_clear(&map);
    ASSERT(map.entries == NULL);
    ASSERT_EQ((size_t)0, map.cap);
    ASSERT_EQ((size_t)0, map.len);
    mt_lookup_clear(&map);
    PASS();
}

TEST binary_empty_keys_and_reuse(void) {
    mt_lookup_t map = {0};
    unsigned char key[] = {'a', 0, 'b'};
    const unsigned char original[] = {'a', 0, 'b'};
    size_t value;
    ASSERT_EQ(MT_OK, mt_lookup_put(&map, key, sizeof(key), 17, NULL));
    memset(key, 0xff, sizeof(key));
    ASSERT(mt_lookup_get(&map, original, sizeof(original), &value));
    ASSERT_EQ((size_t)17, value);
    ASSERT_FALSE(mt_lookup_get(&map, key, sizeof(key), NULL));
    ASSERT_EQ(MT_OK, mt_lookup_put(&map, NULL, 0, 42, NULL));
    ASSERT(mt_lookup_get(&map, NULL, 0, &value));
    ASSERT_EQ((size_t)42, value);
    mt_lookup_clear(&map);
    ASSERT_FALSE(mt_lookup_get(&map, NULL, 0, NULL));
    ASSERT_EQ(MT_OK, mt_lookup_put(&map, original, sizeof(original), 23, NULL));
    ASSERT(mt_lookup_get(&map, original, sizeof(original), &value));
    ASSERT_EQ((size_t)23, value);
    mt_lookup_clear(&map);
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(copied_keys_survive_growth_and_duplicate_insertion);
    RUN_TEST(binary_empty_keys_and_reuse);
    GREATEST_MAIN_END();
}
