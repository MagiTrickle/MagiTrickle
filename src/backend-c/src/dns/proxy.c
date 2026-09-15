/* DNS MITM proxy transport — epoll event-driven (platform layer uses Linux
 * pktinfo/accept4). See include/magitrickle/dnsproxy.h for the model. */
#define _GNU_SOURCE /* NOLINT(bugprone-reserved-identifier) */

#include "magitrickle/dnsproxy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "magitrickle/log.h"

#define TCP_MAX_MSG 65535

/* ---- small connected-socket pool for the single upstream ---- */

typedef struct sock_pool {
    int *fds;
    size_t len;
    size_t cap;
} sock_pool_t;

typedef struct exchange exchange_t;

struct mt_dnsproxy {
    mt_dnsproxy_config_t cfg;
    mt_loop_t *loop;
    mt_dnsproxy_msg_cb cb;
    void *cb_ud;

    int udp_fd;
    int tcp_fd;
    int family; /* AF_INET / AF_INET6 */

    struct sockaddr_storage upstream_sa;
    socklen_t upstream_sa_len;

    sock_pool_t udp_pool;
    exchange_t *exchanges;

    _Atomic uint64_t inflight;
    _Atomic uint64_t dropped;
};

static void pool_init(sock_pool_t *p, size_t cap)
{
    p->fds = calloc(cap > 0 ? cap : 1, sizeof(int));
    p->len = 0;
    p->cap = cap;
}

static void pool_clear(sock_pool_t *p)
{
    for (size_t i = 0; i < p->len; i++) {
        close(p->fds[i]);
    }
    free(p->fds);
    p->fds = NULL;
    p->len = 0;
}

static int pool_get(sock_pool_t *p)
{
    if (p->len > 0) {
        return p->fds[--p->len];
    }
    return -1;
}

static void pool_put(sock_pool_t *p, int fd)
{
    if (p->len < p->cap) {
        p->fds[p->len++] = fd;
    } else {
        close(fd);
    }
}

/* ---- address helpers ---- */

static int parse_listen(const char *addr, uint16_t port, int *family,
                        struct sockaddr_storage *sa, socklen_t *sa_len)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", addr != NULL ? addr : "0.0.0.0");
    /* strip [ ] around IPv6 */
    char *host = buf;
    size_t n = strlen(host);
    if (n >= 2 && host[0] == '[' && host[n - 1] == ']') {
        host[n - 1] = '\0';
        host++;
    }

    memset(sa, 0, sizeof(*sa));
    if (strchr(host, ':') != NULL) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)sa;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(port);
        if (host[0] == '\0') {
            s6->sin6_addr = in6addr_any;
        } else if (inet_pton(AF_INET6, host, &s6->sin6_addr) != 1) {
            return -1;
        }
        *family = AF_INET6;
        *sa_len = sizeof(*s6);
        return 0;
    }
    struct sockaddr_in *s4 = (struct sockaddr_in *)sa;
    s4->sin_family = AF_INET;
    s4->sin_port = htons(port);
    if (inet_pton(AF_INET, host, &s4->sin_addr) != 1) {
        return -1;
    }
    *family = AF_INET;
    *sa_len = sizeof(*s4);
    return 0;
}

/* ---- per-request exchange ---- */

struct exchange {
    exchange_t *next;
    mt_dnsproxy_t *p;
    bool is_tcp;
    int upstream_fd;
    int timer_id;
    bool connecting; /* TCP upstream connect in progress */

    /* raw request to forward */
    uint8_t *req;
    size_t req_len;
    size_t req_sent;

    /* UDP reply addressing */
    struct sockaddr_storage client;
    socklen_t client_len;
    int ifindex;
    int dst_family;
    struct in_addr dst4;
    struct in6_addr dst6;
    bool have_dst;

    /* TCP client */
    int client_fd;
    size_t req_got; /* bytes of the client's request read so far (TCP) */

    /* upstream response read state */
    uint8_t len_buf[2];
    size_t len_got;
    size_t resp_expected;
    uint8_t *resp;
    size_t resp_got;

