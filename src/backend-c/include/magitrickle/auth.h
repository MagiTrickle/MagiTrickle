/* HTTP auth — port of api/auth/{passwd,secret,password,middleware,
 * handlers}.go. See compatibility-contract.md §3.
 *
 * Credential check: crypt(3)-style hash lookup in /etc/shadow (falling
 * back to /etc/passwd when shadow is missing/unreadable, matching Go's
 * loadPasswordHash) via mt_crypt_password (crypt.h). Token: HS256 JWT
 * (jwt.h) whose signing key is HMAC-SHA256(app_secret, passwordHash)
 * hex-encoded — binding the token to the user's *current* password hash,
 * so a password change invalidates every previously issued token.
 * app_secret is 32 random bytes, base64, persisted at
 * "<state_dir>/auth_secret" (0600, dir 0700), generated on first use and
 * cached in memory for the process lifetime (matches Go's sync.Once).
 */
#ifndef MAGITRICKLE_AUTH_H
#define MAGITRICKLE_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "magitrickle/err.h"
#include "magitrickle/httpd.h"

/* Looks up login's crypt(3) hash string (e.g. "$6$...") in shadow
 * (fallback passwd). MT_ERR_NOENT: user not found, or found with an
 * empty/"x"/"*" hash field ("no password", matches Go). out must be
 * >= 128 bytes. */
mt_err_t mt_auth_load_password_hash(const char *login, char *out, size_t out_len);

/* Testable core of the three functions above/below: shadow_path/
 * passwd_path are injectable so tests/unit/test_auth.c can point at temp
 * fixture files instead of the real /etc/shadow. The path-less
 * mt_auth_load_password_hash/mt_auth_authenticate/mt_auth_verify_token
 * are the production entry points, always passing MT_SHADOW_FILE/
 * MT_PASSWD_FILE (paths.h). */
mt_err_t mt_auth_load_password_hash_from(const char *shadow_path, const char *passwd_path,
                                         const char *login, char *out, size_t out_len);
mt_err_t mt_auth_authenticate_from(const char *shadow_path, const char *passwd_path,
                                   const char *state_dir, const char *login,
                                   const char *password, char *token_out, size_t token_out_len);
mt_err_t mt_auth_verify_token_from(const char *shadow_path, const char *passwd_path,
                                   const char *state_dir, const char *token);

/* Adds `years` *calendar* years to a UTC unix timestamp, matching Go's
 * time.Time.AddDate(years,0,0) exactly (including the Feb-29-rolls-to-
 * March-1 behaviour when the target year isn't a leap year). Exposed
 * mainly so tests/unit/test_auth.c can check it against vectors captured
 * from the real Go time package; mt_auth_authenticate is its only
 * production caller. */
int64_t mt_auth_add_years_utc(int64_t unix_ts, int years);

/* Loads (or generates+persists on first use) the HMAC signing secret
 * under state_dir. Cached after the first successful load; out must be
 * >= 32 bytes. */
mt_err_t mt_auth_load_secret(const char *state_dir, uint8_t *out, size_t *out_len);

/* login+password -> signed JWT (20-year expiry from now), or an error
 * when credentials are missing, the user is unknown/has no password, or
 * the password doesn't match. token_out must be >= MT_JWT_MAX_TOKEN. */
mt_err_t mt_auth_authenticate(const char *state_dir, const char *login, const char *password,
                              char *token_out, size_t token_out_len);

/* Full verify: parse (unverified) to find the claimed subject -> look up
 * that user's *current* password hash -> re-derive the signing key ->
 * verify signature + sub/iss/exp. MT_ERR_INVAL on any failure (malformed
 * token, unknown user, bad signature, wrong issuer, expired). */
mt_err_t mt_auth_verify_token(const char *state_dir, const char *token);

/* Supplies the two pieces of app state the auth layer needs without
 * depending on the app module directly: whether HTTPWeb.Auth.Enabled is
 * currently true, and the config's state directory (for the secret
 * file). */
typedef bool (*mt_auth_enabled_fn)(void *ud);
typedef const char *(*mt_auth_state_dir_fn)(void *ud);

typedef struct mt_auth_ctx {
    mt_auth_enabled_fn enabled;
    mt_auth_state_dir_fn state_dir;
    void *ud;
} mt_auth_ctx_t;

/* Register as the TCP server's mt_httpd middleware (never on the Unix
 * socket instance, matching Go: unixsocket.go never wraps auth).
 * Replicates http.go's own gating in a single function since mt_httpd_t
 * applies one middleware to every request: non-"/api/" paths (static
 * files) always proceed; any "/api/" path proceeds unchecked when auth is
 * disabled or the path is exactly "/api/v1/auth"; otherwise requires a
 * valid "Authorization: Bearer <jwt>". ud must be a `mt_auth_ctx_t *`. */
bool mt_auth_middleware(mt_http_req_t *req, mt_http_res_t *res, void *ud);

/* GET /api/v1/auth -> {"enabled": bool}. ud must be a `mt_auth_ctx_t *`. */
void mt_auth_status_handler(mt_http_req_t *req, mt_http_res_t *res, void *ud);
/* POST /api/v1/auth -> {"token": "..."}; 404 if auth disabled, 400 on
 * bad/missing JSON body, 403 on bad credentials. ud must be a
 * `mt_auth_ctx_t *`. */
void mt_auth_login_handler(mt_http_req_t *req, mt_http_res_t *res, void *ud);

#endif /* MAGITRICKLE_AUTH_H */
