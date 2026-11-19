/* Shared local RCI transport and read-only system-name batch. */
#include "magitrickle/keenetic_rci.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "magitrickle/log.h"

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

/* ---- SCGI fallback ------------------------------------------------------------ */

/* KeeneticOS 5.1.9 answers every request on nginx's 127.0.0.1:79 listener with
 * 403 "X-Detail: 0x1867, invalid query string": ndm now requires the
 * AUTH_ALLOWED parameter, which that listener does not pass. nginx itself only
 * forwards to the ndm SCGI socket, so the same read-only RCI is reachable there.
 * The parameters follow nginx's /rci location; without HTTP_HOST the firmware
 * answers 400 "0xcc56", without REMOTE_ADDR 500, and without NORMALIZED_URI an
 * HTML page instead of JSON. The switch is one-way for the process: firmware
 * changes need a reboot, which restarts the daemon. Tests redirect both
 * endpoints to their own servers. */
#define RCI_LOOPBACK_CLOSED "0x1867"
#define RCI_SCGI_HEAD_LIMIT 8192
#define RCI_POLL_SLICE_MS 50
static const char *rci_base_url = MT_KN_RCI_BASE_URL;
static const char *rci_scgi_socket = MT_KN_RCI_SCGI_SOCKET;
static atomic_bool rci_use_scgi;

static size_t receive_rci_header(char *data, size_t size, size_t count, void *ud) {
    bool *loopback_closed = ud;
    if (count && size > SIZE_MAX / count) { return 0; }
    size_t n = size * count;
    static const char name[] = "X-Detail:";
    const size_t name_length = sizeof(name) - 1;
    if (n > name_length && strncasecmp(data, name, name_length) == 0) {
        char value[128];
        size_t v = n - name_length;
        if (v > sizeof(value) - 1) { v = sizeof(value) - 1; }
        memcpy(value, data + name_length, v);
        value[v] = '\0';
        if (strstr(value, RCI_LOOPBACK_CLOSED)) { *loopback_closed = true; }
    }
    return n;
}

/* Path and query of a URL on the local RCI listener; NULL for anything else. */
static const char *rci_local_uri(const char *url) {
    size_t n = strlen(rci_base_url);
    return strncmp(url, rci_base_url, n) == 0 && url[n] == '/' ? url + n : NULL;
}

static struct timespec rci_deadline(long ms) {
    struct timespec t = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return t;
}
static long rci_ms_left(const struct timespec *deadline) {
    struct timespec now = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(deadline->tv_sec - now.tv_sec) * 1000L +
           (deadline->tv_nsec - now.tv_nsec) / 1000000L;
}
/* Poll in short slices so cancellation is observed as promptly as in libcurl. */
static mt_err_t rci_wait(int fd, short events, const struct timespec *deadline,
                         const atomic_bool *stop) {
    for (;;) {
        if (stop && atomic_load(stop)) { return MT_ERR_CANCELED; }
        long left = rci_ms_left(deadline);
        if (left <= 0) { return MT_ERR_TIMEOUT; }
        struct pollfd p = {.fd = fd, .events = events};
        int ready = poll(&p, 1, left < RCI_POLL_SLICE_MS ? (int)left : RCI_POLL_SLICE_MS);
        if (ready > 0) { return MT_OK; } /* readiness, HUP or error: the I/O call reports it */
        if (ready < 0 && errno != EINTR) { return MT_ERR_UPSTREAM; }
    }
}

/* Netstring of NUL-separated parameters (CONTENT_LENGTH first, as SCGI
 * requires), a comma, then the body. */
static char *rci_scgi_request(const char *uri, const char *post, size_t *length) {
    size_t post_length = post ? strlen(post) : 0;
    const char *query = strchr(uri, '?');
    char *path = strndup(uri, query ? (size_t)(query - uri) : strlen(uri));
    if (!path) { return NULL; }
    char content_length[24];
    snprintf(content_length, sizeof(content_length), "%zu", post_length);
    const char *params[][2] = {
        {"CONTENT_LENGTH", content_length},
        {"SCGI", "1"},
        {"REQUEST_METHOD", post ? "POST" : "GET"},
        {"REQUEST_URI", uri},
        {"NORMALIZED_URI", path},
        {"QUERY_STRING", query ? query + 1 : ""},
        {"CONTENT_TYPE", post ? "application/json" : ""},
        {"SERVER_PROTOCOL", "HTTP/1.1"},
        {"REMOTE_ADDR", "127.0.0.1"},
        {"SERVER_ADDR", "127.0.0.1"},
        {"SERVER_PORT", "79"},
        {"HTTP_HOST", "127.0.0.1:79"},
        {"SCGI_AGENT", "http/rci"},
        {"AUTH_ALLOWED", "1"},
    };
    const size_t count = sizeof(params) / sizeof(*params);
    size_t headers = 0;
    for (size_t i = 0; i < count; i++) {
        headers += strlen(params[i][0]) + 1 + strlen(params[i][1]) + 1;
    }
    char prefix[24];
    int prefix_length = snprintf(prefix, sizeof(prefix), "%zu:", headers);
    size_t total = (size_t)prefix_length + headers + 1 + post_length;
    char *out = malloc(total);
    if (!out) { free(path); return NULL; }
    size_t at = 0;
    memcpy(out, prefix, (size_t)prefix_length); at += (size_t)prefix_length;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < 2; j++) {
            size_t n = strlen(params[i][j]) + 1;
            memcpy(out + at, params[i][j], n); at += n;
        }
    }
    out[at++] = ',';
    /* The body is length-delimited by CONTENT_LENGTH, not a C string.
     * NOLINTNEXTLINE(bugprone-not-null-terminated-result) */
    if (post_length) { memcpy(out + at, post, post_length); }
    free(path);
    *length = total;
    return out;
}