    /* TCP client write state */
    uint8_t *out;
    size_t out_len;
    size_t out_sent;
};

static void exchange_free(exchange_t *ex)
{
    free(ex->req);
    free(ex->resp);
    free(ex->out);
    free(ex);
}

/* Re-register an fd with a new event mask AND callback. mt_loop_mod_fd only
 * changes the mask, so switching from the connect/write handler to the read
 * handler needs a del+add (the fd stays open). Returns false on failure. */
static bool rearm_fd(mt_loop_t *loop, int fd, uint32_t events, mt_fd_cb cb,
                     void *ud)
{
    (void)mt_loop_del_fd(loop, fd);
    return mt_loop_add_fd(loop, fd, events, cb, ud) == MT_OK;
}

/* Release an already-unlinked exchange: remove watches and close fds. */
static void exchange_release(exchange_t *ex, bool pool_upstream)
{
    mt_dnsproxy_t *p = ex->p;
    if (ex->timer_id > 0) {
        mt_loop_del_timer(p->loop, ex->timer_id);
        ex->timer_id = 0;
    }
    if (ex->upstream_fd >= 0) {
        mt_loop_del_fd(p->loop, ex->upstream_fd);
        if (pool_upstream && !ex->is_tcp) {
            pool_put(&p->udp_pool, ex->upstream_fd);
        } else {
            close(ex->upstream_fd);
        }
        ex->upstream_fd = -1;
    }
    if (ex->client_fd >= 0) {
        mt_loop_del_fd(p->loop, ex->client_fd);
        close(ex->client_fd);
        ex->client_fd = -1;
    }
    atomic_fetch_sub(&p->inflight, 1);
    exchange_free(ex);
}

static void exchange_finish(exchange_t *ex, bool pool_upstream)
{
    exchange_t **link = &ex->p->exchanges;
    while (*link && *link != ex) { link = &(*link)->next; }
    if (*link) { *link = ex->next; }
    exchange_release(ex, pool_upstream);
}

/* Send a UDP reply to the client with the original destination as source. */
static void udp_reply(mt_dnsproxy_t *p, exchange_t *ex, const uint8_t *data,
                      size_t len)
{
    struct msghdr msg;
    struct iovec iov;
    union {
        char v4[CMSG_SPACE(sizeof(struct in_pktinfo))];
        char v6[CMSG_SPACE(sizeof(struct in6_pktinfo))];
    } cbuf;
    memset(&msg, 0, sizeof(msg));
    memset(&cbuf, 0, sizeof(cbuf));

    /* sendmsg does not modify the payload; the const cast is safe.
     * NOLINTNEXTLINE(performance-no-int-to-ptr) */
    memcpy(&iov.iov_base, &data, sizeof(iov.iov_base));
    iov.iov_len = len;
    msg.msg_name = &ex->client;
    msg.msg_namelen = ex->client_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (ex->have_dst) {
        if (ex->dst_family == AF_INET) {
            msg.msg_control = cbuf.v4;
            msg.msg_controllen = CMSG_SPACE(sizeof(struct in_pktinfo));
            struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
            cm->cmsg_level = IPPROTO_IP;
            cm->cmsg_type = IP_PKTINFO;
            cm->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
            struct in_pktinfo pi;
            memset(&pi, 0, sizeof(pi));
            pi.ipi_spec_dst = ex->dst4;
            pi.ipi_ifindex = ex->ifindex;
            memcpy(CMSG_DATA(cm), &pi, sizeof(pi));
        } else {
            msg.msg_control = cbuf.v6;
            msg.msg_controllen = CMSG_SPACE(sizeof(struct in6_pktinfo));
            struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
            cm->cmsg_level = IPPROTO_IPV6;
            cm->cmsg_type = IPV6_PKTINFO;
            cm->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
            struct in6_pktinfo pi;
            memset(&pi, 0, sizeof(pi));
            pi.ipi6_addr = ex->dst6;
            pi.ipi6_ifindex = (unsigned)ex->ifindex;
            memcpy(CMSG_DATA(cm), &pi, sizeof(pi));
        }
    }
    ssize_t rc = sendmsg(p->udp_fd, &msg, 0);
    if (rc < 0) {
        MT_DEBUG("udp reply failed: %s", strerror(errno));
    }
}

