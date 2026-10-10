/* Linux process/filesystem boundary for the on-demand updater. The daemon only
 * snapshots a request and starts an independently named, detached executable.
 * All network/package-manager waits below run in that executable, not epoll. */
#define _GNU_SOURCE
#include "magitrickle/update.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "magitrickle/auth.h"
#include "magitrickle/hash.h"
#include "magitrickle/httpd.h"
#include "magitrickle/jwt.h"
#include "magitrickle/log.h"
#include "magitrickle/paths.h"
#include "magitrickle/rand.h"
#include "magitrickle/system.h"
#include "magitrickle/version.h"

#define UPDATE_DIR MT_APP_STATE_DIR "/update"
#define STATE_LIMIT 16384U
#define LOG_LIMIT (128U * 1024U)
#if defined(MT_PLATFORM_ENTWARE)
#define UPDATER "/opt/bin/mt-c-updater"
#define INIT_SCRIPT "/opt/etc/init.d/S99magitrickle"
#define INSTALL_ROOT "/opt"
#else
#define UPDATER "/usr/bin/mt-c-updater"
#define INIT_SCRIPT "/etc/init.d/magitrickle"
#define INSTALL_ROOT "/"
#endif

static const char *str(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static bool setstr(cJSON *obj, const char *key, const char *value) {
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    return cJSON_AddStringToObject(obj, key, value) != NULL;
}

static bool write_all(int fd, const void *data, size_t len) {
    const unsigned char *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) { continue; }
        if (n <= 0) { return false; }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static int open_dir(bool create) {
    if (create && mkdir(UPDATE_DIR, 0700) && errno != EEXIST) { return -1; }
    int fd = open(UPDATE_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0) { return -1; }
    if (fstat(fd, &st) || st.st_uid != 0 || (st.st_mode & 0077)) { close(fd); return -1; }
    return fd;
}

static int open_lock(int dir) {
    int fd = openat(dir, "lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat st;
    if (fd < 0) { return -1; }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077)) { close(fd); return -1; }
    return fd;
}

static bool write_json(int dir, const char *name, const cJSON *obj) {
    char *text = cJSON_PrintUnformatted(obj);
    if (!text) { return false; }
    char temp[512];
    snprintf(temp, sizeof(temp), UPDATE_DIR "/.state-XXXXXX");
    int fd = mkstemp(temp);
    bool ok = fd >= 0 && strlen(text) < STATE_LIMIT;
    if (ok) { ok = write_all(fd, text, strlen(text)) && fsync(fd) == 0; }
    if (fd >= 0 && close(fd)) { ok = false; }
    if (ok) { ok = renameat(AT_FDCWD, temp, dir, name) == 0 && fsync(dir) == 0; }
    if (!ok) { (void)unlink(temp); }
    free(text);
    return ok;
}

static cJSON *read_json(int dir, const char *name) {
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { return NULL; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077) ||
        st.st_size <= 0 || st.st_size >= (off_t)STATE_LIMIT) { close(fd); return NULL; }
    char data[STATE_LIMIT];
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, data + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) { continue; }
        if (n <= 0) { break; }
        used += (size_t)n;
    }
    close(fd);
    if (used != (size_t)st.st_size || memchr(data, '\0', used)) { return NULL; }
    return cJSON_ParseWithLength(data, used);
}

/* Do not source /etc/openwrt_release as a shell program or guess from uname.
 * Unknown/rolling snapshot firmware is deliberately not self-updatable. */
static bool identity(char *suffix, size_t cap, const char **manager) {
    if (!MT_BUILD_TARGET[0]) { return false; }
    for (const char *p = MT_BUILD_TARGET; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) { return false; }
    }
#if defined(MT_PLATFORM_ENTWARE)
    *manager = "/opt/bin/opkg";
    int n = snprintf(suffix, cap, "entware_%s.ipk", MT_BUILD_TARGET);
    return n > 0 && (size_t)n < cap;
