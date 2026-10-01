/* DNS wire-format parsing/packing (compatibility-contract §4).
 *
 * Safety invariants (spec §13):
 * - every read is bounds-checked; multi-byte fields read byte-wise in
 *   network order (BE mips targets are first-class);
 * - name decompression is guarded: bounded pointer-jump budget, pointers
 *   must land inside the message, total wire name <= 255 octets,
 *   label <= 63; loops therefore terminate;
 * - section counts are validated against actual content (count lies fail);
 * - no struct casts onto the buffer.
 *
 * The proxy forwards raw bytes; this module is used by the hooks (fake
 * PTR, AAAA strip, cache extraction) and by the differential/fuzz tools.
 * Parsed messages carry *decompressed* names and canonical (uncompressed)
 * rdata for the RFC-compressible types, matching what miekg/dns produces
 * via ToRFC3597 — that equivalence is what the differential suite checks.
 * The packer emits without compression; comparisons are semantic
 * (spec §21: no byte-for-byte requirement when only compression differs).
 */
#ifndef MAGITRICKLE_DNSWIRE_H
#define MAGITRICKLE_DNSWIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "magitrickle/err.h"

#define MT_DNS_MAX_MSG 65535
#define MT_DNS_MAX_NAME 255      /* wire octets incl. root */
#define MT_DNS_HEADER_LEN 12

#define MT_DNS_TYPE_A 1
#define MT_DNS_TYPE_NS 2
#define MT_DNS_TYPE_CNAME 5
#define MT_DNS_TYPE_SOA 6
#define MT_DNS_TYPE_PTR 12
#define MT_DNS_TYPE_MX 15
#define MT_DNS_TYPE_AAAA 28
#define MT_DNS_TYPE_SRV 33
#define MT_DNS_TYPE_OPT 41

#define MT_DNS_FLAG_QR 0x8000
#define MT_DNS_FLAG_TC 0x0200
#define MT_DNS_FLAG_RA 0x0080
#define MT_DNS_RCODE_NXDOMAIN 3

typedef struct mt_dns_question {
    uint8_t name[MT_DNS_MAX_NAME + 1]; /* decompressed wire form */
    size_t name_len;
    uint16_t qtype;
    uint16_t qclass;
} mt_dns_question_t;

typedef struct mt_dns_rr {
    uint8_t name[MT_DNS_MAX_NAME + 1];
    size_t name_len;
    uint16_t rtype;
    uint16_t rclass;
    uint32_t ttl;
    uint8_t *rdata; /* canonical (decompressed) rdata, malloc'd */
    size_t rdata_len;
} mt_dns_rr_t;

typedef struct mt_dns_msg {
    uint16_t id;
    uint16_t flags;
    mt_dns_question_t *questions;
    size_t n_questions;
    mt_dns_rr_t *answers;
    size_t n_answers;
    mt_dns_rr_t *authority;
    size_t n_authority;
    mt_dns_rr_t *additional;
    size_t n_additional;
} mt_dns_msg_t;

/* Full parse. MT_ERR_PROTO on malformed input. */
mt_err_t mt_dns_msg_parse(const uint8_t *buf, size_t len,
                          mt_dns_msg_t **out);
void mt_dns_msg_free(mt_dns_msg_t *msg);

/* Pack without compression. Caller frees *out. */
mt_err_t mt_dns_msg_pack(const mt_dns_msg_t *msg, uint8_t **out,
                         size_t *out_len);

/* Remove all AAAA answers in place (frees their rdata). */
void mt_dns_msg_strip_aaaa(mt_dns_msg_t *msg);

/* Light checks on raw messages (no allocation) for the request hook:
 * true when the message has exactly one question and it is a PTR query
 * (mirrors Go dns.go fake-PTR condition: len(Question)==1 && Qtype==PTR).
 * Malformed input returns false. */
bool mt_dns_is_single_ptr_query(const uint8_t *buf, size_t len);

/* Build the fake-PTR NXDOMAIN reply exactly like the Go hook: fresh
 * header (QR|RA, rcode NXDOMAIN, same id), question section echoed.
 * Caller frees *out. MT_ERR_PROTO if the request is malformed. */
mt_err_t mt_dns_make_fake_ptr_response(const uint8_t *req, size_t len,
                                       uint8_t **out, size_t *out_len);

/* Presentation form of a decompressed wire name with miekg/dns escaping
 * ('.' and specials backslash-escaped, non-printables as \DDD, trailing
 * dot, root == "."). buf_len >= 4*255+2 is always sufficient. */
mt_err_t mt_dns_name_to_string(const uint8_t *name, size_t name_len,
                               char *buf, size_t buf_len);

#endif /* MAGITRICKLE_DNSWIRE_H */