/* Common: process an upstream response (parse, hook, deliver). */
static void deliver_response(exchange_t *ex, const uint8_t *resp,
                             size_t resp_len)
{
    mt_dnsproxy_t *p = ex->p;
    const char *network = ex->is_tcp ? "tcp" : "udp";

    mt_dns_msg_t *msg = NULL;
    if (mt_dns_msg_parse(resp, resp_len, &msg) != MT_OK) {
        /* Go drops on response parse failure (ResponseHook always set). */
        exchange_finish(ex, false);
        return;
    }
    if (p->cb != NULL) {
        p->cb(msg, NULL, network, p->cb_ud);
    }

    const uint8_t *out = resp;
    size_t out_len = resp_len;
    uint8_t *packed = NULL;
    if (!p->cfg.disable_drop_aaaa) {
        mt_dns_msg_strip_aaaa(msg);
        if (mt_dns_msg_pack(msg, &packed, &out_len) != MT_OK) {
            mt_dns_msg_free(msg);
            exchange_finish(ex, false);
            return;
        }
        out = packed;
    }

    if (ex->is_tcp) {
        /* frame: 2-byte length + message; queue for writing to client */
        ex->out = malloc(out_len + 2);
        if (ex->out == NULL) {
            free(packed);
            mt_dns_msg_free(msg);
            exchange_finish(ex, false);
            return;
        }
        ex->out[0] = (uint8_t)(out_len >> 8);
        ex->out[1] = (uint8_t)out_len;
        memcpy(ex->out + 2, out, out_len);
        ex->out_len = out_len + 2;
        ex->out_sent = 0;
        /* upstream done; switch loop interest to client writable */
        mt_loop_del_fd(p->loop, ex->upstream_fd);
        close(ex->upstream_fd);
        ex->upstream_fd = -1;
        mt_loop_mod_fd(p->loop, ex->client_fd, EPOLLOUT);
        free(packed);
        mt_dns_msg_free(msg);
        return;
    }

    udp_reply(p, ex, out, out_len);
    free(packed);
    mt_dns_msg_free(msg);
    exchange_finish(ex, true); /* return udp upstream socket to pool */
}

static void on_timeout(mt_loop_t *loop, void *ud)
{
    (void)loop;
    exchange_t *ex = ud;
    ex->timer_id = 0; /* already fired */
    MT_DEBUG("upstream deadline exceeded");
    exchange_finish(ex, false);
}

/* upstream socket readable */
static void on_upstream(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)loop;
    exchange_t *ex = ud;

    if (events & (EPOLLERR | EPOLLHUP)) {
        exchange_finish(ex, false);
        return;
    }

    if (ex->is_tcp && ex->connecting) {
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 ||
            err != 0) {
            exchange_finish(ex, false);
            return;
        }
        ex->connecting = false;
        mt_loop_mod_fd(loop, fd, EPOLLOUT); /* proceed to send */
        return;
    }

    if (!ex->is_tcp) {
        /* UDP: single datagram response */
        uint8_t buf[MT_DNS_MAX_MSG];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            exchange_finish(ex, false);
            return;
        }
        deliver_response(ex, buf, (size_t)n);
        return;
    }

    /* TCP upstream: read 2-byte length then body */
    while (ex->len_got < 2) {
        ssize_t n = recv(fd, ex->len_buf + ex->len_got, 2 - ex->len_got, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            exchange_finish(ex, false);
            return;
        }
        ex->len_got += (size_t)n;
    }
    if (ex->resp == NULL) {
        ex->resp_expected =
            (size_t)ex->len_buf[0] << 8 | (size_t)ex->len_buf[1];
        if (ex->resp_expected == 0 || ex->resp_expected > TCP_MAX_MSG) {
            exchange_finish(ex, false);
            return;
        }
        ex->resp = malloc(ex->resp_expected);
        if (ex->resp == NULL) {
            exchange_finish(ex, false);
            return;
        }
    }
    while (ex->resp_got < ex->resp_expected) {
        ssize_t n = recv(fd, ex->resp + ex->resp_got,
                         ex->resp_expected - ex->resp_got, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            exchange_finish(ex, false);
            return;
        }
        ex->resp_got += (size_t)n;
    }
    deliver_response(ex, ex->resp, ex->resp_got);
}

