/* Exercise the RCI transport, including the SCGI fallback, against local fake
 * servers. Including the source exposes only test-local internals (the two
 * endpoints and the one-way switch), no production testing API. */
#include "greatest.h"
/* Keep the timeout regressions short; both fallback attempts individually fit
 * within this budget, but their combined duration does not. */
#define RCI_TOTAL_TIMEOUT_MS 1000L
#include "../../src/interfaces/keenetic_rci.c"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

TEST system_name_request_is_one_positional_batch(void) {
    mt_kn_ifinfo_t items[2] = {{.id = "Wireguard0"}, {.id = "Wireguard1"}};
    char *body = mt_kn_build_system_name_request(items, 2); ASSERT(body);
    ASSERT_STR_EQ("[{\"show\":{\"interface\":{\"name\":\"Wireguard0\",\"details\":\"yes\",\"system-name\":\"yes\"}}},"
                  "{\"show\":{\"interface\":{\"name\":\"Wireguard1\",\"details\":\"yes\",\"system-name\":\"yes\"}}}]", body);
    free(body); PASS();
}

/* Value of an SCGI parameter in a request netstring; NULL when absent. */
static const char *scgi_param(const char *request, size_t length, const char *name, size_t *index)
{
    const char *colon = memchr(request, ':', length);
    if (!colon) { return NULL; }
    size_t headers = strtoul(request, NULL, 10);
    const char *p = colon + 1, *end = p + headers;
    for (size_t i = 0; p < end; i++) {
        const char *value = p + strlen(p) + 1;
        if (strcmp(p, name) == 0) { if (index) { *index = i; } return value; }
        p = value + strlen(value) + 1;
    }
    return NULL;
}

TEST scgi_request_mirrors_nginx_rci_location(void)
{
    size_t length = 0, index = 99;
    char *request = rci_scgi_request("/rci/show/ip/policy", NULL, &length);
    ASSERT(request);
    ASSERT_STR_EQ("0", scgi_param(request, length, "CONTENT_LENGTH", &index)); ASSERT_EQ(0u, index);
    ASSERT_STR_EQ("GET", scgi_param(request, length, "REQUEST_METHOD", NULL));
    ASSERT_STR_EQ("/rci/show/ip/policy", scgi_param(request, length, "REQUEST_URI", NULL));
    ASSERT_STR_EQ("/rci/show/ip/policy", scgi_param(request, length, "NORMALIZED_URI", NULL));
    ASSERT_STR_EQ("", scgi_param(request, length, "QUERY_STRING", NULL));
    ASSERT_STR_EQ("127.0.0.1:79", scgi_param(request, length, "HTTP_HOST", NULL));
    ASSERT_STR_EQ("127.0.0.1", scgi_param(request, length, "REMOTE_ADDR", NULL));
    ASSERT_STR_EQ("http/rci", scgi_param(request, length, "SCGI_AGENT", NULL));
    ASSERT_STR_EQ("1", scgi_param(request, length, "AUTH_ALLOWED", NULL));
    ASSERT_EQ(',', request[length - 1]);
    free(request);

    request = rci_scgi_request("/rci/show/x?name=a", "[]", &length);
    ASSERT(request);
    ASSERT_STR_EQ("2", scgi_param(request, length, "CONTENT_LENGTH", NULL));
    ASSERT_STR_EQ("POST", scgi_param(request, length, "REQUEST_METHOD", NULL));
    ASSERT_STR_EQ("application/json", scgi_param(request, length, "CONTENT_TYPE", NULL));
    ASSERT_STR_EQ("/rci/show/x?name=a", scgi_param(request, length, "REQUEST_URI", NULL));
    ASSERT_STR_EQ("/rci/show/x", scgi_param(request, length, "NORMALIZED_URI", NULL));
    ASSERT_STR_EQ("name=a", scgi_param(request, length, "QUERY_STRING", NULL));
    ASSERT_MEM_EQ(",[]", request + length - 3, 3);
    free(request); PASS();
}