#elif defined(MT_PLATFORM_OPENWRT)
    FILE *f = fopen("/etc/openwrt_release", "r");
    if (!f) { return false; }
    char line[256], release[64] = "";
    while (fgets(line, sizeof(line), f)) {
        const char *prefix = "DISTRIB_RELEASE=";
        if (strncmp(line, prefix, strlen(prefix))) { continue; }
        const char *p = line + strlen(prefix);
        char quote = (*p == '\'' || *p == '"') ? *p++ : '\0';
        size_t n = 0;
        while ((*p >= '0' && *p <= '9') || *p == '.') {
            if (n + 1 == sizeof(release)) { n = 0; break; }
            release[n++] = *p++;
        }
        release[n] = '\0';
        if ((quote && *p++ != quote) || (*p && *p != '\n' && *p != '\r')) { release[0] = '\0'; }
        break;
    }
    fclose(f);
    mt_update_version_t os;
    if (!mt_update_version_parse(release, 1, &os) || os.development) { return false; }
    /* Match the exact firmware version, not just CPU or libc. */
    bool apk = os.parts[0] >= 25;
    *manager = apk ? "/sbin/apk" : "/bin/opkg";
    if (access(*manager, X_OK)) { *manager = apk ? "/usr/bin/apk" : "/usr/bin/opkg"; }
    int n = snprintf(suffix, cap, "openwrt-%s_%s.%s", release, MT_BUILD_TARGET, apk ? "apk" : "ipk");
    return n > 0 && (size_t)n < cap;
#else
    (void)suffix; (void)cap; (void)manager;
    return false;
#endif
}

static bool root_authorized(mt_http_req_t *req) {
    const char *auth = mt_http_req_header(req, "Authorization");
    if (!auth || strncmp(auth, "Bearer ", 7) || strlen(auth + 7) >= MT_JWT_MAX_TOKEN) { return false; }
    mt_jwt_claims_t claims;
    /* Parse the subject only AFTER full verification with the current password. */
    return mt_auth_verify_token(MT_APP_STATE_DIR, auth + 7) == MT_OK &&
           mt_jwt_parse_unverified(auth + 7, &claims) == MT_OK && strcmp(claims.sub, "root") == 0;
}

static const char *capability(mt_http_req_t *req, char *suffix, size_t cap) {
    const char *manager = NULL;
    mt_update_version_t version;
    if (!identity(suffix, cap, &manager)) { return "Unsupported build target"; }
    if (!mt_update_version_parse(MT_VERSION, MT_PACKAGE_REVISION, &version)) { return "Unknown installed version"; }
    if (geteuid() != 0 || access(UPDATER, X_OK) || access(manager, X_OK)) { return "Installer unavailable"; }
    if (!root_authorized(req)) { return "Sign in as root to install updates"; }
    return NULL;
}

static void handle_info(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)ud;
    char suffix[192] = "";
    const char *reason = capability(req, suffix, sizeof(suffix));
    cJSON *obj = cJSON_CreateObject();
    if (!obj) { mt_http_res_write_error(res, 500, "Out of memory"); return; }
    cJSON_AddStringToObject(obj, "installed_version", MT_VERSION);
    cJSON_AddNumberToObject(obj, "installed_revision", MT_PACKAGE_REVISION);
    cJSON_AddStringToObject(obj, "asset_suffix", suffix);
    cJSON_AddBoolToObject(obj, "can_install", reason == NULL);
    cJSON_AddStringToObject(obj, "reason", reason ? reason : "");
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    mt_http_res_write_json(res, 200, obj);
}

static bool active(const char *stage) {
    return strcmp(stage, "idle") && strcmp(stage, "succeeded") && strcmp(stage, "failed") && strcmp(stage, "interrupted");
}

/* The worker may finish between the initial snapshot and the lock probe.
 * Re-read while holding the lock before synthesizing an interrupted result. */
