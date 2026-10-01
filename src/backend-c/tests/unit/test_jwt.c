/* Vectors generated from the real Go implementation (api/auth/jwt.go's
 * signJWT/parseAndVerifyJWT) -- see phase-6-report.md for capture method.
 * Checks byte-exact token compatibility (migration-plan.md: "JWT
 * byte-compatible") plus C-side verify of a Go-produced token and
 * vice versa in spirit (same secret/claims -> identical bytes either way).
 */
#include "greatest.h"

#include <stdio.h>
#include <string.h>

#include "magitrickle/jwt.h"

static const uint8_t SECRET[] = "0123456789abcdef0123456789abcdef";
#define SECRET_LEN (sizeof(SECRET) - 1)

TEST sign_matches_go_byte_for_byte(void) {
    mt_jwt_claims_t claims = {0};
    snprintf(claims.sub, sizeof(claims.sub), "admin");
    snprintf(claims.iss, sizeof(claims.iss), "magitrickle");
    claims.iat = 1700000000;
    claims.exp = 2331200000;

    char token[MT_JWT_MAX_TOKEN];
    ASSERT_EQ(MT_OK, mt_jwt_sign(&claims, SECRET, SECRET_LEN, token, sizeof(token)));
    ASSERT_STR_EQ(
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJzdWIiOiJhZG1pbiIsImlzcyI6Im1hZ2l0cmlja2xlIiwiaWF0IjoxNzAwMDAwMDAwLC"
        "JleHAiOjIzMzEyMDAwMDB9."
        "S1_wP15OpZK-KaZzBaGootFwlNY-WCVMXZpJK7EHIcA",
        token);
    PASS();
}

TEST sign_matches_go_byte_for_byte_zero_iat(void) {
    mt_jwt_claims_t claims = {0};
    snprintf(claims.sub, sizeof(claims.sub), "user2");
    snprintf(claims.iss, sizeof(claims.iss), "magitrickle");
    claims.iat = 0;
    claims.exp = 630720000;

    char token[MT_JWT_MAX_TOKEN];
    ASSERT_EQ(MT_OK, mt_jwt_sign(&claims, SECRET, SECRET_LEN, token, sizeof(token)));
    ASSERT_STR_EQ(
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJzdWIiOiJ1c2VyMiIsImlzcyI6Im1hZ2l0cmlja2xlIiwiaWF0IjowLCJleHAiOjYzMD"
        "cyMDAwMH0."
        "ClG_IbarwPSGbSq_Abt4rAG4GdJ6XKZYjZAHV_7s2IY",
        token);
    PASS();
}

TEST verify_go_produced_token(void) {
    const char *go_token =
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJzdWIiOiJhZG1pbiIsImlzcyI6Im1hZ2l0cmlja2xlIiwiaWF0IjoxNzAwMDAwMDAwLC"
        "JleHAiOjIzMzEyMDAwMDB9."
        "S1_wP15OpZK-KaZzBaGootFwlNY-WCVMXZpJK7EHIcA";
    mt_jwt_claims_t claims;
    ASSERT_EQ(MT_OK, mt_jwt_parse_and_verify(go_token, SECRET, SECRET_LEN, &claims));
    ASSERT_STR_EQ("admin", claims.sub);
    ASSERT_STR_EQ("magitrickle", claims.iss);
    ASSERT_EQ(1700000000, claims.iat);
    ASSERT_EQ(2331200000, claims.exp);
    PASS();
}

TEST verify_rejects_tampered_signature(void) {
    const char *tampered =
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJzdWIiOiJhZG1pbiIsImlzcyI6Im1hZ2l0cmlja2xlIiwiaWF0IjoxNzAwMDAwMDAwLC"
        "JleHAiOjIzMzEyMDAwMDB9."
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    mt_jwt_claims_t claims;
    ASSERT_EQ(MT_ERR_INVAL, mt_jwt_parse_and_verify(tampered, SECRET, SECRET_LEN, &claims));
    PASS();
}

TEST verify_rejects_wrong_secret(void) {
    const char *go_token =
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJzdWIiOiJhZG1pbiIsImlzcyI6Im1hZ2l0cmlja2xlIiwiaWF0IjoxNzAwMDAwMDAwLC"
        "JleHAiOjIzMzEyMDAwMDB9."
        "S1_wP15OpZK-KaZzBaGootFwlNY-WCVMXZpJK7EHIcA";
    mt_jwt_claims_t claims;
    ASSERT_EQ(MT_ERR_INVAL,
             mt_jwt_parse_and_verify(go_token, (const uint8_t *)"wrong", 5, &claims));
    PASS();
}

TEST parse_unverified_reads_claims_without_secret(void) {
    const char *go_token =
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJzdWIiOiJhZG1pbiIsImlzcyI6Im1hZ2l0cmlja2xlIiwiaWF0IjoxNzAwMDAwMDAwLC"
        "JleHAiOjIzMzEyMDAwMDB9."
        "S1_wP15OpZK-KaZzBaGootFwlNY-WCVMXZpJK7EHIcA";
    mt_jwt_claims_t claims;
    ASSERT_EQ(MT_OK, mt_jwt_parse_unverified(go_token, &claims));
    ASSERT_STR_EQ("admin", claims.sub);
    PASS();
}

TEST malformed_token_rejected(void) {
    mt_jwt_claims_t claims;
    ASSERT_EQ(MT_ERR_PROTO, mt_jwt_parse_unverified("not.a.jwt.token", &claims));
    ASSERT_EQ(MT_ERR_PROTO, mt_jwt_parse_unverified("onlyonepart", &claims));
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(sign_matches_go_byte_for_byte);
    RUN_TEST(sign_matches_go_byte_for_byte_zero_iat);
    RUN_TEST(verify_go_produced_token);
    RUN_TEST(verify_rejects_tampered_signature);
    RUN_TEST(verify_rejects_wrong_secret);
    RUN_TEST(parse_unverified_reads_claims_without_secret);
    RUN_TEST(malformed_token_rejected);
    GREATEST_MAIN_END();
}