TEST scgi_head_requires_a_valid_status(void)
{
    long status = 0; long long content_length = 0;
    char ok[] = "Status: 200 OK\r\nContent-Type: application/json\r\nContent-Length: 12";
    ASSERT_EQ(MT_OK, rci_scgi_head(ok, &status, &content_length));
    ASSERT_EQ(200, status); ASSERT_EQ(12, content_length);
    char bare[] = "Status: 403\nX-Detail: 0x1867, invalid query string";
    ASSERT_EQ(MT_OK, rci_scgi_head(bare, &status, &content_length));
    ASSERT_EQ(403, status); ASSERT_EQ(-1, content_length);
    char b0[] = "Content-Length: 2", b1[] = "Status: OK", b2[] = "Status: 99",
         b3[] = "Status: 200\r\nContent-Length: -1", b4[] = "Status: 200\r\nContent-Length: 2x",
         b5[] = "garbage";
    char *bad[] = {b0, b1, b2, b3, b4, b5};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        ASSERT_EQ(MT_ERR_PROTO, rci_scgi_head(bad[i], &status, &content_length));
    }
    PASS();
}

TEST http_header_requires_the_exact_error_code(void)
{
    const struct { const char *line; bool expected; } cases[] = {
        {"X-Detail: 0x1867, invalid query string\r\n", true},
        {"x-detail:\t0X1867 \r\n", true},
        {"X-Detail:0x1867\n", true},
        {"X-Detail:0x1867", true},
        {"X-Detail: 0x18670, different code\r\n", false},
        {"X-Detail: 0x1867suffix\r\n", false},
        {"X-Detail: error mentions 0x1867\r\n", false},
        {"X-Detail: 0x1237, not 0x1867\r\n", false},
        {"X-Detail: 0x186\r\n", false},
        {"X-Other: 0x1867\r\n", false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        bool closed = false;
        size_t n = strlen(cases[i].line);
        ASSERT_EQ(n, receive_rci_header((char *)cases[i].line, 1, n, &closed));
        ASSERT_EQ(cases[i].expected, closed);
    }
    /* libcurl supplies status lines for interim responses as well. Their
     * X-Detail must not contaminate the next response's headers. */
    bool closed = true;
    char status[] = "HTTP/1.1 403 Forbidden\r\n";
    ASSERT_EQ(sizeof(status) - 1, receive_rci_header(status, 1, sizeof(status) - 1, &closed));
    ASSERT_FALSE(closed);
    PASS();
}

/* ---- fake endpoints ------------------------------------------------------------ */

typedef struct {
    int listen;
    const char *response;      /* sent after the full request; NULL: never answer */
    char request[4096];
    size_t length;
    int accepted;
    long delay_ms;
    bool keep_open;            /* leave an incomplete body pending until close */
} server_t;

static void delay_ms(long ms)
{
    struct timespec delay = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

/* Read one HTTP request (head and body) or one SCGI netstring and body. */
static void read_request(int fd, server_t *s, bool scgi)
{
    for (;;) {
        ssize_t n = read(fd, s->request + s->length, sizeof(s->request) - 1 - s->length);
        if (n <= 0) { return; }
        s->length += (size_t)n; s->request[s->length] = '\0';
        if (!scgi) {
            const char *end = strstr(s->request, "\r\n\r\n");
            if (!end) { continue; }
            const char *cl = strstr(s->request, "Content-Length: ");
            size_t body = cl ? strtoul(cl + 16, NULL, 10) : 0;
            if (s->length >= (size_t)(end + 4 - s->request) + body) { return; }
            continue;
        }
        const char *colon = memchr(s->request, ':', s->length);
        if (!colon) { continue; }
        size_t headers = strtoul(s->request, NULL, 10);
        size_t head = (size_t)(colon + 1 - s->request) + headers + 1;
        if (s->length < head) { continue; }
        const char *cl = scgi_param(s->request, s->length, "CONTENT_LENGTH", NULL);
        if (s->length >= head + (cl ? strtoul(cl, NULL, 10) : 0)) { return; }
    }
}
static void *serve(server_t *s, bool scgi)
{
    int fd = accept(s->listen, NULL, NULL);
    if (fd < 0) { return NULL; }
    s->accepted++;
    read_request(fd, s, scgi);
    if (s->delay_ms) { delay_ms(s->delay_ms); }
    if (s->response) {
        const char *p = s->response;
        size_t left = strlen(p);
        while (left) {
            ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) { continue; }
            if (n <= 0) { break; }
            p += n; left -= (size_t)n;
        }
    }
    if (!s->response || s->keep_open) {
        char sink[64];
        while (read(fd, sink, sizeof(sink)) > 0) {}   /* until the client gives up */
    }
    close(fd); return NULL;
}
static void *serve_http(void *ud) { return serve(ud, false); }
static void *serve_scgi(void *ud) { return serve(ud, true); }

static char base_url[64];
static char scgi_path[108];

static bool start_http(server_t *s, pthread_t *thread)
{
    s->listen = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof(addr);
    if (s->listen < 0 || bind(s->listen, (void *)&addr, sizeof(addr)) || listen(s->listen, 1) ||
        getsockname(s->listen, (void *)&addr, &len)) { return false; }
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%u", (unsigned)ntohs(addr.sin_port));
    rci_base_url = base_url;
    return pthread_create(thread, NULL, serve_http, s) == 0;
}
static bool start_scgi(server_t *s, pthread_t *thread)
{
    snprintf(scgi_path, sizeof(scgi_path), "/tmp/mt-test-rci-%ld.sock", (long)getpid());
    unlink(scgi_path);
    s->listen = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, scgi_path);
    if (s->listen < 0 || bind(s->listen, (void *)&addr, sizeof(addr)) || listen(s->listen, 1)) { return false; }
    rci_scgi_socket = scgi_path;
    return pthread_create(thread, NULL, serve_scgi, s) == 0;
}
static void stop_server(server_t *s, pthread_t thread)
{
    shutdown(s->listen, SHUT_RDWR); close(s->listen);
    pthread_join(thread, NULL);
}
static void reset_transport(void)
{
    atomic_store(&rci_use_scgi, false);
    rci_base_url = MT_KN_RCI_BASE_URL;
    rci_scgi_socket = MT_KN_RCI_SCGI_SOCKET;
    if (scgi_path[0]) { unlink(scgi_path); }
}

