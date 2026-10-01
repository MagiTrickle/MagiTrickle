#include "greatest.h"

#include <string.h>

#include "magitrickle/dnswire.h"

/* helper: build a query for name (label,label,0) qtype */
static size_t build_query(uint8_t *buf, uint16_t id, const char *const *labels,
                          size_t n_labels, uint16_t qtype)
{
    size_t p = 0;
    buf[p++] = (uint8_t)(id >> 8);
    buf[p++] = (uint8_t)id;
    buf[p++] = 0x01; /* RD */
    buf[p++] = 0x00;
    buf[p++] = 0x00;
    buf[p++] = 0x01; /* qdcount 1 */
    buf[p++] = 0x00;
    buf[p++] = 0x00;
    buf[p++] = 0x00;
    buf[p++] = 0x00;
    buf[p++] = 0x00;
    buf[p++] = 0x00;
    for (size_t i = 0; i < n_labels; i++) {
        size_t l = strlen(labels[i]);
        buf[p++] = (uint8_t)l;
        memcpy(buf + p, labels[i], l);
        p += l;
    }
    buf[p++] = 0;
    buf[p++] = (uint8_t)(qtype >> 8);
    buf[p++] = (uint8_t)qtype;
    buf[p++] = 0x00;
    buf[p++] = 0x01; /* IN */
    return p;
}

TEST parse_simple_query(void)
{
    uint8_t buf[64];
    const char *labels[] = {"example", "com"};
    size_t len = build_query(buf, 0x1234, labels, 2, MT_DNS_TYPE_A);

    mt_dns_msg_t *msg = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(buf, len, &msg));
    ASSERT_EQ(0x1234, msg->id);
    ASSERT_EQ(1u, (unsigned)msg->n_questions);
    ASSERT_EQ(MT_DNS_TYPE_A, msg->questions[0].qtype);

    char name[1024];
    ASSERT_EQ(MT_OK, mt_dns_name_to_string(msg->questions[0].name,
                                           msg->questions[0].name_len, name,
                                           sizeof(name)));
    ASSERT_STR_EQ("example.com.", name);
    mt_dns_msg_free(msg);
    PASS();
}

TEST parse_answer_with_compression(void)
{
    /* header: id, flags QR, qd=1, an=1 */
    uint8_t buf[] = {
        0xab, 0xcd, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00,
        /* question: 3www 7example 3com 0, A, IN */
        0x03, 'w', 'w', 'w', 0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03,
        'c', 'o', 'm', 0x00, 0x00, 0x01, 0x00, 0x01,
        /* answer: name ptr to offset 12, A, IN, ttl 300, rdlen 4, 1.2.3.4 */
        0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2c, 0x00,
        0x04, 0x01, 0x02, 0x03, 0x04};

    mt_dns_msg_t *msg = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(buf, sizeof(buf), &msg));
    ASSERT_EQ(1u, (unsigned)msg->n_answers);
    ASSERT_EQ(MT_DNS_TYPE_A, msg->answers[0].rtype);
    ASSERT_EQ(300u, msg->answers[0].ttl);
    ASSERT_EQ(4u, (unsigned)msg->answers[0].rdata_len);
    ASSERT_EQ(1, msg->answers[0].rdata[0]);
    ASSERT_EQ(4, msg->answers[0].rdata[3]);

    char name[1024];
    mt_dns_name_to_string(msg->answers[0].name, msg->answers[0].name_len,
                          name, sizeof(name));
    ASSERT_STR_EQ("www.example.com.", name);
    mt_dns_msg_free(msg);
    PASS();
}

TEST cname_rdata_decompressed(void)
{
    /* CNAME rdata using a compression pointer must decompress to a full
     * canonical name */
    uint8_t buf[] = {
        0x00, 0x01, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00,
        /* q: 7example 3com 0 CNAME IN */
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x05, 0x00, 0x01,
        /* answer: ptr->12, CNAME, IN, ttl 60, rdlen 6, (3www ptr->12) */
        0xc0, 0x0c, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3c, 0x00,
        0x06, 0x03, 'w', 'w', 'w', 0xc0, 0x0c};

    mt_dns_msg_t *msg = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(buf, sizeof(buf), &msg));
    ASSERT_EQ(MT_DNS_TYPE_CNAME, msg->answers[0].rtype);
    /* canonical rdata is the decompressed wire name www.example.com. */
    uint8_t expect[] = {0x03, 'w', 'w', 'w', 0x07, 'e', 'x', 'a', 'm',
                        'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00};
    ASSERT_EQ(sizeof(expect), msg->answers[0].rdata_len);
    ASSERT_EQ(0, memcmp(expect, msg->answers[0].rdata, sizeof(expect)));
    mt_dns_msg_free(msg);
    PASS();
}

