/* See httpd.h. */
#include "magitrickle/httpd.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "magitrickle/json.h"
#include "magitrickle/log.h"

#define MT_HTTPD_MAX_ROUTES 64
#define MT_HTTPD_MAX_REQ_HEADERS 32
#define MT_HTTPD_MAX_RES_HEADERS 8
#define MT_HTTPD_MAX_QUERY 16
#define MT_HTTPD_MAX_SEGMENTS 8

typedef struct kv {
    char name[64];
    char value[512];
} kv_t;

typedef struct mt_http_conn mt_http_conn_t;

struct mt_http_req {
    mt_http_conn_t *conn;
    char method[8];
    char path[512];
    kv_t query[MT_HTTPD_MAX_QUERY];
    size_t n_query;
    kv_t params[MT_HTTPD_MAX_PARAMS];
    size_t n_params;
    kv_t headers[MT_HTTPD_MAX_REQ_HEADERS];
    size_t n_headers;
    const uint8_t *body;
    size_t body_len;
};

struct mt_http_res {
    mt_http_deferred_t *deferred;
    int status;
    kv_t headers[MT_HTTPD_MAX_RES_HEADERS];
    size_t n_headers;
    uint8_t *body;
    size_t body_len;
    bool responded;
};

typedef struct route {
    char method[8];
    char pattern[256];
    mt_http_handler_fn fn;
    void *ud;
} route_t;

struct mt_http_deferred {
    mt_http_conn_t *conn; /* nulled when its peer disappears */
};

struct mt_httpd {
    mt_loop_t *loop;
    int tcp_fd;
    int unix_fd;
    char unix_path[256];
    route_t routes[MT_HTTPD_MAX_ROUTES];
    size_t n_routes;
    mt_http_middleware_fn middleware;
    void *middleware_ud;
    mt_http_handler_fn not_found;
    void *not_found_ud;
    size_t n_conns;
    size_t reserved_body_bytes;
    mt_http_conn_t *conns;
};

struct mt_http_conn {
    mt_http_deferred_t *pending;
    mt_httpd_t *server;
    mt_http_conn_t *next;
    int fd;
    uint8_t *rbuf;
    size_t rbuf_len;
    size_t rbuf_cap;
    bool headers_done;
    size_t header_end;
    size_t content_length;
    size_t reserved_body_bytes;
    mt_http_req_t req;
    uint8_t *wbuf;
    size_t wbuf_len;
    size_t wbuf_sent;
    bool close_after_write;
    bool http_1_0;
    int idle_timer_id;
};

/* ---- small string helpers -------------------------------------------------- */

typedef struct seg {
    const char *p;
    size_t len;
} seg_t;

static size_t split_segments(const char *s, seg_t *out, size_t max) {
    size_t n = 0;
    size_t i = 0;
    size_t slen = strlen(s);
    while (i < slen && n < max) {
        while (i < slen && s[i] == '/') { i++; }
        if (i >= slen) { break; }
        size_t start = i;
        while (i < slen && s[i] != '/') { i++; }
        out[n].p = s + start;
        out[n].len = i - start;
        n++;
    }
    return n;
}

static size_t percent_decode(const char *in, size_t in_len, char *out, size_t out_cap,
                             bool plus_as_space) {
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 1 < out_cap; i++) {
        char c = in[i];
        if (c == '%' && i + 2 < in_len && isxdigit((unsigned char)in[i + 1]) &&
            isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = {in[i + 1], in[i + 2], '\0'};
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (plus_as_space && c == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return o;
}

static const char *find_header(const kv_t *headers, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp(headers[i].name, name) == 0) { return headers[i].value; }
    }
    return NULL;
}

/* ---- request accessors ----------------------------------------------------- */

const char *mt_http_req_method(const mt_http_req_t *req) { return req->method; }
const char *mt_http_req_path(const mt_http_req_t *req) { return req->path; }

const char *mt_http_req_query(const mt_http_req_t *req, const char *key) {
    for (size_t i = 0; i < req->n_query; i++) {
        if (strcmp(req->query[i].name, key) == 0) { return req->query[i].value; }
    }
    return NULL;
}

bool mt_http_req_query_is_true(const mt_http_req_t *req, const char *key) {
    const char *v = mt_http_req_query(req, key);
    return v != NULL && strcmp(v, "true") == 0;
}

