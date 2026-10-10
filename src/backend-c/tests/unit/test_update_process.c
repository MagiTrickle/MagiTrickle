/* Exercise the real system-package handoff, never a real package manager. */
#define _GNU_SOURCE
#include <sys/stat.h>
static int fixture_fstat(int fd, struct stat *st) {
    int result = fstat(fd, st);
    if (result == 0) { st->st_uid = 0; }
    return result;
}
#define fstat fixture_fstat
#include "../../src/platform/update.c"
#undef fstat
#include <assert.h>

static void script(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
    assert(fd >= 0 && write_all(fd, text, strlen(text)) && close(fd) == 0);
}

static void wait_file(const char *path) {
    for (unsigned i = 0; i < 500; i++) {
        if (access(path, F_OK) == 0) { return; }
        usleep(10000);
    }
    assert(!"timed out waiting for fixture");
}

static void handoff(bool apk, bool fail, unsigned closed_stdio) {
    char root[] = "/tmp/mt-package-handoff-XXXXXX";
    assert(mkdtemp(root));
    int dir = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(dir >= 0);
    char temp[512], package[512], manager[512], init[512], ready[512], gate[512], restarted[512];
    snprintf(temp, sizeof(temp), "%s/payload", root);
    snprintf(package, sizeof(package), "%s/payload/package.%s", root, apk ? "apk" : "ipk");
    snprintf(manager, sizeof(manager), "%s/manager", root);
    snprintf(init, sizeof(init), "%s/init", root);
    snprintf(ready, sizeof(ready), "%s/ready", root);
    snprintf(gate, sizeof(gate), "%s/gate", root);
    snprintf(restarted, sizeof(restarted), "%s/restarted", root);
    assert(mkdir(temp, 0700) == 0);
    script(package, "verified package fixture\n");
    char text[4096];
    snprintf(text, sizeof(text),
        "#!/bin/sh\nset -eu\n[ ! -e /proc/self/fd/3 ]\n[ ! -e /proc/self/fd/19 ]\n"
        "[ \"$1\" = '%s' ]\n%s[ -f \"$%d\" ]\n"
        "echo package-manager-progress\necho package-manager-diagnostics >&2\ntouch '%s'\n"
        "while [ ! -f '%s' ]; do sleep 0.01; done\nexit %d\n",
        apk ? "add" : "install", apk ? "[ \"$2\" = '--allow-untrusted' ]\n" : "",
        apk ? 3 : 2, ready, gate, fail ? 7 : 0);
    script(manager, text);
    snprintf(text, sizeof(text), "#!/bin/sh\nset -eu\n[ \"$1\" = restart ]\n"
        "[ ! -e /proc/self/fd/3 ]\n[ ! -e /proc/self/fd/19 ]\ntouch '%s'\n", restarted);
    script(init, text);
    int notice[2];
    assert(pipe2(notice, O_CLOEXEC) == 0);
    pid_t daemon = fork();
    assert(daemon >= 0);
    if (daemon == 0) {
        close(notice[0]);
        assert(setsid() >= 0);
        int lock = open_lock(dir);
        assert(lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0);
        int inherited = open("/dev/null", O_RDONLY);
        assert(inherited >= 0 && dup2(inherited, 19) == 19);
        assert(fcntl(19, F_SETFD, 0) == 0); /* Intentionally non-CLOEXEC. */
        /* Simulate service managers that close some or all stdio fds. */
        for (int stream = 0; stream < 3; stream++) {
            if (closed_stdio & (1U << stream)) { close(stream); }
        }
        assert(launch_install(lock, manager, package, init, temp, apk, "/dev/null"));
        close(lock);
        assert(write_all(notice[1], "x", 1));
        close(notice[1]);
        for (;;) { pause(); }
    }
    close(notice[1]);
    char byte;
    assert(read(notice[0], &byte, 1) == 1);
    close(notice[0]);
    wait_file(ready);
    /* The launching daemon and its process group die mid-transaction. */
    assert(kill(-daemon, SIGTERM) == 0);
    int status;
    assert(waitpid(daemon, &status, 0) == daemon && WIFSIGNALED(status));
    int lock = open_lock(dir);
    assert(lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == -1);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    script(gate, "continue\n");
    bool unlocked = false;
    for (unsigned i = 0; i < 500; i++) {
        if (flock(lock, LOCK_EX | LOCK_NB) == 0) { unlocked = true; break; }
        usleep(10000);
    }
    assert(unlocked);
    assert(access(restarted, F_OK) == (fail ? -1 : 0));
    assert(access(package, F_OK) == -1 && access(temp, F_OK) == -1);
    assert(faccessat(dir, "install.log", F_OK, 0) == -1);
    assert(faccessat(dir, "status.json", F_OK, 0) == -1);
    assert(faccessat(dir, "request.json", F_OK, 0) == -1);
    close(lock);
    unlink(manager); unlink(init); unlink(ready); unlink(gate); unlink(restarted);
    assert(unlinkat(dir, "lock", 0) == 0);
    close(dir);
    assert(rmdir(root) == 0);
}

int main(void) {
    for (unsigned mask = 0; mask < 8; mask++) {
        handoff(false, false, mask);
        handoff(true, false, mask);
    }
    handoff(false, true, 0);
    handoff(true, true, 1);
    int lock = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(lock >= 0);
    assert(!launch_install(lock, "/nonexistent-manager", "/missing-package", "/missing-init",
                           "/missing-temp", false, "/nonexistent-console"));
    close(lock);
    atomic_store(&stopping, true);
    assert(cancel_transfer(NULL, 0, 0, 0, 0) == 1);
    mt_update_shutdown();
    puts("system package handoff, parent death, descriptor isolation, failure and cleanup: passed");
    return 0;
}
