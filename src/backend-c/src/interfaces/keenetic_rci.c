/* See keenetic_rci.h. Port of Go's
 * internal/interfaces/keenetic_router_specific.go. */
#include "magitrickle/keenetic_rci.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "magitrickle/log.h"

/* ---- small helpers ------------------------------------------------------------ */

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* Copies `src` into `dst` with surrounding whitespace removed,
 * truncating to fit. Go used strings.TrimSpace, which trims Unicode
 * space; RCI labels are practically ASCII here, and trimming only ASCII
 * space is the conservative difference (a stray U+00A0 stays part of
 * the label rather than silently vanishing). */
static void copy_trimmed(char *dst, size_t dst_sz, const char *src) {
    dst[0] = '\0';
    if (!src) { return; }

    while (*src && is_space(*src)) { src++; }
    size_t len = strlen(src);
    while (len > 0 && is_space(src[len - 1])) { len--; }
    if (len >= dst_sz) { len = dst_sz - 1; }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Reads a string member, trimmed; leaves dst empty when absent or not a
 * string (Go's struct decode would leave the field at its zero value). */
static void copy_trimmed_member(char *dst, size_t dst_sz, const cJSON *obj, const char *key) {
    dst[0] = '\0';
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(m)) { copy_trimmed(dst, dst_sz, m->valuestring); }
}

/* ---- parsing ------------------------------------------------------------------ */

mt_err_t mt_kn_parse_interface_list(const char *json, mt_kn_iface_meta_t **out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    if (!json) { return MT_ERR_INVAL; }

    cJSON *root = cJSON_Parse(json);
    if (!root) { return MT_ERR_PROTO; }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return MT_ERR_PROTO;
    }

    size_t cap = 0;
    size_t n = 0;
    mt_kn_iface_meta_t *arr = NULL;

    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, root) {
        /* Go decoded each value into a struct and skipped entries whose
         * decode failed; a non-object value is exactly that case. */
        if (!cJSON_IsObject(entry) || !entry->string) { continue; }

        if (n == cap) {
            size_t new_cap = cap ? cap * 2 : 8;
            mt_kn_iface_meta_t *na = realloc(arr, new_cap * sizeof(*na));
            if (!na) {
                free(arr);
                cJSON_Delete(root);
                return MT_ERR_NOMEM;
            }
            arr = na;
            cap = new_cap;
        }

        memset(&arr[n], 0, sizeof(arr[n]));
        snprintf(arr[n].id, sizeof(arr[n].id), "%s", entry->string);
        copy_trimmed_member(arr[n].description, sizeof(arr[n].description), entry, "description");
        copy_trimmed_member(arr[n].interface_name, sizeof(arr[n].interface_name), entry,
                            "interface-name");
        n++;
    }

    cJSON_Delete(root);
    *out = arr;
    *out_n = n;
    return MT_OK;
}

char *mt_kn_build_system_name_request(const mt_kn_iface_meta_t *metas, size_t n) {
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
        cJSON_AddItemToObject(show, "interface", iface);
        cJSON_AddItemToObject(item, "show", show);
        cJSON_AddItemToArray(arr, item);
    }

    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return body;
}

mt_err_t mt_kn_parse_system_names(const char *json, mt_kn_iface_meta_t *metas, size_t n) {
    if (!json) { return MT_ERR_INVAL; }

    cJSON *root = cJSON_Parse(json);
    if (!root) { return MT_ERR_PROTO; }
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return MT_ERR_PROTO;
    }

    size_t i = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (i >= n) { break; } /* Go: `if i >= len(interfaceIDs) { break }` */

        const cJSON *show = cJSON_GetObjectItemCaseSensitive(item, "show");
        const cJSON *iface = cJSON_GetObjectItemCaseSensitive(show, "interface");
        copy_trimmed_member(metas[i].system_name, sizeof(metas[i].system_name), iface,
                            "system-name");
        i++;
    }

    cJSON_Delete(root);
    return MT_OK;
}

mt_err_t mt_kn_build_aliases(const mt_kn_iface_meta_t *metas, size_t n, mt_kn_aliases_t *out) {
    out->items = NULL;
    out->n = 0;
    if (n == 0) { return MT_OK; }

    mt_kn_alias_t *arr = calloc(n, sizeof(*arr));
    if (!arr) { return MT_ERR_NOMEM; }

    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        /* Trim here as well as at parse time. Go trimmed at exactly this
         * point, and doing so keeps this entry point correct on its own,
         * for callers that populate metas by other means than the
         * parsers above. Trimming already-trimmed input is a no-op. */
        char system_name[sizeof(metas[i].system_name)];
        copy_trimmed(system_name, sizeof(system_name), metas[i].system_name);
        if (system_name[0] == '\0') { continue; }

        char alias[sizeof(arr[count].alias)];
        copy_trimmed(alias, sizeof(alias), metas[i].description);
        if (alias[0] == '\0') { copy_trimmed(alias, sizeof(alias), metas[i].interface_name); }
        if (alias[0] == '\0' || strcmp(alias, system_name) == 0) { continue; }

        snprintf(arr[count].system_name, sizeof(arr[count].system_name), "%s", system_name);
        snprintf(arr[count].alias, sizeof(arr[count].alias), "%s", alias);
        count++;
    }

    if (count == 0) {
        free(arr);
        return MT_OK;
    }
    out->items = arr;
    out->n = count;
    return MT_OK;
}