const char *mt_http_req_param(const mt_http_req_t *req, const char *name) {
    for (size_t i = 0; i < req->n_params; i++) {
        if (strcmp(req->params[i].name, name) == 0) { return req->params[i].value; }
    }
    return NULL;
}

const char *mt_http_req_header(const mt_http_req_t *req, const char *name) {
    return find_header(req->headers, req->n_headers, name);
}

const uint8_t *mt_http_req_body(const mt_http_req_t *req, size_t *len) {
    *len = req->body_len;
    return req->body;
}

/* ---- response building ------------------------------------------------------ */

void mt_http_res_set_header(mt_http_res_t *res, const char *name, const char *value) {
    if (res->n_headers >= MT_HTTPD_MAX_RES_HEADERS) { return; }
    snprintf(res->headers[res->n_headers].name, sizeof(res->headers[0].name), "%s", name);
    snprintf(res->headers[res->n_headers].value, sizeof(res->headers[0].value), "%s", value);
    res->n_headers++;
}

void mt_http_res_write(mt_http_res_t *res, int status, const char *content_type,
                       const uint8_t *data, size_t len) {
    free(res->body);
    res->body = NULL;
    res->body_len = 0;
    if (len > 0) {
        res->body = malloc(len);
        if (res->body) {
            memcpy(res->body, data, len);
            res->body_len = len;
        }
    }
    res->status = status;
    if (content_type) { mt_http_res_set_header(res, "Content-Type", content_type); }
    res->responded = true;
}

void mt_http_res_write_json(mt_http_res_t *res, int status, cJSON *obj) {
    char *dump = mt_json_dump(obj);
    cJSON_Delete(obj);
    if (!dump) {
        mt_http_res_write_error(res, 500, "failed to encode response");
        return;
    }
    mt_http_res_write(res, status, "application/json; charset=utf-8", (const uint8_t *)dump,
                      strlen(dump));
    free(dump);
}

void mt_http_res_write_error(mt_http_res_t *res, int status, const char *msg) {
    cJSON *obj = mt_json_error(msg);
    if (!obj) {
        static const char fallback[] = "{\"error\":\"internal error\"}";
        mt_http_res_write(res, 500, "application/json; charset=utf-8", (const uint8_t *)fallback,
                          strlen(fallback));
        return;
    }
    mt_http_res_write_json(res, status, obj);
}

/* ---- route table ------------------------------------------------------------ */

mt_err_t mt_httpd_route(mt_httpd_t *h, const char *method, const char *pattern,
                        mt_http_handler_fn fn, void *ud) {
    if (h->n_routes >= MT_HTTPD_MAX_ROUTES) { return MT_ERR_LIMIT; }
    route_t *r = &h->routes[h->n_routes++];
    snprintf(r->method, sizeof(r->method), "%s", method);
    snprintf(r->pattern, sizeof(r->pattern), "%s", pattern);
    r->fn = fn;
    r->ud = ud;
    return MT_OK;
}

void mt_httpd_set_middleware(mt_httpd_t *h, mt_http_middleware_fn fn, void *ud) {
    h->middleware = fn;
    h->middleware_ud = ud;
}

void mt_httpd_set_not_found(mt_httpd_t *h, mt_http_handler_fn fn, void *ud) {
    h->not_found = fn;
    h->not_found_ud = ud;
}

static route_t *match_route(mt_httpd_t *h, const char *method, const char *path,
                            mt_http_req_t *req) {
    seg_t path_segs[MT_HTTPD_MAX_SEGMENTS];
    size_t n_path = split_segments(path, path_segs, MT_HTTPD_MAX_SEGMENTS);

    for (size_t ri = 0; ri < h->n_routes; ri++) {
        route_t *r = &h->routes[ri];
        if (strcmp(r->method, method) != 0) { continue; }
        seg_t pat_segs[MT_HTTPD_MAX_SEGMENTS];
        size_t n_pat = split_segments(r->pattern, pat_segs, MT_HTTPD_MAX_SEGMENTS);
        if (n_pat != n_path) { continue; }

        size_t n_params = 0;
        bool ok = true;
        for (size_t i = 0; i < n_pat && ok; i++) {
            if (pat_segs[i].len >= 2 && pat_segs[i].p[0] == '{' &&
                pat_segs[i].p[pat_segs[i].len - 1] == '}') {
                if (n_params >= MT_HTTPD_MAX_PARAMS) {
                    ok = false;
                    break;
                }
                size_t name_len = pat_segs[i].len - 2;
                snprintf(req->params[n_params].name, sizeof(req->params[0].name), "%.*s",
                         (int)name_len, pat_segs[i].p + 1);
                snprintf(req->params[n_params].value, sizeof(req->params[0].value), "%.*s",
                         (int)path_segs[i].len, path_segs[i].p);
                n_params++;
            } else if (pat_segs[i].len != path_segs[i].len ||
                      strncmp(pat_segs[i].p, path_segs[i].p, pat_segs[i].len) != 0) {
                ok = false;
            }
        }
        if (ok) {
            req->n_params = n_params;
            return r;
        }
    }
    return NULL;
}