/* upstream writable: send the (framed) request */
static void on_upstream_writable(mt_loop_t *loop, int fd, uint32_t events,
                                 void *ud)
{
    exchange_t *ex = ud;
    if (events & (EPOLLERR | EPOLLHUP)) {
        exchange_finish(ex, false);
        return;
    }
    if (ex->is_tcp && ex->connecting) {
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 ||
            err != 0) {
            exchange_finish(ex, false);
            return;
        }
        ex->connecting = false;
    }
    while (ex->req_sent < ex->req_len) {
        ssize_t n = send(fd, ex->req + ex->req_sent,
                         ex->req_len - ex->req_sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            exchange_finish(ex, false);
            return;
        }
        ex->req_sent += (size_t)n;
    }
    /* request fully sent; switch to reading the response (new callback) */
    if (!rearm_fd(loop, fd, EPOLLIN, on_upstream, ex)) {
        exchange_finish(ex, false);
    }
}

/* TCP client writable: flush the framed response, then close. */
static void on_client_writable(mt_loop_t *loop, int fd, uint32_t events,
                               void *ud)
{
    (void)loop;
    (void)fd;
    exchange_t *ex = ud;
    if (events & (EPOLLERR | EPOLLHUP)) {
        exchange_finish(ex, false);
        return;
    }
    while (ex->out_sent < ex->out_len) {
        ssize_t n = send(ex->client_fd, ex->out + ex->out_sent,
                         ex->out_len - ex->out_sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            exchange_finish(ex, false);
            return;
        }
        ex->out_sent += (size_t)n;
    }
    exchange_finish(ex, false); /* one query per connection: close */
}

/* Start an upstream exchange for already-captured request bytes. Consumes
 * ex on failure. */
static void start_upstream(mt_dnsproxy_t *p, exchange_t *ex)
{
    /* Reset the response-read state. For TCP, on_tcp_client_read already
     * used len_buf/len_got to read the client's request length prefix, so
     * these must be cleared before they are reused for the upstream reply. */
    ex->len_got = 0;
    ex->resp_expected = 0;
    ex->resp_got = 0;

    if (ex->is_tcp) {
        /* TCP upstream framing: prepend the 2-byte length prefix. The raw
         * message has already been parsed / fake-PTR-checked, so reframing
         * ex->req in place now is safe. */
        uint8_t *framed = malloc(ex->req_len + 2);
        if (framed == NULL) {
            exchange_finish(ex, false);
            return;
        }
        framed[0] = (uint8_t)(ex->req_len >> 8);
        framed[1] = (uint8_t)ex->req_len;
        memcpy(framed + 2, ex->req, ex->req_len);
        free(ex->req);
        ex->req = framed;
        ex->req_len += 2;
        ex->req_sent = 0;

        int fd = socket(p->family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                        0);
        if (fd < 0) {
            exchange_finish(ex, false);
            return;
        }
        ex->upstream_fd = fd;
        int rc = connect(fd, (struct sockaddr *)&p->upstream_sa,
                         p->upstream_sa_len);
        if (rc < 0 && errno != EINPROGRESS) {
            exchange_finish(ex, false);
            return;
        }
        ex->connecting = (rc < 0);
        if (mt_loop_add_fd(p->loop, fd, EPOLLOUT, on_upstream_writable, ex) !=
            MT_OK) {
            exchange_finish(ex, false);
            return;
        }
    } else {
        int fd = pool_get(&p->udp_pool);
        if (fd < 0) {
            fd = socket(p->family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                        0);
            if (fd < 0) {
                exchange_finish(ex, false);
                return;
            }
            if (connect(fd, (struct sockaddr *)&p->upstream_sa,
                        p->upstream_sa_len) != 0) {
                close(fd);
                exchange_finish(ex, false);
                return;
            }
        }
        ex->upstream_fd = fd;
        /* UDP: send immediately, then wait for the reply */
        ssize_t n = send(fd, ex->req, ex->req_len, MSG_NOSIGNAL);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            exchange_finish(ex, false);
            return;
        }
        ex->req_sent = (n > 0) ? (size_t)n : 0;
        uint32_t ev = (ex->req_sent < ex->req_len) ? EPOLLOUT : EPOLLIN;
        mt_fd_cb cb = (ev == EPOLLOUT) ? on_upstream_writable : on_upstream;
        if (mt_loop_add_fd(p->loop, fd, ev, cb, ex) != MT_OK) {
            exchange_finish(ex, false);
            return;
        }
    }

    /* TCP already has a processing deadline, including local PTR replies.
     * UDP arms its processing deadline here. */
    if (ex->timer_id == 0 &&
        mt_loop_add_timer(p->loop, p->cfg.timeout_ms, 0, on_timeout, ex,
                          &ex->timer_id) != MT_OK) {
        exchange_finish(ex, false);
    }
}

