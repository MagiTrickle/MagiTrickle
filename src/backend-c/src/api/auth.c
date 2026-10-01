/* See auth.h. */
#include "magitrickle/auth.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>

#include "magitrickle/crypt.h"
#include "magitrickle/hash.h"
#include "magitrickle/jwt.h"
#include "magitrickle/paths.h"
#include "magitrickle/rand.h"

#define MT_AUTH_SECRET_LEN 32
#define MT_AUTH_JWT_ISSUER "magitrickle"
#define MT_AUTH_JWT_YEARS 20

static bool g_secret_loaded = false;
static uint8_t g_secret[MT_AUTH_SECRET_LEN];
static size_t g_secret_len = 0;
static mt_err_t g_secret_err = MT_OK;

static void hex_encode(const uint8_t *data, size_t len, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0xf];
    }
    out[len * 2] = '\0';
}

/* ---- civil-calendar arithmetic (Howard Hinnant's public-domain
 * days_from_civil/civil_from_days algorithms) -- used to add N *calendar*
 * years to a UTC unix timestamp exactly like Go's time.Time.AddDate,
 * without timegm(3) (a GNU/BSD extension; this project keeps GNU
 * extensions confined to src/platform/, and auth.c isn't there). Handles
 * the Feb-29-in-a-non-leap-target-year case the same way Go's
 * normalizing time.Date does (rolls over to March 1), since the formula
 * is pure arithmetic with no calendar validation step. */
static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y_ = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d_ = doy - (153 * mp + 2) / 5 + 1;
    unsigned m_ = mp + (mp < 10 ? 3 : (unsigned)-9);
    y_ += (m_ <= 2);
    *y = y_;
    *m = m_;
    *d = d_;
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t mt_auth_add_years_utc(int64_t unix_ts, int years) {
    int64_t days = unix_ts >= 0 ? unix_ts / 86400 : (unix_ts - 86399) / 86400;
    int64_t secs_of_day = unix_ts - days * 86400;
    int64_t y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    y += years;
    int64_t new_days = days_from_civil(y, m, d);
    return new_days * 86400 + secs_of_day;
}

/* ---- password hash lookup -------------------------------------------------- */

/* Testable core: shadow_path/passwd_path are injectable so
 * tests/unit/test_auth.c can point at temp fixture files instead of the
 * real /etc/shadow -- mt_auth_load_password_hash below is the only
 * production caller, always passing the real MT_SHADOW_FILE/
 * MT_PASSWD_FILE paths. */
mt_err_t mt_auth_load_password_hash_from(const char *shadow_path, const char *passwd_path,
                                        const char *login, char *out, size_t out_len) {
    FILE *f = fopen(shadow_path, "r");
    if (!f) { f = fopen(passwd_path, "r"); }
    if (!f) { return mt_err_from_errno(errno); }

    char *line = NULL;
    size_t cap = 0;
    mt_err_t result = MT_ERR_NOENT;
    ssize_t n;
    size_t login_len = strlen(login);
    while ((n = getline(&line, &cap, f)) >= 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) { line[--n] = '\0'; }
        if (n == 0 || line[0] == '#') { continue; }
        char *colon1 = memchr(line, ':', (size_t)n);
        if (!colon1) { continue; }
        size_t name_len = (size_t)(colon1 - line);
        if (name_len != login_len || strncmp(line, login, name_len) != 0) { continue; }

        char *hash_start = colon1 + 1;
        char *colon2 = strchr(hash_start, ':');
        size_t hash_len = colon2 ? (size_t)(colon2 - hash_start) : strlen(hash_start);
        while (hash_len > 0 && isspace((unsigned char)hash_start[hash_len - 1])) { hash_len--; }
        while (hash_len > 0 && isspace((unsigned char)*hash_start)) {
            hash_start++;
            hash_len--;
        }
        if (hash_len == 0 || (hash_len == 1 && (hash_start[0] == 'x' || hash_start[0] == '*'))) {
            result = MT_ERR_NOENT;
            break;
        }
        if (hash_len >= out_len) {
            result = MT_ERR_LIMIT;
            break;
        }
        memcpy(out, hash_start, hash_len);
        out[hash_len] = '\0';
        result = MT_OK;
        break;
    }
    free(line);
    fclose(f);
    return result;
}

mt_err_t mt_auth_load_password_hash(const char *login, char *out, size_t out_len) {
    return mt_auth_load_password_hash_from(MT_SHADOW_FILE, MT_PASSWD_FILE, login, out, out_len);
}

/* ---- app secret ------------------------------------------------------------ */

