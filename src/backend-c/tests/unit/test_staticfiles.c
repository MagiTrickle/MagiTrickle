/* HTTP-level tests for staticfiles.c, wired as the not-found fallback on
 * a bare mt_httpd_t (no app/groups needed) -- same harness pattern as
 * test_httpd.c. */
#include "greatest.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "magitrickle/loop.h"
#include "magitrickle/staticfiles.h"

static char g_skins_dir[] = "/tmp/mt_staticfiles_test_XXXXXX";
static const char *skin_name(void *ud) {
    (void)ud;
    return "default";
}

typedef struct harness {
    mt_loop_t *loop;
    mt_httpd_t *tcp;
    mt_static_ctx_t ctx;
    pthread_t thread;
} harness_t;

static void *loop_thread(void *ud) {
    harness_t *h = ud;
    mt_loop_run(h->loop);
    return NULL;
}

#define TEST_PORT 18083

static harness_t *harness_start(const char *skins_dir) {
    harness_t *h = calloc(1, sizeof(*h));
    h->ctx.skins_dir = skins_dir;
    h->ctx.skin_name = skin_name;
    if (mt_loop_create(&h->loop) != MT_OK) { return NULL; }
    if (mt_httpd_create(h->loop, &h->tcp) != MT_OK) { return NULL; }
    mt_httpd_set_not_found(h->tcp, mt_static_handler, &h->ctx);
    if (mt_httpd_listen_tcp(h->tcp, "127.0.0.1", TEST_PORT) != MT_OK) { return NULL; }
    pthread_create(&h->thread, NULL, loop_thread, h);
    return h;
}

static void harness_stop(harness_t *h) {
    mt_loop_stop(h->loop);
    pthread_join(h->thread, NULL);
    mt_httpd_destroy(h->tcp);
    mt_loop_destroy(h->loop);
    free(h);
}

/* ---- tiny blocking HTTP/1.1 client ------------------------------------------- */

static int connect_tcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    for (int i = 0; i < 50; i++) {
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) { return fd; }
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
    }
    close(fd);
    return -1;
}

static ssize_t recv_response(int fd, char *buf, size_t cap) {
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    size_t total = 0;
    char *body_start = NULL;
    long content_length = -1;
    for (;;) {
        if (total >= cap - 1) { break; }
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0) { break; }
        total += (size_t)n;
        buf[total] = '\0';
        if (!body_start) {
            char *marker = strstr(buf, "\r\n\r\n");
            if (marker) {
                body_start = marker + 4;
                char *cl = strstr(buf, "Content-Length:");
                if (cl && cl < marker) { content_length = strtol(cl + 15, NULL, 10); }
            }
        }
        if (body_start && content_length >= 0) {
            size_t body_have = (size_t)(buf + total - body_start);
            if ((long)body_have >= content_length) { break; }
        }
    }
    return (ssize_t)total;
}

static int status_code_of(const char *resp) {
    int code = 0;
    sscanf(resp, "HTTP/1.1 %d", &code);
    return code;
}

static const char *body_of(const char *resp) {
    const char *marker = strstr(resp, "\r\n\r\n");
    return marker ? marker + 4 : "";
}

static const char *header_of(const char *resp, const char *name) {
    static char buf[128];
    const char *p = strstr(resp, name);
    if (!p) { return NULL; }
    p += strlen(name);
    while (*p == ' ') { p++; }
    const char *end = strstr(p, "\r\n");
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= sizeof(buf)) { len = sizeof(buf) - 1; }
    memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

static int do_get(const char *path, char *resp, size_t cap) {
    int fd = connect_tcp(TEST_PORT);
    if (fd < 0) { return -1; }
    char req[512];
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", path);
    if (send(fd, req, strlen(req), 0) <= 0) {
        close(fd);
        return -1;
    }
    ssize_t got = recv_response(fd, resp, cap);
    close(fd);
    return got > 0 ? status_code_of(resp) : -1;
}

/* ---- fixture skin dir --------------------------------------------------------- */

