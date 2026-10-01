/* Direct port of src/backend/utils/recordsCache/records_test.go. Uses an
 * injected `now` instead of wall-clock sleeps (Go's tests really sleep;
 * we don't need to burn wall time to prove the same logic). */
#include "greatest.h"

#include <string.h>

#include "magitrickle/dns_cache.h"

static const uint8_t IP1234[4] = {1, 2, 3, 4};
static const uint8_t IP5678[4] = {5, 6, 7, 8};
static const uint8_t IP9101112[4] = {9, 10, 11, 12};

static bool strs_contain(char **strs, size_t n, const char *s)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(strs[i], s) == 0) {
            return true;
        }
    }
    return false;
}

TEST loop_detection(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_alias(r, "1", "2", 60, 1000);
    mt_cache_add_alias(r, "2", "1", 60, 1000);

    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    mt_cache_get_addresses(r, "1", 1000, &a, &n);
    ASSERT_EQ(0u, (unsigned)n);
    free(a);
    mt_cache_get_addresses(r, "2", 1000, &a, &n);
    ASSERT_EQ(0u, (unsigned)n);
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST cname_resolution(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "example.com", IP1234, 4, 60, 1000);
    mt_cache_add_alias(r, "gateway.example.com", "example.com", 60, 1000);

    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK,
              mt_cache_get_addresses(r, "gateway.example.com", 1000, &a, &n));
    ASSERT(n > 0);
    ASSERT_EQ(0, memcmp(a[0].addr, IP1234, 4));
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST plain_a_record(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "example.com", IP1234, 4, 60, 1000);
    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    mt_cache_get_addresses(r, "example.com", 1000, &a, &n);
    ASSERT(n > 0);
    ASSERT_EQ(0, memcmp(a[0].addr, IP1234, 4));
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST deprecated_expires(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "example.com", IP1234, 4, 0, 1000);
    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    /* ttl=0 => deadline==now; "now" one second later must be expired
     * (Go: !now.After(deadline) keeps; here strict > excludes deadline
     * itself, matching deadline == insert_time meaning already expired a
     * moment later) */
    mt_cache_get_addresses(r, "example.com", 1001, &a, &n);
    ASSERT_EQ(0u, (unsigned)n);
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST not_existed_a(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    mt_cache_get_addresses(r, "example.com", 1000, &a, &n);
    ASSERT_EQ(0u, (unsigned)n);
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST not_existed_cname_alias(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_alias(r, "gateway.example.com", "example.com", 60, 1000);
    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    mt_cache_get_addresses(r, "gateway.example.com", 1000, &a, &n);
    ASSERT_EQ(0u, (unsigned)n);
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST replacing_alias_with_direct_address(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_alias(r, "gateway.example.com", "example.com", 60, 1000);
    mt_cache_add_address(r, "gateway.example.com", IP1234, 4, 60, 1000);
    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    mt_cache_get_addresses(r, "gateway.example.com", 1000, &a, &n);
    ASSERT_EQ(0, memcmp(a[0].addr, IP1234, 4));
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST aliases_bfs(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "1", IP1234, 4, 60, 1000);
    mt_cache_add_alias(r, "2", "1", 60, 1000);
    mt_cache_add_alias(r, "3", "2", 60, 1000);
    mt_cache_add_alias(r, "4", "2", 60, 1000);
    mt_cache_add_alias(r, "5", "1", 60, 1000);

    char **aliases = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_cache_get_aliases(r, "1", &aliases, &n));
    ASSERT(strs_contain(aliases, n, "1"));
    ASSERT(strs_contain(aliases, n, "2"));
    ASSERT(strs_contain(aliases, n, "3"));
    ASSERT(strs_contain(aliases, n, "4"));
    ASSERT(strs_contain(aliases, n, "5"));
    mt_cache_free_strings(aliases, n);
    mt_cache_destroy(r);
    PASS();
}

TEST multiple_addresses(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "example.com", IP1234, 4, 60, 1000);
    mt_cache_add_address(r, "example.com", IP5678, 4, 60, 1000);
    mt_cache_add_address(r, "example.com", IP9101112, 4, 60, 1000);

    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    mt_cache_get_addresses(r, "example.com", 1000, &a, &n);
    ASSERT_EQ(3u, (unsigned)n);
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST address_deadline_update(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "example.com", IP1234, 4, 1, 1000);
    mt_cache_add_address(r, "example.com", IP1234, 4, 60, 1000); /* refresh */

    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    mt_cache_get_addresses(r, "example.com", 1000, &a, &n);
    ASSERT_EQ(1u, (unsigned)n);
    free(a);

    /* 2s later: original 1s ttl would have expired, but refresh (60s) keeps
     * it valid */
    mt_cache_get_addresses(r, "example.com", 1002, &a, &n);
    ASSERT_EQ(1u, (unsigned)n);
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST alias_update_repoints(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "target1", IP1234, 4, 60, 1000);
    mt_cache_add_address(r, "target2", IP5678, 4, 60, 1000);

    mt_cache_add_alias(r, "alias", "target1", 60, 1000);
    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    mt_cache_get_addresses(r, "alias", 1000, &a, &n);
    ASSERT_EQ(0, memcmp(a[0].addr, IP1234, 4));
    free(a);

    mt_cache_add_alias(r, "alias", "target2", 60, 1000);
    mt_cache_get_addresses(r, "alias", 1000, &a, &n);
    ASSERT_EQ(0, memcmp(a[0].addr, IP5678, 4));
    free(a);

    char **aliases = NULL;
    mt_cache_get_aliases(r, "target1", &aliases, &n);
    ASSERT_FALSE(strs_contain(aliases, n, "alias"));
    mt_cache_free_strings(aliases, n);

    mt_cache_get_aliases(r, "target2", &aliases, &n);
    ASSERT(strs_contain(aliases, n, "alias"));
    mt_cache_free_strings(aliases, n);

    mt_cache_destroy(r);
    PASS();
}