typedef enum request_disp {
    REQ_DROP = 0,     /* consumed: caller must finish/free */
    REQ_REPLIED_ASYNC, /* TCP local reply queued: keep ex, wait for write */
    REQ_FORWARD,      /* send upstream */
} request_disp_t;

/* Decide: fake-PTR local reply, drop (malformed), or forward. */
static request_disp_t handle_request_common(mt_dnsproxy_t *p, exchange_t *ex)
{
    /* Go parses the request (hooks set) and drops on parse failure. */
    mt_dns_msg_t *req_msg = NULL;
    if (mt_dns_msg_parse(ex->req, ex->req_len, &req_msg) != MT_OK) {
        return REQ_DROP;
    }
    mt_dns_msg_free(req_msg);

    if (!p->cfg.disable_fake_ptr &&
        mt_dns_is_single_ptr_query(ex->req, ex->req_len)) {
        uint8_t *resp = NULL;
        size_t rlen = 0;
        if (mt_dns_make_fake_ptr_response(ex->req, ex->req_len, &resp,
                                          &rlen) != MT_OK) {
            return REQ_DROP;
        }
        if (ex->is_tcp) {
            ex->out = malloc(rlen + 2);
            if (ex->out == NULL) {
                free(resp);
                return REQ_DROP;
            }
            ex->out[0] = (uint8_t)(rlen >> 8);
            ex->out[1] = (uint8_t)rlen;
            memcpy(ex->out + 2, resp, rlen);
            ex->out_len = rlen + 2;
            free(resp);
            mt_loop_mod_fd(p->loop, ex->client_fd, EPOLLOUT);
            return REQ_REPLIED_ASYNC;
        }
        udp_reply(p, ex, resp, rlen);
        free(resp);
        return REQ_DROP; /* UDP reply is synchronous; free ex */
    }
    return REQ_FORWARD;
}

/* ---- UDP listener ---- */

