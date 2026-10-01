#include "greatest.h"

#include <stdlib.h>
#include <string.h>

#include "magitrickle/json.h"

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

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(error_shape_matches_go_ErrorRes);
    RUN_TEST(dump_is_compact_no_whitespace);
    RUN_TEST(parse_roundtrip);
    GREATEST_MAIN_END();
}