static const char closed_403[] =
    "HTTP/1.1 403 Forbidden\r\nX-Detail: 0x1867, invalid query string\r\n"
    "Content-Length: 9\r\nConnection: close\r\n\r\nForbidden";
static const char scgi_ok[] =
    "Status: 200 OK\r\nContent-Type: application/json\r\nContent-Length: 14\r\n\r\n{\"Policy0\":{}}";

TEST closed_loopback_switches_to_scgi_for_the_process(void)
{
    ASSERT_EQ(CURLE_OK, curl_global_init(CURL_GLOBAL_DEFAULT));
    server_t http = {.response = closed_403}, scgi = {.response = scgi_ok};
    pthread_t http_thread, scgi_thread;
    ASSERT(start_http(&http, &http_thread)); ASSERT(start_scgi(&scgi, &scgi_thread));
    char url[128]; snprintf(url, sizeof(url), "%s/rci/show/ip/policy", base_url);
    char *body = NULL; size_t length = 0;
    mt_err_t err = mt_kn_rci_request(url, NULL, 1024, NULL, &body, &length);
    stop_server(&http, http_thread); stop_server(&scgi, scgi_thread);
    ASSERT_EQ_FMT(MT_OK, err, "%d");
    ASSERT_STR_EQ("{\"Policy0\":{}}", body); ASSERT_EQ(14u, length);
    ASSERT(atomic_load(&rci_use_scgi));
    ASSERT_STR_EQ("/rci/show/ip/policy", scgi_param(scgi.request, scgi.length, "REQUEST_URI", NULL));
    free(body);

    /* Later requests, POST batches included, go straight to the socket: the
     * HTTP listener is already closed here. */
    server_t post = {.response = scgi_ok};
    ASSERT(start_scgi(&post, &scgi_thread));
    snprintf(url, sizeof(url), "%s/rci/", base_url);
    err = mt_kn_rci_request(url, "[{\"show\":{}}]", 1024, NULL, &body, &length);
    stop_server(&post, scgi_thread);
    ASSERT_EQ_FMT(MT_OK, err, "%d"); free(body);
    ASSERT_STR_EQ("POST", scgi_param(post.request, post.length, "REQUEST_METHOD", NULL));
    ASSERT_MEM_EQ(",[{\"show\":{}}]", post.request + post.length - 14, 14);
    reset_transport(); curl_global_cleanup(); PASS();
}

