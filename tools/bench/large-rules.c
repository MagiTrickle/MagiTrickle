/* Deterministic local CPU benchmark; no public list or router required.
 * Build from src/backend-c: make bench_large_rules
 * Run: src/backend-c/build/host/bench-large-rules 50000 */
#include "magitrickle/subparse.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
int main(int argc, char **argv) {
    unsigned long count = 50000;
    if (argc > 1) {
        char *end; errno = 0; count = strtoul(argv[1], &end, 10);
        if (errno || *end || !count || count > 100000) { fprintf(stderr, "count must be 1..100000\n"); return 2; }
    }
    size_t n = (size_t)count, cap = n * 32 + 1, off = 0;
    char *text = malloc(cap); if (!text) { return 2; }
    for (size_t i = 0; i < n; i++) {
        int wrote = snprintf(text + off, cap - off, "10.%zu.%zu.0/24\n", i / 256, i % 256);
        if (wrote < 0 || (size_t)wrote >= cap - off) { free(text); return 2; }
        off += (size_t)wrote;
    }
    mt_sub_rule_t **rules = NULL, **fresh = NULL; size_t parsed = 0, refreshed = 0;
    double start = now(); mt_err_t err = mt_sub_parse_rules(text, &rules, &parsed);
    double parse_time = now() - start;
    start = now();
    if (err == MT_OK) { err = mt_sub_refresh_rules(text, rules, parsed, &fresh, &refreshed); }
    double refresh_time = now() - start;
    start = now(); bool same = err == MT_OK && mt_sub_same_rules(rules, parsed, fresh, refreshed);
    printf("rules=%zu parsed=%zu error=%d parse_s=%.6f refresh_s=%.6f compare_s=%.6f same=%s\n",
           n, parsed, (int)err, parse_time, refresh_time, now() - start, same ? "yes" : "no");
    mt_sub_rules_free(rules, parsed); mt_sub_rules_free(fresh, refreshed); free(text);
    return err == MT_OK && same && parsed == n ? 0 : 1;
}