static void on_udp_readable(mt_loop_t *loop, int fd, uint32_t events,
                            void *ud)
{
    (void)loop;
    mt_dnsproxy_t *p = ud;
    if (events & (EPOLLERR | EPOLLHUP)) {
        return;
    }

    for (;;) {
        uint8_t buf[MT_DNS_MAX_MSG];
        struct sockaddr_storage client;
        struct iovec iov = {buf, sizeof(buf)};
        union {
            char v4[CMSG_SPACE(sizeof(struct in_pktinfo))];
            char v6[CMSG_SPACE(sizeof(struct in6_pktinfo))];
        } cbuf;
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_name = &client;
        msg.msg_namelen = sizeof(client);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = &cbuf;
        msg.msg_controllen = sizeof(cbuf);

        ssize_t n = recvmsg(fd, &msg, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            return;
        }

        /* backpressure: bounded in-flight budget */
        uint64_t cur = atomic_load(&p->inflight);
        if (cur >= p->cfg.max_concurrent) {
            atomic_fetch_add(&p->dropped, 1);
            continue;
        }

        exchange_t *ex = calloc(1, sizeof(*ex));
        if (ex == NULL) {
            continue;
        }
        ex->p = p;
        ex->is_tcp = false;
        ex->upstream_fd = -1;
        ex->client_fd = -1;
        ex->timer_id = 0;
        ex->client_len = msg.msg_namelen;
        memcpy(&ex->client, &client, sizeof(client));
        ex->req = malloc((size_t)n);
        if (ex->req == NULL) {
            free(ex);
            continue;
        }
        memcpy(ex->req, buf, (size_t)n);
        ex->req_len = (size_t)n;

        /* capture original destination for the source-address reply */
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm != NULL;
             cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level == IPPROTO_IP &&
                cm->cmsg_type == IP_PKTINFO) {
                struct in_pktinfo pi;
                memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
                ex->dst_family = AF_INET;
                ex->dst4 = pi.ipi_addr;
                ex->ifindex = pi.ipi_ifindex;
                ex->have_dst = true;
            } else if (cm->cmsg_level == IPPROTO_IPV6 &&
                       cm->cmsg_type == IPV6_PKTINFO) {
                struct in6_pktinfo pi;
                memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
                ex->dst_family = AF_INET6;
                ex->dst6 = pi.ipi6_addr;
                ex->ifindex = (int)pi.ipi6_ifindex;
                ex->have_dst = true;
            }
        }

        ex->next = p->exchanges;
        p->exchanges = ex;
        atomic_fetch_add(&p->inflight, 1);
        /* UDP: fake-PTR replies synchronously, so only DROP/FORWARD occur */
        if (handle_request_common(p, ex) != REQ_FORWARD) {
            exchange_finish(ex, false);
            continue;
        }
        start_upstream(p, ex);
    }
}

/* ---- TCP listener ---- */

static void on_tcp_client_read(mt_loop_t *loop, int fd, uint32_t events,
                               void *ud)
{
    (void)loop;
    exchange_t *ex = ud;

    if (events & (EPOLLERR | EPOLLHUP)) {
        exchange_finish(ex, false);
        return;
    }

    /* read length prefix + body (one query per connection) */
    while (ex->len_got < 2) {
        ssize_t n = recv(fd, ex->len_buf + ex->len_got, 2 - ex->len_got, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            exchange_finish(ex, false);
            return;
        }
        ex->len_got += (size_t)n;
    }
    if (ex->req == NULL) {
        size_t want = (size_t)ex->len_buf[0] << 8 | (size_t)ex->len_buf[1];
        if (want == 0 || want > TCP_MAX_MSG) {
            exchange_finish(ex, false);
            return;
        }
        ex->req = malloc(want);
        if (ex->req == NULL) {
            exchange_finish(ex, false);
            return;
        }
        ex->req_len = want;
        ex->req_got = 0;
    }
    while (ex->req_got < ex->req_len) {
        ssize_t n = recv(fd, ex->req + ex->req_got, ex->req_len - ex->req_got,
                         0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            exchange_finish(ex, false);
            return;
        }
        ex->req_got += (size_t)n;
    }

    /* Match Go's separate request-read and processing deadlines. */
    (void)mt_loop_del_timer(ex->p->loop, ex->timer_id);
    ex->timer_id = 0;
    if (mt_loop_add_timer(ex->p->loop, ex->p->cfg.timeout_ms, 0,
                          on_timeout, ex, &ex->timer_id) != MT_OK) {
        exchange_finish(ex, false);
        return;
    }

    /* full request read; stop reading from the client */
    mt_loop_del_fd(ex->p->loop, ex->client_fd);
    /* re-register the client fd only when we have a response to write; keep
     * the fd open. Mark it un-registered by re-adding on write. */
    if (mt_loop_add_fd(ex->p->loop, ex->client_fd, 0, on_client_writable,
                       ex) != MT_OK) {
        exchange_finish(ex, false);
        return;
    }

    request_disp_t disp = handle_request_common(ex->p, ex);
    if (disp == REQ_DROP) {
        exchange_finish(ex, false);
        return;
    }
    if (disp == REQ_REPLIED_ASYNC) {
        return; /* client-writable callback will flush + close */
    }
    start_upstream(ex->p, ex);
}

