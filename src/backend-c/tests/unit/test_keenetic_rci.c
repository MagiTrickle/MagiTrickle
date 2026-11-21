#include "greatest.h"
#include "magitrickle/keenetic_rci.h"
#include <stdlib.h>
TEST system_name_request_is_one_positional_batch(void) {
    mt_kn_ifinfo_t items[2] = {{.id = "Wireguard0"}, {.id = "Wireguard1"}};
    char *body = mt_kn_build_system_name_request(items, 2); ASSERT(body);
    ASSERT_STR_EQ("[{\"show\":{\"interface\":{\"name\":\"Wireguard0\",\"details\":\"yes\",\"system-name\":\"yes\"}}},"
                  "{\"show\":{\"interface\":{\"name\":\"Wireguard1\",\"details\":\"yes\",\"system-name\":\"yes\"}}}]", body);
    free(body); PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN(); RUN_TEST(system_name_request_is_one_positional_batch); GREATEST_MAIN_END();
}
