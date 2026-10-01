#include "magitrickle/id.h"

#include <stdio.h>
#include <string.h>

#include "magitrickle/rand.h"

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

mt_err_t mt_id_parse(const char *s, mt_id_t *out)
{
    if (s == NULL || strlen(s) != 8) {
        return MT_ERR_INVAL;
    }
    mt_id_t id;
    for (int i = 0; i < 4; i++) {
        int hi = hex_val(s[(size_t)i * 2]);
        int lo = hex_val(s[(size_t)i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return MT_ERR_INVAL;
        }
        id.b[i] = (uint8_t)((hi << 4) | lo);
    }
    *out = id;
    return MT_OK;
}

void mt_id_format(mt_id_t id, char *buf)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        buf[(size_t)i * 2] = digits[id.b[i] >> 4];
        buf[(size_t)i * 2 + 1] = digits[id.b[i] & 0x0f];
    }
    buf[8] = '\0';
}

bool mt_id_is_zero(mt_id_t id)
{
    return id.b[0] == 0 && id.b[1] == 0 && id.b[2] == 0 && id.b[3] == 0;
}

bool mt_id_equal(mt_id_t a, mt_id_t b)
{
    return memcmp(a.b, b.b, 4) == 0;
}

mt_id_t mt_id_random(void)
{
    mt_id_t id = {{0, 0, 0, 0}};
    mt_random_bytes(id.b, sizeof(id.b));
    return id;
}
