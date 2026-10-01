/* Ports the corpus from src/backend/subscriptions/{parse,validate}_test.go */
#include "greatest.h"

#include <string.h>

#include "magitrickle/subparse.h"

TEST type_detection(void)
{
    ASSERT_STR_EQ("namespace", mt_sub_detect_type("example.com"));
    ASSERT_STR_EQ("namespace", mt_sub_detect_type("sub.example.net"));
    ASSERT_STR_EQ("subnet", mt_sub_detect_type("10.0.0.0/8"));
    ASSERT_STR_EQ("subnet", mt_sub_detect_type("192.168.1.1"));
    ASSERT_STR_EQ("subnet6", mt_sub_detect_type("fd00::/8"));
    ASSERT_STR_EQ("subnet6", mt_sub_detect_type("2001:db8::1"));
    ASSERT_STR_EQ("wildcard", mt_sub_detect_type("*.example.com"));
    /* regex checked before wildcard: strings valid as regex win */
    ASSERT_STR_EQ("regex", mt_sub_detect_type("^example\\.com$"));
    /* invalid octet -> not subnet; dots make it a valid domain? no:
     * "300.1.2.3" chars are digits+dots and no leading/trailing dot,
     * so it is a valid domain -> namespace (Go behaviour) */
    ASSERT_STR_EQ("namespace", mt_sub_detect_type("300.1.2.3"));
    PASS();
}

TEST parse_dedup_and_comments(void)
{
    const char *list =
        "# comment\n"
        "example.com\n"
        "example.com\n"
        "  spaced.example.com  \n"
        "\n"
        "a.com,b.com\r\nc.com";
    mt_sub_rule_t **rules = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules(list, &rules, &n));
    ASSERT_EQ(5u, (unsigned)n);
    ASSERT_STR_EQ("example.com", rules[0]->rule);
    ASSERT_STR_EQ("spaced.example.com", rules[1]->rule);
    ASSERT_STR_EQ("a.com", rules[2]->rule);
    ASSERT_STR_EQ("b.com", rules[3]->rule);
    ASSERT_STR_EQ("c.com", rules[4]->rule);
    for (size_t i = 0; i < n; i++) {
        ASSERT(rules[i]->enable);
        ASSERT_FALSE(mt_id_is_zero(rules[i]->id));
        mt_sub_rule_free(rules[i]);
    }
    free(rules);
    PASS();
}

TEST refresh_preserves_existing_overrides(void)
{
    /* TestRefreshRulesPreservesExistingOverrides port */
    mt_sub_rule_t *ex0 = mt_sub_rule_new();
    mt_sub_rule_t *ex1 = mt_sub_rule_new();
    ASSERT(ex0 != NULL && ex1 != NULL);
    ex0->id = (mt_id_t){{0xaa, 0xbb, 0xcc, 0xdd}};
    mt_strset(&ex0->rule, "example.com");
    mt_strset(&ex0->type, "domain");
    ex0->enable = false;
    ex1->id = (mt_id_t){{0x11, 0x22, 0x33, 0x44}};
    mt_strset(&ex1->rule, "*.example.org");
    mt_strset(&ex1->type, "wildcard");
    ex1->enable = true;
    mt_sub_rule_t *existing[] = {ex0, ex1};

    mt_sub_rule_t **out = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_sub_refresh_rules("example.com\nsub.example.net\n",
                                          existing, 2, &out, &n));
    ASSERT_EQ(2u, (unsigned)n);
    ASSERT_STR_EQ("example.com", out[0]->rule);
    ASSERT(mt_id_equal(out[0]->id, ex0->id));
    ASSERT_STR_EQ("domain", out[0]->type);
    ASSERT_FALSE(out[0]->enable);
    ASSERT_STR_EQ("sub.example.net", out[1]->rule);
    ASSERT_FALSE(mt_id_is_zero(out[1]->id));
    ASSERT(out[1]->enable);

    mt_sub_rule_free(ex0);
    mt_sub_rule_free(ex1);
    for (size_t i = 0; i < n; i++) {
        mt_sub_rule_free(out[i]);
    }
    free(out);
    PASS();
}

TEST same_rules_ignores_order(void)
{
    mt_sub_rule_t *l0 = mt_sub_rule_new(), *l1 = mt_sub_rule_new();
    mt_sub_rule_t *r0 = mt_sub_rule_new(), *r1 = mt_sub_rule_new();
    l0->id = (mt_id_t){{1, 2, 3, 4}};
    mt_strset(&l0->rule, "example.com");
    mt_strset(&l0->type, "domain");
    l0->enable = true;
    l1->id = (mt_id_t){{5, 6, 7, 8}};
    mt_strset(&l1->rule, "*.example.org");
    mt_strset(&l1->type, "wildcard");
    l1->enable = false;
    r0->id = l1->id;
    mt_strset(&r0->rule, "*.example.org");
    mt_strset(&r0->type, "wildcard");
    r0->enable = false;
    r1->id = l0->id;
    mt_strset(&r1->rule, "example.com");
    mt_strset(&r1->type, "domain");
    r1->enable = true;

    mt_sub_rule_t *left[] = {l0, l1};
    mt_sub_rule_t *right[] = {r0, r1};
    ASSERT(mt_sub_same_rules(left, 2, right, 2));

    /* flip enable -> not same */
    r1->enable = false;
    ASSERT_FALSE(mt_sub_same_rules(left, 2, right, 2));

    mt_sub_rule_free(l0);
    mt_sub_rule_free(l1);
    mt_sub_rule_free(r0);
    mt_sub_rule_free(r1);
    PASS();
}

TEST is_due_logic(void)
{
    mt_subscription_t *s = mt_subscription_new();
    ASSERT(s != NULL);
    mt_strset(&s->url, "https://example.com/list.txt");
    s->enable = true;
    s->interval = 3600;
    s->last_update = 0;
    s->last_check = 0;
    ASSERT(mt_sub_is_due(s, 1000000)); /* never checked -> due */
    s->last_check = 999000;
    ASSERT_FALSE(mt_sub_is_due(s, 1000000)); /* 1000s ago < 3600 */
    ASSERT(mt_sub_is_due(s, 999000 + 3600));
    s->enable = false;
    ASSERT_FALSE(mt_sub_is_due(s, 999000 + 7200));
    s->enable = true;
    s->interval = 0;
    ASSERT_FALSE(mt_sub_is_due(s, 999000 + 7200));
    mt_subscription_free(s);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(type_detection);
    RUN_TEST(parse_dedup_and_comments);
    RUN_TEST(refresh_preserves_existing_overrides);
    RUN_TEST(same_rules_ignores_order);
    RUN_TEST(is_due_logic);
    GREATEST_MAIN_END();
}
