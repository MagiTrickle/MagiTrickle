/* Faithful ports of Go's time.ParseDuration and time.Duration.String().
 * Reference: go/src/time/format.go (leadingInt/leadingFraction/ParseDuration)
 * and go/src/time/time.go (Duration.String, fmtFrac, fmtInt).
 */
#include "magitrickle/duration.h"

#include <stdbool.h>
#include <string.h>

#define U64_MAX_DIV10 (UINT64_MAX / 10)

/* leadingInt: consume [0-9]*, error on overflow past 1<<63-1. */
static mt_err_t leading_int(const char **sp, uint64_t *out)
{
    const char *s = *sp;
    uint64_t x = 0;
    while (*s >= '0' && *s <= '9') {
        if (x > (UINT64_C(1) << 63) / 10) {
            return MT_ERR_INVAL; /* overflow */
        }
        x = x * 10 + (uint64_t)(*s - '0');
        if (x > (UINT64_C(1) << 63)) {
            return MT_ERR_INVAL;
        }
        s++;
    }
    *sp = s;
    *out = x;
    return MT_OK;
}

/* leadingFraction: consume [0-9]* as scaled fraction. */
static void leading_fraction(const char **sp, uint64_t *num, double *scale)
{
    const char *s = *sp;
    uint64_t x = 0;
    double sc = 1.0;
    bool overflow = false;
    while (*s >= '0' && *s <= '9') {
        if (overflow) {
            s++;
            continue;
        }
        if (x > (UINT64_C(1) << 63) / 10) {
            overflow = true;
            s++;
            continue;
        }
        uint64_t y = x * 10 + (uint64_t)(*s - '0');
        if (y > (UINT64_C(1) << 63)) {
            overflow = true;
            s++;
            continue;
        }
        x = y;
        sc *= 10;
        s++;
    }
    *sp = s;
    *num = x;
    *scale = sc;
}

struct unit_entry {
    const char *name;
    uint64_t nanos;
};

static const struct unit_entry UNITS[] = {
    {"ns", 1},
    {"us", 1000},
    {"\xc2\xb5s", 1000},     /* µs U+00B5 */
    {"\xce\xbcs", 1000},     /* μs U+03BC */
    {"ms", 1000000},
    {"s", 1000000000},
    {"m", UINT64_C(60000000000)},
    {"h", UINT64_C(3600000000000)},
};

static bool lookup_unit(const char *s, size_t len, uint64_t *nanos)
{
    for (size_t i = 0; i < sizeof(UNITS) / sizeof(UNITS[0]); i++) {
        if (strlen(UNITS[i].name) == len &&
            memcmp(UNITS[i].name, s, len) == 0) {
            *nanos = UNITS[i].nanos;
            return true;
        }
    }
    return false;
}

mt_err_t mt_duration_parse(const char *s, mt_duration_t *out)
{
    /* [-+]? ([0-9]*(\.[0-9]*)?[a-z]+)+ */
    bool neg = false;
    uint64_t d = 0;

    if (*s == '-' || *s == '+') {
        neg = (*s == '-');
        s++;
    }
    if (strcmp(s, "0") == 0) {
        *out = 0;
        return MT_OK;
    }
    if (*s == '\0') {
        return MT_ERR_INVAL;
    }
    while (*s != '\0') {
        uint64_t v = 0;
        uint64_t f = 0;
        double scale = 1.0;

        /* next char must be [0-9.] */
        if (!(*s == '.' || (*s >= '0' && *s <= '9'))) {
            return MT_ERR_INVAL;
        }
        const char *pl = s;
        if (leading_int(&s, &v) != MT_OK) {
            return MT_ERR_INVAL;
        }
        bool pre = (pl != s); /* digits before period */

        bool post = false;
        if (*s == '.') {
            s++;
            const char *pf = s;
            leading_fraction(&s, &f, &scale);
            post = (pf != s);
        }
        if (!pre && !post) {
            return MT_ERR_INVAL; /* no digits (e.g. ".s" or "-.s") */
        }

        /* unit: consume until next digit or dot */
        const char *u = s;
        while (*s != '\0' && *s != '.' && !(*s >= '0' && *s <= '9')) {
            s++;
        }
        if (u == s) {
            return MT_ERR_INVAL; /* missing unit */
        }
        uint64_t unit = 0;
        if (!lookup_unit(u, (size_t)(s - u), &unit)) {
            return MT_ERR_INVAL; /* unknown unit */
        }

        if (v > ((UINT64_C(1) << 63) - 1) / unit) {
            return MT_ERR_INVAL; /* overflow */
        }
        v *= unit;
        if (f > 0) {
            /* float64 is enough like in Go */
            v += (uint64_t)((double)f * ((double)unit / scale));
            if (v > (UINT64_C(1) << 63)) {
                return MT_ERR_INVAL;
            }
        }
        d += v;
        if (d > (UINT64_C(1) << 63)) {
            return MT_ERR_INVAL;
        }
    }

    if (neg) {
        *out = (mt_duration_t)(-(int64_t)d); /* -1<<63 allowed */
        return MT_OK;
    }
    if (d > (UINT64_C(1) << 63) - 1) {
        return MT_ERR_INVAL;
    }
    *out = (mt_duration_t)d;
    return MT_OK;
}

