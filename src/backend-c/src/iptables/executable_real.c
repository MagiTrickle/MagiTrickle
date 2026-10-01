/* Real Executable backend — port of utils/iptables/executable-real.go.
 *
 * Runs {iptables,ip6tables}-{save,restore} via posix_spawnp with a fixed,
 * hard-coded argv array (no shell, ever): posix_spawnp resolves argv[0]
 * through PATH the same way execvp does, so there is no interpolation of
 * any rule/interface/group content into a shell command line -- none of
 * that content reaches this file at all; only the iptables-restore
 * transcript bytes built by engine.c are piped to the child's stdin.
 *
 * Save(): drains stdout (captured, bounded) and stderr (captured for
 * error reporting, bounded) concurrently via poll() until both close,
 * then waits for the child.
 * Cancellation: when a token is attached (mt_ipt_set_cancel), its fd
 * joins both poll() sets. Raising it while a child is running kills that
 * child and returns MT_ERR_CANCELED -- iptables-restore applies each
 * table in one atomic setsockopt at its COMMIT line, so a killed child
 * leaves whole tables applied or not applied, never a half-written one,
 * and the caller starts over from a full rebuild anyway.
 *
 * Restore(): writes the transcript to the child's stdin while
 * concurrently draining stdout (discarded, matching Go which never reads
 * it either) and stderr (captured for error reporting), all through
 * poll() on non-blocking fds -- this avoids the classic pipe deadlock
 * (child blocks writing to a full stderr pipe while we block writing more
 * to its stdin) that a naive synchronous write-then-read would hit.
 */
#include "magitrickle/iptables.h"
#include "magitrickle/bytebuf.h"
#include "magitrickle/log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define MT_IPT_STDERR_CAP ((size_t)256 * 1024)

typedef struct exe_real {
    mt_ipt_executable_t base;
    mt_ipt_proto_t proto;
    const char *save_cmd;
    const char *restore_cmd;
    mt_cancel_t *cancel; /* borrowed, nullable */
} exe_real_t;

/* Messages iptables emits when the table changed under us: a chain we
 * reference was deleted, one we create already exists, or another writer
 * holds the lock. None of these mean our rules are wrong -- they mean the
 * table we diffed against is gone -- so they map to MT_ERR_AGAIN and the
 * caller rebuilds from scratch rather than reporting a failure. */
static const char *const k_retryable_messages[] = {
    "No chain/target/match by that name",
    "No such file or directory",
    "Chain already exists",
    "does a rule with that number exist",
    "Resource temporarily unavailable",
    "Device or resource busy",
    "holding the xtables lock",
    "doesn't exist",
    "does not exist",
};

static bool stderr_is_retryable(const mt_bytebuf_t *err_buf) {
    if (err_buf->len == 0 || err_buf->data == NULL) { return false; }

    /* mt_bytebuf_t is not NUL-terminated; copy into a bounded scratch
     * buffer so the substring search cannot run off the end. */
    char scratch[4096];
    size_t n = err_buf->len < sizeof(scratch) - 1 ? err_buf->len : sizeof(scratch) - 1;
    memcpy(scratch, err_buf->data, n);
    scratch[n] = '\0';

    for (size_t i = 0; i < sizeof(k_retryable_messages) / sizeof(k_retryable_messages[0]); i++) {
        if (strstr(scratch, k_retryable_messages[i]) != NULL) { return true; }
    }
    return false;
}

/* Kills and reaps a child whose work we no longer want. */
static void kill_child(pid_t pid) {
    kill(pid, SIGKILL);
    int status;
    pid_t w;
    do {
        w = waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR);
}

static mt_err_t set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) { return mt_err_from_errno(errno); }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { return mt_err_from_errno(errno); }
    return MT_OK;
}

static void close_if_valid(int fd) {
    if (fd >= 0) { close(fd); }
}