/* ---- connection lifecycle ----------------------------------------------------- */

/* Release an already-unlinked connection. Keep registry mutation separate
 * so bulk teardown never relies on an alias through c->server. */
static void conn_destroy(mt_http_conn_t *c) {
    if (c->pending) { c->pending->conn = NULL; c->pending = NULL; }
    if (c->idle_timer_id) { mt_loop_del_timer(c->server->loop, c->idle_timer_id); }
    mt_loop_del_fd(c->server->loop, c->fd);
    close(c->fd);
    c->server->reserved_body_bytes -= c->reserved_body_bytes;
    c->reserved_body_bytes = 0;
    free(c->rbuf);
    free(c->wbuf);
    free(c);
}

static void conn_close(mt_http_conn_t *c) {
    mt_http_conn_t **link = &c->server->conns;
    while (*link != NULL && *link != c) { link = &(*link)->next; }
    if (*link == c) {
        *link = c->next;
        c->server->n_conns--;
    }
    conn_destroy(c);
}

static void on_idle_timeout(mt_loop_t *loop, void *ud) {
    (void)loop;
    mt_http_conn_t *c = ud;
    c->idle_timer_id = 0;
    conn_close(c);
}

static void reset_idle_timer(mt_http_conn_t *c) {
    if (c->idle_timer_id) {
        mt_loop_del_timer(c->server->loop, c->idle_timer_id);
        c->idle_timer_id = 0;
    }
    mt_loop_add_timer(c->server->loop, MT_HTTPD_IDLE_TIMEOUT_MS, 0, on_idle_timeout, c,
                      &c->idle_timer_id);
}

static void conn_reset_for_next_request(mt_http_conn_t *c) {
    c->server->reserved_body_bytes -= c->reserved_body_bytes;
    c->reserved_body_bytes = 0;
    free(c->rbuf); c->rbuf = NULL; c->rbuf_cap = 0;
    c->rbuf_len = 0;
    c->headers_done = false;
    c->header_end = 0;
    c->content_length = 0;
    memset(&c->req, 0, sizeof(c->req));
    free(c->wbuf);
    c->wbuf = NULL;
    c->wbuf_len = 0;
    c->wbuf_sent = 0;
}

static void on_conn_writable(mt_loop_t *loop, int fd, uint32_t events, void *ud);
static void on_conn_readable(mt_loop_t *loop, int fd, uint32_t events, void *ud);

/* mt_loop_mod_fd only changes the epoll interest mask, not the callback
 * bound to the fd (see loop.c watch_t) -- switching which callback
 * handles a fd requires del_fd + add_fd with the new callback, the same
 * pattern dns/proxy.c uses for its client-writable handoff. */
static void start_write(mt_http_conn_t *c) {
    while (c->wbuf_sent < c->wbuf_len) {
        ssize_t n = send(c->fd, c->wbuf + c->wbuf_sent, c->wbuf_len - c->wbuf_sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                mt_loop_del_fd(c->server->loop, c->fd);
                if (mt_loop_add_fd(c->server->loop, c->fd, EPOLLOUT, on_conn_writable, c) !=
                    MT_OK) {
                    conn_close(c);
                }
                return;
            }
            conn_close(c);
            return;
        }
        c->wbuf_sent += (size_t)n;
    }
    /* fully written */
    if (c->close_after_write) {
        conn_close(c);
        return;
    }
    conn_reset_for_next_request(c);
    mt_loop_del_fd(c->server->loop, c->fd);
    if (mt_loop_add_fd(c->server->loop, c->fd, EPOLLIN, on_conn_readable, c) != MT_OK) {
        conn_close(c);
        return;
    }
    reset_idle_timer(c);
}

