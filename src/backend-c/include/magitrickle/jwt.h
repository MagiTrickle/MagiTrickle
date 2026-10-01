/* Hand-rolled JWT HS256 sign/verify — port of api/auth/jwt.go. Not a
 * general-purpose JWT library: fixed header {"alg":"HS256","typ":"JWT"},
 * fixed claim set (sub/iss/iat/exp), matching Go's jwtHeader/jwtClaims
 * exactly (see compatibility-contract.md §3).
 */
#ifndef MAGITRICKLE_JWT_H
#define MAGITRICKLE_JWT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "magitrickle/err.h"

#define MT_JWT_MAX_SUB 256
#define MT_JWT_MAX_ISS 64
/* header + claims + 2 base64url signatures + dots, generous bound for the
 * fixed claim shape above. */
#define MT_JWT_MAX_TOKEN 1024

typedef struct mt_jwt_claims {
    char sub[MT_JWT_MAX_SUB];
    char iss[MT_JWT_MAX_ISS];
    int64_t iat;
    int64_t exp;
} mt_jwt_claims_t;

/* Signs {alg:HS256,typ:JWT} + claims with HMAC-SHA256(secret). Writes a
 * NUL-terminated compact JWT into out (>= MT_JWT_MAX_TOKEN bytes). */
mt_err_t mt_jwt_sign(const mt_jwt_claims_t *claims, const uint8_t *secret, size_t secret_len,
                    char *out, size_t out_len);

/* Parses claims out of a token WITHOUT verifying the signature (matches
 * Go's parseJWTWithoutVerification -- used to look up which user's
 * current password hash to re-derive the signing key from, before the
 * real verification pass). MT_ERR_PROTO on malformed token/claims. */
mt_err_t mt_jwt_parse_unverified(const char *token, mt_jwt_claims_t *out);

/* Full parse + signature verification (HMAC compare via mt_hash_equal).
 * MT_ERR_PROTO on malformed token, MT_ERR_INVAL on signature mismatch. */
mt_err_t mt_jwt_parse_and_verify(const char *token, const uint8_t *secret, size_t secret_len,
                                mt_jwt_claims_t *out);

#endif /* MAGITRICKLE_JWT_H */
