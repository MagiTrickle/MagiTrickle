/* PCRE2 side of the regex compatibility spike.
 * Mirrors the intended production configuration: PCRE2_CASELESS (the Go
 * backend compiles with regexp2.IgnoreCase), PCRE2_UTF + PCRE2_UCP so \w,
 * \p{L} etc. behave Unicode-aware like .NET, plus match/depth limits
 * (regex-DoS hardening planned in decisions.md D-07).
 * Reads the corpus on stdin, writes PATTERN\tINPUT\tRESULT on stdout.
 */
#define PCRE2_CODE_UNIT_WIDTH 8

#include <pcre2.h>
#include <stdio.h>
#include <string.h>

#define LINE_MAX_LEN 4096

int main(void)
{
    char line[LINE_MAX_LEN];

    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }

        char *tab = strchr(line, '\t');
        const char *input = "";
        if (tab != NULL) {
            *tab = '\0';
            input = tab + 1;
        }
        const char *pattern = line;

        int errcode = 0;
        PCRE2_SIZE erroff = 0;
        pcre2_code *code = pcre2_compile(
            (PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
            PCRE2_CASELESS | PCRE2_UTF | PCRE2_UCP, &errcode, &erroff, NULL);
        if (code == NULL) {
            printf("%s\t%s\tcompile_error\n", pattern, input);
            continue;
        }

        pcre2_match_context *mctx = pcre2_match_context_create(NULL);
        pcre2_set_match_limit(mctx, 1000000);
        pcre2_set_depth_limit(mctx, 10000);

        pcre2_match_data *md =
            pcre2_match_data_create_from_pattern(code, NULL);
        int rc = pcre2_match(code, (PCRE2_SPTR)input,
                             PCRE2_ZERO_TERMINATED, 0, 0, md, mctx);
        if (rc >= 0) {
            printf("%s\t%s\tmatch\n", pattern, input);
        } else if (rc == PCRE2_ERROR_NOMATCH) {
            printf("%s\t%s\tnomatch\n", pattern, input);
        } else {
            printf("%s\t%s\tmatch_error\n", pattern, input);
        }
        pcre2_match_data_free(md);
        pcre2_match_context_free(mctx);
        pcre2_code_free(code);
    }
    return 0;
}
