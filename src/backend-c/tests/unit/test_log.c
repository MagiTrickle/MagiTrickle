#include "greatest.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "magitrickle/log.h"

static ssize_t capture(int level_to_log, const char *msg, char *out,
                       size_t out_len)
{
    int fds[2];
    if (pipe(fds) != 0) {
        return -1;
    }
    mt_log_set_fd(fds[1]);
    mt_log((mt_log_level_t)level_to_log, "%s", msg);
    mt_log_set_fd(STDOUT_FILENO);
    close(fds[1]);
    ssize_t n = read(fds[0], out, out_len - 1);
    close(fds[0]);
    if (n < 0) {
        n = 0;
    }
    out[n] = '\0';
    return n;
}

TEST level_filtering(void)
{
    char buf[256];
    mt_log_set_level(MT_LOG_WARN);
    ASSERT_EQ(0, capture(MT_LOG_INFO, "hidden", buf, sizeof(buf)));
    ASSERT(capture(MT_LOG_ERROR, "shown", buf, sizeof(buf)) > 0);
    ASSERT(strstr(buf, "ERR") != NULL);
    ASSERT(strstr(buf, "shown") != NULL);
    mt_log_set_level(MT_LOG_INFO);
    PASS();
}

TEST line_format_has_timestamp_and_tag(void)
{
    char buf[256];
    mt_log_set_level(MT_LOG_TRACE);
    ASSERT(capture(MT_LOG_INFO, "fmt-check", buf, sizeof(buf)) > 0);
    /* "YYYY-MM-DDTHH:MM:SSZ INF fmt-check\n" */
    ASSERT_EQ('2', buf[0]);
    ASSERT_EQ('T', buf[10]);
    ASSERT_EQ('Z', buf[19]);
    ASSERT(strstr(buf, " INF fmt-check\n") != NULL);
    mt_log_set_level(MT_LOG_INFO);
    PASS();
}

TEST level_parsing_matches_go_config_values(void)
{
    ASSERT_EQ(MT_LOG_TRACE, mt_log_level_from_str("trace"));
    ASSERT_EQ(MT_LOG_DEBUG, mt_log_level_from_str("debug"));
    ASSERT_EQ(MT_LOG_INFO, mt_log_level_from_str("info"));
    ASSERT_EQ(MT_LOG_WARN, mt_log_level_from_str("warn"));
    ASSERT_EQ(MT_LOG_ERROR, mt_log_level_from_str("error"));
    ASSERT_EQ(MT_LOG_FATAL, mt_log_level_from_str("fatal"));
    ASSERT_EQ(MT_LOG_PANIC, mt_log_level_from_str("panic"));
    ASSERT_EQ(MT_LOG_NOLEVEL, mt_log_level_from_str("nolevel"));
    ASSERT_EQ(MT_LOG_DISABLED, mt_log_level_from_str("disabled"));
    /* unknown falls back to info, like Go setupLogging */
    ASSERT_EQ(MT_LOG_INFO, mt_log_level_from_str("bogus"));
    ASSERT_EQ(MT_LOG_INFO, mt_log_level_from_str(NULL));
    PASS();
}

TEST disabled_level_suppresses_everything(void)
{
    char buf[256];
    mt_log_set_level(MT_LOG_DISABLED);
    ASSERT_EQ(0, capture(MT_LOG_ERROR, "nope", buf, sizeof(buf)));
    mt_log_set_level(MT_LOG_INFO);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(level_filtering);
    RUN_TEST(line_format_has_timestamp_and_tag);
    RUN_TEST(level_parsing_matches_go_config_values);
    RUN_TEST(disabled_level_suppresses_everything);
    GREATEST_MAIN_END();
}
