/* Own vendored MD5/SHA-256/SHA-512/HMAC-SHA256 implementations (decisions.md
 * D-05-adjacent auth crypto decision) — per dependencies.md "Own vendored
 * single-file implementations" verdict: these primitives back the crypt(3)
 * formats and JWT signing api/auth needs, without pulling in OpenSSL/
 * mbedtls just for three primitives (that verdict explicitly rejected
 * linking the whole app to a TLS library for this). Standard, unkeyed
 * algorithms (RFC 1321, FIPS 180-4, RFC 2104) — not novel cryptographic
 * code, just implementations of well-known fixed algorithms, verified
 * against NIST/RFC test vectors in tests/unit/test_hash.c.
 *
 * Not constant-time; these are used for password-hash verification
 * (already keyed by a per-install secret / a system-file lookup, not a
 * network-facing timing-sensitive comparison) and JWT signing, matching
 * Go's crypto/md5, crypto/sha256, crypto/sha512, crypto/hmac (also not
 * documented as constant-time beyond the HMAC compare itself, which uses
 * `hmac.Equal` — mirrored here by memcmp-based mt_hash_equal, still not
 * hardened further than Go's own guarantee).
 */
#ifndef MAGITRICKLE_HASH_H
#define MAGITRICKLE_HASH_H

#include <stddef.h>
#include <stdint.h>

#define MT_MD5_DIGEST_LEN 16
#define MT_SHA256_DIGEST_LEN 32
#define MT_SHA512_DIGEST_LEN 64

typedef struct mt_md5_ctx {
    uint32_t state[4];
    uint64_t total_len;
    uint8_t buf[64];
    size_t buf_len;
} mt_md5_ctx_t;

void mt_md5_init(mt_md5_ctx_t *ctx);
void mt_md5_update(mt_md5_ctx_t *ctx, const uint8_t *data, size_t len);
void mt_md5_final(mt_md5_ctx_t *ctx, uint8_t out[MT_MD5_DIGEST_LEN]);
void mt_md5(const uint8_t *data, size_t len, uint8_t out[MT_MD5_DIGEST_LEN]);

typedef struct mt_sha256_ctx {
    uint32_t state[8];
    uint64_t total_len;
    uint8_t buf[64];
    size_t buf_len;
} mt_sha256_ctx_t;

void mt_sha256_init(mt_sha256_ctx_t *ctx);
void mt_sha256_update(mt_sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void mt_sha256_final(mt_sha256_ctx_t *ctx, uint8_t out[MT_SHA256_DIGEST_LEN]);
void mt_sha256(const uint8_t *data, size_t len, uint8_t out[MT_SHA256_DIGEST_LEN]);

typedef struct mt_sha512_ctx {
    uint64_t state[8];
    uint64_t total_len; /* bytes; bit-length always fits uint64_t for any
                         * realistic input, so the full 128-bit FIPS
                         * length field is not needed here */
    uint8_t buf[128];
    size_t buf_len;
} mt_sha512_ctx_t;

void mt_sha512_init(mt_sha512_ctx_t *ctx);
void mt_sha512_update(mt_sha512_ctx_t *ctx, const uint8_t *data, size_t len);
void mt_sha512_final(mt_sha512_ctx_t *ctx, uint8_t out[MT_SHA512_DIGEST_LEN]);
void mt_sha512(const uint8_t *data, size_t len, uint8_t out[MT_SHA512_DIGEST_LEN]);

/* RFC 2104. out must have room for MT_SHA256_DIGEST_LEN bytes. */
void mt_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
                    size_t msg_len, uint8_t out[MT_SHA256_DIGEST_LEN]);

/* Constant-time-ish compare (mirrors Go's hmac.Equal contract, see header
 * comment). */
int mt_hash_equal(const uint8_t *a, const uint8_t *b, size_t len);

/* ---- base64 (RFC 4648) --------------------------------------------------- */

/* Standard alphabet with padding (base64.StdEncoding). out buf must be
 * >= mt_base64_encoded_len(len)+1 (NUL terminator included). */
size_t mt_base64_encoded_len(size_t in_len);
void mt_base64_encode(const uint8_t *data, size_t len, char *out);

/* URL-safe, no padding (base64.RawURLEncoding). out buf must be >=
 * mt_base64url_encoded_len(len)+1. */
size_t mt_base64url_encoded_len(size_t in_len);
void mt_base64url_encode(const uint8_t *data, size_t len, char *out);

/* Decodes URL-safe unpadded base64. out must have room for
 * ((strlen(in)+3)/4)*3 bytes; *out_len set to actual decoded length.
 * Returns 0 on success, -1 on malformed input. */
int mt_base64url_decode(const char *in, size_t in_len, uint8_t *out, size_t *out_len);

/* Decodes standard padded base64. Same contract as above. */
int mt_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t *out_len);

#endif /* MAGITRICKLE_HASH_H */
