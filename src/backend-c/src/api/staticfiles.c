/* See staticfiles.h. Port of http.go's wildcard GET-fallback route. */
#include "magitrickle/staticfiles.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char NO_SKIN_PLACEHOLDER[] =
    "<!DOCTYPE html><html><head><title>MagiTrickle</title></head><body><h1>MagiTrickle</h1>"
    "<p>Please install MagiTrickle skin before using WebUI!</p></body></html>";

static const char *content_type_for_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) { return "text/plain"; }
    if (strcasecmp(dot, ".html") == 0) { return "text/html"; }
    if (strcasecmp(dot, ".css") == 0) { return "text/css"; }
    if (strcasecmp(dot, ".js") == 0) { return "application/javascript"; }
    if (strcasecmp(dot, ".ico") == 0) { return "image/x-icon"; }
    if (strcasecmp(dot, ".png") == 0) { return "image/png"; }
    if (strcasecmp(dot, ".svg") == 0) { return "image/svg+xml"; }
    return "text/plain";
}

void mt_static_handler(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_static_ctx_t *ctx = ud;
    if (strcmp(mt_http_req_method(req), "GET") != 0) {
        mt_http_res_write_error(res, 404, "not found");
        return;
    }

    const char *original = mt_http_req_path(req);
    const char *skin = ctx->skin_name(ctx->ud);

    char file_path[1024];
    if (snprintf(file_path, sizeof(file_path), "%s/%s%s", ctx->skins_dir, skin, original) >=
        (int)sizeof(file_path)) {
        mt_http_res_write_error(res, 500, "path too long");
        return;
    }

    /* Up to 2 stat attempts, matching Go's `for i := 0; i < 2; i++`: a
     * directory gets "/index.html" appended and is retried once; if that
     * retry is *also* a directory (pathological), Go still falls through
     * to a doomed os.ReadFile with a second "/index.html" appended -- the
     * fopen() below reproduces the same "file not found"-shaped failure
     * for that edge case. */
    struct stat st;
    for (int i = 0; i < 2; i++) {
        if (stat(file_path, &st) != 0) {
            if (errno == ENOENT) {
                if (strcmp(original, "/") == 0) {
                    mt_http_res_write(res, 404, "text/html", (const uint8_t *)NO_SKIN_PLACEHOLDER,
                                      sizeof(NO_SKIN_PLACEHOLDER) - 1);
                } else {
                    mt_http_res_write_error(res, 404, "file not found");
                }
                return;
            }
            mt_http_res_write_error(res, 500, "failed to stat file");
            return;
        }
        if (S_ISDIR(st.st_mode)) {
            size_t len = strlen(file_path);
            if (snprintf(file_path + len, sizeof(file_path) - len, "/index.html") >=
                (int)(sizeof(file_path) - len)) {
                mt_http_res_write_error(res, 500, "path too long");
                return;
            }
            continue;
        }
        break;
    }

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        mt_http_res_write_error(res, 500, "failed to read file");
        return;
    }
    /* fstat the opened fd rather than trusting the loop's last `st` --
     * that struct can describe a stale path in the pathological
     * double-directory case noted above. */
    struct stat fst;
    if (fstat(fileno(f), &fst) != 0) {
        fclose(f);
        mt_http_res_write_error(res, 500, "failed to read file");
        return;
    }
    size_t size = (size_t)fst.st_size;
    uint8_t *data = malloc(size > 0 ? size : 1);
    if (!data) {
        fclose(f);
        mt_http_res_write_error(res, 500, "out of memory");
        return;
    }
    size_t n = fread(data, 1, size, f);
    fclose(f);
    if (n != size) {
        free(data);
        mt_http_res_write_error(res, 500, "failed to read file");
        return;
    }
    mt_http_res_write(res, 200, content_type_for_ext(file_path), data, size);
    free(data);
}