static void on_tcp_accept(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)loop;
    mt_dnsproxy_t *p = ud;
    if (events & (EPOLLERR | EPOLLHUP)) {
        return;
    }
    for (;;) {
        int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            return;
        }
        /* backpressure: bounded in-flight budget */
        if (atomic_load(&p->inflight) >= p->cfg.max_concurrent) {
            atomic_fetch_add(&p->dropped, 1);
            close(cfd);
            continue;
        }
        exchange_t *ex = calloc(1, sizeof(*ex));
        if (ex == NULL) {
            close(cfd);
            continue;
        }
        ex->p = p;
        ex->is_tcp = true;
        ex->upstream_fd = -1;
        ex->client_fd = cfd;
        ex->next = p->exchanges;
        p->exchanges = ex;
        atomic_fetch_add(&p->inflight, 1);
        if (mt_loop_add_fd(p->loop, cfd, EPOLLIN, on_tcp_client_read, ex) !=
            MT_OK) {
            exchange_finish(ex, false);
            continue;
        }
        /* A peer that never finishes its length prefix/body must not
         * retain a shared UDP/TCP concurrency slot indefinitely. */
        if (mt_loop_add_timer(p->loop, p->cfg.timeout_ms, 0, on_timeout, ex,
                              &ex->timer_id) != MT_OK) {
            exchange_finish(ex, false);
        }
    }
}

/* ---- public API ---- */

mt_err_t mt_dnsproxy_create(const mt_dnsproxy_config_t *cfg, mt_loop_t *loop,
                            mt_dnsproxy_msg_cb cb, void *cb_ud,
                            mt_dnsproxy_t **out)
{
    mt_dnsproxy_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return MT_ERR_NOMEM;
    }
    p->cfg = *cfg;
    p->loop = loop;
    p->cb = cb;
    p->cb_ud = cb_ud;
    p->udp_fd = -1;
    p->tcp_fd = -1;

    int fam_listen;
    struct sockaddr_storage listen_sa;
    socklen_t listen_len;
    if (parse_listen(cfg->listen_addr, cfg->listen_port, &fam_listen,
                     &listen_sa, &listen_len) != 0) {
        free(p);
        return MT_ERR_INVAL;
    }
    p->family = fam_listen;

    /* Unlike inet_pton, getaddrinfo accepts the hostnames accepted by
     * Go's net.Dialer. Resolve once at startup, outside the event loop;
     * upstream and listener families are independent. */
    const char *upstream = cfg->upstream_addr;
    char host[256];
    if (!upstream || strlen(upstream) >= sizeof(host)) {
        free(p);
        return MT_ERR_INVAL;
    }
    snprintf(host, sizeof(host), "%s", upstream);
    size_t host_len = strlen(host);
    if (host_len >= 2 && host[0] == '[' && host[host_len - 1] == ']') {
        host[host_len - 1] = '\0';
        memmove(host, host + 1, host_len - 1);
    }
    char service[6];
    snprintf(service, sizeof(service), "%u", cfg->upstream_port);
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(host, service, &hints, &addresses) != 0 || !addresses) {
        free(p);
        return MT_ERR_INVAL;
    }
    if (addresses->ai_addrlen > sizeof(p->upstream_sa)) {
        freeaddrinfo(addresses);
        free(p);
        return MT_ERR_INVAL;
    }
    memcpy(&p->upstream_sa, addresses->ai_addr, addresses->ai_addrlen);
    p->upstream_sa_len = addresses->ai_addrlen;
    p->family = addresses->ai_family;
    freeaddrinfo(addresses);

    p->cfg.listen_addr = strdup(cfg->listen_addr ? cfg->listen_addr : "0.0.0.0");
    p->cfg.upstream_addr = strdup(upstream);
    pool_init(&p->udp_pool, cfg->max_idle_conns);
    if (!p->cfg.listen_addr || !p->cfg.upstream_addr || !p->udp_pool.fds) {
        mt_dnsproxy_destroy(p);
        return MT_ERR_NOMEM;
    }
    *out = p;
    return MT_OK;
}

