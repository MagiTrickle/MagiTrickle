#include "magitrickle/restore_diagnostic.h"
#include "magitrickle/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MT_IPT_CONTEXT_MAX 240u
#define MT_IPT_TABLE_MAX 31u

static size_t error_line_number(const uint8_t *err, size_t len)
{
    if (!err || len < 6) { return 0; }
    for (size_t i = 0; i + 5 < len; i++) {
        if (memcmp(err + i, "line ", 5) != 0) { continue; }
        size_t n = 0, j = i + 5;
        while (j < len && err[j] >= '0' && err[j] <= '9') {
            size_t digit = (size_t)(err[j] - '0');
            if (n > (SIZE_MAX - digit) / 10u) { return 0; }
            n = n * 10u + digit;
            j++;
        }
        if (j > i + 5 && n > 0 &&
            (j == len || err[j] < '0' || err[j] > '9')) { return n; }
    }
    return 0;
}

/* Each emitted log entry is one bounded, printable line even for malformed
 * input. Avoid dumping a full iptables transaction into router logs. */
static void printable(const uint8_t *src, size_t len, char *dst, size_t cap)
{
    size_t n = len < cap - 1 ? len : cap - 1;
    for (size_t i = 0; i < n; i++) {
        dst[i] = src[i] >= 32 && src[i] <= 126 ? (char)src[i] : '?';
    }
    dst[n] = '\0';
}

void mt_ipt_log_restore_context(const char *cmd, const uint8_t *input, size_t input_len,
                                const uint8_t *stderr_data, size_t stderr_len)
{
    if (!cmd || !input || !input_len) { return; }
    size_t target = error_line_number(stderr_data, stderr_len);
    if (!target) { return; }

    char table[MT_IPT_TABLE_MAX + 1] = "?";
    char failed_table[MT_IPT_TABLE_MAX + 1] = "?";
    char lines[4][MT_IPT_CONTEXT_MAX + 1] = {{0}};
    size_t numbers[4] = {0};
    size_t kept = 0, start = 0, num = 1;
    for (size_t pos = 0; pos <= input_len; pos++) {
        if (pos < input_len && input[pos] != '\n') { continue; }
        size_t len = pos - start;
        const uint8_t *line = input + start;
        if (len > 1 && line[0] == '*') {
            printable(line + 1, len - 1, table, sizeof(table));
        }
        if (num == target) { memcpy(failed_table, table, sizeof(table)); }
        if (len && num <= target && target - num <= 2u) {
            size_t idx = kept++;
            printable(line, len, lines[idx], sizeof(lines[idx]));
            numbers[idx] = num;
        } else if (len && num > target && num - target <= 1u && kept < 4u) {
            size_t idx = kept++;
            printable(line, len, lines[idx], sizeof(lines[idx]));
            numbers[idx] = num;
        }
        if (num > target && num - target > 1u) { break; }
        start = pos + 1;
        num++;
    }
    if (!kept) { return; }
    MT_ERROR("%s failed transaction: table=%s reported_line=%zu", cmd, failed_table, target);
    for (size_t i = 0; i < kept; i++) {
        MT_ERROR("%s input %zu%s: %s", cmd, numbers[i],
                 numbers[i] == target ? " (failed)" : "", lines[i]);
    }
}
