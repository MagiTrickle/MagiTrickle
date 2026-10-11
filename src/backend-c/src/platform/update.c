/* On-demand update preparation runs in a daemon thread. Installation is handed
 * to the system package manager; no second mt-c binary/process or update journal.
 * The short system-shell command only waits for the manager, restarts the service
 * and removes its temporary package. It does not track/persist update results. */
#define _GNU_SOURCE
#include "magitrickle/update.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "magitrickle/hash.h"
#include "magitrickle/httpd.h"
#include "magitrickle/log.h"
#include "magitrickle/paths.h"
#include "magitrickle/rand.h"
#include "magitrickle/system.h"
#include "magitrickle/version.h"

/* Retain the lock path so a still-running old updater also serializes with us.
 * No request/status JSON, install.log or backup files are created here. */
#define UPDATE_DIR MT_APP_STATE_DIR "/update"
#if defined(MT_PLATFORM_ENTWARE)
#define INIT_SCRIPT "/opt/etc/init.d/S99magitrickle"
#define INSTALL_ROOT "/opt"
#else
#define INIT_SCRIPT "/etc/init.d/magitrickle"
#define INSTALL_ROOT "/"
#endif

static const char *str(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
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
    if (fstat(fd, &st)) { close(fd); return -1; }
    if (st.st_uid != 0 || (st.st_mode & 0077)) { close(fd); errno = EACCES; return -1; }
    return fd;
}

static int open_lock(int dir) {
    int fd = openat(dir, "lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat st;
    if (fd < 0) { return -1; }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077)) { close(fd); return -1; }
    return fd;
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

static const char *capability(char *suffix, size_t cap) {
    const char *manager = NULL;
    mt_update_version_t version;
    if (!identity(suffix, cap, &manager)) { return "Unsupported build target"; }
    if (!mt_update_version_parse(MT_VERSION, MT_PACKAGE_REVISION, &version)) { return "Unknown installed version"; }
    if (geteuid() != 0 || access("/bin/sh", X_OK) || access(manager, X_OK)) { return "Installer unavailable"; }
    return NULL;
}