static void on_conn_writable(mt_loop_t *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)fd;
    mt_http_conn_t *c = ud;
    if (events & (EPOLLERR | EPOLLHUP)) {
        conn_close(c);
        return;
    }
    start_write(c);
}

static const char *reason_phrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "";
    }
}

static void build_response_bytes(mt_http_conn_t *c, const mt_http_res_t *res) {
    char head[2048];
    int off = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\n", res->status,
                       reason_phrase(res->status));
    for (size_t i = 0; i < res->n_headers && off < (int)sizeof(head); i++) {
        off += snprintf(head + off, sizeof(head) - (size_t)off, "%s: %s\r\n",
                        res->headers[i].name, res->headers[i].value);
    }
    off += snprintf(head + off, sizeof(head) - (size_t)off, "Content-Length: %zu\r\n",
                    res->body_len);
    off += snprintf(head + off, sizeof(head) - (size_t)off, "Connection: %s\r\n",
                    c->close_after_write ? "close" : "keep-alive");
    off += snprintf(head + off, sizeof(head) - (size_t)off, "\r\n");

    size_t head_len = (size_t)off;
    c->wbuf = malloc(head_len + res->body_len);
    if (!c->wbuf) {
        c->wbuf_len = 0;
        return;
    }
    memcpy(c->wbuf, head, head_len);
    if (res->body_len > 0) { memcpy(c->wbuf + head_len, res->body, res->body_len); }
    c->wbuf_len = head_len + res->body_len;
    c->wbuf_sent = 0;
}

mt_http_deferred_t *mt_http_res_defer(mt_http_req_t *req, mt_http_res_t *res) {
    if (!req->conn || req->conn->pending || res->deferred) { return NULL; }
    mt_http_deferred_t *pending = calloc(1, sizeof(*pending));
    if (!pending) { return NULL; }
    pending->conn = req->conn;
    req->conn->pending = pending;
    res->deferred = pending;
    return pending;
}

void mt_http_res_cancel_defer(mt_http_res_t *res) {
    mt_http_deferred_t *p = res->deferred;
    if (!p) { return; }
    if (p->conn) { p->conn->pending = NULL; }
    free(p);
    res->deferred = NULL;
}

void mt_http_deferred_json(mt_http_deferred_t *p, int status, cJSON *obj) {
    if (!p) { cJSON_Delete(obj); return; }
    mt_http_conn_t *c = p->conn;
    free(p);
    if (!c) { cJSON_Delete(obj); return; }
    c->pending = NULL;
    mt_http_res_t res = {0};
    mt_http_res_write_json(&res, status, obj);
    build_response_bytes(c, &res);
    free(res.body);
    start_write(c);
}

void mt_http_deferred_error(mt_http_deferred_t *p, int status, const char *msg) {
    mt_http_deferred_json(p, status, mt_json_error(msg));
}

/* Collapses "." and ".." segments the way Go's path.Clean does for a
 * rooted path (the input here always starts with '/'): a ".." at or
 * above the root is simply dropped rather than escaping upward. This is
 * what makes it safe for the static file handler (see the not-found
 * fallback wired up by main.c) to join mt_http_req_path()'s result onto
 * a filesystem root without a path-traversal hole -- matches the
 * contract already documented on mt_http_req_path in httpd.h. */
#define MT_HTTPD_MAX_CLEAN_SEGMENTS 64
static void path_clean(const char *in, char *out, size_t out_cap) {
    seg_t kept[MT_HTTPD_MAX_CLEAN_SEGMENTS];
    size_t n_kept = 0;
    size_t len = strlen(in);
    size_t i = 0;
    while (i < len) {
        while (i < len && in[i] == '/') { i++; }
        if (i >= len) { break; }
        size_t start = i;
        while (i < len && in[i] != '/') { i++; }
        size_t seg_len = i - start;
        if (seg_len == 1 && in[start] == '.') {
            continue;
        } else if (seg_len == 2 && in[start] == '.' && in[start + 1] == '.') {
            if (n_kept > 0) { n_kept--; }
        } else if (n_kept < MT_HTTPD_MAX_CLEAN_SEGMENTS) {
            kept[n_kept].p = in + start;
            kept[n_kept].len = seg_len;
            n_kept++;
        }
    }
    size_t o = 0;
    if (o + 1 < out_cap) { out[o++] = '/'; }
    for (size_t k = 0; k < n_kept; k++) {
        if (k > 0 && o + 1 < out_cap) { out[o++] = '/'; }
        size_t copy = kept[k].len;
        if (o + copy >= out_cap) { copy = out_cap > o + 1 ? out_cap - 1 - o : 0; }
        memcpy(out + o, kept[k].p, copy);
        o += copy;
    }
    out[o < out_cap ? o : out_cap - 1] = '\0';
}