static mt_err_t load_or_create_secret(const char *state_dir) {
    char path[512];
    if (snprintf(path, sizeof(path), "%s/auth_secret", state_dir) >= (int)sizeof(path)) {
        return MT_ERR_INVAL;
    }

    FILE *f = fopen(path, "rb");
    if (f) {
        char buf[256];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        size_t start = 0;
        size_t end = n;
        while (start < end && isspace((unsigned char)buf[start])) { start++; }
        while (end > start && isspace((unsigned char)buf[end - 1])) { end--; }
        if (end == start) { return MT_ERR_INVAL; }

        uint8_t decoded[64];
        size_t decoded_len = 0;
        if (mt_base64_decode(buf + start, end - start, decoded, &decoded_len) != 0) {
            return MT_ERR_INVAL;
        }
        size_t copy_len = decoded_len > sizeof(g_secret) ? sizeof(g_secret) : decoded_len;
        memcpy(g_secret, decoded, copy_len);
        g_secret_len = copy_len;
        return MT_OK;
    }
    if (errno != ENOENT) { return mt_err_from_errno(errno); }

    uint8_t secret[MT_AUTH_SECRET_LEN];
    if (mt_random_bytes(secret, sizeof(secret)) != MT_OK) { return MT_ERR_SYS; }

    char encoded[64];
    mt_base64_encode(secret, sizeof(secret), encoded);

    if (mkdir(state_dir, 0700) != 0 && errno != EEXIST) { return mt_err_from_errno(errno); }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { return mt_err_from_errno(errno); }
    size_t encoded_len = strlen(encoded);
    ssize_t written = write(fd, encoded, encoded_len);
    close(fd);
    if (written < 0 || (size_t)written != encoded_len) { return MT_ERR_IO; }

    memcpy(g_secret, secret, sizeof(secret));
    g_secret_len = sizeof(secret);
    return MT_OK;
}

mt_err_t mt_auth_load_secret(const char *state_dir, uint8_t *out, size_t *out_len) {
    if (!g_secret_loaded) {
        g_secret_err = load_or_create_secret(state_dir);
        g_secret_loaded = true;
    }
    if (g_secret_err != MT_OK) { return g_secret_err; }
    memcpy(out, g_secret, g_secret_len);
    *out_len = g_secret_len;
    return MT_OK;
}

/* ---- signing key derivation ------------------------------------------------ */

static mt_err_t derive_signing_key(const char *state_dir, const char *password_hash,
                                   char *out_hex, size_t out_hex_len) {
    uint8_t secret[MT_AUTH_SECRET_LEN];
    size_t secret_len;
    mt_err_t err = mt_auth_load_secret(state_dir, secret, &secret_len);
    if (err != MT_OK) { return err; }

    uint8_t mac[MT_SHA256_DIGEST_LEN];
    mt_hmac_sha256(secret, secret_len, (const uint8_t *)password_hash, strlen(password_hash), mac);
    if (out_hex_len < sizeof(mac) * 2 + 1) { return MT_ERR_LIMIT; }
    hex_encode(mac, sizeof(mac), out_hex);
    return MT_OK;
}

/* ---- authenticate / verify --------------------------------------------------- */

/* Testable core (see load_password_hash_from): shadow_path/passwd_path
 * injectable for tests/unit/test_auth.c; mt_auth_authenticate always
 * passes the real system paths. */
mt_err_t mt_auth_authenticate_from(const char *shadow_path, const char *passwd_path,
                                  const char *state_dir, const char *login, const char *password,
                                  char *token_out, size_t token_out_len) {
    if (!login || login[0] == '\0' || !password || password[0] == '\0') { return MT_ERR_INVAL; }

    char password_hash[128];
    mt_err_t err =
        mt_auth_load_password_hash_from(shadow_path, passwd_path, login, password_hash,
                                sizeof(password_hash));
    if (err != MT_OK) { return err; }

    char computed[128];
    err = mt_crypt_password(password, password_hash, computed, sizeof(computed));
    if (err != MT_OK || strcmp(computed, password_hash) != 0) { return MT_ERR_INVAL; }

    char signing_key[MT_SHA256_DIGEST_LEN * 2 + 1];
    err = derive_signing_key(state_dir, password_hash, signing_key, sizeof(signing_key));
    if (err != MT_OK) { return err; }

    int64_t now = (int64_t)time(NULL);
    mt_jwt_claims_t claims = {0};
    snprintf(claims.sub, sizeof(claims.sub), "%s", login);
    snprintf(claims.iss, sizeof(claims.iss), "%s", MT_AUTH_JWT_ISSUER);
    claims.iat = now;
    claims.exp = mt_auth_add_years_utc(now, MT_AUTH_JWT_YEARS);

    return mt_jwt_sign(&claims, (const uint8_t *)signing_key, strlen(signing_key), token_out,
                       token_out_len);
}