static bool refresh_unlocked_status(int dir, cJSON **snapshot) {
    int lock = open_lock(dir);
    if (lock < 0) { return false; }
    bool ok = true;
    if (flock(lock, LOCK_EX | LOCK_NB) == 0) {
        cJSON *latest = read_json(dir, "status.json");
        if (!latest) {
            ok = false;
        } else {
            cJSON_Delete(*snapshot);
            *snapshot = latest;
            if (active(str(latest, "stage"))) {
                ok = setstr(latest, "stage", "interrupted") &&
                     setstr(latest, "error", "Update interrupted; check installation before retrying");
            }
        }
    } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
        ok = false;
    }
    close(lock);
    return ok;
}

static void handle_status(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req; (void)ud;
    int dir = open_dir(false);
    cJSON *obj = dir >= 0 ? read_json(dir, "status.json") : NULL;
    if (!obj) {
        if (dir >= 0 && faccessat(dir, "status.json", F_OK, 0) == 0) {
            close(dir); mt_http_res_write_error(res, 500, "Cannot read update status"); return;
        }
        obj = cJSON_CreateObject();
        if (obj) { cJSON_AddStringToObject(obj, "stage", "idle"); }
    } else if (active(str(obj, "stage")) && !refresh_unlocked_status(dir, &obj)) {
        cJSON_Delete(obj);
        close(dir);
        mt_http_res_write_error(res, 500, "Cannot read update status");
        return;
    }
    if (dir >= 0) { close(dir); }
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    if (!obj) { mt_http_res_write_error(res, 500, "Out of memory"); return; }
    mt_http_res_write_json(res, 200, obj);
}

/* Only async-signal-safe operations between fork and exec in a threaded daemon.
 * A different executable name avoids Entware's killall magitrickled; setsid and
 * double-fork detach the worker from the HTTP connection/service process. */
/* The intermediate child cannot acknowledge the worker on its behalf:
 * access(X_OK) does not imply execve() will succeed (missing ELF loader,
 * corrupted binary, etc). A close-on-exec pipe provides the handshake:
 * an errno value means setup/exec failed; EOF means the grandchild exec'd.
 * Keep the write end away from fd 3, which carries the inherited lock. */
