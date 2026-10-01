/* Ports the corpus from src/backend/models/rule_test.go plus wildcard-dot
 * quirk cases; the full Go↔C corpus runs in tests/differential. */
#include "greatest.h"

#include "magitrickle/match.h"

static bool m1(const char *type, const char *rule, const char *domain)
{
    mt_rule_matcher_t *m = mt_rule_matcher_new(type, rule);
    if (m == NULL) {
        return false;
    }
    bool ok = mt_rule_matcher_match(m, domain);
    mt_rule_matcher_free(m);
    return ok;
}

TEST domain_exact(void)
{
    ASSERT(m1("domain", "example.com", "example.com"));
    ASSERT_FALSE(m1("domain", "example.com", "noexample.com"));
    ASSERT_FALSE(m1("domain", "example.com", "sub.example.com"));
    ASSERT_FALSE(m1("domain", "example.com", "example.com.ru"));
    ASSERT_FALSE(m1("domain", "example.com", ""));
    PASS();
}

TEST namespace_suffix(void)
{
    ASSERT(m1("namespace", "example.com", "example.com"));
    ASSERT(m1("namespace", "example.com", "sub.example.com"));
    ASSERT(m1("namespace", "example.com", "deep.sub.example.com"));
    ASSERT_FALSE(m1("namespace", "example.com", "noexample.com"));
    ASSERT_FALSE(m1("namespace", "example.com", "notexample.com"));
    ASSERT_FALSE(m1("namespace", "example.com", "example.com.ru"));
    ASSERT_FALSE(m1("namespace", "example.com", "fakeexample.com"));
    ASSERT_FALSE(m1("namespace", "example.com", ""));
    ASSERT_FALSE(m1("namespace", "example.com", "com"));
    ASSERT(m1("namespace", "example.com", ".example.com"));
    /* short namespace */
    ASSERT(m1("namespace", "ru", "ru"));
    ASSERT(m1("namespace", "ru", "example.ru"));
    ASSERT(m1("namespace", "ru", "sub.example.ru"));
    ASSERT_FALSE(m1("namespace", "ru", "ru.com"));
    ASSERT_FALSE(m1("namespace", "ru", ""));
    ASSERT_FALSE(m1("namespace", "ru", "r"));
    PASS();
}

TEST wildcard_go_corpus(void)
{
    ASSERT(m1("wildcard", "ex*le.com", "example.com"));
    ASSERT(m1("wildcard", "ex*le.com", "exle.com"));
    ASSERT_FALSE(m1("wildcard", "ex*le.com", "noexample.com"));
    ASSERT(m1("wildcard", "*.example.com", "sub.example.com"));
    ASSERT_FALSE(m1("wildcard", "*.example.com", "example.com"));
    ASSERT(m1("wildcard", "*example*", "example.com"));
    ASSERT(m1("wildcard", "*example*", "myexamplesite.ru"));
    ASSERT(m1("wildcard", "test*.com", "test123.com"));
    ASSERT(m1("wildcard", "test??.com", "test12.com"));
    ASSERT_FALSE(m1("wildcard", "test??.com", "test123.com"));
    ASSERT_FALSE(m1("wildcard", "test.com", "test12.com"));
    PASS();
}

TEST wildcard_dot_matches_any_byte(void)
{
    /* go-wildcard v2 quirk: '.' in the pattern is a single-char wildcard */
    ASSERT(m1("wildcard", "test.com", "testXcom"));
    ASSERT(m1("wildcard", "a.b", "aXb"));
    ASSERT_FALSE(m1("wildcard", "a.b", "ab"));
    PASS();
}

TEST regex_pcre2(void)
{
    ASSERT(m1("regex", "^ex[apm]{3}le.com$", "example.com"));
    ASSERT(m1("regex", "^ex[apm]{3}le.com$", "exapple.com"));
    ASSERT_FALSE(m1("regex", "^ex[apm]{3}le.com$", "noexample.com"));
    ASSERT(m1("regex", "example", "example.com"));       /* substring */
    ASSERT_FALSE(m1("regex", "^example$", "example.com"));
    ASSERT(m1("regex", "(?i)EXAMPLE", "example.com"));
    ASSERT(m1("regex", "\\d+\\.example", "123.example"));
    /* case-insensitive by default (IgnoreCase) */
    ASSERT(m1("regex", "EXAMPLE", "example.com"));
    PASS();
}

TEST invalid_regex_never_matches(void)
{
    mt_rule_matcher_t *m = mt_rule_matcher_new("regex", "[invalid(regex");
    ASSERT(m != NULL);
    ASSERT_FALSE(mt_rule_matcher_ok(m));
    ASSERT_FALSE(mt_rule_matcher_match(m, "anything"));
    mt_rule_matcher_free(m);
    PASS();
}

TEST subnet_types_never_match(void)
{
    ASSERT_FALSE(m1("subnet", "10.0.0.0/8", "example.com"));
    ASSERT_FALSE(m1("subnet6", "fd00::/8", "example.com"));
    ASSERT_FALSE(m1("unknown", "example.com", "example.com"));
    PASS();
}

TEST group_index_matches_like_rules(void)
{
    mt_matcher_t *m = mt_matcher_new();
    ASSERT(m != NULL);
    ASSERT_EQ(MT_OK, mt_matcher_add(m, "domain", "exact.example.com"));
    ASSERT_EQ(MT_OK, mt_matcher_add(m, "namespace", "ns.example.org"));
    ASSERT_EQ(MT_OK, mt_matcher_add(m, "wildcard", "*.wild.net"));
    ASSERT_EQ(MT_OK, mt_matcher_add(m, "regex", "^r[0-9]+\\.example\\.io$"));
    ASSERT_EQ(MT_OK, mt_matcher_add(m, "subnet", "10.0.0.0/8"));

    ASSERT(mt_matcher_match(m, "exact.example.com"));
    ASSERT_FALSE(mt_matcher_match(m, "sub.exact.example.com"));
    ASSERT(mt_matcher_match(m, "ns.example.org"));
    ASSERT(mt_matcher_match(m, "a.b.ns.example.org"));
    ASSERT(mt_matcher_match(m, ".ns.example.org"));
    ASSERT_FALSE(mt_matcher_match(m, "xns.example.org"));
    ASSERT(mt_matcher_match(m, "a.wild.net"));
    ASSERT_FALSE(mt_matcher_match(m, "wild.net"));
    ASSERT(mt_matcher_match(m, "r42.example.io"));
    ASSERT_FALSE(mt_matcher_match(m, "rx.example.io"));
    ASSERT_FALSE(mt_matcher_match(m, "10.0.0.1"));

    mt_matcher_free(m);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(domain_exact);
    RUN_TEST(namespace_suffix);
    RUN_TEST(wildcard_go_corpus);
    RUN_TEST(wildcard_dot_matches_any_byte);
    RUN_TEST(regex_pcre2);
    RUN_TEST(invalid_regex_never_matches);
    RUN_TEST(subnet_types_never_match);
    RUN_TEST(group_index_matches_like_rules);
    GREATEST_MAIN_END();
}
