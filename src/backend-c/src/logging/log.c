#include "magitrickle/log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MT_LOG_LINE_MAX 1024

static _Atomic int g_level = MT_LOG_INFO;
static _Atomic int g_fd = STDOUT_FILENO;

void mt_log_set_level(mt_log_level_t level)
{
    atomic_store_explicit(&g_level, (int)level, memory_order_relaxed);
}

mt_log_level_t mt_log_level(void)
{
    return (mt_log_level_t)atomic_load_explicit(&g_level,
                                                memory_order_relaxed);
}

void mt_log_set_fd(int fd)
{
    atomic_store_explicit(&g_fd, fd, memory_order_relaxed);
}

mt_log_level_t mt_log_level_from_str(const char *s)
{
    if (s == NULL) {
        return MT_LOG_INFO;
    }
    static const struct {
        const char *name;
        mt_log_level_t level;
    } levels[] = {
        {"trace", MT_LOG_TRACE},     {"debug", MT_LOG_DEBUG},
        {"info", MT_LOG_INFO},       {"warn", MT_LOG_WARN},
        {"error", MT_LOG_ERROR},     {"fatal", MT_LOG_FATAL},
        {"panic", MT_LOG_PANIC},     {"nolevel", MT_LOG_NOLEVEL},
        {"disabled", MT_LOG_DISABLED},
    };
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (strcmp(s, levels[i].name) == 0) {
            return levels[i].level;
        }
    }
    return MT_LOG_INFO; /* Go setupLogging falls back to info */
}

static const char *level_tag(mt_log_level_t level)
{
    switch (level) {
    case MT_LOG_TRACE:   return "TRC";
    case MT_LOG_DEBUG:   return "DBG";
    case MT_LOG_INFO:    return "INF";
    case MT_LOG_WARN:    return "WRN";
    case MT_LOG_ERROR:   return "ERR";
    case MT_LOG_FATAL:   return "FTL";
    case MT_LOG_PANIC:   return "PNC";
    case MT_LOG_NOLEVEL: return "???";
    case MT_LOG_DISABLED: return "OFF";
    }
    return "???";
}

void mt_log(mt_log_level_t level, const char *fmt, ...)
{
    if ((int)level < atomic_load_explicit(&g_level, memory_order_relaxed) ||
        level >= MT_LOG_DISABLED) {
        return;
    }

    char buf[MT_LOG_LINE_MAX];
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);

    int off = snprintf(buf, sizeof(buf),
                       "%04d-%02d-%02dT%02d:%02d:%02dZ %s ",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec, level_tag(level));
    if (off < 0 || (size_t)off >= sizeof(buf)) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) — va_start above */
    int n = vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    size_t len = (size_t)off + (size_t)n;
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1; /* truncated */
    }
    buf[len] = '\n';
    len += 1;

    /* Single write keeps concurrent lines whole; partial writes to a
     * console/pipe are acceptable best-effort for logging. */
    ssize_t rc = write(atomic_load_explicit(&g_fd, memory_order_relaxed),
                       buf, len);
    (void)rc;
}
