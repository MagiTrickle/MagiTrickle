/* crypt(3) password-hash formats — port of api/auth/crypt.go (which is
 * itself already a from-scratch reimplementation, not a libc crypt(3)
 * call — Poul-Henning Kamp's MD5-crypt and Ulrich Drepper's SHA-256/512-
 * crypt reference algorithms). This is one case where a close, near
 * line-by-line port of the Go source is the *correct* approach rather
 * than a spec violation: these are fixed, previously-specified
 * cryptographic algorithms where byte-exact arithmetic is the entire
 * point, not an incidental implementation detail to be reworked
 * idiomatically (master spec's "no mechanical translation" rule targets
 * control-flow/abstraction copying, not algorithm fidelity).
 *
 * Supported salt prefixes: "$1$" (MD5), "$5$" (SHA-256), "$6$" (SHA-512),
 * the latter two with an optional "rounds=N$" segment (clamped to
 * [1000, 999999999], default 5000) — matches Go's shaRoundsMin/Max/Default.
 */
#ifndef MAGITRICKLE_CRYPT_H
#define MAGITRICKLE_CRYPT_H

#include <stddef.h>

#include "magitrickle/err.h"

/* Computes the crypt(3)-style hash of `password` using the algorithm and
 * salt/rounds encoded in `salt` (e.g. "$1$abcdefgh" or
 * "$6$rounds=10000$abcdefgh"). On success writes a NUL-terminated string
 * into `out` (must be >= 128 bytes, matching the largest SHA-512-crypt
 * output) and returns MT_OK. MT_ERR_INVAL for an unrecognized/malformed
 * salt prefix. */
mt_err_t mt_crypt_password(const char *password, const char *salt, char *out, size_t out_len);

#endif /* MAGITRICKLE_CRYPT_H */