TEST other_forbidden_answers_keep_http(void)
{
    ASSERT_EQ(CURLE_OK, curl_global_init(CURL_GLOBAL_DEFAULT));
    const char *responses[] = {
        "HTTP/1.1 403 Forbidden\r\nX-Detail: 0x1237, rci-local only\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 401 Unauthorized\r\nX-Detail: 0x1867\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 403 Forbidden\r\nX-Detail: 0x18670\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 403 Forbidden\r\nX-Detail: error mentions 0x1867\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 100 Continue\r\nX-Detail: 0x1867\r\n\r\n"
        "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(responses) / sizeof(*responses); i++) {
        server_t http = {.response = responses[i]}, scgi = {.response = scgi_ok};
        pthread_t http_thread, scgi_thread;
        ASSERT(start_http(&http, &http_thread)); ASSERT(start_scgi(&scgi, &scgi_thread));
        char url[128]; snprintf(url, sizeof(url), "%s/rci/show/ip/policy", base_url);
        char *body = NULL; size_t length = 0;
        mt_err_t err = mt_kn_rci_request(url, NULL, 1024, NULL, &body, &length);
        stop_server(&http, http_thread); stop_server(&scgi, scgi_thread);
        ASSERT_EQ_FMT(MT_ERR_UPSTREAM, err, "%d"); ASSERT(body == NULL);
        ASSERT_EQ(0, scgi.accepted); ASSERT_FALSE(atomic_load(&rci_use_scgi));
    }
    /* A URL outside the local listener never falls back. */
    server_t http = {.response = closed_403}, scgi = {.response = scgi_ok};
    pthread_t http_thread, scgi_thread;
    ASSERT(start_http(&http, &http_thread)); ASSERT(start_scgi(&scgi, &scgi_thread));
    char url[128]; snprintf(url, sizeof(url), "%s/rci/", base_url);
    rci_base_url = "http://127.0.0.1:1";
    char *body = NULL; size_t length = 0;
    mt_err_t err = mt_kn_rci_request(url, NULL, 1024, NULL, &body, &length);
    stop_server(&http, http_thread); stop_server(&scgi, scgi_thread);
    ASSERT_EQ_FMT(MT_ERR_UPSTREAM, err, "%d"); ASSERT_EQ(0, scgi.accepted);
    ASSERT_FALSE(atomic_load(&rci_use_scgi));
    reset_transport(); curl_global_cleanup(); PASS();
}