static mt_err_t spawn_with_pipes(const char *const argv[], bool need_stdin_pipe, pid_t *pid_out,
                                 int *stdin_wr, int *stdout_rd, int *stderr_rd) {
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};

    /* Set before any early return: mt_err_from_errno(errno) is only
     * guaranteed non-MT_OK because POSIX guarantees a failing syscall
     * sets errno to a nonzero value, not because this function enforces
     * it -- initializing the out-params up front means a stale/zero
     * errno on some exotic libc can never leave them looking valid. */
    *pid_out = -1;
    *stdin_wr = -1;
    *stdout_rd = -1;
    *stderr_rd = -1;

    if (need_stdin_pipe && pipe(in_pipe) != 0) { return mt_err_from_errno(errno); }
    if (pipe(out_pipe) != 0) {
        close_if_valid(in_pipe[0]);
        close_if_valid(in_pipe[1]);
        return mt_err_from_errno(errno);
    }
    if (pipe(err_pipe) != 0) {
        close_if_valid(in_pipe[0]);
        close_if_valid(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return mt_err_from_errno(errno);
    }

    posix_spawn_file_actions_t fa;
    int aerr = posix_spawn_file_actions_init(&fa);

    if (aerr == 0) {
        if (need_stdin_pipe) {
            aerr |= posix_spawn_file_actions_adddup2(&fa, in_pipe[0], STDIN_FILENO);
            aerr |= posix_spawn_file_actions_addclose(&fa, in_pipe[0]);
            aerr |= posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
        } else {
            aerr |= posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        }
        aerr |= posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
        aerr |= posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
        aerr |= posix_spawn_file_actions_addclose(&fa, out_pipe[1]);
        aerr |= posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
        aerr |= posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
        aerr |= posix_spawn_file_actions_addclose(&fa, err_pipe[1]);
    }

    pid_t pid = -1;
    int rc = aerr;
    if (aerr == 0) {
        rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
    }
    posix_spawn_file_actions_destroy(&fa);

    if (need_stdin_pipe) { close(in_pipe[0]); }
    close(out_pipe[1]);
    close(err_pipe[1]);

    if (rc != 0) {
        close_if_valid(in_pipe[1]);
        close(out_pipe[0]);
        close(err_pipe[0]);
        return mt_err_from_errno(rc);
    }

    *pid_out = pid;
    *stdin_wr = need_stdin_pipe ? in_pipe[1] : -1;
    *stdout_rd = out_pipe[0];
    *stderr_rd = err_pipe[0];
    return MT_OK;
}

static mt_err_t wait_child(pid_t pid, const char *cmd, mt_bytebuf_t *err_buf) {
    int status = 0;
    pid_t w;
    do {
        w = waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR);

    if (w < 0) { return mt_err_from_errno(errno); }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (stderr_is_retryable(err_buf)) {
            MT_DEBUG("%s lost a race with another writer (status=%d): %.*s", cmd, status,
                     (int)err_buf->len, (const char *)err_buf->data);
            return MT_ERR_AGAIN;
        }
        MT_ERROR("%s failed (status=%d): %.*s", cmd, status, (int)err_buf->len,
                 (const char *)err_buf->data);
        return MT_ERR_IO;
    }
    return MT_OK;
}

