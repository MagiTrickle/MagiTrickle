/* libFuzzer target: DNS wire parser. Must never crash, leak, read OOB, or
 * hang on any input (spec §13/§21). On a successful parse it also exercises
 * pack + strip-aaaa + re-parse to catch inconsistencies. */
#include <stdint.h>
#include <stdlib.h>

#include "magitrickle/dnswire.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    mt_dns_msg_t *msg = NULL;
    if (mt_dns_msg_parse(data, size, &msg) != MT_OK) {
        return 0;
    }

    /* raw-message helpers must also tolerate the same input */
    (void)mt_dns_is_single_ptr_query(data, size);

    /* pack round-trips without blowing up */
    uint8_t *packed = NULL;
    size_t plen = 0;
    if (mt_dns_msg_pack(msg, &packed, &plen) == MT_OK) {
        mt_dns_msg_t *rt = NULL;
        if (mt_dns_msg_parse(packed, plen, &rt) == MT_OK) {
            mt_dns_msg_strip_aaaa(rt);
            uint8_t *p2 = NULL;
            size_t p2len = 0;
            if (mt_dns_msg_pack(rt, &p2, &p2len) == MT_OK) {
                free(p2);
            }
            mt_dns_msg_free(rt);
        }
        free(packed);
    }

    /* name rendering on the first question/answer */
    char name[1100];
    if (msg->n_questions > 0) {
        (void)mt_dns_name_to_string(msg->questions[0].name,
                                    msg->questions[0].name_len, name,
                                    sizeof(name));
    }
    mt_dns_msg_free(msg);
    return 0;
}