static bool launch_executable(int lock, const char *executable) {
    int ack[2];
    if (pipe2(ack, O_CLOEXEC) != 0) { return false; }
    if (ack[1] == 3) {
        int high = fcntl(ack[1], F_DUPFD_CLOEXEC, 4);
        if (high < 0) { close(ack[0]); close(ack[1]); return false; }
        close(ack[1]);
        ack[1] = high;
    }

    pid_t child = fork();
    if (child < 0) { close(ack[0]); close(ack[1]); return false; }
    if (child == 0) {
        close(ack[0]);
        if (setsid() < 0) { goto exec_failed; }
        pid_t worker = fork();
        if (worker < 0) { goto exec_failed; }
        if (worker > 0) { close(ack[1]); _exit(0); }
        if (lock != 3 && dup2(lock, 3) < 0) { goto exec_failed; }
        if (fcntl(3, F_SETFD, 0) < 0) { goto exec_failed; }
        sigset_t empty;
        if (sigemptyset(&empty) || sigprocmask(SIG_SETMASK, &empty, NULL)) {
            goto exec_failed;
        }
        char *const args[] = {(char *)executable, "--worker", NULL};
        char *const env[] = {"PATH=/opt/sbin:/opt/bin:/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", "HOME=/root", NULL};
        execve(executable, args, env);

exec_failed: {
        /* Only async-signal-safe functions are called in the forked child. */
        int exec_error = errno ? errno : EIO;
        ssize_t sent;
        do { sent = write(ack[1], &exec_error, sizeof(exec_error)); }
        while (sent < 0 && errno == EINTR);
        _exit(127);
    }
    }

    close(ack[1]);
    /* Bound the handshake so a wedged pre-exec worker cannot freeze epoll.
     * A timeout also terminates the newly detached process group. */
    bool confirmed = false;
    struct timespec started, current;
    if (clock_gettime(CLOCK_MONOTONIC, &started) == 0) {
        for (;;) {
            if (clock_gettime(CLOCK_MONOTONIC, &current) != 0) { break; }
            int64_t elapsed_ms = (int64_t)(current.tv_sec - started.tv_sec) * 1000 +
                                 (int64_t)(current.tv_nsec - started.tv_nsec) / 1000000;
            if (elapsed_ms >= 3000) { break; }
            struct pollfd pfd = {.fd = ack[0], .events = POLLIN | POLLHUP};
            int ready = poll(&pfd, 1, (int)(3000 - elapsed_ms));
            if (ready < 0 && errno == EINTR) { continue; }
            if (ready <= 0) { break; }
            int exec_error = 0;
            ssize_t n;
            do { n = read(ack[0], &exec_error, sizeof(exec_error)); }
            while (n < 0 && errno == EINTR);
            confirmed = n == 0; /* EOF: the worker reached execve(). */
            break;
        }
    }
    close(ack[0]);
    if (!confirmed) {
        /* setsid() creates a group led by child; kill a stalled pre-exec
         * worker before reporting an unsuccessful launch. */
        (void)kill(-child, SIGKILL);
        (void)kill(child, SIGKILL);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    return confirmed && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool launch(int lock) { return launch_executable(lock, UPDATER); }

static void handle_install(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_system_ctx_t *ctx = ud;
    char suffix[192] = "";
    const char *error = capability(req, suffix, sizeof(suffix));
    if (error) { mt_http_res_write_error(res, 403, error); return; }
    const char *site = mt_http_req_header(req, "Sec-Fetch-Site");
    const char *type = mt_http_req_header(req, "Content-Type");
    if ((site && strcmp(site, "cross-site") == 0) || !type || strncmp(type, "application/json", 16)) {
        mt_http_res_write_error(res, 400, "Expected same-origin JSON request"); return;
    }
    size_t len = 0;
    const uint8_t *body = mt_http_req_body(req, &len);
    cJSON *request = len && len <= 1024 ? cJSON_ParseWithLength((const char *)body, len) : NULL;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "release_id");
    const cJSON *preview = cJSON_GetObjectItemCaseSensitive(request, "preview");
    mt_update_version_t target, current;
    bool valid = cJSON_IsObject(request) && cJSON_GetArraySize(request) == 3 && cJSON_IsBool(preview) &&
        cJSON_IsNumber(id) && id->valuedouble >= 1 && id->valuedouble <= 9007199254740991.0 &&
        (double)(uint64_t)id->valuedouble == id->valuedouble &&
        mt_update_version_parse(str(request, "tag"), 1, &target) && !target.development &&
        mt_update_version_parse(MT_VERSION, MT_PACKAGE_REVISION, &current) && mt_update_version_compare(&target, &current) > 0;
    if (!valid) { cJSON_Delete(request); mt_http_res_write_error(res, 400, "Invalid or non-newer release"); return; }
    int dir = open_dir(true);
    int lock = dir >= 0 ? open_lock(dir) : -1;
    int code = 500;
    error = "Cannot prepare update state";
    if (lock < 0) { goto done; }
    if (flock(lock, LOCK_EX | LOCK_NB)) { code = 409; error = "An update is already running"; goto done; }
    if (!ctx->config_path || mt_app_save_config(ctx->app, ctx->config_path, ctx->config_version) != MT_OK) {
        error = "Cannot save configuration before update"; goto done;
    }
    uint8_t bytes[16];
    if (mt_random_bytes(bytes, sizeof(bytes)) != MT_OK) { goto done; }
    char job_id[33];
    for (size_t i = 0; i < sizeof(bytes); i++) { snprintf(job_id + i * 2, 3, "%02x", bytes[i]); }
    cJSON_AddStringToObject(request, "job_id", job_id);
    cJSON_AddStringToObject(request, "installed_version", MT_VERSION);
    cJSON_AddNumberToObject(request, "installed_revision", MT_PACKAGE_REVISION);
    cJSON_AddStringToObject(request, "asset_suffix", suffix);
    cJSON_AddStringToObject(request, "config_path", ctx->config_path);
    cJSON *state = cJSON_CreateObject();
    if (!state) { goto done; }
    cJSON_AddStringToObject(state, "job_id", job_id);
    cJSON_AddStringToObject(state, "stage", "queued");
    cJSON_AddStringToObject(state, "tag", str(request, "tag"));
    cJSON_AddStringToObject(state, "previous_version", MT_VERSION);
    cJSON_AddNumberToObject(state, "started_at", (double)time(NULL));
    bool written = write_json(dir, "request.json", request) && write_json(dir, "status.json", state);
    if (!written) { cJSON_Delete(state); goto done; }
    if (!launch(lock)) {
        error = "Cannot execute updater";
        (void)setstr(state, "stage", "failed");
        (void)setstr(state, "error", error);
        cJSON_AddNumberToObject(state, "finished_at", (double)time(NULL));
        (void)write_json(dir, "status.json", state);
        cJSON_Delete(state);
        goto done;
    }
    cJSON_Delete(state);
    cJSON *out = cJSON_CreateObject();
    if (!out) { goto done; }
    cJSON_AddStringToObject(out, "job_id", job_id);
    cJSON_AddStringToObject(out, "stage", "queued");
    mt_http_res_write_json(res, 202, out);
    error = NULL;
done:
    if (lock >= 0) { close(lock); }
    if (dir >= 0) { close(dir); }
    cJSON_Delete(request);
    if (error) { mt_http_res_write_error(res, code, error); }
}

void mt_update_register_routes(mt_httpd_t *http, mt_system_ctx_t *ctx) {
    if (mt_httpd_route(http, "GET", "/api/v1/system/update", handle_info, ctx) != MT_OK ||
        mt_httpd_route(http, "GET", "/api/v1/system/update/status", handle_status, ctx) != MT_OK ||
        mt_httpd_route(http, "POST", "/api/v1/system/update/install", handle_install, ctx) != MT_OK) {
        MT_ERROR("cannot register update routes");
    }
}

/* ---- independent worker ------------------------------------------------ */

typedef struct transfer {
    char *data;
    size_t len;
    size_t limit;
    int fd;
    mt_sha256_ctx_t hash;
} transfer_t;

static size_t receive(void *data, size_t size, size_t count, void *ud) {
    transfer_t *t = ud;
    if (size && count > SIZE_MAX / size) { return 0; }
    size_t len = size * count;
    if (len > t->limit - t->len) { return 0; }
    if (t->fd >= 0) {
        if (!write_all(t->fd, data, len)) { return 0; }
        mt_sha256_update(&t->hash, data, len);
    } else {
        char *next = realloc(t->data, t->len + len + 1);
        if (!next) { return 0; }
        t->data = next;
        memcpy(t->data + t->len, data, len);
        t->data[t->len + len] = '\0';
    }
    t->len += len;
    return len;
}

static CURL *http_client(transfer_t *t, const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) { return NULL; }
#define SET(option, value) do { if (curl_easy_setopt(curl, option, value) != CURLE_OK) { curl_easy_cleanup(curl); return NULL; } } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_USERAGENT, "mt-c-updater/1");
    SET(CURLOPT_CONNECTTIMEOUT, 15L);
    SET(CURLOPT_TIMEOUT, 180L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_WRITEFUNCTION, receive);
    SET(CURLOPT_WRITEDATA, t);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, "https");
