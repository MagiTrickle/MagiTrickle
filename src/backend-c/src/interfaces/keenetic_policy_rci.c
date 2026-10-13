#include "magitrickle/keenetic_policy_rci.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

static bool parse_mark(const cJSON *value, uint32_t *out)
{
    if (cJSON_IsNumber(value)) {
        double n = value->valuedouble;
        if (!isfinite(n) || n < 0 || n > MT_KN_POLICY_MASK || n != floor(n)) { return false; }
        *out = (uint32_t)n;
        return true;
    }
    if (!cJSON_IsString(value)) { return false; }
    const char *s = value->valuestring;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; }
    size_t n = strlen(s);
    if (!n || n > 8) { return false; }
    uint32_t mark = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') { d = (uint32_t)(c - '0'); }
        else if (c >= 'a' && c <= 'f') { d = (uint32_t)(c - 'a') + 10u; }
        else if (c >= 'A' && c <= 'F') { d = (uint32_t)(c - 'A') + 10u; }
        else { return false; }
        mark = mark * 16u + d;
    }
    if (mark > MT_KN_POLICY_MASK) { return false; }
    *out = mark;
    return true;
}

mt_err_t mt_kn_policy_parse(const char *json, size_t len, mt_kn_policy_entry_t **entries, size_t *count)
{
    *entries = NULL; *count = 0;
    if (!json || !len) { return MT_ERR_PROTO; }
    if (len > MT_KN_POLICY_BODY_LIMIT) { return MT_ERR_LIMIT; }
    if (memchr(json, '\0', len)) { return MT_ERR_PROTO; }
    /* cJSON's length-bounded parser accepts a nonterminated input. Check that
     * it consumed the whole document, including only optional JSON whitespace. */
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &end, false);
    while (end && end < json + len && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) { end++; }
    if (!cJSON_IsObject(root) || end != json + len) { cJSON_Delete(root); return MT_ERR_PROTO; }
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (status) { cJSON_Delete(root); return MT_ERR_UPSTREAM; }
    int number = cJSON_GetArraySize(root);
    if (number < 0 || (unsigned)number > MT_KN_POLICY_LIST_LIMIT) { cJSON_Delete(root); return MT_ERR_LIMIT; }
    /* An empty inventory owns no allocation. Keep it out of the population
     * path so that every entry below has non-NULL storage. */
    if (number == 0) { cJSON_Delete(root); return MT_OK; }
    mt_kn_policy_entry_t *out = calloc((size_t)number, sizeof(*out));
    if (!out) { cJSON_Delete(root); return MT_ERR_NOMEM; }
    size_t n = 0;
    mt_err_t err = MT_OK;
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsObject(item) || !item->string || !*item->string ||
            strlen(item->string) >= sizeof(out[n].id)) { err = MT_ERR_PROTO; break; }
        mt_kn_policy_config_t test = {.enabled = true};
        strcpy(test.policy_id, item->string);
        if (!mt_kn_policy_config_valid(&test)) { err = MT_ERR_PROTO; break; }
        for (size_t i = 0; i < n; i++) {
            if (strcmp(out[i].id, item->string) == 0) { err = MT_ERR_PROTO; break; }
        }
        if (err != MT_OK) { break; }
        strcpy(out[n].id, item->string);
        const cJSON *description = cJSON_GetObjectItemCaseSensitive(item, "description");
        if (description && !cJSON_IsString(description)) { err = MT_ERR_PROTO; break; }
        const char *label = description ? description->valuestring : item->string;
        if (strlen(label) >= sizeof(out[n].description)) { err = MT_ERR_LIMIT; break; }
        strcpy(out[n].description, label);
        const cJSON *mark = cJSON_GetObjectItemCaseSensitive(item, "mark");
        if (mark && !cJSON_IsNull(mark)) {
            if (!parse_mark(mark, &out[n].mark)) { err = MT_ERR_PROTO; break; }
            out[n].available = true;
        }
        n++;
    }
    cJSON_Delete(root);
    if (err != MT_OK) { free(out); return err; }
    *entries = out; *count = n;
    return MT_OK;
}

typedef struct { char *data; size_t len; mt_err_t error; } response_t;
static size_t receive_body(char *data, size_t size, size_t count, void *ud)
{
    response_t *r = ud;
    if (count && size > SIZE_MAX / count) { r->error = MT_ERR_LIMIT; return 0; }
    size_t n = size * count;
    if (n > MT_KN_POLICY_BODY_LIMIT - r->len) { r->error = MT_ERR_LIMIT; return 0; }
    char *next = realloc(r->data, r->len + n + 1);
    if (!next) { r->error = MT_ERR_NOMEM; return 0; }
    r->data = next;
    memcpy(next + r->len, data, n); r->len += n; next[r->len] = '\0';
    return n;
}
static int progress(void *ud, curl_off_t total, curl_off_t now, curl_off_t up_total, curl_off_t up_now)
{
    (void)total; (void)now; (void)up_total; (void)up_now;
    const atomic_bool *stop = ud;
    return stop && atomic_load(stop) ? 1 : 0;
}
mt_err_t mt_kn_policy_fetch(mt_kn_policy_entry_t **entries, size_t *count, const atomic_bool *stop)
{
    *entries = NULL; *count = 0;
    CURL *curl = curl_easy_init();
    if (!curl) { return MT_ERR_NOMEM; }
    response_t response = {0};
    CURLcode setup = CURLE_OK;
#define SET(option, value) do { if (setup == CURLE_OK) { setup = curl_easy_setopt(curl, option, value); } } while (0)
    SET(CURLOPT_URL, "http://127.0.0.1:79/rci/show/ip/policy");
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_CONNECTTIMEOUT_MS, 500L);
    SET(CURLOPT_TIMEOUT_MS, 2000L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, receive_body);
    SET(CURLOPT_WRITEDATA, &response);
    SET(CURLOPT_XFERINFOFUNCTION, progress);
    SET(CURLOPT_XFERINFODATA, stop);
    SET(CURLOPT_NOPROGRESS, 0L);
#undef SET
    CURLcode result = setup == CURLE_OK ? curl_easy_perform(curl) : setup;
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    mt_err_t err = response.error;
    if (err == MT_OK && result != CURLE_OK) {
        err = result == CURLE_ABORTED_BY_CALLBACK ? MT_ERR_CANCELED :
              result == CURLE_OPERATION_TIMEDOUT ? MT_ERR_TIMEOUT : MT_ERR_UPSTREAM;
    }
    if (err == MT_OK && status != 200) { err = MT_ERR_UPSTREAM; }
    if (err == MT_OK) { err = mt_kn_policy_parse(response.data, response.len, entries, count); }
    free(response.data);
    return err;
}
