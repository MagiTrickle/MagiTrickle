/* Shared local RCI transport and read-only system-name batch. */
#include "magitrickle/keenetic_rci.h"

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>

char *mt_kn_build_system_name_request(const mt_kn_ifinfo_t *metas, size_t n) {
    if (n && !metas) { return NULL; }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) { return NULL; }

    for (size_t i = 0; i < n; i++) {
        cJSON *iface = cJSON_CreateObject();
        cJSON *show = cJSON_CreateObject();
        cJSON *item = cJSON_CreateObject();
        if (!iface || !show || !item) {
            cJSON_Delete(iface);
            cJSON_Delete(show);
            cJSON_Delete(item);
            cJSON_Delete(arr);
            return NULL;
        }

        /* Field order matches Go's struct definition so the two produce
         * byte-identical bodies. */
        if (!cJSON_AddStringToObject(iface, "name", metas[i].id) ||
            !cJSON_AddStringToObject(iface, "details", "yes") ||
            !cJSON_AddStringToObject(iface, "system-name", "yes")) {
            cJSON_Delete(iface);
            cJSON_Delete(show);
            cJSON_Delete(item);
            cJSON_Delete(arr);
            return NULL;
        }
        if (!cJSON_AddItemToObject(show, "interface", iface)) {
            cJSON_Delete(iface); cJSON_Delete(show); cJSON_Delete(item); cJSON_Delete(arr);
            return NULL;
        }
        if (!cJSON_AddItemToObject(item, "show", show)) {
            cJSON_Delete(show); cJSON_Delete(item); cJSON_Delete(arr);
            return NULL;
        }
        if (!cJSON_AddItemToArray(arr, item)) {
            cJSON_Delete(item); cJSON_Delete(arr);
            return NULL;
        }
    }

    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return body;
}

/* ---- RCI transport ------------------------------------------------------------ */

typedef struct rci_buf {
    char *data;
    size_t length, capacity, limit;
    mt_err_t error;
} rci_buf_t;

static size_t receive_rci(char *data, size_t size, size_t count, void *ud) {
    rci_buf_t *b = ud;
    if (count && size > SIZE_MAX / count) { b->error = MT_ERR_LIMIT; return 0; }
    size_t n = size * count;
    if (n > b->limit - b->length) { b->error = MT_ERR_LIMIT; return 0; }
    size_t needed = b->length + n + 1;
    if (needed > b->capacity) {
        size_t capacity = b->capacity ? b->capacity : 4096;
        while (capacity < needed) {
            if (capacity > b->limit / 2) { capacity = b->limit + 1; break; }
            capacity *= 2;
        }
        if (capacity > b->limit + 1) { capacity = b->limit + 1; }
        char *p = realloc(b->data, capacity);
        if (!p) { b->error = MT_ERR_NOMEM; return 0; }
        b->data = p; b->capacity = capacity;
    }
    memcpy(b->data + b->length, data, n);
    b->length += n; b->data[b->length] = '\0';
    return n;
}
static int rci_progress(void *ud, curl_off_t total, curl_off_t now,
                         curl_off_t up_total, curl_off_t up_now) {
    (void)total; (void)now; (void)up_total; (void)up_now;
    const atomic_bool *stop = ud;
    return stop && atomic_load(stop) ? 1 : 0;
}
mt_err_t mt_kn_rci_request(const char *url, const char *post, size_t limit,
                            const atomic_bool *stop, char **body, size_t *length) {
    if (!body || !length) { return MT_ERR_INVAL; }
    *body = NULL; *length = 0;
    if (!url || !limit || limit == SIZE_MAX || (post && strlen(post) > LONG_MAX)) {
        return MT_ERR_INVAL;
    }
    if (stop && atomic_load(stop)) { return MT_ERR_CANCELED; }
    CURL *curl = curl_easy_init();
    if (!curl) { return MT_ERR_NOMEM; }
    struct curl_slist *headers = NULL;
    if (post) {
        headers = curl_slist_append(NULL, "Content-Type: application/json");
        if (!headers) { curl_easy_cleanup(curl); return MT_ERR_NOMEM; }
    }
    rci_buf_t response = {.limit = limit};
    CURLcode setup = CURLE_OK;
#define SET(option, value) do { if (setup == CURLE_OK) { setup = curl_easy_setopt(curl, option, value); } } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_CONNECTTIMEOUT_MS, 500L);
    SET(CURLOPT_TIMEOUT_MS, 2000L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, receive_rci);
    SET(CURLOPT_WRITEDATA, &response);
    SET(CURLOPT_XFERINFOFUNCTION, rci_progress);
    SET(CURLOPT_XFERINFODATA, stop);
    SET(CURLOPT_NOPROGRESS, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, "http");
#else
    SET(CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTP);
#endif
    if (post) {
        SET(CURLOPT_HTTPHEADER, headers);
        SET(CURLOPT_POSTFIELDS, post);
        SET(CURLOPT_POSTFIELDSIZE, (long)strlen(post));
    }
#undef SET
    CURLcode result = setup == CURLE_OK ? curl_easy_perform(curl) : setup;
    long status = 0;
    if (result == CURLE_OK) { result = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status); }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    mt_err_t error = response.error;
    if (stop && atomic_load(stop)) { error = MT_ERR_CANCELED; }
    else if (error == MT_OK && result != CURLE_OK) {
        error = result == CURLE_OPERATION_TIMEDOUT ? MT_ERR_TIMEOUT :
                result == CURLE_ABORTED_BY_CALLBACK ? MT_ERR_CANCELED : MT_ERR_UPSTREAM;
    }
    if (error == MT_OK && status != 200) { error = MT_ERR_UPSTREAM; }
    if (error == MT_OK && !response.length) { error = MT_ERR_PROTO; }
    if (error != MT_OK) { free(response.data); return error; }
    *body = response.data; *length = response.length;
    return MT_OK;
}