TEST strip_aaaa(void)
{
    /* qd=1, an=2 (A then AAAA) */
    uint8_t buf[] = {
        0x00, 0x02, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x01, 'a', 0x00, 0x00, 0x01, 0x00, 0x01,
        /* A */
        0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1e, 0x00,
        0x04, 0x0a, 0x00, 0x00, 0x01,
        /* AAAA */
        0xc0, 0x0c, 0x00, 0x1c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1e, 0x00,
        0x10, 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

    mt_dns_msg_t *msg = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(buf, sizeof(buf), &msg));
    ASSERT_EQ(2u, (unsigned)msg->n_answers);
    mt_dns_msg_strip_aaaa(msg);
    ASSERT_EQ(1u, (unsigned)msg->n_answers);
    ASSERT_EQ(MT_DNS_TYPE_A, msg->answers[0].rtype);

    uint8_t *packed = NULL;
    size_t plen = 0;
    ASSERT_EQ(MT_OK, mt_dns_msg_pack(msg, &packed, &plen));
    /* re-parse the packed result: 1 answer, A */
    mt_dns_msg_t *rt = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(packed, plen, &rt));
    ASSERT_EQ(1u, (unsigned)rt->n_answers);
    ASSERT_EQ(MT_DNS_TYPE_A, rt->answers[0].rtype);
    free(packed);
    mt_dns_msg_free(rt);
    mt_dns_msg_free(msg);
    PASS();
}

TEST fake_ptr_detection_and_response(void)
{
    uint8_t buf[64];
    const char *labels[] = {"1", "0", "0", "127", "in-addr", "arpa"};
    size_t len = build_query(buf, 0x7777, labels, 6, MT_DNS_TYPE_PTR);
    ASSERT(mt_dns_is_single_ptr_query(buf, len));

    const char *alabels[] = {"example", "com"};
    size_t alen = build_query(buf, 0x7777, alabels, 2, MT_DNS_TYPE_A);
    ASSERT_FALSE(mt_dns_is_single_ptr_query(buf, alen));

    /* rebuild PTR and make response */
    len = build_query(buf, 0x7777, labels, 6, MT_DNS_TYPE_PTR);
    uint8_t *resp = NULL;
    size_t rlen = 0;
    ASSERT_EQ(MT_OK, mt_dns_make_fake_ptr_response(buf, len, &resp, &rlen));
    mt_dns_msg_t *msg = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(resp, rlen, &msg));
    ASSERT_EQ(0x7777, msg->id);
    ASSERT(msg->flags & MT_DNS_FLAG_QR);
    ASSERT(msg->flags & MT_DNS_FLAG_RA);
    ASSERT_EQ(MT_DNS_RCODE_NXDOMAIN, msg->flags & 0x000f);
    ASSERT_EQ(1u, (unsigned)msg->n_questions);
    ASSERT_EQ(0u, (unsigned)msg->n_answers);
    free(resp);
    mt_dns_msg_free(msg);
    PASS();
}

TEST rejects_malformed(void)
{
    mt_dns_msg_t *msg = NULL;
    /* too short (below header) */
    uint8_t a[] = {0, 0, 0};
    ASSERT_EQ(MT_ERR_PROTO, mt_dns_msg_parse(a, sizeof(a), &msg));
    /* qdcount claims 1 but content is truncated after the header */
    uint8_t c[] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0x3f, 'x'};
    ASSERT_EQ(MT_ERR_PROTO, mt_dns_msg_parse(c, sizeof(c), &msg));
    PASS();
}

TEST header_only_is_lenient(void)
{
    /* miekg leniency: a 12-byte message with non-zero counts parses as
     * header-only (empty sections). */
    uint8_t b[] = {0x12, 0x34, 0x81, 0x80, 0, 1, 0, 2, 0, 0, 0, 0};
    mt_dns_msg_t *msg = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(b, sizeof(b), &msg));
    ASSERT_EQ(0u, (unsigned)msg->n_questions);
    ASSERT_EQ(0u, (unsigned)msg->n_answers);
    mt_dns_msg_free(msg);
    PASS();
}

TEST rejects_compression_loop(void)
{
    /* pointer at offset 12 points to itself */
    uint8_t buf[] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xc0, 0x0c, 0, 1,
                     0, 1};
    mt_dns_msg_t *msg = NULL;
    ASSERT_EQ(MT_ERR_PROTO, mt_dns_msg_parse(buf, sizeof(buf), &msg));
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(parse_simple_query);
    RUN_TEST(parse_answer_with_compression);
    RUN_TEST(cname_rdata_decompressed);
    RUN_TEST(strip_aaaa);
    RUN_TEST(fake_ptr_detection_and_response);
    RUN_TEST(rejects_malformed);
    RUN_TEST(header_only_is_lenient);
    RUN_TEST(rejects_compression_loop);
    GREATEST_MAIN_END();
}
