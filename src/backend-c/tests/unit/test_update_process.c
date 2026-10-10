/* Exercise the real subprocess boundary, including descendants that retain stdout. */
#define _GNU_SOURCE
#include <sys/stat.h>

/* Model root-owned files without requiring sudo for make test. All descriptors
 * here refer to our private mkdtemp fixtures; modes/inodes/locks are real. */
static int fixture_fstat(int fd, struct stat *st) {
    int result = fstat(fd, st);
    if (result == 0) { st->st_uid = 0; }
    return result;
}
#define fstat fixture_fstat
#include "../../src/platform/update.c"
#undef fstat
#include <assert.h>

static void replace_status(int dir, const char *value) {
    int fd = openat(dir, "status.next", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    assert(write_all(fd, value, strlen(value)));
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);
    assert(renameat(dir, "status.next", dir, "status.json") == 0);
}

static void test_status_race(int dir, const char *terminal) {
    replace_status(dir, "{\"stage\":\"downloading\"}");
    int held = open_lock(dir);
    assert(held >= 0 && flock(held, LOCK_EX | LOCK_NB) == 0);
    int gate[2];
    assert(pipe(gate) == 0);
    pid_t worker = fork();
    assert(worker >= 0);
    if (worker == 0) {
        close(gate[1]);
        char ready;
        assert(read(gate[0], &ready, 1) == 1);
        replace_status(dir, terminal);
        assert(flock(held, LOCK_UN) == 0);
        close(held); close(gate[0]);
        _exit(0);
    }
    close(gate[0]);
    close(held);
    cJSON *snapshot = read_json(dir, "status.json");
    assert(snapshot && strcmp(str(snapshot, "stage"), "downloading") == 0);
    /* Force the worker to finish after the first read and before the probe. */
    assert(write_all(gate[1], "x", 1));
    close(gate[1]);
    int status;
    assert(waitpid(worker, &status, 0) == worker && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(refresh_unlocked_status(dir, &snapshot));
    cJSON *expected = cJSON_Parse(terminal);
    assert(expected && strcmp(str(snapshot, "stage"), str(expected, "stage")) == 0);
    assert(strcmp(str(snapshot, "error"), str(expected, "error")) == 0);
    cJSON_Delete(snapshot); cJSON_Delete(expected);
}

static void test_status_recovery(int dir) {
    test_status_race(dir, "{\"stage\":\"succeeded\",\"error\":\"\"}");
    test_status_race(dir, "{\"stage\":\"failed\",\"error\":\"Package checksum mismatch\"}");
    replace_status(dir, "{\"stage\":\"installing\"}");
    cJSON *snapshot = read_json(dir, "status.json");
    assert(snapshot);
    int held = open_lock(dir);
    assert(held >= 0 && flock(held, LOCK_EX | LOCK_NB) == 0);
    assert(refresh_unlocked_status(dir, &snapshot));
    assert(strcmp(str(snapshot, "stage"), "installing") == 0);
    assert(flock(held, LOCK_UN) == 0);
    close(held);
    assert(refresh_unlocked_status(dir, &snapshot));
    assert(strcmp(str(snapshot, "stage"), "interrupted") == 0);
    assert(unlinkat(dir, "status.json", 0) == 0);
    assert(!refresh_unlocked_status(dir, &snapshot));
    cJSON_Delete(snapshot);
    assert(unlinkat(dir, "lock", 0) == 0);
}

int main(void) {
    char path[] = "/tmp/mt-c-command-test-XXXXXX";
    assert(mkdtemp(path));
    int dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(dir >= 0);
    test_status_recovery(dir);
    char *ok[] = {"/bin/sh", "-c", "printf 'package log'; exit 0", NULL};
    char *bad[] = {"/bin/sh", "-c", "exit 7", NULL};
    char *missing[] = {"/nonexistent-mt-c-test-program", NULL};
    assert(command(dir, ok));
    assert(!command(dir, bad));
    assert(!command(dir, missing));
    /* A successful intermediate fork is not enough: only a successful
     * grandchild execve() can make launch_executable() return true. */
    int lock = openat(dir, "launch.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    assert(lock >= 0);
    assert(flock(lock, LOCK_EX | LOCK_NB) == 0);
    assert(launch_executable(lock, "/bin/true"));
    assert(!launch_executable(lock, "/nonexistent-mt-c-updater"));

    /* access(X_OK) succeeds for a corrupt binary but execve() fails with
     * ENOEXEC; the install endpoint must return 500, never a false 202. */
    char corrupt[512];
    assert(snprintf(corrupt, sizeof(corrupt), "%s/broken-updater", path) > 0);
    int bad_exec = open(corrupt, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0700);
    assert(bad_exec >= 0);
    assert(write_all(bad_exec, "not an executable", 17));
    assert(close(bad_exec) == 0);
    assert(access(corrupt, X_OK) == 0);
    assert(!launch_executable(lock, corrupt));
    assert(unlink(corrupt) == 0);
    assert(flock(lock, LOCK_UN) == 0);
    close(lock);
    assert(unlinkat(dir, "launch.lock", 0) == 0);
    char *inherited_pipe[] = {"/bin/sh", "-c", "sleep 4 & exit 0", NULL};
    struct timespec before, after;
    assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    assert(command(dir, inherited_pipe));
    assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    double elapsed = (double)(after.tv_sec - before.tv_sec) +
        (double)(after.tv_nsec - before.tv_nsec) / 1000000000.0;
    assert(elapsed < 3.0); /* Must not wait for the background descendant's EOF. */
    char *large[] = {"/bin/sh", "-c", "head -c 200000 /dev/zero", NULL};
    assert(command(dir, large));
    struct stat st;
    assert(fstatat(dir, "install.log", &st, 0) == 0);
    assert(st.st_size == LOG_LIMIT);
    assert(unlinkat(dir, "install.log", 0) == 0);
    close(dir);
    assert(rmdir(path) == 0);
    puts("update process boundary checks passed");
    return 0;
}