/* Parses the request line + headers already accumulated in
 * c->rbuf[0..c->header_end); fills *req (body left for the caller to
 * attach once fully read). Returns false on malformed input. */
static bool parse_headers(mt_http_conn_t *c, mt_http_req_t *req) {
    const char *buf = (const char *)c->rbuf;
    size_t pos = 0;

    /* request line: "METHOD path HTTP/1.x\r\n" */
    const char *line_end = memchr(buf, '\n', c->header_end);
    if (!line_end) { return false; }
    size_t line_len = (size_t)(line_end - buf);
    if (line_len > 0 && buf[line_len - 1] == '\r') { line_len--; }

    const char *sp1 = memchr(buf, ' ', line_len);
    if (!sp1) { return false; }
    size_t method_len = (size_t)(sp1 - buf);
    if (method_len >= sizeof(req->method)) { return false; }
    memcpy(req->method, buf, method_len);
    req->method[method_len] = '\0';

    const char *path_start = sp1 + 1;
    const char *sp2 = memchr(path_start, ' ', line_len - method_len - 1);
    if (!sp2) { return false; }
    size_t full_path_len = (size_t)(sp2 - path_start);

    if (line_len - method_len - 1 - full_path_len >= 9 &&
        strncmp(sp2 + 1, "HTTP/1.0", 8) == 0) {
        c->http_1_0 = true;
    }

    const char *qmark = memchr(path_start, '?', full_path_len);
    size_t path_only_len = qmark ? (size_t)(qmark - path_start) : full_path_len;
    char decoded_path[sizeof(req->path)] = {0};
    percent_decode(path_start, path_only_len, decoded_path, sizeof(decoded_path), false);
    path_clean(decoded_path, req->path, sizeof(req->path));

    if (qmark) {
        const char *q = qmark + 1;
        size_t qlen = full_path_len - path_only_len - 1;
        size_t i = 0;
        while (i < qlen && req->n_query < MT_HTTPD_MAX_QUERY) {
            size_t start = i;
            while (i < qlen && q[i] != '&') { i++; }
            size_t pair_len = i - start;
            const char *eq = memchr(q + start, '=', pair_len);
            size_t name_raw_len = eq ? (size_t)(eq - (q + start)) : pair_len;
            percent_decode(q + start, name_raw_len, req->query[req->n_query].name,
                          sizeof(req->query[0].name), true);
            if (eq) {
                size_t val_start = start + name_raw_len + 1;
                size_t val_len = pair_len - name_raw_len - 1;
                percent_decode(q + val_start, val_len, req->query[req->n_query].value,
                              sizeof(req->query[0].value), true);
            } else {
                req->query[req->n_query].value[0] = '\0';
            }
            req->n_query++;
            if (i < qlen) { i++; /* skip '&' */ }
        }
    }

    pos = (size_t)(line_end - buf) + 1;

    while (pos < c->header_end && req->n_headers < MT_HTTPD_MAX_REQ_HEADERS) {
        const char *hline = buf + pos;
        const char *hend = memchr(hline, '\n', c->header_end - pos);
        if (!hend) { break; }
        size_t hlen = (size_t)(hend - hline);
        if (hlen > 0 && hline[hlen - 1] == '\r') { hlen--; }
        pos = (size_t)(hend - buf) + 1;
        if (hlen == 0) { break; /* blank line: end of headers */ }

        const char *colon = memchr(hline, ':', hlen);
        if (!colon) { continue; }
        size_t name_len = (size_t)(colon - hline);
        const char *vstart = colon + 1;
        size_t vlen = hlen - name_len - 1;
        while (vlen > 0 && *vstart == ' ') {
            vstart++;
            vlen--;
        }
        if (name_len >= sizeof(req->headers[0].name)) { continue; }
        kv_t *h = &req->headers[req->n_headers];
        memcpy(h->name, hline, name_len);
        h->name[name_len] = '\0';
        size_t copy_len = vlen < sizeof(h->value) - 1 ? vlen : sizeof(h->value) - 1;
        memcpy(h->value, vstart, copy_len);
        h->value[copy_len] = '\0';
        req->n_headers++;
    }
    return true;
}