#else
    SET(CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
#undef SET
    return curl;
}

static bool fetch_metadata(transfer_t *t) {
    /* Share the browser's bounded list, but fetch it independently at install
     * time. Both sides select by published_at with the same channel filter. */
    CURL *curl = http_client(t, MT_UPDATE_API "?per_page=100");
    if (!curl) { return false; }
    struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    if (!headers) { curl_easy_cleanup(curl); return false; }
    bool ok = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK;
    long status = 0;
    if (ok) { ok = curl_easy_perform(curl) == CURLE_OK && curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status == 200; }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

static bool download(const mt_update_asset_t *asset, transfer_t *t) {
    char url[4096];
    snprintf(url, sizeof(url), "%s", asset->url);
    for (unsigned i = 0; i < 4; i++) {
        if (!mt_update_download_host_allowed(url) || ftruncate(t->fd, 0) || lseek(t->fd, 0, SEEK_SET) < 0) { return false; }
        t->len = 0;
        mt_sha256_init(&t->hash);
        CURL *curl = http_client(t, url);
        if (!curl) { return false; }
        CURLcode code = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (code == CURLE_OK && status == 200) { curl_easy_cleanup(curl); return true; }
        char *redirect = NULL;
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect);
        bool next = code == CURLE_OK && status >= 300 && status < 400 && redirect && strlen(redirect) < sizeof(url) && mt_update_download_host_allowed(redirect);
        if (next) { snprintf(url, sizeof(url), "%s", redirect); }
        curl_easy_cleanup(curl);
        if (!next) { return false; }
    }
    return false;
}