TEST forbidden_body_does_not_block_scgi_fallback(void)
{
    ASSERT_EQ(CURLE_OK, curl_global_init(CURL_GLOBAL_DEFAULT));
    const struct { const char *response; bool keep_open; } cases[] = {
        {closed_403, false},  /* nine-byte error body exceeds the two-byte limit */
        {"HTTP/1.1 403 Forbidden\r\nX-Detail: 0x1867\r\nContent-Length: 100\r\n\r\nForbidden", false},
        {"HTTP/1.1 403 Forbidden\r\nx-detail:\t0X1867\r\nContent-Length: 100\r\n\r\n", true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        server_t http = {.response = cases[i].response, .keep_open = cases[i].keep_open};
        server_t scgi = {.response = "Status: 200 OK\r\nContent-Length: 2\r\n\r\n{}"};
        pthread_t http_thread, scgi_thread;
        ASSERT(start_http(&http, &http_thread)); ASSERT(start_scgi(&scgi, &scgi_thread));
        char url[128]; snprintf(url, sizeof(url), "%s/rci/show/ip/policy", base_url);
        char *body = NULL; size_t length = 0;
        mt_err_t err = mt_kn_rci_request(url, NULL, 2, NULL, &body, &length);
        stop_server(&http, http_thread); stop_server(&scgi, scgi_thread);
        ASSERT_EQ_FMT(MT_OK, err, "%d"); ASSERT_STR_EQ("{}", body); ASSERT_EQ(2u, length);
        ASSERT_EQ(1, scgi.accepted); ASSERT(atomic_load(&rci_use_scgi));
        free(body); reset_transport();
    }
    curl_global_cleanup(); PASS();
}

TEST scgi_answers_are_bounded_like_http(void)
{
    const struct { const char *response; size_t limit; mt_err_t expected; } cases[] = {
        {"Status: 403 Forbidden\r\nX-Detail: 0x1867\r\n\r\n", 1024, MT_ERR_UPSTREAM},
        {"Status: 200 OK\r\nContent-Length: 14\r\n\r\n{\"Policy0\":{}}", 4, MT_ERR_LIMIT},
        {"Status: 200 OK\r\n\r\n{\"Policy0\":{}}", 4, MT_ERR_LIMIT},
        {"Status: 200 OK\r\nContent-Length: 20\r\n\r\n{\"Policy0\":{}}", 1024, MT_ERR_PROTO},
        {"Status: 200 OK\r\nContent-Length: 0\r\n\r\n", 1024, MT_ERR_PROTO},
        {"Content-Type: application/json\r\n\r\n{}", 1024, MT_ERR_PROTO},
        {"Status: 200 OK\r\n", 1024, MT_ERR_PROTO},
        {"Status: 200 OK\n\n{}", 1024, MT_OK},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        server_t scgi = {.response = cases[i].response};
        pthread_t thread;
        ASSERT(start_scgi(&scgi, &thread));
        atomic_store(&rci_use_scgi, true);
        char *body = NULL; size_t length = 0;
        mt_err_t err = mt_kn_rci_request(MT_KN_RCI_BASE_URL "/rci/show/ip/policy", NULL,
                                         cases[i].limit, NULL, &body, &length);
        stop_server(&scgi, thread);
        ASSERT_EQ_FMT(cases[i].expected, err, "%d");
        ASSERT_EQ(cases[i].expected == MT_OK, body != NULL);
        if (cases[i].expected != MT_OK) { ASSERT_EQ(0u, length); }
        free(body);
    }
    /* No socket at all. */
    rci_scgi_socket = "/tmp/mt-test-rci-missing.sock";
    char *body = NULL; size_t length = 0;
    ASSERT_EQ_FMT(MT_ERR_UPSTREAM, mt_kn_rci_request(MT_KN_RCI_BASE_URL "/rci/", NULL, 1024,
                                                     NULL, &body, &length), "%d");
    reset_transport(); PASS();
}

TEST expired_deadline_stops_ready_scgi_io(void)
{
    int pair[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    ASSERT_EQ(1, send(pair[0], "x", 1, MSG_NOSIGNAL));
    struct timespec expired = rci_deadline(0);
    expired.tv_sec--;
    char data = '\0'; size_t received = 0;
    mt_err_t read_error = rci_recv(pair[1], &data, 1, &received, &expired, NULL);
    mt_err_t write_error = rci_send(pair[1], "y", 1, &expired, NULL);
    /* Reading must leave the queued byte untouched; writing must not enqueue
     * anything even though the send buffer has room. */
    char queued = '\0', unexpected = '\0';
    ssize_t queued_length = recv(pair[1], &queued, 1, MSG_DONTWAIT);
    ssize_t unexpected_length = recv(pair[0], &unexpected, 1, MSG_DONTWAIT);
    int unexpected_errno = errno;
    close(pair[0]); close(pair[1]);
    ASSERT_EQ_FMT(MT_ERR_TIMEOUT, read_error, "%d");
    ASSERT_EQ_FMT(MT_ERR_TIMEOUT, write_error, "%d");
    ASSERT_EQ(0u, received); ASSERT_EQ('\0', data);
    ASSERT_EQ(1, queued_length); ASSERT_EQ('x', queued);
    ASSERT_EQ(-1, unexpected_length);
    ASSERT(unexpected_errno == EAGAIN || unexpected_errno == EWOULDBLOCK);
    PASS();
}

TEST http_and_scgi_share_one_deadline(void)
{
    ASSERT_EQ(CURLE_OK, curl_global_init(CURL_GLOBAL_DEFAULT));
    server_t http = {.response = closed_403, .delay_ms = 650};
    server_t scgi = {.response = scgi_ok, .delay_ms = 650};
    pthread_t http_thread, scgi_thread;
    ASSERT(start_http(&http, &http_thread)); ASSERT(start_scgi(&scgi, &scgi_thread));
    char url[128]; snprintf(url, sizeof(url), "%s/rci/show/ip/policy", base_url);
    char *body = NULL; size_t length = 0;
    mt_err_t err = mt_kn_rci_request(url, NULL, 1024, NULL, &body, &length);
    stop_server(&http, http_thread); stop_server(&scgi, scgi_thread);
    ASSERT_EQ_FMT(MT_ERR_TIMEOUT, err, "%d"); ASSERT(body == NULL); ASSERT_EQ(0u, length);
    ASSERT_EQ(1, http.accepted); ASSERT_EQ(1, scgi.accepted);
    ASSERT(atomic_load(&rci_use_scgi));
    reset_transport(); curl_global_cleanup(); PASS();
}

TEST silent_scgi_peer_times_out(void)
{
    server_t scgi = {.response = NULL};
    pthread_t thread;
    ASSERT(start_scgi(&scgi, &thread));
    atomic_store(&rci_use_scgi, true);
    char *body = NULL; size_t length = 0;
    mt_err_t err = mt_kn_rci_request(MT_KN_RCI_BASE_URL "/rci/", NULL, 1024, NULL, &body, &length);
    stop_server(&scgi, thread);
    ASSERT_EQ_FMT(MT_ERR_TIMEOUT, err, "%d"); ASSERT(body == NULL); ASSERT_EQ(0u, length);
    reset_transport(); PASS();
}

static void *cancel_later(void *ud)
{
    atomic_bool *stop = ud;
    const struct timespec delay = {.tv_nsec = 100 * 1000000L};
    nanosleep(&delay, NULL);
    atomic_store(stop, true);
    return NULL;
}
TEST silent_scgi_peer_is_cancelable(void)
{
    server_t scgi = {.response = NULL};
    pthread_t thread, canceler;
    ASSERT(start_scgi(&scgi, &thread));
    atomic_store(&rci_use_scgi, true);
    atomic_bool stop = false;
    ASSERT_EQ(0, pthread_create(&canceler, NULL, cancel_later, &stop));
    struct timespec started = rci_deadline(0);
    char *body = NULL; size_t length = 0;
    mt_err_t err = mt_kn_rci_request(MT_KN_RCI_BASE_URL "/rci/", NULL, 1024, &stop, &body, &length);
    long elapsed = -rci_ms_left(&started);
    pthread_join(canceler, NULL);
    stop_server(&scgi, thread);
    ASSERT_EQ_FMT(MT_ERR_CANCELED, err, "%d"); ASSERT(body == NULL);
    ASSERT(elapsed < 1000);
    reset_transport(); PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(system_name_request_is_one_positional_batch);
    RUN_TEST(scgi_request_mirrors_nginx_rci_location);
    RUN_TEST(scgi_head_requires_a_valid_status);
    RUN_TEST(http_header_requires_the_exact_error_code);
    RUN_TEST(closed_loopback_switches_to_scgi_for_the_process);
    RUN_TEST(other_forbidden_answers_keep_http);
    RUN_TEST(forbidden_body_does_not_block_scgi_fallback);
    RUN_TEST(scgi_answers_are_bounded_like_http);
    RUN_TEST(expired_deadline_stops_ready_scgi_io);
    RUN_TEST(http_and_scgi_share_one_deadline);
    RUN_TEST(silent_scgi_peer_times_out);
    RUN_TEST(silent_scgi_peer_is_cancelable);
    GREATEST_MAIN_END();
}
