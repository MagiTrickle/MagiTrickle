/* Subscription list fetch — port of subscriptions/fetch.go's FetchList.
 * Uses libcurl (dependencies.md: "TLS is non-negotiable for https
 * subscription URLs and hand-rolling TLS is out of the question").
 *
 * Replicates Go's exact redirect semantics via libcurl's URL API +
 * FOLLOWLOCATION off, not libcurl's own automatic redirect handling
 * (which follows more status codes and doesn't cap identically):
 * - only HTTP 301/302 are followed as redirects; any other 3xx is
 *   treated as a plain non-2xx terminal failure (a real Go quirk, kept
 *   deliberately -- 303/307/308 are NOT special-cased);
 * - at most MT_SUB_FETCH_MAX_REDIRECTS hops, loop detection against
 *   every URL visited so far (including the original);
 * - only http/https schemes with a non-empty host are accepted, at the
 *   original URL and at every redirect target.
 *
 * mt_sub_fetch_list's MT_SUB_FETCH_MAX_BODY_BYTES cap is C-side hardening
 * (dependencies.md's "size bound") -- Go's io.ReadAll has no such limit.
 * No real subscription list approaches this size; it exists only to
 * bound memory against a hostile or misbehaving server.
 */
#ifndef MAGITRICKLE_SUB_FETCH_H
#define MAGITRICKLE_SUB_FETCH_H

#include <stddef.h>

#include "magitrickle/err.h"
#include "magitrickle/loop.h"
#include <stdatomic.h>
#include <stdbool.h>

#define MT_SUB_FETCH_TIMEOUT_SECONDS 15
#define MT_SUB_FETCH_MAX_REDIRECTS 5
#define MT_SUB_FETCH_MAX_BODY_BYTES ((size_t)8 * 1024 * 1024)

/* Must be called once at process startup before any thread calls
 * mt_sub_fetch_list (libcurl's global init is not thread-safe to call
 * concurrently) -- matches curl_global_init's own documented contract.
 * mt_sub_fetch_global_cleanup should be called once at shutdown. */
void mt_sub_fetch_global_init(void);
void mt_sub_fetch_global_cleanup(void);

/* Fetches `url`, following only 301/302 redirects (see header comment),
 * up to MT_SUB_FETCH_MAX_REDIRECTS hops. On success, *out_body is a
 * NUL-terminated malloc'd buffer of *out_len bytes (caller frees).
 * Errors: MT_ERR_INVAL (malformed URL, unsupported scheme, redirect
 * loop, missing/malformed redirect Location), MT_ERR_LIMIT (too many
 * redirects, or body exceeds MT_SUB_FETCH_MAX_BODY_BYTES), MT_ERR_PROTO
 * (non-2xx terminal response), MT_ERR_IO (network/TLS/timeout
 * failure), MT_ERR_NOMEM. */
mt_err_t mt_sub_fetch_list(const char *url, char **out_body, size_t *out_len);

/* Cancellable worker primitive; NULL cancel keeps the synchronous contract. */
mt_err_t mt_sub_fetch_list_cancel(const char *url, char **body, size_t *len,
                                 const atomic_bool *cancel);

typedef struct mt_sub_fetcher mt_sub_fetcher_t;
typedef void (*mt_sub_fetch_done_fn)(void *ud, mt_err_t err, const char *body, size_t len);
/* Two bounded workers perform network I/O only. submit/done/destroy run
 * on the loop owner; done borrows body only for the callback. At most 32
 * queued/active/completed requests exist. Accepted jobs get exactly one
 * callback, including MT_ERR_CANCELED during destroy. Destroy/join before
 * freeing callback contexts or calling curl_global_cleanup. */
mt_err_t mt_sub_fetcher_create(mt_loop_t *loop, mt_sub_fetcher_t **out);
mt_err_t mt_sub_fetcher_submit(mt_sub_fetcher_t *fetcher, const char *url,
                              mt_sub_fetch_done_fn done, void *ud);
void mt_sub_fetcher_destroy(mt_sub_fetcher_t *fetcher);

#endif /* MAGITRICKLE_SUB_FETCH_H */