static bool stage(int dir, cJSON *state, const char *value, const char *error) {
    return setstr(state, "stage", value) && setstr(state, "error", error ? error : "") && write_json(dir, "status.json", state);
}

static bool space_for(const char *path, size_t size) {
    struct statvfs st;
    if (statvfs(path, &st)) { return false; }
    /* Conservative preflight, not a promise of transactional package storage. */
    uint64_t need = (uint64_t)size * 6U + UINT64_C(8) * 1024U * 1024U;
    return (uint64_t)st.f_bavail * (uint64_t)st.f_frsize >= need;
}

static bool backup(int dir, const char *source, const char *name, bool optional) {
    int in = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0) { return optional && errno == ENOENT; }
    struct stat st;
    if (fstat(in, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > (off_t)MT_UPDATE_PACKAGE_LIMIT) { close(in); return false; }
    char temp[512];
    snprintf(temp, sizeof(temp), UPDATE_DIR "/.backup-XXXXXX");
    int out = mkstemp(temp);
    bool ok = out >= 0;
    char data[8192];
    size_t total = 0;
    while (ok) {
        ssize_t n = read(in, data, sizeof(data));
        if (n < 0 && errno == EINTR) { continue; }
        if (n == 0) { break; }
        if (n < 0 || (size_t)n > MT_UPDATE_PACKAGE_LIMIT - total) { ok = false; break; }
        total += (size_t)n;
        ok = write_all(out, data, (size_t)n);
    }
    close(in);
    if (ok) { ok = fsync(out) == 0; }
    if (out >= 0 && close(out)) { ok = false; }
    if (ok) { ok = renameat(AT_FDCWD, temp, dir, name) == 0 && fsync(dir) == 0; }
    if (!ok) { unlink(temp); }
    return ok;
}

