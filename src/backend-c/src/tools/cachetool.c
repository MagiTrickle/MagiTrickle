/* mt-cachetool — differential-test driver for the DNS records cache.
 * Mirrors the command protocol of tests/differential/cache_oracle_go so
 * both sides can run the identical script and diff their output.
 * See cache_oracle_go/main.go for the full protocol docs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "magitrickle/dns_cache.h"

static int64_t now_unix(void)
{
    return (int64_t)time(NULL);
}

static int parse_ip(const char *s, uint8_t *out, uint8_t *out_len)
{
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        out[0] = (uint8_t)a;
        out[1] = (uint8_t)b;
        out[2] = (uint8_t)c;
        out[3] = (uint8_t)d;
        *out_len = 4;
        return 0;
    }
    /* IPv6: accept only fully-expanded colon-hex form (the script never
     * needs "::" compression) */
    unsigned groups[8];
    int n = sscanf(s, "%x:%x:%x:%x:%x:%x:%x:%x", &groups[0], &groups[1],
                   &groups[2], &groups[3], &groups[4], &groups[5],
                   &groups[6], &groups[7]);
    if (n == 8) {
        for (size_t i = 0; i < 8; i++) {
            out[i * 2] = (uint8_t)(groups[i] >> 8);
            out[i * 2 + 1] = (uint8_t)(groups[i] & 0xff);
        }
        *out_len = 16;
        return 0;
    }
    return -1;
}

/* Same textual form as Go's net.IP.String() for the plain cases this tool
 * needs (dotted IPv4; full colon-hex IPv6 — the script never feeds
 * addresses that would trigger Go's "::" compression, so a byte-for-byte
 * match isn't required here). */
static void format_ip(const mt_cache_addr_t *a, char *buf, size_t buf_len)
{
    if (a->addr_len == 4) {
        snprintf(buf, buf_len, "%u.%u.%u.%u", a->addr[0], a->addr[1],
                 a->addr[2], a->addr[3]);
    } else {
        snprintf(buf, buf_len,
                 "%x:%x:%x:%x:%x:%x:%x:%x",
                 (a->addr[0] << 8) | a->addr[1], (a->addr[2] << 8) | a->addr[3],
                 (a->addr[4] << 8) | a->addr[5], (a->addr[6] << 8) | a->addr[7],
                 (a->addr[8] << 8) | a->addr[9],
                 (a->addr[10] << 8) | a->addr[11],
                 (a->addr[12] << 8) | a->addr[13],
                 (a->addr[14] << 8) | a->addr[15]);
    }
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(void)
{
    mt_cache_t *c = mt_cache_create(0);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    while ((n = getline(&line, &cap, stdin)) != -1) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0 || line[0] == '#') {
            continue;
        }

        char cmd[32], a1[512], a2[512], a3[64];
        int got = sscanf(line, "%31s %511s %511s %63s", cmd, a1, a2, a3);

        if (strcmp(cmd, "ADDR") == 0 && got >= 4) {
            uint8_t addr[16], addr_len;
            if (parse_ip(a2, addr, &addr_len) == 0) {
                uint32_t ttl = (uint32_t)strtoul(a3, NULL, 10);
                mt_cache_add_address(c, a1, addr, addr_len, ttl, now_unix());
            }
        } else if (strcmp(cmd, "ALIAS") == 0 && got >= 4) {
            uint32_t ttl = (uint32_t)strtoul(a3, NULL, 10);
            mt_cache_add_alias(c, a1, a2, ttl, now_unix());
        } else if (strcmp(cmd, "GETADDRS") == 0 && got >= 2) {
            mt_cache_addr_t *addrs = NULL;
            size_t na = 0;
            mt_cache_get_addresses(c, a1, now_unix(), &addrs, &na);
            if (na == 0) {
                printf("ADDRS %s: NONE\n", a1);
            } else {
                char **strs = calloc(na, sizeof(char *));
                for (size_t i = 0; i < na; i++) {
                    char buf[64];
                    format_ip(&addrs[i], buf, sizeof(buf));
                    strs[i] = strdup(buf);
                }
                qsort(strs, na, sizeof(char *), cmp_str);
                printf("ADDRS %s: ", a1);
                for (size_t i = 0; i < na; i++) {
                    printf("%s%s", i > 0 ? "," : "", strs[i]);
                    free(strs[i]);
                }
                printf("\n");
                free(strs);
            }
            free(addrs);
        } else if (strcmp(cmd, "GETALIASES") == 0 && got >= 2) {
            char **aliases = NULL;
            size_t na = 0;
            mt_cache_get_aliases(c, a1, &aliases, &na);
            qsort(aliases, na, sizeof(char *), cmp_str);
            printf("ALIASES %s: ", a1);
            for (size_t i = 0; i < na; i++) {
                printf("%s%s", i > 0 ? "," : "", aliases[i]);
            }
            printf("\n");
            mt_cache_free_strings(aliases, na);
        } else if (strcmp(cmd, "KNOWNDOMAINS") == 0) {
            char **domains = NULL;
            size_t nd = 0;
            mt_cache_list_known_domains(c, &domains, &nd);
            qsort(domains, nd, sizeof(char *), cmp_str);
            printf("KNOWN: ");
            for (size_t i = 0; i < nd; i++) {
                printf("%s%s", i > 0 ? "," : "", domains[i]);
            }
            printf("\n");
            mt_cache_free_strings(domains, nd);
        } else if (strcmp(cmd, "CLEANUP") == 0) {
            mt_cache_cleanup(c, now_unix());
        } else if (strcmp(cmd, "SLEEP") == 0 && got >= 2) {
            struct timespec ts;
            long ms = strtol(a1, NULL, 10);
            ts.tv_sec = ms / 1000;
            ts.tv_nsec = (ms % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
    }
    free(line);
    mt_cache_destroy(c);
    return 0;
}