TEST aliases_with_expired_address_after_cleanup(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "1", IP1234, 4, 0, 1000);
    mt_cache_add_alias(r, "2", "1", 60, 1000);

    mt_cache_cleanup(r, 1001); /* expires "1"'s address */

    char **aliases = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_cache_get_aliases(r, "1", &aliases, &n));
    ASSERT(n > 0);
    mt_cache_free_strings(aliases, n);

    mt_cache_addr_t *a = NULL;
    mt_cache_get_addresses(r, "2", 1001, &a, &n);
    ASSERT_EQ(0u, (unsigned)n);
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST expired_alias_in_chain(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "target", IP1234, 4, 60, 1000);
    mt_cache_add_alias(r, "middle", "target", 0, 1000); /* expires immediately */
    mt_cache_add_alias(r, "start", "middle", 60, 1000);

    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    mt_cache_get_addresses(r, "start", 1001, &a, &n);
    ASSERT_EQ(0u, (unsigned)n);
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST deep_cname_chain(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "level5", (const uint8_t[]){5, 5, 5, 5}, 4, 60,
                         1000);
    mt_cache_add_alias(r, "level4", "level5", 60, 1000);
    mt_cache_add_alias(r, "level3", "level4", 60, 1000);
    mt_cache_add_alias(r, "level2", "level3", 60, 1000);
    mt_cache_add_alias(r, "level1", "level2", 60, 1000);

    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    mt_cache_get_addresses(r, "level1", 1000, &a, &n);
    ASSERT(n > 0);
    uint8_t want[4] = {5, 5, 5, 5};
    ASSERT_EQ(0, memcmp(a[0].addr, want, 4));
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST list_known_domains(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "domain1.com", IP1234, 4, 60, 1000);
    mt_cache_add_address(r, "domain2.com", IP5678, 4, 60, 1000);
    mt_cache_add_alias(r, "alias1.com", "domain1.com", 60, 1000);
    mt_cache_add_alias(r, "alias2.com", "domain2.com", 60, 1000);

    char **domains = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_cache_list_known_domains(r, &domains, &n));
    ASSERT_EQ(4u, (unsigned)n);
    ASSERT(strs_contain(domains, n, "domain1.com"));
    ASSERT(strs_contain(domains, n, "domain2.com"));
    ASSERT(strs_contain(domains, n, "alias1.com"));
    ASSERT(strs_contain(domains, n, "alias2.com"));
    mt_cache_free_strings(domains, n);
    mt_cache_destroy(r);
    PASS();
}

TEST self_alias_ignored(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "example.com", IP1234, 4, 60, 1000);
    mt_cache_add_alias(r, "example.com", "example.com", 60, 1000);

    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    mt_cache_get_addresses(r, "example.com", 1000, &a, &n);
    ASSERT(n > 0);
    ASSERT_EQ(0, memcmp(a[0].addr, IP1234, 4));
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST partial_expired_addresses(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "example.com", (const uint8_t[]){1, 1, 1, 1}, 4,
                         0, 1000); /* expires immediately */
    mt_cache_add_address(r, "example.com", (const uint8_t[]){2, 2, 2, 2}, 4,
                         60, 1000); /* valid */

    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    mt_cache_get_addresses(r, "example.com", 1001, &a, &n);
    ASSERT_EQ(1u, (unsigned)n);
    uint8_t want[4] = {2, 2, 2, 2};
    ASSERT_EQ(0, memcmp(a[0].addr, want, 4));
    free(a);
    mt_cache_destroy(r);
    PASS();
}

