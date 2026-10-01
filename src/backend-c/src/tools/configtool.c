/* mt-configtool — differential-test driver for the C config engine.
 *
 * Commands:
 *   resave <file> <version>   load config (defaults + overlay) and emit the
 *                             canonical save to stdout; "ERROR" on failure
 *   match                     stdin lines: TYPE\tRULE\tDOMAIN ->
 *                             "TYPE\tRULE\tDOMAIN\tmatch|nomatch"
 *   subparse                  stdin: subscription list -> "type|rule" lines
 *   duration                  stdin lines: duration string -> formatted or
 *                             "ERROR"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "magitrickle/duration.h"
#include "magitrickle/log.h"
#include "magitrickle/match.h"
#include "magitrickle/models.h"
#include "magitrickle/subparse.h"
#include "magitrickle/yamlio.h"

static int cmd_resave(const char *path, const char *version)
{
    mt_config_t cfg;
    if (mt_config_init_defaults(&cfg) != MT_OK) {
        printf("ERROR\n");
        return 0;
    }
    mt_err_t err = mt_config_load_file(&cfg, path);
    if (err == MT_ERR_NOENT) {
        /* missing file keeps defaults (Go New() behaviour) */
        err = MT_OK;
    }
    if (err != MT_OK) {
        printf("ERROR\n");
        mt_config_clear(&cfg);
        return 0;
    }
    char *out = NULL;
    size_t out_len = 0;
    err = mt_config_save_buffer(&cfg, version, &out, &out_len);
    if (err != MT_OK) {
        printf("ERROR\n");
        mt_config_clear(&cfg);
        return 0;
    }
    fwrite(out, 1, out_len, stdout);
    free(out);
    mt_config_clear(&cfg);
    return 0;
}

static int cmd_match(void)
{
    char line[4096];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }
        char *type = line;
        char *rule = strchr(type, '\t');
        if (rule == NULL) {
            continue;
        }
        *rule++ = '\0';
        char *domain = strchr(rule, '\t');
        if (domain == NULL) {
            continue;
        }
        *domain++ = '\0';

        mt_rule_matcher_t *m = mt_rule_matcher_new(type, rule);
        if (m == NULL) {
            printf("%s\t%s\t%s\tERROR\n", type, rule, domain);
            continue;
        }
        bool ok = mt_rule_matcher_match(m, domain);
        printf("%s\t%s\t%s\t%s\n", type, rule, domain,
               ok ? "match" : "nomatch");
        mt_rule_matcher_free(m);
    }
    return 0;
}

static int cmd_subparse(void)
{
    char *input = NULL;
    size_t cap = 0, len = 0;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), stdin)) > 0) {
        if (len + n + 1 > cap) {
            cap = (cap == 0 ? 8192 : cap * 2);
            while (len + n + 1 > cap) {
                cap *= 2;
            }
            char *grown = realloc(input, cap);
            if (grown == NULL) {
                free(input);
                return 1;
            }
            input = grown;
        }
        memcpy(input + len, chunk, n);
        len += n;
    }
    if (input == NULL) {
        input = calloc(1, 1);
        if (input == NULL) {
            return 1;
        }
    } else {
        input[len] = '\0';
    }

    mt_sub_rule_t **rules = NULL;
    size_t n_rules = 0;
    if (mt_sub_parse_rules(input, &rules, &n_rules) != MT_OK) {
        printf("ERROR\n");
        free(input);
        return 0;
    }
    for (size_t i = 0; i < n_rules; i++) {
        printf("%s|%s|%s\n", rules[i]->type, rules[i]->rule,
               rules[i]->enable ? "true" : "false");
        mt_sub_rule_free(rules[i]);
    }
    free(rules);
    free(input);
    return 0;
}

static int cmd_duration(void)
{
    char line[256];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        mt_duration_t d;
        if (mt_duration_parse(line, &d) != MT_OK) {
            printf("%s\tERROR\n", line);
            continue;
        }
        char buf[40];
        mt_duration_format(d, buf, sizeof(buf));
        printf("%s\t%s\n", line, buf);
    }
    return 0;
}

int main(int argc, char **argv)
{
    mt_log_set_fd(2); /* keep stdout clean for differential comparison */
    if (argc >= 2 && strcmp(argv[1], "resave") == 0 && argc == 4) {
        return cmd_resave(argv[2], argv[3]);
    }
    if (argc == 2 && strcmp(argv[1], "match") == 0) {
        return cmd_match();
    }
    if (argc == 2 && strcmp(argv[1], "subparse") == 0) {
        return cmd_subparse();
    }
    if (argc == 2 && strcmp(argv[1], "duration") == 0) {
        return cmd_duration();
    }
    fprintf(stderr,
            "usage: mt-configtool resave <file> <version> | match | "
            "subparse | duration\n");
    return 2;
}