static bool command(int dir, char *const argv[]) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) { return false; }
    int flags = fcntl(pipefd[0], F_GETFL);
    if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK)) {
        close(pipefd[0]); close(pipefd[1]); return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0) { close(pipefd[0]); return false; }
    int log = openat(dir, "install.log", O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat st;
    size_t logged = log >= 0 && fstat(log, &st) == 0 && st.st_size > 0 ? (size_t)st.st_size : 0;
    char buf[4096];
    int status = 0;
    pid_t waited = 0;
    bool eof = false;
    for (;;) {
        /* A restarted daemon can inherit a maintainer script's output pipe.
         * Wait for the command PID, not EOF from all its descendants. */
        struct pollfd pfd = {.fd = eof ? -1 : pipefd[0], .events = POLLIN};
        (void)poll(&pfd, 1, 100);
        for (unsigned chunk = 0; chunk < 32; chunk++) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n < 0 && errno == EINTR) { continue; }
            if (n == 0) { eof = true; break; }
            if (n < 0) { break; }
            if (log >= 0 && logged < LOG_LIMIT) {
                size_t len = (size_t)n < LOG_LIMIT - logged ? (size_t)n : LOG_LIMIT - logged;
                (void)write_all(log, buf, len);
                logged += len;
            }
        }
        if (waited == pid) { break; }
        waited = waitpid(pid, &status, WNOHANG);
        if (waited < 0 && errno != EINTR) { break; }
    }
    if (log >= 0) { close(log); }
    close(pipefd[0]);
    /* Never kill a live package manager on an arbitrary timer. */
    return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool healthy(const mt_update_asset_t *asset) {
    transfer_t t = {.fd = -1, .limit = STATE_LIMIT};
    CURL *curl = curl_easy_init();
    if (!curl) { return false; }
    bool ok = curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, MT_SOCK_PATH) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_URL, "http://localhost/api/v1/system/update") == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_PROXY, "") == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1500L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t) == CURLE_OK && curl_easy_perform(curl) == CURLE_OK;
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    cJSON *obj = ok && status == 200 && t.data ? cJSON_ParseWithLength(t.data, t.len) : NULL;
    const cJSON *rev = cJSON_GetObjectItemCaseSensitive(obj, "installed_revision");
    ok = obj && strcmp(str(obj, "installed_version"), asset->version) == 0 && cJSON_IsNumber(rev) && rev->valuedouble == (double)asset->revision;
    cJSON_Delete(obj); free(t.data);
    return ok;
}

static void close_inherited(void) {
    DIR *fds = opendir("/proc/self/fd");
    if (fds) {
        struct dirent *entry;
        while ((entry = readdir(fds)) != NULL) {
            char *end;
            long fd = strtol(entry->d_name, &end, 10);
            if (!*end && fd > 3 && fd != dirfd(fds) && fd <= INT32_MAX) { close((int)fd); }
        }
        closedir(fds);
    } else {
        long max = sysconf(_SC_OPEN_MAX);
        for (int fd = 4; fd < max; fd++) { close(fd); }
    }
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        if (null > 3) { close(null); }
    }
    (void)fcntl(3, F_SETFD, FD_CLOEXEC); /* package children must not own the lock */
}