static void handle_complete_request(mt_http_conn_t *c) {
    mt_http_req_t *req = &c->req;
    req->conn = c;
    req->body = c->content_length > 0 ? c->rbuf + c->header_end : NULL;
    req->body_len = c->content_length;

    const char *conn_hdr = find_header(req->headers, req->n_headers, "Connection");
    bool want_close = c->http_1_0;
    if (conn_hdr) {
        if (strcasecmp(conn_hdr, "close") == 0) { want_close = true; }
        if (strcasecmp(conn_hdr, "keep-alive") == 0) { want_close = false; }
    }
    if (c->server->n_conns > MT_HTTPD_MAX_CONNS) { want_close = true; }
    c->close_after_write = want_close;

    mt_http_res_t res = {0};
    res.status = 200;

    bool proceed = true;
    if (c->server->middleware) {
        proceed = c->server->middleware(req, &res, c->server->middleware_ud);
    }
    if (proceed) {
        route_t *r = match_route(c->server, req->method, req->path, req);
        if (r) {
            r->fn(req, &res, r->ud);
        } else if (c->server->not_found) {
            c->server->not_found(req, &res, c->server->not_found_ud);
        } else {
            mt_http_res_write_error(&res, 404, "not found");
        }
    }
    if (res.deferred) {
        /* Disable reads until the response completes; keep HUP/ERR and
         * the idle timer so disconnect/timeout detaches the weak handle. */
        free(res.body);
        if (mt_loop_mod_fd(c->server->loop, c->fd, 0) != MT_OK) { conn_close(c); }
        return;
    }
    if (!res.responded) { mt_http_res_write_error(&res, 500, "handler produced no response"); }

    build_response_bytes(c, &res);
    free(res.body);
    start_write(c);
}

static size_t request_body_limit(const mt_http_req_t *req) {
    if (strcmp(req->method, "PUT") != 0 && strcmp(req->method, "POST") != 0) {
        return MT_HTTPD_MAX_BODY_BYTES;
    }
    const char *prefixes[] = {"/api/v1/groups", "/api/v1/subscriptions"};
    for (size_t i = 0; i < 2; i++) {
        size_t len = strlen(prefixes[i]);
        if (strncmp(req->path, prefixes[i], len) == 0 &&
            (req->path[len] == '\0' || req->path[len] == '/')) { return MT_HTTPD_MAX_RULE_BODY_BYTES; }
    }
    return MT_HTTPD_MAX_BODY_BYTES;
}

static void reject_input(mt_http_conn_t *c, int status, const char *message) {
    mt_http_res_t res = {0};
    c->close_after_write = true;
    mt_http_res_write_error(&res, status, message);
    build_response_bytes(c, &res); free(res.body); start_write(c);
}