static char *make_skin_fixture(void) {
    char *dir = strdup(g_skins_dir);
    if (!mkdtemp(dir)) { return NULL; }
    char path[512];
    snprintf(path, sizeof(path), "%s/default", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/default/index.html", dir);
    FILE *f = fopen(path, "w");
    fputs("<html>index</html>", f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/default/style.css", dir);
    f = fopen(path, "w");
    fputs("body{}", f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/default/sub", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/default/sub/index.html", dir);
    f = fopen(path, "w");
    fputs("<html>sub</html>", f);
    fclose(f);
    return dir;
}

TEST serves_index_at_root(void) {
    char *dir = make_skin_fixture();
    ASSERT(dir != NULL);
    harness_t *h = harness_start(dir);
    ASSERT(h != NULL);
    char resp[4096];
    ASSERT_EQ(200, do_get("/", resp, sizeof(resp)));
    ASSERT_STR_EQ("<html>index</html>", body_of(resp));
    ASSERT(header_of(resp, "Content-Type:") != NULL);
    ASSERT(strstr(header_of(resp, "Content-Type:"), "text/html") != NULL);
    harness_stop(h);
    free(dir);
    PASS();
}

TEST serves_file_by_extension_content_type(void) {
    char *dir = make_skin_fixture();
    ASSERT(dir != NULL);
    harness_t *h = harness_start(dir);
    ASSERT(h != NULL);
    char resp[4096];
    ASSERT_EQ(200, do_get("/style.css", resp, sizeof(resp)));
    ASSERT_STR_EQ("body{}", body_of(resp));
    ASSERT(strstr(header_of(resp, "Content-Type:"), "text/css") != NULL);
    harness_stop(h);
    free(dir);
    PASS();
}

TEST serves_index_for_subdirectory(void) {
    char *dir = make_skin_fixture();
    ASSERT(dir != NULL);
    harness_t *h = harness_start(dir);
    ASSERT(h != NULL);
    char resp[4096];
    ASSERT_EQ(200, do_get("/sub", resp, sizeof(resp)));
    ASSERT_STR_EQ("<html>sub</html>", body_of(resp));
    harness_stop(h);
    free(dir);
    PASS();
}

TEST missing_file_is_404_json(void) {
    char *dir = make_skin_fixture();
    ASSERT(dir != NULL);
    harness_t *h = harness_start(dir);
    ASSERT(h != NULL);
    char resp[4096];
    ASSERT_EQ(404, do_get("/nope.js", resp, sizeof(resp)));
    ASSERT(strstr(body_of(resp), "\"error\"") != NULL);
    harness_stop(h);
    free(dir);
    PASS();
}

TEST missing_skin_at_root_is_html_placeholder(void) {
    /* skins_dir exists but has no "default" subdir at all */
    char dir[] = "/tmp/mt_staticfiles_empty_XXXXXX";
    ASSERT(mkdtemp(dir) != NULL);
    harness_t *h = harness_start(dir);
    ASSERT(h != NULL);
    char resp[4096];
    ASSERT_EQ(404, do_get("/", resp, sizeof(resp)));
    ASSERT(strstr(body_of(resp), "install MagiTrickle skin") != NULL);
    harness_stop(h);
    PASS();
}

TEST path_traversal_is_contained_under_skin_root(void) {
    char *dir = make_skin_fixture();
    ASSERT(dir != NULL);
    harness_t *h = harness_start(dir);
    ASSERT(h != NULL);
    char resp[4096];
    /* "/../../../../etc/passwd", cleaned, cannot escape the skins root
     * (mirrors Go's path.Clean behavior on a rooted path) -- expect a
     * plain within-skin 404, never file contents from outside the skin
     * dir. */
    int status = do_get("/../../../../etc/passwd", resp, sizeof(resp));
    ASSERT_EQ(404, status);
    ASSERT(strstr(body_of(resp), "root:") == NULL);
    harness_stop(h);
    free(dir);
    PASS();
}

TEST non_get_method_is_404(void) {
    char *dir = make_skin_fixture();
    ASSERT(dir != NULL);
    harness_t *h = harness_start(dir);
    ASSERT(h != NULL);
    int fd = connect_tcp(TEST_PORT);
    ASSERT(fd >= 0);
    const char *req = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    ASSERT(send(fd, req, strlen(req), 0) > 0);
    char resp[4096];
    ASSERT(recv_response(fd, resp, sizeof(resp)) > 0);
    ASSERT_EQ(404, status_code_of(resp));
    close(fd);
    harness_stop(h);
    free(dir);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(serves_index_at_root);
    RUN_TEST(serves_file_by_extension_content_type);
    RUN_TEST(serves_index_for_subdirectory);
    RUN_TEST(missing_file_is_404_json);
    RUN_TEST(missing_skin_at_root_is_html_placeholder);
    RUN_TEST(path_traversal_is_contained_under_skin_root);
    RUN_TEST(non_get_method_is_404);
    GREATEST_MAIN_END();
}