int mt_update_worker(void) {
    if (geteuid() != 0) { return 1; }
    close_inherited();
    int dir = open_dir(false);
    if (dir < 0) { return 1; }
    struct stat inherited, named;
    if (fstat(3, &inherited) || fstatat(dir, "lock", &named, AT_SYMLINK_NOFOLLOW) ||
        inherited.st_dev != named.st_dev || inherited.st_ino != named.st_ino ||
        flock(3, LOCK_EX | LOCK_NB)) { close(dir); return 1; }
    cJSON *request = read_json(dir, "request.json");
    cJSON *state = read_json(dir, "status.json");
    const char *error = "Invalid update state";
    bool curl_ready = false, installed = false;
    char temp[] = "/tmp/mt-c-update-XXXXXX";
    char package[512] = "";
    bool temp_ready = false;
    int fd = -1;
    transfer_t metadata = {.fd = -1, .limit = MT_UPDATE_JSON_LIMIT};
    mt_update_asset_t asset;
    char suffix[192] = "";
    const char *manager = NULL;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "release_id");
    if (!request || !state || strlen(str(state, "job_id")) != 32 || strcmp(str(state, "job_id"), str(request, "job_id")) ||
        !cJSON_IsNumber(id) || id->valuedouble < 1 || id->valuedouble > 9007199254740991.0 ||
        strcmp(str(request, "installed_version"), MT_VERSION) || !identity(suffix, sizeof(suffix), &manager) ||
        strcmp(suffix, str(request, "asset_suffix"))) { goto done; }
    if (!stage(dir, state, "checking", NULL)) { goto done; }
    error = "Cannot contact GitHub; try again later";
    curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    bool preview = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "preview"));
    if (!curl_ready || !fetch_metadata(&metadata)) { goto done; }
    error = mt_update_resolve(metadata.data, metadata.len, preview, str(request, "tag"), (uint64_t)id->valuedouble,
                              suffix, MT_VERSION, MT_PACKAGE_REVISION, &asset);
    free(metadata.data); metadata.data = NULL;
    if (error) { goto done; }
    setstr(state, "target_version", asset.version);
    cJSON_AddNumberToObject(state, "target_revision", asset.revision);
    cJSON_AddNumberToObject(state, "package_size", (double)asset.size);
    error = "Not enough free space";
    if (!space_for("/tmp", asset.size) || !space_for(INSTALL_ROOT, asset.size) || !space_for(UPDATE_DIR, asset.size)) { goto done; }
    error = "Cannot prepare package download";
    if (!mkdtemp(temp)) { goto done; }
    temp_ready = true;
    bool apk = strlen(suffix) >= 4 && strcmp(suffix + strlen(suffix) - 4, ".apk") == 0;
    snprintf(package, sizeof(package), "%s/package.%s", temp, apk ? "apk" : "ipk");
    fd = open(package, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || !stage(dir, state, "downloading", NULL)) { goto done; }
    transfer_t t = {.fd = fd, .limit = asset.size};
    error = "Package download failed";
    if (!download(&asset, &t) || fsync(fd)) { goto done; }
    close(fd); fd = -1;
    error = "Package checksum mismatch";
    if (!stage(dir, state, "verifying", NULL)) { goto done; }
    uint8_t digest[32]; char hex[65];
    mt_sha256_final(&t.hash, digest);
    for (size_t i = 0; i < sizeof(digest); i++) { snprintf(hex + i * 2, 3, "%02x", digest[i]); }
    if (t.len != asset.size || strcmp(hex, asset.sha256)) { goto done; }
    char *ipk_pre[] = {(char *)manager, "--noaction", "install", package, NULL};
    char *apk_pre[] = {(char *)manager, "add", "--simulate", "--allow-untrusted", package, NULL};
    (void)unlinkat(dir, "install.log", 0);
    error = "Package manager preflight failed; see install.log";
    if (!command(dir, apk ? apk_pre : ipk_pre)) { goto done; }
    error = "Configuration backup failed";
    if (!stage(dir, state, "backing_up", NULL) || !backup(dir, str(request, "config_path"), "config.before.yaml", false) ||
        !backup(dir, MT_APP_STATE_DIR "/auth_secret", "auth_secret.before", true)) { goto done; }
    setstr(state, "backup_path", UPDATE_DIR "/config.before.yaml");
    error = "Cannot persist installation state";
    if (!stage(dir, state, "installing", NULL)) { goto done; }
    char *ipk_install[] = {(char *)manager, "install", package, NULL};
    char *apk_install[] = {(char *)manager, "add", "--allow-untrusted", package, NULL};
    error = "Package installation failed; see install.log";
    if (!command(dir, apk ? apk_install : ipk_install)) {
        char *recover[] = {INIT_SCRIPT, "start", NULL};
        (void)command(dir, recover);
        goto done;
    }
    installed = true;
    error = "Service did not start after update";
    if (!stage(dir, state, "restarting", NULL)) { goto done; }
    for (unsigned attempt = 0; attempt < 30; attempt++) {
        if (healthy(&asset)) { error = NULL; break; }
        if (attempt == 2) {
            char *restart[] = {INIT_SCRIPT, "restart", NULL};
            (void)command(dir, restart);
        }
        sleep(2);
    }
done:
    if (fd >= 0) { close(fd); }
    if (package[0]) { unlink(package); }
    if (temp_ready) { rmdir(temp); }
    free(metadata.data);
    if (state) {
        cJSON_AddBoolToObject(state, "package_installed", installed);
        cJSON_AddNumberToObject(state, "finished_at", (double)time(NULL));
        if (!stage(dir, state, error ? "failed" : "succeeded", error)) { error = "Cannot persist update result"; }
    }
    cJSON_Delete(request); cJSON_Delete(state);
    if (curl_ready) { curl_global_cleanup(); }
    close(dir); close(3);
    return error ? 1 : 0;
}