mt_err_t mt_auth_authenticate(const char *state_dir, const char *login, const char *password,
                              char *token_out, size_t token_out_len) {
    return mt_auth_authenticate_from(MT_SHADOW_FILE, MT_PASSWD_FILE, state_dir, login, password,
                             token_out, token_out_len);
}

mt_err_t mt_auth_verify_token_from(const char *shadow_path, const char *passwd_path,
                                  const char *state_dir, const char *token) {
    mt_jwt_claims_t unverified;
    if (mt_jwt_parse_unverified(token, &unverified) != MT_OK) { return MT_ERR_INVAL; }
    if (unverified.sub[0] == '\0') { return MT_ERR_INVAL; }

    char password_hash[128];
    if (mt_auth_load_password_hash_from(shadow_path, passwd_path, unverified.sub, password_hash,
                                sizeof(password_hash)) != MT_OK) {
        return MT_ERR_INVAL;
    }

    char signing_key[MT_SHA256_DIGEST_LEN * 2 + 1];
    if (derive_signing_key(state_dir, password_hash, signing_key, sizeof(signing_key)) != MT_OK) {
        return MT_ERR_INVAL;
    }

    mt_jwt_claims_t verified;
    if (mt_jwt_parse_and_verify(token, (const uint8_t *)signing_key, strlen(signing_key),
                                &verified) != MT_OK) {
        return MT_ERR_INVAL;
    }
    if (strcmp(verified.sub, unverified.sub) != 0) { return MT_ERR_INVAL; }
    if (strcmp(verified.iss, MT_AUTH_JWT_ISSUER) != 0) { return MT_ERR_INVAL; }
    if (verified.exp <= (int64_t)time(NULL)) { return MT_ERR_INVAL; }
    return MT_OK;
}

mt_err_t mt_auth_verify_token(const char *state_dir, const char *token) {
    return mt_auth_verify_token_from(MT_SHADOW_FILE, MT_PASSWD_FILE, state_dir, token);
}

/* ---- HTTP glue --------------------------------------------------------------- */

bool mt_auth_middleware(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_auth_ctx_t *ctx = ud;
    const char *path = mt_http_req_path(req);
    if (strncmp(path, "/api/", 5) != 0) { return true; }
    if (!ctx->enabled(ctx->ud) || strcmp(path, "/api/v1/auth") == 0) { return true; }

    const char *authz = mt_http_req_header(req, "Authorization");
    if (!authz) {
        mt_http_res_write_error(res, 401, "Unauthorized");
        return false;
    }
    while (*authz == ' ' || *authz == '\t') { authz++; }
    static const char prefix[] = "Bearer ";
    if (strncmp(authz, prefix, sizeof(prefix) - 1) != 0) {
        mt_http_res_write_error(res, 401, "Unauthorized");
        return false;
    }
    const char *token = authz + sizeof(prefix) - 1;
    if (*token == '\0') {
        mt_http_res_write_error(res, 401, "Unauthorized");
        return false;
    }

    if (mt_auth_verify_token(ctx->state_dir(ctx->ud), token) != MT_OK) {
        mt_http_res_write_error(res, 401, "Unauthorized");
        return false;
    }
    return true;
}

void mt_auth_status_handler(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    mt_auth_ctx_t *ctx = ud;
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "enabled", ctx->enabled(ctx->ud));
    mt_http_res_write_json(res, 200, obj);
}

void mt_auth_login_handler(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_auth_ctx_t *ctx = ud;
    if (!ctx->enabled(ctx->ud)) {
        mt_http_res_write_error(res, 404, "Auth disabled");
        return;
    }

    size_t body_len;
    const uint8_t *body = mt_http_req_body(req, &body_len);
    cJSON *json = body_len > 0 ? cJSON_ParseWithLength((const char *)body, body_len) : NULL;
    if (!json) {
        mt_http_res_write_error(res, 400, "failed to parse request");
        return;
    }

    cJSON *login_j = cJSON_GetObjectItemCaseSensitive(json, "login");
    cJSON *password_j = cJSON_GetObjectItemCaseSensitive(json, "password");
    const char *login = cJSON_IsString(login_j) ? login_j->valuestring : "";
    const char *password = cJSON_IsString(password_j) ? password_j->valuestring : "";
    if (login[0] == '\0' || password[0] == '\0') {
        cJSON_Delete(json);
        mt_http_res_write_error(res, 400, "missing credentials");
        return;
    }

    char token[MT_JWT_MAX_TOKEN];
    mt_err_t err =
        mt_auth_authenticate(ctx->state_dir(ctx->ud), login, password, token, sizeof(token));
    cJSON_Delete(json);
    if (err != MT_OK) {
        mt_http_res_write_error(res, 403, "Invalid credentials");
        return;
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "token", token);
    mt_http_res_write_json(res, 200, out);
}
