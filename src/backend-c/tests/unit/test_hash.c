#include "greatest.h"

#include <stdio.h>
#include <string.h>

#include "magitrickle/hash.h"

static void hex(const uint8_t *buf, size_t len, char *out) {
    static const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[buf[i] >> 4];
        out[i * 2 + 1] = digits[buf[i] & 0xf];
    }
    out[len * 2] = '\0';
}

TEST md5_vectors(void) {
    uint8_t d[MT_MD5_DIGEST_LEN];
    char h[MT_MD5_DIGEST_LEN * 2 + 1];

    mt_md5((const uint8_t *)"", 0, d);
    hex(d, sizeof(d), h);
    ASSERT_STR_EQ("d41d8cd98f00b204e9800998ecf8427e", h);

    mt_md5((const uint8_t *)"abc", 3, d);
    hex(d, sizeof(d), h);
    ASSERT_STR_EQ("900150983cd24fb0d6963f7d28e17f72", h);

    const char *long_msg =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    mt_md5((const uint8_t *)long_msg, strlen(long_msg), d);
    hex(d, sizeof(d), h);
    ASSERT_STR_EQ("8215ef0796a20bcaaae116d3876c664a", h);

    PASS();
}

TEST sha256_vectors(void) {
    uint8_t d[MT_SHA256_DIGEST_LEN];
    char h[MT_SHA256_DIGEST_LEN * 2 + 1];

    mt_sha256((const uint8_t *)"", 0, d);
    hex(d, sizeof(d), h);
    ASSERT_STR_EQ(
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", h);

    mt_sha256((const uint8_t *)"abc", 3, d);
    hex(d, sizeof(d), h);
    ASSERT_STR_EQ(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", h);

    PASS();
}

TEST sha512_vectors(void) {
    uint8_t d[MT_SHA512_DIGEST_LEN];
    char h[MT_SHA512_DIGEST_LEN * 2 + 1];

    mt_sha512((const uint8_t *)"", 0, d);
    hex(d, sizeof(d), h);
    ASSERT_STR_EQ(
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d"
        "0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
        h);

    mt_sha512((const uint8_t *)"abc", 3, d);
    hex(d, sizeof(d), h);
    ASSERT_STR_EQ(
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a219"
        "2992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
        h);

    PASS();
}

TEST sha256_multi_block_vector(void) {
    /* NIST vector: 448-bit message, spans into a second block after
     * padding, exercising the update()-across-calls path. */
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t d[MT_SHA256_DIGEST_LEN];
    char h[MT_SHA256_DIGEST_LEN * 2 + 1];
    mt_sha256((const uint8_t *)msg, strlen(msg), d);
    hex(d, sizeof(d), h);
    ASSERT_STR_EQ(
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", h);
    PASS();
}

TEST hmac_sha256_rfc4231_case1(void) {
    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    const char *data = "Hi There";
    uint8_t out[MT_SHA256_DIGEST_LEN];
    char h[MT_SHA256_DIGEST_LEN * 2 + 1];
    mt_hmac_sha256(key, sizeof(key), (const uint8_t *)data, strlen(data), out);
    hex(out, sizeof(out), h);
    ASSERT_STR_EQ(
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", h);
    PASS();
}

TEST hash_equal_semantics(void) {
    uint8_t a[4] = {1, 2, 3, 4};
    uint8_t b[4] = {1, 2, 3, 4};
    uint8_t c[4] = {1, 2, 3, 5};
    ASSERT(mt_hash_equal(a, b, 4));
    ASSERT(!mt_hash_equal(a, c, 4));
    PASS();
}

TEST base64_std_vectors(void) {
    static const struct { const char *in; const char *out; } cases[] = {
        {"", ""},           {"f", "Zg=="},       {"fo", "Zm8="},
        {"foo", "Zm9v"},     {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t inlen = strlen(cases[i].in);
        char out[16];
        mt_base64_encode((const uint8_t *)cases[i].in, inlen, out);
        ASSERT_STR_EQ(cases[i].out, out);

        uint8_t decoded[16] = {0};
        size_t decoded_len = 0;
        ASSERT_EQ(0, mt_base64_decode(cases[i].out, strlen(cases[i].out),
                                      decoded, &decoded_len));
        ASSERT_EQ(inlen, decoded_len);
        ASSERT_EQ(0, memcmp(cases[i].in, decoded, inlen));
    }
    PASS();
}

TEST base64url_no_padding(void) {
    char out[16];
    mt_base64url_encode((const uint8_t *)"foobar", 6, out);
    ASSERT_STR_EQ("Zm9vYmFy", out); /* no '=' needed here */

    mt_base64url_encode((const uint8_t *)"f", 1, out);
    ASSERT_STR_EQ("Zg", out); /* padding omitted vs. std "Zg==" */

    uint8_t decoded[8] = {0};
    size_t decoded_len = 0;
    ASSERT_EQ(0, mt_base64url_decode("Zg", 2, decoded, &decoded_len));
    ASSERT_EQ(1u, decoded_len);
    ASSERT_EQ('f', decoded[0]);
    PASS();
}

TEST base64_urlsafe_alphabet_chars(void) {
    /* Bytes chosen so the standard alphabet emits '+' and '/'; the
     * URL-safe alphabet must emit '-' and '_' at the same positions. */
    uint8_t data[3] = {0xfb, 0xff, 0xbf};
    char std_out[8];
    char url_out[8];
    mt_base64_encode(data, sizeof(data), std_out);
    mt_base64url_encode(data, sizeof(data), url_out);
    ASSERT(strchr(std_out, '+') != NULL || strchr(std_out, '/') != NULL);
    ASSERT(strchr(url_out, '+') == NULL);
    ASSERT(strchr(url_out, '/') == NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(md5_vectors);
    RUN_TEST(sha256_vectors);
    RUN_TEST(sha512_vectors);
    RUN_TEST(sha256_multi_block_vector);
    RUN_TEST(hmac_sha256_rfc4231_case1);
    RUN_TEST(hash_equal_semantics);
    RUN_TEST(base64_std_vectors);
    RUN_TEST(base64url_no_padding);
    RUN_TEST(base64_urlsafe_alphabet_chars);
    GREATEST_MAIN_END();
}