const char *mt_kn_aliases_lookup(const mt_kn_aliases_t *aliases, const char *system_name) {
    if (!aliases || !aliases->items || !system_name) { return NULL; }
    for (size_t i = 0; i < aliases->n; i++) {
        if (strcmp(aliases->items[i].system_name, system_name) == 0) {
            return aliases->items[i].alias;
        }
    }
    return NULL;
}

void mt_kn_aliases_free(mt_kn_aliases_t *aliases) {
    if (!aliases) { return; }
    free(aliases->items);
    aliases->items = NULL;
    aliases->n = 0;
}

/* ---- RCI transport ------------------------------------------------------------ */

typedef struct rci_buf {
    char *data;
    size_t len;
    size_t cap;
    bool truncated;
} rci_buf_t;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    rci_buf_t *buf = ud;
    size_t n = size * nmemb;
    if (buf->len + n > (size_t)MT_KN_RCI_MAX_BODY_BYTES) {
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

/* Performs one RCI call. `post_body` NULL means GET. On success stores a
 * NUL-terminated body in *out_body (caller frees). */
static mt_err_t rci_call(const char *url, const char *post_body, char **out_body) {
    *out_body = NULL;

    CURL *curl = curl_easy_init();
    if (!curl) { return MT_ERR_NOMEM; }

    rci_buf_t buf = {0};
    struct curl_slist *headers = NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, MT_KN_RCI_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (post_body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!headers) {
            curl_easy_cleanup(curl);
            return MT_ERR_NOMEM;
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(post_body));
    }

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK) { curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status); }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        free(buf.data);
        if (buf.truncated) {
            MT_DEBUG("keenetic rci: %s response exceeded %d bytes", url,
                     MT_KN_RCI_MAX_BODY_BYTES);
            return MT_ERR_LIMIT;
        }
        MT_DEBUG("keenetic rci: %s failed: %s", url, curl_easy_strerror(rc));
        return rc == CURLE_OPERATION_TIMEDOUT ? MT_ERR_TIMEOUT : MT_ERR_UPSTREAM;
    }
    if (status != 200) {
        free(buf.data);
        MT_DEBUG("keenetic rci: %s returned status %ld", url, status);
        return MT_ERR_UPSTREAM;
    }
    if (!buf.data) {
        /* 200 with an empty body: nothing to parse. */
        free(buf.data);
        return MT_ERR_PROTO;
    }

    *out_body = buf.data;
    return MT_OK;
}

mt_err_t mt_kn_get_iface_aliases_from(const char *base_url, mt_kn_aliases_t *out) {
    out->items = NULL;
    out->n = 0;
    if (!base_url) { return MT_ERR_INVAL; }

    /* Trim a trailing '/' so base + "/rci/..." never doubles it, as
     * Go's strings.TrimRight(a.BaseURL, "/") did. */
    size_t base_len = strlen(base_url);
    while (base_len > 0 && base_url[base_len - 1] == '/') { base_len--; }

    char list_url[512];
    snprintf(list_url, sizeof(list_url), "%.*s/rci/show/interface", (int)base_len, base_url);

    char *list_body = NULL;
    mt_err_t err = rci_call(list_url, NULL, &list_body);
    if (err != MT_OK) { return err; }

    mt_kn_iface_meta_t *metas = NULL;
    size_t n = 0;
    err = mt_kn_parse_interface_list(list_body, &metas, &n);
    free(list_body);
    if (err != MT_OK) { return err; }

    /* Go returned an empty map without issuing the batch call. */
    if (n == 0) {
        free(metas);
        return MT_OK;
    }

    char *req_body = mt_kn_build_system_name_request(metas, n);
    if (!req_body) {
        free(metas);
        return MT_ERR_NOMEM;
    }

    char batch_url[512];
    snprintf(batch_url, sizeof(batch_url), "%.*s/rci/", (int)base_len, base_url);

    char *batch_body = NULL;
    err = rci_call(batch_url, req_body, &batch_body);
    free(req_body);
    if (err != MT_OK) {
        free(metas);
        return err;
    }

    err = mt_kn_parse_system_names(batch_body, metas, n);
    free(batch_body);
    if (err != MT_OK) {
        free(metas);
        return err;
    }

    err = mt_kn_build_aliases(metas, n, out);
    free(metas);
    return err;
}

mt_err_t mt_kn_get_iface_aliases(mt_kn_aliases_t *out) {
#ifdef MT_ENTWARE_KN
    return mt_kn_get_iface_aliases_from(MT_KN_RCI_BASE_URL, out);
#else
    /* Go's DummyRouterSpecificAPI: an empty map and no error, so
     * interfaces.List simply reports no friendly names. */
    out->items = NULL;
    out->n = 0;
    return MT_OK;
#endif
}