static mt_err_t real_save(mt_ipt_executable_t *self, uint8_t **out, size_t *out_len) {
    exe_real_t *e = (exe_real_t *)self;
    const char *argv[] = {e->save_cmd, NULL};

    pid_t pid;
    int stdin_wr, stdout_rd, stderr_rd;
    if (mt_cancel_raised(e->cancel)) { return MT_ERR_CANCELED; }

    mt_err_t err = spawn_with_pipes(argv, false, &pid, &stdin_wr, &stdout_rd, &stderr_rd);
    if (err != MT_OK) { return err; }
    (void)stdin_wr;

    const int cancel_fd = mt_cancel_fd(e->cancel);
    bool canceled = false;

    if (err == MT_OK) { err = set_nonblock(stdout_rd); }
    if (err == MT_OK) { err = set_nonblock(stderr_rd); }

    mt_bytebuf_t out_buf, err_buf;
    mt_bytebuf_init(&out_buf);
    mt_bytebuf_init_bounded(&err_buf, MT_IPT_STDERR_CAP);

    bool stdout_eof = (err != MT_OK);
    bool stderr_eof = (err != MT_OK);
    bool stdout_limited = false;

    while (err == MT_OK && (!stdout_eof || !stderr_eof)) {
        struct pollfd pfds[3];
        int n = 0, idx_out = -1, idx_err = -1, idx_cancel = -1;
        if (!stdout_eof) {
            pfds[n].fd = stdout_rd;
            pfds[n].events = POLLIN;
            idx_out = n++;
        }
        if (!stderr_eof) {
            pfds[n].fd = stderr_rd;
            pfds[n].events = POLLIN;
            idx_err = n++;
        }
        if (cancel_fd >= 0) {
            pfds[n].fd = cancel_fd;
            pfds[n].events = POLLIN;
            idx_cancel = n++;
        }

        int pr = poll(pfds, (nfds_t)n, -1);
        if (pr < 0) {
            if (errno == EINTR) { continue; }
            err = mt_err_from_errno(errno);
            break;
        }

        if (idx_cancel >= 0 && pfds[idx_cancel].revents != 0) {
            canceled = true;
            break;
        }

        if (idx_out >= 0 && pfds[idx_out].revents != 0) {
            uint8_t tmp[4096];
            ssize_t rn = read(stdout_rd, tmp, sizeof(tmp));
            if (rn < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    err = mt_err_from_errno(errno);
                    break;
                }
            } else if (rn == 0) {
                stdout_eof = true;
            } else if (!stdout_limited) {
                mt_err_t aerr = mt_bytebuf_append(&out_buf, tmp, (size_t)rn);
                if (aerr == MT_ERR_LIMIT) {
                    stdout_limited = true;
                } else if (aerr != MT_OK) {
                    err = aerr;
                    break;
                }
            }
        }
        if (idx_err >= 0 && pfds[idx_err].revents != 0) {
            uint8_t tmp[1024];
            ssize_t rn = read(stderr_rd, tmp, sizeof(tmp));
            if (rn < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    err = mt_err_from_errno(errno);
                    break;
                }
            } else if (rn == 0) {
                stderr_eof = true;
            } else {
                (void)mt_bytebuf_append(&err_buf, tmp, (size_t)rn); /* best effort */
            }
        }
    }

    close(stdout_rd);
    close(stderr_rd);

    if (canceled) {
        kill_child(pid);
        err = MT_ERR_CANCELED;
    } else {
        mt_err_t wait_err = wait_child(pid, e->save_cmd, &err_buf);
        if (err == MT_OK) { err = wait_err; }
        if (err == MT_OK && stdout_limited) { err = MT_ERR_LIMIT; }
    }

    mt_bytebuf_free(&err_buf);

    if (err != MT_OK) {
        mt_bytebuf_free(&out_buf);
        return err;
    }

    *out = out_buf.data;
    *out_len = out_buf.len;
    return MT_OK;
}

