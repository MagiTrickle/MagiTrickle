#include "greatest.h"

#include <string.h>

#include "magitrickle/duration.h"

static enum greatest_test_res roundtrip(const char *in, const char *want)
{
    mt_duration_t d;
    ASSERT_EQm(in, MT_OK, mt_duration_parse(in, &d));
    char buf[40];
    mt_duration_format(d, buf, sizeof(buf));
    ASSERT_STR_EQm(in, want, buf);
    PASS();
}

TEST parse_format_pairs(void)
{
    /* vectors cross-checked against Go time.ParseDuration/String */
    CHECK_CALL(roundtrip("0", "0s"));
    CHECK_CALL(roundtrip("5s", "5s"));
    CHECK_CALL(roundtrip("5000ms", "5s"));
    CHECK_CALL(roundtrip("1h", "1h0m0s"));
    CHECK_CALL(roundtrip("1h0m0s", "1h0m0s"));
    CHECK_CALL(roundtrip("90s", "1m30s"));
    CHECK_CALL(roundtrip("1.5s", "1.5s"));
    CHECK_CALL(roundtrip("300ms", "300ms"));
    CHECK_CALL(roundtrip("1.5h", "1h30m0s"));
    CHECK_CALL(roundtrip("2h45m", "2h45m0s"));
    CHECK_CALL(roundtrip("-5s", "-5s"));
    CHECK_CALL(roundtrip("1m", "1m0s"));
    CHECK_CALL(roundtrip("500us", "500\xc2\xb5s"));
    CHECK_CALL(roundtrip("500\xc2\xb5s", "500\xc2\xb5s"));
    CHECK_CALL(roundtrip("500\xce\xbcs", "500\xc2\xb5s"));
    CHECK_CALL(roundtrip("42ns", "42ns"));
    CHECK_CALL(roundtrip("1s500ms", "1.5s"));
    CHECK_CALL(roundtrip("+5s", "5s"));
    CHECK_CALL(roundtrip("1.000000001s", "1.000000001s"));
    CHECK_CALL(roundtrip("24h", "24h0m0s"));
    PASS();
}

TEST parse_rejects_invalid(void)
{
    mt_duration_t d;
    ASSERT_EQ(MT_ERR_INVAL, mt_duration_parse("", &d));
    ASSERT_EQ(MT_ERR_INVAL, mt_duration_parse("5", &d));
    ASSERT_EQ(MT_ERR_INVAL, mt_duration_parse("s", &d));
    ASSERT_EQ(MT_ERR_INVAL, mt_duration_parse("5x", &d));
    ASSERT_EQ(MT_ERR_INVAL, mt_duration_parse(".s", &d));
    ASSERT_EQ(MT_ERR_INVAL, mt_duration_parse("-", &d));
    ASSERT_EQ(MT_ERR_INVAL, mt_duration_parse("5d", &d));
    ASSERT_EQ(MT_ERR_INVAL, mt_duration_parse("5 s", &d));
    PASS();
}

TEST nanosecond_math(void)
{
    mt_duration_t d;
    ASSERT_EQ(MT_OK, mt_duration_parse("1ms", &d));
    ASSERT_EQ(MT_DURATION_MS, d);
    ASSERT_EQ(MT_OK, mt_duration_parse("1s", &d));
    ASSERT_EQ(MT_DURATION_SEC, d);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(parse_format_pairs);
    RUN_TEST(parse_rejects_invalid);
    RUN_TEST(nanosecond_math);
    GREATEST_MAIN_END();
}