static void on_conn_readable(mt_loop_t *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    mt_http_conn_t *c = ud;
    if (events & (EPOLLERR | EPOLLHUP)) {
        conn_close(c);
        return;
    }
    reset_idle_timer(c);

    for (;;) {
        size_t cap_needed = c->headers_done ? c->header_end + c->content_length
                                            : MT_HTTPD_MAX_HEADER_BYTES;
        if (c->rbuf_cap < cap_needed + 1) {
            size_t new_cap = cap_needed + 1;
            uint8_t *nb = realloc(c->rbuf, new_cap);
            if (!nb) {
                conn_close(c);
                return;
            }
            c->rbuf = nb;
            c->rbuf_cap = new_cap;
        }

        size_t want = cap_needed - c->rbuf_len;
        if (want == 0) { break; }
        ssize_t n = recv(fd, c->rbuf + c->rbuf_len, want, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { return; }
            conn_close(c);
            return;
        }
        if (n == 0) {
            conn_close(c);
            return;
        }
        c->rbuf_len += (size_t)n;

        if (!c->headers_done) {
            /* End-of-headers is a blank line. Accept both CRLFCRLF and bare
             * LFLF: the entware_kn ndm netfilter.d self-heal hook posts its
             * request through a shell here-doc, which emits bare-LF line
             * endings. Go's net/http (the old unix-socket API server)
             * accepted those, so a strict CRLFCRLF-only scan would silently
             * drop the hook and never re-commit iptables after ndm flushes
             * the tables. parse_headers() already tolerates the optional
             * per-line CR. */
            uint8_t *marker = NULL;
            size_t marker_len = 0;
            for (size_t i = 0; i + 1 < c->rbuf_len; i++) {
                if (i + 3 < c->rbuf_len && c->rbuf[i] == '\r' && c->rbuf[i + 1] == '\n' &&
                    c->rbuf[i + 2] == '\r' && c->rbuf[i + 3] == '\n') {
                    marker = c->rbuf + i;
                    marker_len = 4;
                    break;
                }
                if (c->rbuf[i] == '\n' && c->rbuf[i + 1] == '\n') {
                    marker = c->rbuf + i;
                    marker_len = 2;
                    break;
                }
            }
            if (marker) {
                c->header_end = (size_t)(marker - c->rbuf) + marker_len;
                c->headers_done = true;

                if (!parse_headers(c, &c->req)) {
                    static const char bad[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: "
                                              "0\r\nConnection: close\r\n\r\n";
                    send(fd, bad, strlen(bad), MSG_NOSIGNAL);
                    conn_close(c);
                    return;
                }
                const char *cl = find_header(c->req.headers, c->req.n_headers, "Content-Length");
                if (cl) {
                    size_t length = 0;
                    if (!*cl) { reject_input(c, 400, "invalid Content-Length"); return; }
                    for (const char *p = cl; *p; p++) {
                        if (*p < '0' || *p > '9' || length > (SIZE_MAX - (size_t)(*p - '0')) / 10) {
                            reject_input(c, 400, "invalid Content-Length"); return;
                        }
                        length = length * 10 + (size_t)(*p - '0');
                    }
                    c->content_length = length;
                }
                if (c->content_length > request_body_limit(&c->req)) {
                    reject_input(c, 413, "request body exceeds route size limit"); return;
                }
                /* Authenticate large input before allocating its body. The
                 * existing middleware only reads headers/path; normal requests
                 * still take the original dispatch-time authentication path. */
                if (c->content_length > MT_HTTPD_MAX_BODY_BYTES && c->server->middleware) {
                    mt_http_res_t auth = {0};
                    if (!c->server->middleware(&c->req, &auth, c->server->middleware_ud)) {
                        c->close_after_write = true;
                        build_response_bytes(c, &auth); free(auth.body); start_write(c); return;
                    }
                    free(auth.body);
                }
                if (c->content_length > MT_HTTPD_MAX_INFLIGHT_BODY_BYTES - c->server->reserved_body_bytes) {
                    reject_input(c, 503, "request body budget exhausted; retry later"); return;
                }
                c->reserved_body_bytes = c->content_length;
                c->server->reserved_body_bytes += c->reserved_body_bytes;
            } else if (c->rbuf_len >= MT_HTTPD_MAX_HEADER_BYTES) {
                static const char resp[] =
                    "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: "
                    "0\r\nConnection: close\r\n\r\n";
                send(fd, resp, strlen(resp), MSG_NOSIGNAL);
                conn_close(c);
                return;
            }
        }

        if (c->headers_done && c->rbuf_len >= c->header_end + c->content_length) {
            handle_complete_request(c);
            return;
        }
    }
}

static mt_http_conn_t *conn_new(mt_httpd_t *h, int fd) {
    mt_http_conn_t *c = calloc(1, sizeof(*c));
    if (!c) { return NULL; }
    c->server = h;
    c->fd = fd;
    return c;
}

static void on_accept(mt_loop_t *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    mt_httpd_t *h = ud;
    if (events & (EPOLLERR | EPOLLHUP)) { return; }
    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { return; }
            if (errno == EINTR) { continue; }
            return;
        }
        int flags = fcntl(cfd, F_GETFL, 0);
        if (flags < 0 || fcntl(cfd, F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(cfd, F_SETFD, FD_CLOEXEC) < 0) {
            close(cfd);
            continue;
        }
        if (h->n_conns >= MT_HTTPD_MAX_CONNS) {
            close(cfd); /* backpressure: no queue, matches DNS proxy's semaphore model */
            continue;
        }
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        mt_http_conn_t *c = conn_new(h, cfd);
        if (!c) {
            close(cfd);
            continue;
        }
        if (mt_loop_add_fd(loop, cfd, EPOLLIN, on_conn_readable, c) != MT_OK) {
            close(cfd);
            free(c);
            continue;
        }
        h->n_conns++;
        c->next = h->conns;
        h->conns = c;
        reset_idle_timer(c);
    }
}

