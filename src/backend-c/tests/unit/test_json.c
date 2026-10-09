#include "greatest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "magitrickle/json.h"
#include "magitrickle/models.h"

TEST error_shape_matches_go_ErrorRes(void)
{
    cJSON *obj = mt_json_error("something went wrong");
    ASSERT(obj != NULL);

    char *dump = mt_json_dump(obj);
    ASSERT(dump != NULL);
    ASSERT_STR_EQ("{\"error\":\"something went wrong\"}", dump);

    free(dump);
    cJSON_Delete(obj);
    PASS();
}

TEST dump_is_compact_no_whitespace(void)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "a", "b");
    cJSON_AddNumberToObject(obj, "n", 42);

    char *dump = mt_json_dump(obj);
    ASSERT(dump != NULL);
    ASSERT(strchr(dump, '\n') == NULL);
    ASSERT(strchr(dump, ' ') == NULL);
    ASSERT_STR_EQ("{\"a\":\"b\",\"n\":42}", dump);

    free(dump);
    cJSON_Delete(obj);
    PASS();
}

TEST parse_roundtrip(void)
{
    const char *text = "{\"login\":\"admin\",\"password\":\"hunter2\"}";
    cJSON *parsed = cJSON_Parse(text);
    ASSERT(parsed != NULL);
    cJSON *login = cJSON_GetObjectItemCaseSensitive(parsed, "login");
    ASSERT(cJSON_IsString(login));
    ASSERT_STR_EQ("admin", login->valuestring);
    cJSON_Delete(parsed);
    PASS();
}

TEST priority_accepts_only_bounded_integer_numbers(void)
{
    const char *valid[] = {"1", "100", "300", "1000", "300.0", "3e2"};
    const uint16_t expected[] = {1, 100, 300, 1000, 300, 300};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        char text[64];
        snprintf(text, sizeof(text), "{\"priority\":%s}", valid[i]);
        cJSON *obj = cJSON_Parse(text);
        ASSERT(obj != NULL);
        uint16_t priority = MT_SUBSCRIPTION_DEFAULT_PRIORITY;
        ASSERT_EQ(MT_OK, mt_json_parse_priority(obj, &priority));
        ASSERT_EQ(expected[i], priority);
        cJSON_Delete(obj);
    }
    const char *invalid[] = {
        "0", "-1", "1001", "65536", "1.5", "1e309", "-1e309",
        "null", "true", "false", "\"300\"", "{}", "[]"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char text[64];
        snprintf(text, sizeof(text), "{\"priority\":%s}", invalid[i]);
        cJSON *obj = cJSON_Parse(text);
        ASSERT(obj != NULL);
        uint16_t priority = MT_GROUP_DEFAULT_PRIORITY;
        ASSERT_EQ(MT_ERR_INVAL, mt_json_parse_priority(obj, &priority));
        ASSERT_EQ(MT_GROUP_DEFAULT_PRIORITY, priority);
        cJSON_Delete(obj);
    }
    cJSON *omitted = cJSON_Parse("{\"name\":\"unchanged\"}");
    uint16_t priority = 777;
    ASSERT_EQ(MT_OK, mt_json_parse_priority(omitted, &priority));
    ASSERT_EQ(777, priority);
    cJSON_Delete(omitted);
    cJSON *duplicate = cJSON_Parse("{\"priority\":100,\"priority\":300}");
    ASSERT_EQ(MT_ERR_INVAL, mt_json_parse_priority(duplicate, &priority));
    ASSERT_EQ(777, priority);
    cJSON_Delete(duplicate);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(error_shape_matches_go_ErrorRes);
    RUN_TEST(dump_is_compact_no_whitespace);
    RUN_TEST(parse_roundtrip);
    RUN_TEST(priority_accepts_only_bounded_integer_numbers);
    GREATEST_MAIN_END();
}
