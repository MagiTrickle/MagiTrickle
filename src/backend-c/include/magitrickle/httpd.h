/* Bounded HTTP/1.1 server on mt_loop (decisions.md D-06) — own
 * implementation, not a library: this is a small, purpose-built server
 * for a bounded API surface (JSON API plus in-memory static file serving), not a general
 * web server. Shared between a TCP listener (HTTPWeb.Host) and a Unix
 * socket listener (SockPath) — mirrors api/http.go and api/unixsocket.go
 * mounting the same v1 router.
 *
 * Compatibility-contract.md §2 explicitly allows bounded hardening here
 * ("C version may add bounded limits — document as hardening... keep
 * normal-size behaviour identical"): header block, body size, connection
 * count, and per-connection idle time are all capped (see the MT_HTTPD_*
 * constants below) where Go's zero-value http.Server has none. No real
 * small request behavior is preserved; bulk rule routes have a separate cap.
 *
 * Supports HTTP/1.1 keep-alive (non-pipelined: one request read to
 * completion, response written, then the next request read from the same
 * connection) and HTTP/1.0/`Connection: close` single-request mode.
 * Chunked request bodies are NOT supported (every real client here sends
 * fixed Content-Length JSON) — see decisions.md.
 *
 * Path patterns use "{name}" segments for parameters (e.g.
 * "/groups/{groupID}/rules/{ruleID}"), matched positionally, one capture
 * per non-literal segment. A leading/trailing slash mismatch is
 * tolerated (normalized) rather than treated as distinct routes — a
 * deliberate, documented simplification of chi's stricter behaviour
 * (decisions.md), since it only ever *broadens* what matches.
 */
#ifndef MAGITRICKLE_HTTPD_H
#define MAGITRICKLE_HTTPD_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "magitrickle/err.h"
#include "magitrickle/loop.h"

#define MT_HTTPD_MAX_HEADER_BYTES 8192
#define MT_HTTPD_MAX_BODY_BYTES ((size_t)1024 * 1024)
/* Large rule edits/imports only; unrelated routes keep the 1 MiB cap.
 * Aggregate in-flight input reservation is per listener, released after each
 * response (including keep-alive), timeout, error and disconnect. */
#define MT_HTTPD_MAX_RULE_BODY_BYTES ((size_t)16 * 1024 * 1024)
#define MT_HTTPD_MAX_INFLIGHT_BODY_BYTES ((size_t)32 * 1024 * 1024)
#define MT_HTTPD_MAX_CONNS 64
#define MT_HTTPD_MAX_PARAMS 4
#define MT_HTTPD_IDLE_TIMEOUT_MS 30000

typedef struct mt_httpd mt_httpd_t;
typedef struct mt_http_req mt_http_req_t;
typedef struct mt_http_res mt_http_res_t;
typedef struct mt_http_deferred mt_http_deferred_t;
/* Defer only during a handler. Complete exactly once, on the loop thread.
 * Completion remains safe after peer disconnect/server destruction. On a
 * failed enqueue cancel the deferral and write a normal immediate error. */
mt_http_deferred_t *mt_http_res_defer(mt_http_req_t *req, mt_http_res_t *res);
void mt_http_res_cancel_defer(mt_http_res_t *res);
void mt_http_deferred_json(mt_http_deferred_t *pending, int status, cJSON *obj);
void mt_http_deferred_error(mt_http_deferred_t *pending, int status, const char *msg);

typedef void (*mt_http_handler_fn)(mt_http_req_t *req, mt_http_res_t *res, void *ud);

/* Returns true to let the request proceed to the matched route handler;
 * false if the middleware already wrote a response (e.g. 401) and
 * dispatch should stop. Applied to every request on the server instance
 * it's registered on (callers register it only on the TCP instance,
 * gated internally on path, matching Go's `/api/` + `/api/v1/auth`
 * exemption -- see auth.c). */
typedef bool (*mt_http_middleware_fn)(mt_http_req_t *req, mt_http_res_t *res, void *ud);

mt_err_t mt_httpd_create(mt_loop_t *loop, mt_httpd_t **out);
void mt_httpd_destroy(mt_httpd_t *h);

mt_err_t mt_httpd_listen_tcp(mt_httpd_t *h, const char *addr, uint16_t port);
/* Removes any existing socket file at `path` first (matches Go's
 * os.Remove-if-exists in SetupUnixSocket), then binds/listens. */
mt_err_t mt_httpd_listen_unix(mt_httpd_t *h, const char *path);

/* method: "GET"/"PUT"/"POST"/"DELETE" (exact match). pattern: e.g.
 * "/api/v1/groups/{groupID}". Routes are matched in registration order;
 * the first matching (method, pattern) wins. */
mt_err_t mt_httpd_route(mt_httpd_t *h, const char *method, const char *pattern,
                        mt_http_handler_fn fn, void *ud);

/* Optional: called before route dispatch for every request on this
 * server instance. */
void mt_httpd_set_middleware(mt_httpd_t *h, mt_http_middleware_fn fn, void *ud);

/* Optional: called when no route matches (e.g. static file serving on
 * the TCP instance); when unset, an unmatched request gets a plain 404
 * JSON error. */
void mt_httpd_set_not_found(mt_httpd_t *h, mt_http_handler_fn fn, void *ud);

/* ---- request accessors ---------------------------------------------------- */

const char *mt_http_req_method(const mt_http_req_t *req);
/* Decoded path, no query string, percent-decoded, path-cleaned (matches
 * Go's path.Clean(r.URL.Path) used by the static handler). */
const char *mt_http_req_path(const mt_http_req_t *req);
/* NULL if the query key is absent; value is NOT percent-decoded further
 * beyond the initial parse (matches how the Go handlers only ever check
 * for literal "true"/"false"/URLs, no embedded '&' or '='). */
const char *mt_http_req_query(const mt_http_req_t *req, const char *key);
/* true iff mt_http_req_query(req,key) exists and equals "true" exactly,
 * matching Go's `r.URL.Query().Get(x) == "true"` idiom used throughout
 * the handlers. */
bool mt_http_req_query_is_true(const mt_http_req_t *req, const char *key);
const char *mt_http_req_param(const mt_http_req_t *req, const char *name);
const char *mt_http_req_header(const mt_http_req_t *req, const char *name);
const uint8_t *mt_http_req_body(const mt_http_req_t *req, size_t *len);

/* ---- response building ---------------------------------------------------- */

void mt_http_res_set_header(mt_http_res_t *res, const char *name, const char *value);
/* Sets status + Content-Type + raw body bytes (copied). Overwrites any
 * previously set body. */
void mt_http_res_write(mt_http_res_t *res, int status, const char *content_type,
                       const uint8_t *data, size_t len);
/* Convenience: status + application/json body from a cJSON value (which
 * this call deletes). */
void mt_http_res_write_json(mt_http_res_t *res, int status, cJSON *obj);
/* Convenience: {"error": msg} via mt_json_error, matching
 * api/utils.WriteError. */
void mt_http_res_write_error(mt_http_res_t *res, int status, const char *msg);

#endif /* MAGITRICKLE_HTTPD_H */