static void handle_info(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req; (void)ud;
    char suffix[192] = "";
    const char *reason = capability(suffix, sizeof(suffix));
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

typedef struct update_job {
    char job_id[33];
    char tag[80];
    char suffix[192];
    char target_version[64];
    uint32_t target_revision;
    uint64_t release_id;
    bool preview;
    int lock;
    const char *manager;
} update_job_t;

typedef struct update_state {
    char job_id[33];
    char tag[80];
    char target_version[64];
    uint32_t target_revision;
    const char *stage;
    const char *error;
} update_state_t;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static update_state_t state = {.stage = "idle", .error = ""};
static pthread_t prepare_thread;
/* Creation/join are loop-thread-only; the download thread never owns app/HTTP. */
static bool prepare_joinable;
static atomic_bool stopping;

static void set_stage(const char *value, const char *error) {
    pthread_mutex_lock(&state_mutex);
    state.stage = value;
    state.error = error ? error : "";
    pthread_mutex_unlock(&state_mutex);
}

/* 1 = a job owns the lock, 0 = free, -1 = unsafe/unreadable lock. */
static int installation_busy(void) {
    int dir = open_dir(false);
    if (dir < 0) { return errno == ENOENT ? 0 : -1; }
    int lock = open_lock(dir);
    close(dir);
    if (lock < 0) { return -1; }
    int result = 0;
    if (flock(lock, LOCK_EX | LOCK_NB)) {
        result = errno == EAGAIN || errno == EWOULDBLOCK ? 1 : -1;
    }
    close(lock);
    return result;
}

static void handle_status(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req; (void)ud;
    pthread_mutex_lock(&state_mutex);
    update_state_t snapshot = state;
    pthread_mutex_unlock(&state_mutex);
    int busy = installation_busy();
    if (busy < 0) { mt_http_res_write_error(res, 500, "Cannot read update status"); return; }
    bool external = busy && !snapshot.job_id[0];
    /* A restarted daemon has no historical job state. A live package manager
     * still holds the lock; after it exits the browser compares the version.
     * Never infer package-manager success from either a restart or a free lock. */
    if (external) { snapshot.stage = "installing"; }
    if (!busy && strcmp(snapshot.stage, "installing") == 0) {
        snapshot.stage = "idle";
    }
    cJSON *obj = cJSON_CreateObject();
    if (!obj) { mt_http_res_write_error(res, 500, "Out of memory"); return; }
    cJSON_AddStringToObject(obj, "stage", snapshot.stage);
    if (external) { cJSON_AddBoolToObject(obj, "external", true); }
    if (snapshot.job_id[0]) {
        cJSON_AddStringToObject(obj, "job_id", snapshot.job_id);
        cJSON_AddStringToObject(obj, "tag", snapshot.tag);
        cJSON_AddStringToObject(obj, "target_version", snapshot.target_version);
        cJSON_AddNumberToObject(obj, "target_revision", snapshot.target_revision);
    }
    if (snapshot.error[0]) { cJSON_AddStringToObject(obj, "error", snapshot.error); }
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    mt_http_res_write_json(res, 200, obj);
}

/* Fixed shell text; every path/argument is passed separately, never interpolated.
 * fd 3 keeps the update lock in this short-lived system shell only. Neither
 * opkg/apk's maintainer scripts nor the restarted daemon may inherit that fd.
 * stdout/stderr go to the system console, NOT the dying service's output pipe.
 * Detailed diagnostics/recovery are deliberately left to the system/CLI. */
static const char install_command[] =
    "trap 'rm -f -- \"$2\"; rmdir -- \"$4\"' EXIT\n"
    "read -r ready && [ \"$ready\" = go ] || exit 1\n"
    "exec </dev/null\n"
    "if [ \"$5\" = apk ]; then\n"
    "  \"$1\" add --allow-untrusted \"$2\" 3>&-\n"
    "else\n"
    "  \"$1\" install \"$2\" 3>&-\n"
    "fi\n"
    "result=$?\n"
    "if [ \"$result\" -eq 0 ]; then \"$3\" restart 3>&-; fi\n"
    "exit \"$result\"\n";

static bool launch_install(int lock, const char *manager, const char *package,
                           const char *init_script, const char *temp, bool apk,
                           const char *console) {
    int ack[2], gate[2];
    if (pipe2(ack, O_CLOEXEC)) { return false; }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, gate)) {
        close(ack[0]); close(ack[1]); return false;
    }
    /* Normalize sources above stdio/the lock before dup2, even if the service
     * started with closed stdin/stdout. The gate prevents an exec-timeout from
     * ever killing a package manager: the shell cannot start it before "go". */
    int writer = fcntl(ack[1], F_DUPFD_CLOEXEC, 5);
    int input = fcntl(gate[1], F_DUPFD_CLOEXEC, 5);
    int inherited_lock = fcntl(lock, F_DUPFD_CLOEXEC, 5);
    close(ack[1]); close(gate[1]);
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (writer < 0 || input < 0 || inherited_lock < 0 || max_fd < 0 || max_fd > INT_MAX) {
        if (writer >= 0) { close(writer); }
        if (input >= 0) { close(input); }
        if (inherited_lock >= 0) { close(inherited_lock); }
        close(ack[0]); close(gate[0]); return false;
    }
    char *const args[] = {"sh", "-c", (char *)install_command, "mt-c-install",
        (char *)manager, (char *)package, (char *)init_script, (char *)temp,
        apk ? "apk" : "ipk", NULL};
    char *const env[] = {"PATH=/opt/sbin:/opt/bin:/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", "HOME=/root", NULL};
    pid_t child = fork();
    if (child < 0) {
        close(ack[0]); close(writer); close(gate[0]); close(input); close(inherited_lock);
        return false;
    }
    if (child == 0) {
        /* Only async-signal-safe calls before exec in this threaded process. */
        close(ack[0]); close(gate[0]);
        if (setsid() < 0) { goto failed; }
        pid_t grandchild = fork();
        if (grandchild < 0) { goto failed; }
        if (grandchild > 0) { close(writer); _exit(0); }
        if (dup2(inherited_lock, 3) < 0 || fcntl(3, F_SETFD, 0) < 0) { goto failed; }
        /* With closed stdin/stdout/stderr, open() could return 0/1/2.
         * Reserve a high descriptor before dup2(input,0) so a console
         * stream cannot be replaced by the short-lived gate socket. */
        int raw_output = open(console, O_WRONLY | O_NOCTTY | O_CLOEXEC);
        int output = raw_output >= 0 ? fcntl(raw_output, F_DUPFD_CLOEXEC, 5) : -1;
        if (raw_output >= 0) { close(raw_output); }
        if (output < 0 || dup2(input, 0) < 0 || dup2(output, 1) < 0 || dup2(output, 2) < 0) {
            goto failed;
        }
        for (int fd = 4; fd < max_fd; fd++) {
            if (fd != writer) { close(fd); }
        }
        sigset_t empty;
        sigemptyset(&empty);
        if (sigprocmask(SIG_SETMASK, &empty, NULL)) { goto failed; }
        execve("/bin/sh", args, env);
failed: {
            int err = errno;
            ssize_t sent;
            do { sent = write(writer, &err, sizeof(err)); } while (sent < 0 && errno == EINTR);
            _exit(127);
        }
    }
    close(writer); close(input); close(inherited_lock);
    /* This wait is on the preparation thread, never the DNS/HTTP loop. */
    struct pollfd pfd = {.fd = ack[0], .events = POLLIN | POLLHUP};
    int ready;
    do { ready = poll(&pfd, 1, 3000); } while (ready < 0 && errno == EINTR);
    int err = 0;
    ssize_t n = -1;
    if (ready > 0) {
        do { n = read(ack[0], &err, sizeof(err)); } while (n < 0 && errno == EINTR);
    }
    close(ack[0]);
    bool ok = ready > 0 && n == 0;
    if (!ok) { (void)kill(-child, SIGKILL); (void)kill(child, SIGKILL); }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    ok = ok && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    /* After this send succeeds, nothing in the daemon kills/waits for the
     * transaction. MSG_NOSIGNAL handles a shell that exited before the gate. */
    if (ok) { ok = send(gate[0], "go\n", 3, MSG_NOSIGNAL) == 3; }
    close(gate[0]);
    return ok;
}