mt_err_t mt_dnsproxy_start(mt_dnsproxy_t *p)
{
    struct sockaddr_storage listen_sa;
    socklen_t listen_len;
    int fam;
    if (parse_listen(p->cfg.listen_addr, p->cfg.listen_port, &fam, &listen_sa,
                     &listen_len) != 0) {
        return MT_ERR_INVAL;
    }

    /* UDP */
    int ufd = socket(fam, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (ufd < 0) {
        return mt_err_from_errno(errno);
    }
    int one = 1;
    setsockopt(ufd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (fam == AF_INET6) {
        int zero = 0;
        setsockopt(ufd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        setsockopt(ufd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one));
        setsockopt(ufd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
    } else {
        setsockopt(ufd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
    }
    if (bind(ufd, (struct sockaddr *)&listen_sa, listen_len) != 0) {
        mt_err_t e = mt_err_from_errno(errno);
        close(ufd);
        return e;
    }
    p->udp_fd = ufd;

    /* TCP */
    int tfd = socket(fam, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (tfd < 0) {
        mt_err_t e = mt_err_from_errno(errno);
        close(ufd);
        p->udp_fd = -1;
        return e;
    }
    setsockopt(tfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (fam == AF_INET6) {
        int zero = 0;
        setsockopt(tfd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
    }
    if (bind(tfd, (struct sockaddr *)&listen_sa, listen_len) != 0 ||
        listen(tfd, 128) != 0) {
        mt_err_t e = mt_err_from_errno(errno);
        close(tfd);
        close(ufd);
        p->udp_fd = -1;
        return e;
    }
    p->tcp_fd = tfd;

    mt_err_t err = mt_loop_add_fd(p->loop, ufd, EPOLLIN, on_udp_readable, p);
    if (err != MT_OK) {
        return err;
    }
    err = mt_loop_add_fd(p->loop, tfd, EPOLLIN, on_tcp_accept, p);
    if (err != MT_OK) {
        return err;
    }
    MT_INFO("DNS proxy listening on %s:%u (udp+tcp), upstream %s:%u",
            p->cfg.listen_addr, p->cfg.listen_port, p->cfg.upstream_addr,
            p->cfg.upstream_port);
    return MT_OK;
}

void mt_dnsproxy_destroy(mt_dnsproxy_t *p)
{
    if (p == NULL) {
        return;
    }
    while (p->exchanges) {
        exchange_t *ex = p->exchanges;
        p->exchanges = ex->next;
        exchange_release(ex, false);
    }
    if (p->udp_fd >= 0) {
        mt_loop_del_fd(p->loop, p->udp_fd);
        close(p->udp_fd);
    }
    if (p->tcp_fd >= 0) {
        mt_loop_del_fd(p->loop, p->tcp_fd);
        close(p->tcp_fd);
    }
    pool_clear(&p->udp_pool);
    free((void *)p->cfg.listen_addr);
    free((void *)p->cfg.upstream_addr);
    free(p);
}

uint64_t mt_dnsproxy_dropped(const mt_dnsproxy_t *p)
{
    return atomic_load(&p->dropped);
}

uint64_t mt_dnsproxy_inflight(const mt_dnsproxy_t *p)
{
    return atomic_load(&p->inflight);
}

void mt_dnsproxy_set_disable_flags(mt_dnsproxy_t *p, bool disable_fake_ptr,
                                   bool disable_drop_aaaa)
{
    p->cfg.disable_fake_ptr = disable_fake_ptr;
    p->cfg.disable_drop_aaaa = disable_drop_aaaa;
}
