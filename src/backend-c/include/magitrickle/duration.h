/* Go-compatible time.Duration handling.
 *
 * The YAML contract stores durations exactly as Go writes them
 * (Duration.String(): "5s", "1h0m0s", "300ms") and parses everything Go's
 * yaml.v2 accepts: Go duration strings via time.ParseDuration semantics and
 * bare integers (nanoseconds). Both functions are faithful ports of the Go
 * stdlib algorithms — do not "simplify" them, byte-identical round-trips
 * are a differential-test gate.
 */
#ifndef MAGITRICKLE_DURATION_H
#define MAGITRICKLE_DURATION_H

#include <stddef.h>
#include <stdint.h>

#include "magitrickle/err.h"

/* Nanosecond duration, like Go's time.Duration. */
typedef int64_t mt_duration_t;

#define MT_DURATION_MS  INT64_C(1000000)
#define MT_DURATION_SEC INT64_C(1000000000)

/* time.ParseDuration port: "300ms", "1.5h", "2h45m", "-5s", "1h0m0s".
 * Units: ns, us, µs, μs, ms, s, m, h. MT_ERR_INVAL on anything Go rejects
 * (including overflow and bare numbers without unit except "0"). */
mt_err_t mt_duration_parse(const char *s, mt_duration_t *out);

/* Duration.String() port. buf must hold at least 32 bytes + sign. */
void mt_duration_format(mt_duration_t d, char *buf, size_t buf_len);

#endif /* MAGITRICKLE_DURATION_H */