/* ---- server lifecycle -------------------------------------------------------- */

mt_err_t mt_httpd_create(mt_loop_t *loop, mt_httpd_t **out) {
    mt_httpd_t *h = calloc(1, sizeof(*h));
    if (!h) { return MT_ERR_NOMEM; }
    h->loop = loop;
    h->tcp_fd = -1;
    h->unix_fd = -1;
    *out = h;
    return MT_OK;
}

void mt_httpd_destroy(mt_httpd_t *h) {
    if (!h) { return; }
    while (h->conns != NULL) {
        mt_http_conn_t *c = h->conns;
        h->conns = c->next;
        h->n_conns--;
        conn_destroy(c);
    }
    if (h->tcp_fd >= 0) {
        mt_loop_del_fd(h->loop, h->tcp_fd);
        close(h->tcp_fd);
    }
    if (h->unix_fd >= 0) {
        mt_loop_del_fd(h->loop, h->unix_fd);
        close(h->unix_fd);
        unlink(h->unix_path);
    }
    free(h);
}

mt_err_t mt_httpd_listen_tcp(mt_httpd_t *h, const char *addr, uint16_t port) {
    struct sockaddr_storage sa;
    memset(&sa, 0, sizeof(sa));
    socklen_t sa_len;
    int fam;

    char buf[128];
    snprintf(buf, sizeof(buf), "%s", addr != NULL ? addr : "::");
    char *hostpart = buf;
    size_t n = strlen(hostpart);
    if (n >= 2 && hostpart[0] == '[' && hostpart[n - 1] == ']') {
        hostpart[n - 1] = '\0';
        hostpart++;
    }

    if (strchr(hostpart, ':') != NULL || hostpart[0] == '\0') {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&sa;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(port);
        if (hostpart[0] == '\0') {
            s6->sin6_addr = in6addr_any;
        } else if (inet_pton(AF_INET6, hostpart, &s6->sin6_addr) != 1) {
            return MT_ERR_INVAL;
        }
        fam = AF_INET6;
        sa_len = sizeof(*s6);
    } else {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&sa;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(port);
        if (inet_pton(AF_INET, hostpart, &s4->sin_addr) != 1) { return MT_ERR_INVAL; }
        fam = AF_INET;
        sa_len = sizeof(*s4);
    }

    int fd = socket(fam, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { return mt_err_from_errno(errno); }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&sa, sa_len) != 0) {
        mt_err_t e = mt_err_from_errno(errno);
        close(fd);
        return e;
    }
    if (listen(fd, 128) != 0) {
        mt_err_t e = mt_err_from_errno(errno);
        close(fd);
        return e;
    }
    mt_err_t err = mt_loop_add_fd(h->loop, fd, EPOLLIN, on_accept, h);
    if (err != MT_OK) {
        close(fd);
        return err;
    }
    h->tcp_fd = fd;
    return MT_OK;
}

mt_err_t mt_httpd_listen_unix(mt_httpd_t *h, const char *path) {
    if (unlink(path) != 0 && errno != ENOENT) { return mt_err_from_errno(errno); }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) { return MT_ERR_INVAL; }
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { return mt_err_from_errno(errno); }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        mt_err_t e = mt_err_from_errno(errno);
        close(fd);
        return e;
    }
    if (listen(fd, 128) != 0) {
        mt_err_t e = mt_err_from_errno(errno);
        close(fd);
        return e;
    }
    mt_err_t err = mt_loop_add_fd(h->loop, fd, EPOLLIN, on_accept, h);
    if (err != MT_OK) {
        close(fd);
        return err;
    }
    h->unix_fd = fd;
    snprintf(h->unix_path, sizeof(h->unix_path), "%s", path);
    return MT_OK;
}
