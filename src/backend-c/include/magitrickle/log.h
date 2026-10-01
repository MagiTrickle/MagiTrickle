/* Leveled console logger.
 *
 * Mirrors the Go backend's zerolog console style closely enough for humans
 * and init-script pipes: one line per event on stdout,
 * "<RFC3339 UTC> <LVL> <message>". Level names match the Go config values
 * ("trace".."disabled") so `logLevel` keeps working unchanged.
 *
 * Thread-safe: each event is formatted into a stack buffer and emitted with
 * a single write(2). Log calls never allocate.
 */
#ifndef MAGITRICKLE_LOG_H
#define MAGITRICKLE_LOG_H

typedef enum mt_log_level {
    MT_LOG_TRACE = 0,
    MT_LOG_DEBUG,
    MT_LOG_INFO,
    MT_LOG_WARN,
    MT_LOG_ERROR,
    MT_LOG_FATAL,
    MT_LOG_PANIC,
    MT_LOG_NOLEVEL,
    MT_LOG_DISABLED,
} mt_log_level_t;

void mt_log_set_level(mt_log_level_t level);
mt_log_level_t mt_log_level(void);

/* Parse a config string ("trace", "debug", ...). Unknown -> MT_LOG_INFO,
 * matching Go's setupLogging default. */
mt_log_level_t mt_log_level_from_str(const char *s);

/* Redirect output (default: fd 1). Used by tests. */
void mt_log_set_fd(int fd);

#if defined(__GNUC__) || defined(__clang__)
#define MT_PRINTF(fmt_idx, arg_idx) \
    __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define MT_PRINTF(fmt_idx, arg_idx)
#endif

void mt_log(mt_log_level_t level, const char *fmt, ...) MT_PRINTF(2, 3);

#define MT_TRACE(...) mt_log(MT_LOG_TRACE, __VA_ARGS__)
#define MT_DEBUG(...) mt_log(MT_LOG_DEBUG, __VA_ARGS__)
#define MT_INFO(...)  mt_log(MT_LOG_INFO, __VA_ARGS__)
#define MT_WARN(...)  mt_log(MT_LOG_WARN, __VA_ARGS__)
#define MT_ERROR(...) mt_log(MT_LOG_ERROR, __VA_ARGS__)

#endif /* MAGITRICKLE_LOG_H */
