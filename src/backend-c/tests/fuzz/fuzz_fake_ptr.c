/* libFuzzer target: fake-PTR response builder. Given arbitrary bytes as a
 * "request", the detector and response builder must stay memory-safe and
 * the built response (when produced) must itself parse. */
#include <stdint.h>
#include <stdlib.h>

#include "magitrickle/dnswire.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (mt_dns_is_single_ptr_query(data, size)) {
        uint8_t *resp = NULL;
        size_t rlen = 0;
        if (mt_dns_make_fake_ptr_response(data, size, &resp, &rlen) ==
            MT_OK) {
            mt_dns_msg_t *msg = NULL;
            if (mt_dns_msg_parse(resp, rlen, &msg) == MT_OK) {
                mt_dns_msg_free(msg);
            }
            free(resp);
        }
    }
    return 0;
}