/* CGI response head: "Status: 200 OK", other fields. Content-Length is
 * optional (-1); a missing or malformed Status is a protocol error. */
static mt_err_t rci_scgi_head(char *head, long *status, long long *content_length) {
    *status = 0; *content_length = -1;
    char *save = NULL;
    for (char *line = strtok_r(head, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') { line[n - 1] = '\0'; }
        char *value = strchr(line, ':');
        if (!value) { return MT_ERR_PROTO; }
        *value++ = '\0';
        while (*value == ' ' || *value == '\t') { value++; }
        char *end = NULL;
        errno = 0;
        if (strcasecmp(line, "Status") == 0) {
            long code = strtol(value, &end, 10);
            if (errno || end == value || code < 100 || code > 599 || (*end && *end != ' ')) {
                return MT_ERR_PROTO;
            }
            *status = code;
        } else if (strcasecmp(line, "Content-Length") == 0) {
            long long n_bytes = strtoll(value, &end, 10);
            if (errno || end == value || *end || n_bytes < 0) { return MT_ERR_PROTO; }
            *content_length = n_bytes;
        }
    }
    return *status ? MT_OK : MT_ERR_PROTO;
}

static mt_err_t rci_send(int fd, const char *data, size_t length,
                         const struct timespec *deadline, const atomic_bool *stop) {
    while (length) {
        if (stop && atomic_load(stop)) { return MT_ERR_CANCELED; }
        ssize_t n = send(fd, data, length, MSG_NOSIGNAL);
        if (n > 0) { data += n; length -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) { continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            mt_err_t err = rci_wait(fd, POLLOUT, deadline, stop);
            if (err != MT_OK) { return err; }
            continue;
        }
        return MT_ERR_UPSTREAM;
    }
    return MT_OK;
}
/* *received == 0 means EOF. */
static mt_err_t rci_recv(int fd, char *data, size_t capacity, size_t *received,
                         const struct timespec *deadline, const atomic_bool *stop) {
    for (;;) {
        if (stop && atomic_load(stop)) { return MT_ERR_CANCELED; }
        ssize_t n = recv(fd, data, capacity, 0);
        if (n >= 0) { *received = (size_t)n; return MT_OK; }
        if (errno == EINTR) { continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            mt_err_t err = rci_wait(fd, POLLIN, deadline, stop);
            if (err != MT_OK) { return err; }
            continue;
        }
        return MT_ERR_UPSTREAM;
    }
}

static mt_err_t rci_scgi_exchange(int fd, const char *uri, const char *post, size_t limit,
                                  const atomic_bool *stop, rci_buf_t *response) {
    const struct timespec connect_deadline = rci_deadline(500);
    const struct timespec deadline = rci_deadline(2000);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    size_t path_length = strlen(rci_scgi_socket);
    if (path_length >= sizeof(address.sun_path)) { return MT_ERR_INVAL; }
    memcpy(address.sun_path, rci_scgi_socket, path_length + 1);
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        if (errno != EINPROGRESS && errno != EINTR) { return MT_ERR_UPSTREAM; }
        mt_err_t err = rci_wait(fd, POLLOUT, &connect_deadline, stop);
        if (err != MT_OK) { return err; }
        int so_error = 0;
        socklen_t so_length = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_length) != 0 || so_error) {
            return MT_ERR_UPSTREAM;
        }
    }

    size_t request_length = 0;
    char *request = rci_scgi_request(uri, post, &request_length);
    if (!request) { return MT_ERR_NOMEM; }
    mt_err_t err = rci_send(fd, request, request_length, &deadline, stop);
    free(request);
    if (err != MT_OK) { return err; }

    char head[RCI_SCGI_HEAD_LIMIT + 1];
    size_t head_length = 0, body_start = 0;
    while (!body_start) {
        if (head_length == RCI_SCGI_HEAD_LIMIT) { return MT_ERR_PROTO; }
        size_t received = 0;
        err = rci_recv(fd, head + head_length, RCI_SCGI_HEAD_LIMIT - head_length,
                       &received, &deadline, stop);
        if (err != MT_OK) { return err; }
        if (!received) { return MT_ERR_PROTO; }
        head_length += received;
        head[head_length] = '\0';
        char *crlf = strstr(head, "\r\n\r\n"), *lf = strstr(head, "\n\n");
        if (crlf && (!lf || crlf < lf)) { *crlf = '\0'; body_start = (size_t)(crlf - head) + 4; }
        else if (lf) { *lf = '\0'; body_start = (size_t)(lf - head) + 2; }
    }
    /* The body may contain NUL bytes; keep its start before the head is parsed. */
    char *body_part = head + body_start;
    size_t body_part_length = head_length - body_start;
    long status = 0;
    long long content_length = -1;
    err = rci_scgi_head(head, &status, &content_length);
    if (err != MT_OK) { return err; }
    if (status != 200) { return MT_ERR_UPSTREAM; }
    if (content_length >= 0 && (unsigned long long)content_length > limit) { return MT_ERR_LIMIT; }

    if (body_part_length && receive_rci(body_part, 1, body_part_length, response) != body_part_length) {
        return response->error;
    }
    char chunk[4096];
    while (content_length < 0 || response->length < (size_t)content_length) {
        size_t received = 0;
        err = rci_recv(fd, chunk, sizeof(chunk), &received, &deadline, stop);
        if (err != MT_OK) { return err; }
        if (!received) { break; }
        if (receive_rci(chunk, 1, received, response) != received) { return response->error; }
    }
    if (content_length >= 0 && response->length != (size_t)content_length) { return MT_ERR_PROTO; }
    return response->length ? MT_OK : MT_ERR_PROTO;
}