static mt_err_t real_restore(mt_ipt_executable_t *self, const uint8_t *data, size_t len) {
    exe_real_t *e = (exe_real_t *)self;
    const char *argv[] = {e->restore_cmd, "--noflush", NULL};

    pid_t pid;
    int stdin_wr, stdout_rd, stderr_rd;
    if (mt_cancel_raised(e->cancel)) { return MT_ERR_CANCELED; }

    mt_err_t err = spawn_with_pipes(argv, true, &pid, &stdin_wr, &stdout_rd, &stderr_rd);
    if (err != MT_OK) { return err; }

    const int cancel_fd = mt_cancel_fd(e->cancel);
    bool canceled = false;

    if (err == MT_OK) { err = set_nonblock(stdin_wr); }
    if (err == MT_OK) { err = set_nonblock(stdout_rd); }
    if (err == MT_OK) { err = set_nonblock(stderr_rd); }

    mt_bytebuf_t err_buf;
    mt_bytebuf_init_bounded(&err_buf, MT_IPT_STDERR_CAP);

    size_t written = 0;
    bool stdin_done = (len == 0) || (err != MT_OK);
    if ((len == 0 || err != MT_OK) && stdin_wr >= 0) {
        close(stdin_wr);
        stdin_wr = -1;
    }
    bool stdout_eof = (err != MT_OK);
    bool stderr_eof = (err != MT_OK);

    while (err == MT_OK && (!stdin_done || !stdout_eof || !stderr_eof)) {
        struct pollfd pfds[4];
        int n = 0, idx_in = -1, idx_out = -1, idx_err = -1, idx_cancel = -1;
        if (!stdin_done) {
            pfds[n].fd = stdin_wr;
            pfds[n].events = POLLOUT;
            idx_in = n++;
        }
        if (!stdout_eof) {
            pfds[n].fd = stdout_rd;
            pfds[n].events = POLLIN;
            idx_out = n++;
        }
        if (!stderr_eof) {
            pfds[n].fd = stderr_rd;
            pfds[n].events = POLLIN;
            idx_err = n++;
        }
        if (cancel_fd >= 0) {
            pfds[n].fd = cancel_fd;
            pfds[n].events = POLLIN;
            idx_cancel = n++;
        }

        int pr = poll(pfds, (nfds_t)n, -1);
        if (pr < 0) {
            if (errno == EINTR) { continue; }
            err = mt_err_from_errno(errno);
            break;
        }

        if (idx_cancel >= 0 && pfds[idx_cancel].revents != 0) {
            canceled = true;
            break;
        }

        if (idx_in >= 0 && pfds[idx_in].revents != 0) {
            if (pfds[idx_in].revents & POLLOUT) {
                ssize_t wn = write(stdin_wr, data + written, len - written);
                if (wn < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                        /* retry next iteration */
                    } else if (errno == EPIPE) {
                        close(stdin_wr);
                        stdin_wr = -1;
                        stdin_done = true;
                    } else {
                        err = mt_err_from_errno(errno);
                        break;
                    }
                } else {
                    written += (size_t)wn;
                    if (written == len) {
                        close(stdin_wr);
                        stdin_wr = -1;
                        stdin_done = true;
                    }
                }
            } else {
                close(stdin_wr);
                stdin_wr = -1;
                stdin_done = true;
            }
        }
        if (idx_out >= 0 && pfds[idx_out].revents != 0) {
            uint8_t tmp[1024];
            ssize_t rn = read(stdout_rd, tmp, sizeof(tmp));
            if (rn < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    err = mt_err_from_errno(errno);
                    break;
                }
            } else if (rn == 0) {
                stdout_eof = true;
            }
            /* stdout content discarded: Go's Restore() doesn't capture it either */
        }
        if (idx_err >= 0 && pfds[idx_err].revents != 0) {
            uint8_t tmp[1024];
            ssize_t rn = read(stderr_rd, tmp, sizeof(tmp));
            if (rn < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    err = mt_err_from_errno(errno);
                    break;
                }
            } else if (rn == 0) {
                stderr_eof = true;
            } else {
                (void)mt_bytebuf_append(&err_buf, tmp, (size_t)rn);
            }
        }
    }

    close_if_valid(stdin_wr);
    close(stdout_rd);
    close(stderr_rd);

    if (canceled) {
        kill_child(pid);
        err = MT_ERR_CANCELED;
    } else {
        mt_err_t wait_err = wait_child(pid, e->restore_cmd, &err_buf);
        if (err == MT_OK) { err = wait_err; }
    }

    mt_bytebuf_free(&err_buf);
    return err;
}

static void real_set_cancel(mt_ipt_executable_t *self, mt_cancel_t *cancel) {
    ((exe_real_t *)self)->cancel = cancel;
}

static mt_ipt_proto_t real_proto(mt_ipt_executable_t *self) {
    return ((exe_real_t *)self)->proto;
}

static void real_destroy(mt_ipt_executable_t *self) {
    free(self);
}

static const mt_ipt_executable_ops_t k_real_ops = {
    .save = real_save,
    .restore = real_restore,
    .proto = real_proto,
    .destroy = real_destroy,
    .set_cancel = real_set_cancel,
};

mt_ipt_executable_t *mt_ipt_executable_real_new(mt_ipt_proto_t proto) {
    exe_real_t *e = calloc(1, sizeof(*e));
    if (!e) { return NULL; }
    e->base.ops = &k_real_ops;
    e->proto = proto;
    if (proto == MT_IPT_PROTO_IPV6) {
        e->save_cmd = "ip6tables-save";
        e->restore_cmd = "ip6tables-restore";
    } else {
        e->save_cmd = "iptables-save";
        e->restore_cmd = "iptables-restore";
    }
    return &e->base;
}