static int cancel_transfer(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)ud; (void)dt; (void)dn; (void)ut; (void)un;
    return atomic_load(&stopping) ? 1 : 0;
}

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
    SET(CURLOPT_USERAGENT, "magitrickled-update/1");
    SET(CURLOPT_CONNECTTIMEOUT, 15L);
    SET(CURLOPT_TIMEOUT, 180L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_NOPROGRESS, 0L);
    SET(CURLOPT_XFERINFOFUNCTION, cancel_transfer);
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

static bool space_for(const char *path, size_t size) {
    struct statvfs st;
    if (statvfs(path, &st)) { return false; }
    /* Conservative preflight, not a promise of transactional package storage. */
    uint64_t need = (uint64_t)size * 6U + UINT64_C(8) * 1024U * 1024U;
    return (uint64_t)st.f_bavail * (uint64_t)st.f_frsize >= need;
}

static void *prepare_install(void *ud) {
    update_job_t *job = ud;
    const char *error = "Cannot contact GitHub; try again later";
    char temp[] = "/tmp/mt-c-update-XXXXXX";
    char package[512] = "";
    bool handed_off = false, temp_ready = false;
    int fd = -1;
    transfer_t metadata = {.fd = -1, .limit = MT_UPDATE_JSON_LIMIT};
    mt_update_asset_t asset;
    set_stage("checking", NULL);
    if (atomic_load(&stopping) || !fetch_metadata(&metadata)) { goto done; }
    error = mt_update_resolve(metadata.data, metadata.len, job->preview, job->tag,
        job->release_id, job->suffix, MT_VERSION, MT_PACKAGE_REVISION, &asset);
    free(metadata.data); metadata.data = NULL;
    if (error) { goto done; }
    error = "Not enough free space";
    if (!space_for("/tmp", asset.size) || !space_for(INSTALL_ROOT, asset.size)) { goto done; }
    error = "Cannot prepare package download";
    if (!mkdtemp(temp)) { goto done; }
    temp_ready = true;
    bool apk = strlen(job->suffix) >= 4 && strcmp(job->suffix + strlen(job->suffix) - 4, ".apk") == 0;
    snprintf(package, sizeof(package), "%s/package.%s", temp, apk ? "apk" : "ipk");
    fd = open(package, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { goto done; }
    set_stage("downloading", NULL);
    transfer_t data = {.fd = fd, .limit = asset.size};
    error = "Package download failed";
    if (!download(&asset, &data) || fsync(fd)) { goto done; }
    close(fd); fd = -1;
    set_stage("verifying", NULL);
    error = "Package checksum mismatch";
    uint8_t digest[32]; char hex[65];
    mt_sha256_final(&data.hash, digest);
    for (size_t i = 0; i < sizeof(digest); i++) { snprintf(hex + i * 2, 3, "%02x", digest[i]); }
    if (data.len != asset.size || strcmp(hex, asset.sha256)) { goto done; }
    error = "Cannot start package manager";
    if (atomic_load(&stopping)) { goto done; }
    set_stage("installing", NULL);
    /* No daemon PID or descriptors are needed after this handoff. The system
     * shell and manager survive rc.func/procd stopping magitrickled. */
    handed_off = launch_install(job->lock, job->manager, package, INIT_SCRIPT, temp, apk, "/dev/console");
    if (handed_off) { error = NULL; }
done:
    if (fd >= 0) { close(fd); }
    if (!handed_off) {
        if (package[0]) { unlink(package); }
        if (temp_ready) { rmdir(temp); }
        set_stage("failed", atomic_load(&stopping) ? "Update preparation stopped" : error);
    }
    free(metadata.data);
    close(job->lock);
    free(job);
    return NULL;
}

void mt_update_shutdown(void) {
    atomic_store(&stopping, true);
    if (prepare_joinable) {
        pthread_join(prepare_thread, NULL);
        prepare_joinable = false;
    }
    /* Never wait for or kill a live package-manager transaction. */
}

static void handle_install(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_system_ctx_t *ctx = ud;
    char suffix[192] = "";
    const char *error = capability(suffix, sizeof(suffix));
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
    if (dir >= 0) { close(dir); }
    int code = 500;
    error = "Cannot prepare update";
    update_job_t *job = NULL;
    cJSON *accepted = NULL;
    if (lock < 0) { goto done; }
    if (flock(lock, LOCK_EX | LOCK_NB)) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) { code = 409; error = "An update is already running"; }
        goto done;
    }
    if (prepare_joinable) { pthread_join(prepare_thread, NULL); prepare_joinable = false; }
    if (atomic_load(&stopping)) { goto done; }
    if (!ctx->config_path || mt_app_save_config(ctx->app, ctx->config_path, ctx->config_version) != MT_OK) {
        error = "Cannot save configuration before update"; goto done;
    }
    job = calloc(1, sizeof(*job));
    accepted = cJSON_CreateObject();
    if (!job || !accepted) { goto done; }
    uint8_t random[16];
    if (mt_random_bytes(random, sizeof(random)) != MT_OK) { goto done; }
    for (size_t i = 0; i < sizeof(random); i++) { snprintf(job->job_id + i * 2, 3, "%02x", random[i]); }
    snprintf(job->tag, sizeof(job->tag), "%s", str(request, "tag"));
    snprintf(job->suffix, sizeof(job->suffix), "%s", suffix);
    snprintf(job->target_version, sizeof(job->target_version), "%s", target.base);
    job->target_revision = target.revision;
    job->release_id = (uint64_t)id->valuedouble;
    job->preview = cJSON_IsTrue(preview);
    job->lock = lock;
    if (!identity(suffix, sizeof(suffix), &job->manager) ||
        !cJSON_AddStringToObject(accepted, "job_id", job->job_id) ||
        !cJSON_AddStringToObject(accepted, "stage", "queued")) { goto done; }
    pthread_mutex_lock(&state_mutex);
    snprintf(state.job_id, sizeof(state.job_id), "%s", job->job_id);
    snprintf(state.tag, sizeof(state.tag), "%s", job->tag);
    snprintf(state.target_version, sizeof(state.target_version), "%s", target.base);
    state.target_revision = target.revision;
    state.stage = "queued";
    state.error = "";
    pthread_mutex_unlock(&state_mutex);
    if (pthread_create(&prepare_thread, NULL, prepare_install, job)) {
        set_stage("failed", "Cannot start update preparation"); goto done;
    }
    prepare_joinable = true;
    job = NULL; lock = -1; /* owned by the preparation thread, then system shell */
    mt_http_res_write_json(res, 202, accepted);
    accepted = NULL;
    error = NULL;
done:
    if (lock >= 0) { close(lock); }
    free(job);
    cJSON_Delete(accepted);
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