TEST cleanup_removes_only_expired(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "expired.com", IP1234, 4, 0, 1000);
    mt_cache_add_address(r, "valid.com", IP5678, 4, 60, 1000);
    mt_cache_add_alias(r, "expired-alias.com", "valid.com", 0, 1000);
    mt_cache_add_alias(r, "valid-alias.com", "valid.com", 60, 1000);
    mt_cache_add_alias(r, "valid-expired-alias.com", "expired.com", 60, 1000);

    mt_cache_cleanup(r, 1001);

    char **domains = NULL;
    size_t n = 0;
    mt_cache_list_known_domains(r, &domains, &n);
    ASSERT_FALSE(strs_contain(domains, n, "expired.com"));
    ASSERT_FALSE(strs_contain(domains, n, "expired-alias.com"));
    ASSERT(strs_contain(domains, n, "valid.com"));
    ASSERT(strs_contain(domains, n, "valid-alias.com"));
    ASSERT(strs_contain(domains, n, "valid-expired-alias.com"));
    mt_cache_free_strings(domains, n);
    mt_cache_destroy(r);
    PASS();
}

TEST get_aliases_no_reverse(void)
{
    mt_cache_t *r = mt_cache_create(0);
    mt_cache_add_address(r, "standalone.com", IP1234, 4, 60, 1000);
    char **aliases = NULL;
    size_t n = 0;
    mt_cache_get_aliases(r, "standalone.com", &aliases, &n);
    ASSERT_EQ(1u, (unsigned)n);
    ASSERT_STR_EQ("standalone.com", aliases[0]);
    mt_cache_free_strings(aliases, n);
    mt_cache_destroy(r);
    PASS();
}

TEST get_aliases_unknown_domain(void)
{
    mt_cache_t *r = mt_cache_create(0);
    char **aliases = NULL;
    size_t n = 0;
    mt_cache_get_aliases(r, "unknown.com", &aliases, &n);
    ASSERT_EQ(1u, (unsigned)n);
    ASSERT_STR_EQ("unknown.com", aliases[0]);
    mt_cache_free_strings(aliases, n);
    mt_cache_destroy(r);
    PASS();
}

TEST bounded_domain_count_drops(void)
{
    mt_cache_t *r = mt_cache_create(2);
    mt_cache_add_address(r, "a.com", IP1234, 4, 60, 1000);
    mt_cache_add_address(r, "b.com", IP1234, 4, 60, 1000);
    ASSERT_EQ(2u, (unsigned)mt_cache_domain_count(r));
    mt_cache_add_address(r, "c.com", IP1234, 4, 60, 1000); /* over cap */
    ASSERT_EQ(2u, (unsigned)mt_cache_domain_count(r));
    ASSERT_EQ(1u, (unsigned)mt_cache_dropped(r));

    mt_cache_addr_t *a = NULL;
    size_t n = 0;
    mt_cache_get_addresses(r, "c.com", 1000, &a, &n);
    ASSERT_EQ(0u, (unsigned)n);
    free(a);
    /* existing domains can still gain addresses/aliases past the cap */
    mt_cache_add_address(r, "a.com", IP5678, 4, 60, 1000);
    mt_cache_get_addresses(r, "a.com", 1000, &a, &n);
    ASSERT_EQ(2u, (unsigned)n);
    free(a);
    mt_cache_destroy(r);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(loop_detection);
    RUN_TEST(cname_resolution);
    RUN_TEST(plain_a_record);
    RUN_TEST(deprecated_expires);
    RUN_TEST(not_existed_a);
    RUN_TEST(not_existed_cname_alias);
    RUN_TEST(replacing_alias_with_direct_address);
    RUN_TEST(aliases_bfs);
    RUN_TEST(multiple_addresses);
    RUN_TEST(address_deadline_update);
    RUN_TEST(alias_update_repoints);
    RUN_TEST(aliases_with_expired_address_after_cleanup);
    RUN_TEST(expired_alias_in_chain);
    RUN_TEST(deep_cname_chain);
    RUN_TEST(list_known_domains);
    RUN_TEST(self_alias_ignored);
    RUN_TEST(partial_expired_addresses);
    RUN_TEST(cleanup_removes_only_expired);
    RUN_TEST(get_aliases_no_reverse);
    RUN_TEST(get_aliases_unknown_domain);
    RUN_TEST(bounded_domain_count_drops);
    GREATEST_MAIN_END();
}
