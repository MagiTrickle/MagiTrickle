/* See sub_fetch.h. Port of subscriptions/fetch.go's FetchList. */
#include "magitrickle/sub_fetch.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>

void mt_sub_fetch_global_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

void mt_sub_fetch_global_cleanup(void) { curl_global_cleanup(); }

static bool url_is_supported(const char *url) {
    CURLU *cu = curl_url();
    if (!cu) { return false; }
    bool ok = false;
    if (curl_url_set(cu, CURLUPART_URL, url, 0) == CURLUE_OK) {
        char *scheme = NULL;
        char *host = NULL;
        curl_url_get(cu, CURLUPART_SCHEME, &scheme, 0);
        curl_url_get(cu, CURLUPART_HOST, &host, 0);
        ok = scheme && host && host[0] != '\0' &&
             (strcasecmp(scheme, "http") == 0 || strcasecmp(scheme, "https") == 0);
        curl_free(scheme);
        curl_free(host);
    }
    curl_url_cleanup(cu);
    return ok;
}

typedef struct fetch_buf {
    char *data;
    size_t len;
    size_t cap;
    bool truncated;
} fetch_buf_t;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    fetch_buf_t *buf = ud;
    size_t n = size * nmemb;
    if (buf->len + n > MT_SUB_FETCH_MAX_BODY_BYTES) {
        buf->truncated = true;
        return 0; /* abort the transfer */
    }
    if (buf->len + n + 1 > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 4096;
        while (new_cap < buf->len + n + 1) { new_cap *= 2; }
        char *na = realloc(buf->data, new_cap);
        if (!na) { return 0; }
        buf->data = na;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return n;
}

/* Bounded by MT_SUB_FETCH_MAX_REDIRECTS+1 (the original URL plus every
 * successfully-followed redirect target) -- a fixed array is simpler
 * than a hash set at this size and matches the small, bounded nature of
 * the loop it backs. */
typedef struct visited_set {
    char *urls[MT_SUB_FETCH_MAX_REDIRECTS + 1];
    size_t n;
} visited_set_t;

static bool visited_contains(const visited_set_t *v, const char *url) {
    for (size_t i = 0; i < v->n; i++) {
        if (strcmp(v->urls[i], url) == 0) { return true; }
    }
    return false;
}

static mt_err_t visited_add(visited_set_t *v, const char *url) {
    if (v->n >= sizeof(v->urls) / sizeof(v->urls[0])) { return MT_ERR_LIMIT; }
    char *copy = strdup(url);
    if (!copy) { return MT_ERR_NOMEM; }
    v->urls[v->n++] = copy;
    return MT_OK;
}

static void visited_clear(visited_set_t *v) {
    for (size_t i = 0; i < v->n; i++) { free(v->urls[i]); }
    v->n = 0;
}

static int transfer_progress(void *ud, curl_off_t total_down, curl_off_t down,
                             curl_off_t total_up, curl_off_t up) {
    (void)total_down; (void)down; (void)total_up; (void)up;
    const atomic_bool *cancel = ud;
    return cancel && atomic_load(cancel) ? 1 : 0;
}

mt_err_t mt_sub_fetch_list(const char *url, char **out_body, size_t *out_len) {
    return mt_sub_fetch_list_cancel(url, out_body, out_len, NULL);
}

mt_err_t mt_sub_fetch_list_cancel(const char *url, char **out_body, size_t *out_len,
                                  const atomic_bool *cancel) {
    *out_body = NULL;
    *out_len = 0;

    visited_set_t visited = {0};
    char *current = strdup(url);
    if (!current) { return MT_ERR_NOMEM; }

    mt_err_t result = MT_ERR_SYS;
    int redirects = 0;
    for (;;) {
        if (cancel && atomic_load(cancel)) { result = MT_ERR_CANCELED; break; }
        if (!url_is_supported(current)) {
            result = MT_ERR_INVAL;
            break;
        }
        if (visited_contains(&visited, current)) {
            result = MT_ERR_INVAL; /* redirect loop detected */
            break;
        }
        mt_err_t verr = visited_add(&visited, current);
        if (verr != MT_OK) {
            result = verr;
            break;
        }

        CURL *curl = curl_easy_init();
        if (!curl) {
            result = MT_ERR_NOMEM;
            break;
        }
        fetch_buf_t buf = {0};
        curl_easy_setopt(curl, CURLOPT_URL, current);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)MT_SUB_FETCH_TIMEOUT_SECONDS);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);

        CURLcode rc = curl_easy_perform(curl);
        if (rc != CURLE_OK) {
            free(buf.data);
            curl_easy_cleanup(curl);
            result = (cancel && atomic_load(cancel)) ? MT_ERR_CANCELED :
                     (buf.truncated ? MT_ERR_LIMIT : MT_ERR_IO);
            break;
        }

        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

        if (status == 301 || status == 302) {
            char *location = NULL;
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
            if (!location) {
                free(buf.data);
                curl_easy_cleanup(curl);
                result = MT_ERR_INVAL; /* bad/missing redirect location */
                break;
            }
            if (!url_is_supported(location)) {
                free(buf.data);
                curl_easy_cleanup(curl);
                result = MT_ERR_INVAL;
                break;
            }
            if (visited_contains(&visited, location)) {
                free(buf.data);
                curl_easy_cleanup(curl);
                result = MT_ERR_INVAL; /* redirect loop detected */
                break;
            }
            if (redirects >= MT_SUB_FETCH_MAX_REDIRECTS) {
                free(buf.data);
                curl_easy_cleanup(curl);
                result = MT_ERR_LIMIT; /* too many redirects */
                break;
            }
            char *next = strdup(location);
            free(buf.data);
            curl_easy_cleanup(curl);
            if (!next) {
                result = MT_ERR_NOMEM;
                break;
            }
            free(current);
            current = next;
            redirects++;
            continue;
        }

        curl_easy_cleanup(curl);
        if (status < 200 || status >= 300) {
            free(buf.data);
            result = MT_ERR_PROTO;
            break;
        }

        if (!buf.data) {
            /* Empty (0-byte) body: write_cb was never called, so the
             * buffer was never allocated -- always hand back a valid,
             * NUL-terminated, heap-owned empty string rather than NULL. */
            buf.data = calloc(1, 1);
            if (!buf.data) {
                result = MT_ERR_NOMEM;
                break;
            }
        }
        *out_body = buf.data;
        *out_len = buf.len;
        result = MT_OK;
        break;
    }

    free(current);
    visited_clear(&visited);
    return result;
}