static mt_err_t rci_scgi_fetch(const char *uri, const char *post, size_t limit,
                               const atomic_bool *stop, char **body, size_t *length) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { return MT_ERR_UPSTREAM; }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        close(fd);
        return MT_ERR_UPSTREAM;
    }
    rci_buf_t response = {.limit = limit};
    mt_err_t error = rci_scgi_exchange(fd, uri, post, limit, stop, &response);
    close(fd);
    if (stop && atomic_load(stop)) { error = MT_ERR_CANCELED; }
    if (error != MT_OK) { free(response.data); return error; }
    *body = response.data; *length = response.length;
    return MT_OK;
}

mt_err_t mt_kn_rci_request(const char *url, const char *post, size_t limit,
                            const atomic_bool *stop, char **body, size_t *length) {
    if (!body || !length) { return MT_ERR_INVAL; }
    *body = NULL; *length = 0;
    if (!url || !limit || limit == SIZE_MAX || (post && strlen(post) > LONG_MAX)) {
        return MT_ERR_INVAL;
    }
    if (stop && atomic_load(stop)) { return MT_ERR_CANCELED; }
    const char *local_uri = rci_local_uri(url);
    if (local_uri && atomic_load(&rci_use_scgi)) {
        return rci_scgi_fetch(local_uri, post, limit, stop, body, length);
    }
    CURL *curl = curl_easy_init();
    if (!curl) { return MT_ERR_NOMEM; }
    struct curl_slist *headers = NULL;
    if (post) {
        headers = curl_slist_append(NULL, "Content-Type: application/json");
        if (!headers) { curl_easy_cleanup(curl); return MT_ERR_NOMEM; }
    }
    rci_buf_t response = {.limit = limit};
    bool loopback_closed = false;
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
    SET(CURLOPT_HEADERFUNCTION, receive_rci_header);
    SET(CURLOPT_HEADERDATA, &loopback_closed);
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
    if (error == MT_OK && status != 200) {
        if (status == 403 && loopback_closed && local_uri) {
            free(response.data);
            if (!atomic_exchange(&rci_use_scgi, true)) {
                MT_INFO("Keenetic RCI: %s rejected the request (403 %s); using %s",
                        rci_base_url, RCI_LOOPBACK_CLOSED, rci_scgi_socket);
            }
            return rci_scgi_fetch(local_uri, post, limit, stop, body, length);
        }
        error = MT_ERR_UPSTREAM;
    }
    if (error == MT_OK && !response.length) { error = MT_ERR_PROTO; }
    if (error != MT_OK) { free(response.data); return error; }
    *body = response.data; *length = response.length;
    return MT_OK;
}
