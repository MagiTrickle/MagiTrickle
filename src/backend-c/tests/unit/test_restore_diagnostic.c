#include "greatest.h"

#include "magitrickle/log.h"
#include "magitrickle/restore_diagnostic.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static size_t collect_log(char *dst, size_t cap, const char *transcript,
                          const char *stderr_message)
{
    int fds[2];
    if (pipe(fds) != 0) { return 0; }
    mt_log_level_t old_level = mt_log_level();
    mt_log_set_level(MT_LOG_ERROR);
    mt_log_set_fd(fds[1]);
    mt_ipt_log_restore_context("iptables-restore", (const uint8_t *)transcript,
                               strlen(transcript), (const uint8_t *)stderr_message,
                               strlen(stderr_message));
    mt_log_set_fd(STDOUT_FILENO);
    mt_log_set_level(old_level);
    close(fds[1]);
    size_t used = 0;
    ssize_t got;
    while (used < cap - 1 && (got = read(fds[0], dst + used, cap - 1 - used)) > 0) {
        used += (size_t)got;
    }
    close(fds[0]);
    dst[used] = '\0';
    return used;
}

TEST prints_exact_failed_command_and_table(void)
{
    const char *transcript = "*filter\n:FORWARD - [0:0]\n"
                             "-A FORWARD -j MT_EXAMPLE\n"
                             "-A MT_EXAMPLE -m set --match-set mt_x_4 dst -j ACCEPT\n"
                             "COMMIT\n";
    char logbuf[4096];
    ASSERT(collect_log(logbuf, sizeof(logbuf), transcript,
                       "iptables-restore: line 4 failed") > 0);
    ASSERT(strstr(logbuf, "table=filter reported_line=4") != NULL);
    ASSERT(strstr(logbuf, "input 4 (failed): -A MT_EXAMPLE -m set") != NULL);
    ASSERT(strstr(logbuf, "input 3: -A FORWARD") != NULL);
    PASS();
}

TEST skips_invalid_or_unlocated_stderr(void)
{
    const char *transcript = "*filter\nCOMMIT\n";
    char logbuf[4096];
    ASSERT_EQ(0u, collect_log(logbuf, sizeof(logbuf), transcript,
                               "iptables-restore: unknown error"));
    ASSERT_EQ(0u, collect_log(logbuf, sizeof(logbuf), transcript,
                               "iptables-restore: line 0 failed"));
    ASSERT_EQ(0u, collect_log(logbuf, sizeof(logbuf), transcript,
                               "iptables-restore: line 9999999999999999999999999 failed"));
    PASS();
}

TEST bounds_and_sanitizes_malformed_script_line(void)
{
    char transcript[512] = "*nat\n";
    size_t len = strlen(transcript);
    transcript[len++] = '-';
    for (size_t i = 0; i < 310; i++) { transcript[len++] = i == 3 ? '\r' : 'X'; }
    transcript[len++] = '\n';
    transcript[len] = '\0';
    char logbuf[4096];
    ASSERT(collect_log(logbuf, sizeof(logbuf), transcript, "line 2 failed") > 0);
    ASSERT(strstr(logbuf, "input 2 (failed): -XXX?X") != NULL);
    ASSERT(strlen(logbuf) < 900);
    PASS();
}

TEST context_follows_current_table(void)
{
    const char *transcript = "*filter\n-A FORWARD -j ACCEPT\nCOMMIT\n"
                             "*mangle\n-A PREROUTING -j MARK --set-mark 1\nCOMMIT\n";
    char logbuf[4096];
    ASSERT(collect_log(logbuf, sizeof(logbuf), transcript, "line 5 failed") > 0);
    ASSERT(strstr(logbuf, "table=mangle reported_line=5") != NULL);
    ASSERT(collect_log(logbuf, sizeof(logbuf), transcript, "line 3 failed") > 0);
    ASSERT(strstr(logbuf, "table=filter reported_line=3") != NULL);
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(prints_exact_failed_command_and_table);
    RUN_TEST(skips_invalid_or_unlocated_stderr);
    RUN_TEST(bounds_and_sanitizes_malformed_script_line);
    RUN_TEST(context_follows_current_table);
    GREATEST_MAIN_END();
}