/* fmtFrac: write fraction (up to prec digits, trailing zeros dropped,
 * dot omitted when fraction is zero) BEFORE position w, backwards. */
static size_t fmt_frac(char *buf, size_t w, uint64_t v, int prec,
                       uint64_t *nv)
{
    bool print = false;
    for (int i = 0; i < prec; i++) {
        uint64_t digit = v % 10;
        print = print || digit != 0;
        if (print) {
            w--;
            buf[w] = (char)('0' + digit);
        }
        v /= 10;
    }
    if (print) {
        w--;
        buf[w] = '.';
    }
    *nv = v;
    return w;
}

static size_t fmt_int(char *buf, size_t w, uint64_t v)
{
    if (v == 0) {
        w--;
        buf[w] = '0';
    } else {
        while (v > 0) {
            w--;
            buf[w] = (char)('0' + v % 10);
            v /= 10;
        }
    }
    return w;
}

void mt_duration_format(mt_duration_t d, char *out, size_t out_len)
{
    char buf[32];
    size_t w = sizeof(buf);
    uint64_t u;
    bool neg = d < 0;
    if (neg) {
        u = (uint64_t)(-(d + 1)) + 1; /* handles INT64_MIN */
    } else {
        u = (uint64_t)d;
    }

    if (u < (uint64_t)MT_DURATION_SEC) {
        /* less than a second: ns / µs / ms with fraction */
        int prec;
        w--;
        buf[w] = 's';
        if (u == 0) {
            if (out_len >= 3) {
                memcpy(out, "0s", 3);
            }
            return;
        } else if (u < 1000) {
            prec = 0;
            w--;
            buf[w] = 'n';
        } else if (u < 1000000) {
            prec = 3;
            /* U+00B5 'µ' */
            w--;
            buf[w] = '\xb5';
            w--;
            buf[w] = '\xc2';
        } else {
            prec = 6;
            w--;
            buf[w] = 'm';
        }
        w = fmt_frac(buf, w, u, prec, &u);
        w = fmt_int(buf, w, u);
    } else {
        w--;
        buf[w] = 's';
        w = fmt_frac(buf, w, u, 9, &u);
        w = fmt_int(buf, w, u % 60); /* seconds */
        u /= 60;
        if (u > 0) {
            w--;
            buf[w] = 'm';
            w = fmt_int(buf, w, u % 60);
            u /= 60;
            if (u > 0) {
                w--;
                buf[w] = 'h';
                w = fmt_int(buf, w, u);
            }
        }
    }
    if (neg) {
        w--;
        buf[w] = '-';
    }

    size_t len = sizeof(buf) - w;
    if (len + 1 <= out_len) {
        memcpy(out, buf + w, len);
        out[len] = '\0';
    } else if (out_len > 0) {
        out[0] = '\0';
    }
}
