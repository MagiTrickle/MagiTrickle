/* Config serializer: byte-identical to Go SaveConfig (yaml.v2 emitter).
 * Key order = Go struct order; scalar styles per encode.go stringv
 * (plain / double-quoted for would-be-non-strings / literal for \n) with
 * libyaml handling structurally-unsafe plain scalars via single quotes;
 * empty sequences in flow style ([]); durations as Go strings. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <yaml.h>

#include "magitrickle/yamlio.h"
#include "yaml_scalar.h"

typedef struct emit_buf {
    char *data;
    size_t len;
    size_t cap;
} emit_buf_t;

static int emit_buf_write(void *ext, unsigned char *buffer, size_t size)
{
    emit_buf_t *b = ext;
    if (b->len + size + 1 > b->cap) {
        size_t cap = b->cap == 0 ? 4096 : b->cap;
        while (b->len + size + 1 > cap) {
            cap *= 2;
        }
        char *grown = realloc(b->data, cap);
        if (grown == NULL) {
            return 0;
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, buffer, size);
    b->len += size;
    b->data[b->len] = '\0';
    return 1;
}

typedef struct emitter_ctx {
    yaml_emitter_t emitter;
    bool failed;
} emitter_ctx_t;

static void emit_event(emitter_ctx_t *ctx, yaml_event_t *ev)
{
    if (ctx->failed) {
        yaml_event_delete(ev);
        return;
    }
    if (!yaml_emitter_emit(&ctx->emitter, ev)) {
        ctx->failed = true;
    }
}

static void emit_scalar_styled(emitter_ctx_t *ctx, const char *value,
                               yaml_scalar_style_t style)
{
    yaml_event_t ev;
    if (!yaml_scalar_event_initialize(&ev, NULL, NULL,
                                      (yaml_char_t *)value,
                                      (int)strlen(value), 1, 1, style)) {
        ctx->failed = true;
        return;
    }
    emit_event(ctx, &ev);
}

/* String value with yaml.v2 stringv style selection. */
static void emit_string(emitter_ctx_t *ctx, const char *value)
{
    yaml_scalar_style_t style = YAML_PLAIN_SCALAR_STYLE;
    if (strchr(value, '\n') != NULL) {
        style = YAML_LITERAL_SCALAR_STYLE;
    } else if (mt_yaml_string_needs_quote(value)) {
        style = YAML_DOUBLE_QUOTED_SCALAR_STYLE;
    }
    emit_scalar_styled(ctx, value, style);
}

static void emit_plain(emitter_ctx_t *ctx, const char *value)
{
    emit_scalar_styled(ctx, value, YAML_PLAIN_SCALAR_STYLE);
}

static void emit_bool(emitter_ctx_t *ctx, bool v)
{
    emit_plain(ctx, v ? "true" : "false");
}

static void emit_u64(emitter_ctx_t *ctx, uint64_t v)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%" PRIu64, v);
    emit_plain(ctx, buf);
}

static void emit_duration(emitter_ctx_t *ctx, mt_duration_t d)
{
    char buf[40];
    mt_duration_format(d, buf, sizeof(buf));
    /* Go duration strings never resolve as numbers/bools -> plain */
    emit_plain(ctx, buf);
}

static void emit_id(emitter_ctx_t *ctx, mt_id_t id)
{
    char buf[MT_ID_STR_LEN];
    mt_id_format(id, buf);
    emit_string(ctx, buf); /* "12345678"/"666e0000" must be quoted */
}

static void map_start(emitter_ctx_t *ctx)
{
    yaml_event_t ev;
    if (!yaml_mapping_start_event_initialize(&ev, NULL, NULL, 1,
                                             YAML_BLOCK_MAPPING_STYLE)) {
        ctx->failed = true;
        return;
    }
    emit_event(ctx, &ev);
}

static void map_end(emitter_ctx_t *ctx)
{
    yaml_event_t ev;
    if (!yaml_mapping_end_event_initialize(&ev)) {
        ctx->failed = true;
        return;
    }
    emit_event(ctx, &ev);
}

static void seq_start(emitter_ctx_t *ctx, bool empty)
{
    yaml_event_t ev;
    if (!yaml_sequence_start_event_initialize(
            &ev, NULL, NULL, 1,
            empty ? YAML_FLOW_SEQUENCE_STYLE : YAML_BLOCK_SEQUENCE_STYLE)) {
        ctx->failed = true;
        return;
    }
    emit_event(ctx, &ev);
}

static void seq_end(emitter_ctx_t *ctx)
{
    yaml_event_t ev;
    if (!yaml_sequence_end_event_initialize(&ev)) {
        ctx->failed = true;
        return;
    }
    emit_event(ctx, &ev);
}

static void emit_rule(emitter_ctx_t *ctx, const mt_rule_t *r)
{
    map_start(ctx);
    emit_plain(ctx, "id");
    emit_id(ctx, r->id);
    emit_plain(ctx, "name");
    emit_string(ctx, r->name != NULL ? r->name : "");
    emit_plain(ctx, "type");
    emit_string(ctx, r->type != NULL ? r->type : "");
    emit_plain(ctx, "rule");
    emit_string(ctx, r->rule != NULL ? r->rule : "");
    emit_plain(ctx, "enable");
    emit_bool(ctx, r->enable);
    map_end(ctx);
}

static void emit_group(emitter_ctx_t *ctx, const mt_group_t *g)
{
    map_start(ctx);
    emit_plain(ctx, "id");
    emit_id(ctx, g->id);
    emit_plain(ctx, "name");
    emit_string(ctx, g->name != NULL ? g->name : "");
    emit_plain(ctx, "color");
    emit_string(ctx, g->color != NULL ? g->color : "");
    emit_plain(ctx, "interface");
    emit_string(ctx, g->iface != NULL ? g->iface : "");
    emit_plain(ctx, "enable");
    emit_bool(ctx, g->enable);
    emit_plain(ctx, "rules");
    seq_start(ctx, g->n_rules == 0);
    for (size_t i = 0; i < g->n_rules; i++) {
        emit_rule(ctx, g->rules[i]);
    }
    seq_end(ctx);
    map_end(ctx);
}

static void emit_sub_rule(emitter_ctx_t *ctx, const mt_sub_rule_t *r)
{
    map_start(ctx);
    emit_plain(ctx, "id");
    emit_id(ctx, r->id);
    emit_plain(ctx, "rule");
    emit_string(ctx, r->rule != NULL ? r->rule : "");
    emit_plain(ctx, "type");
    emit_string(ctx, r->type != NULL ? r->type : "");
    emit_plain(ctx, "enable");
    emit_bool(ctx, r->enable);
    map_end(ctx);
}

static void emit_subscription(emitter_ctx_t *ctx, const mt_subscription_t *s)
{
    map_start(ctx);
    emit_plain(ctx, "id");
    emit_id(ctx, s->id);
    emit_plain(ctx, "name");
    emit_string(ctx, s->name != NULL ? s->name : "");
    emit_plain(ctx, "interface");
    emit_string(ctx, s->iface != NULL ? s->iface : "");
    emit_plain(ctx, "enable");
    emit_bool(ctx, s->enable);
    emit_plain(ctx, "url");
    emit_string(ctx, s->url != NULL ? s->url : "");
    emit_plain(ctx, "interval");
    emit_u64(ctx, s->interval);
    emit_plain(ctx, "last_update");
    emit_u64(ctx, s->last_update);
    emit_plain(ctx, "rules");
    seq_start(ctx, s->n_rules == 0);
    for (size_t i = 0; i < s->n_rules; i++) {
        emit_sub_rule(ctx, s->rules[i]);
    }
    seq_end(ctx);
    map_end(ctx);
}

mt_err_t mt_config_save_buffer(const mt_config_t *cfg, const char *version,
                               char **out, size_t *out_len)
{
    emitter_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    emit_buf_t buf = {NULL, 0, 0};

    if (!yaml_emitter_initialize(&ctx.emitter)) {
        return MT_ERR_NOMEM;
    }
    yaml_emitter_set_output(&ctx.emitter, emit_buf_write, &buf);
    yaml_emitter_set_indent(&ctx.emitter, 2);
    yaml_emitter_set_width(&ctx.emitter, -1);
    yaml_emitter_set_unicode(&ctx.emitter, 1);

    yaml_event_t ev;
    if (!yaml_stream_start_event_initialize(&ev, YAML_UTF8_ENCODING)) {
        ctx.failed = true;
    } else {
        emit_event(&ctx, &ev);
    }
    if (!yaml_document_start_event_initialize(&ev, NULL, NULL, NULL, 1)) {
        ctx.failed = true;
    } else {
        emit_event(&ctx, &ev);
    }

    const mt_app_config_t *a = &cfg->app;

    map_start(&ctx);
    emit_plain(&ctx, "configVersion");
    emit_string(&ctx, version);

    emit_plain(&ctx, "app");
    map_start(&ctx);

    emit_plain(&ctx, "httpWeb");
    map_start(&ctx);
    emit_plain(&ctx, "enabled");
    emit_bool(&ctx, a->http_web.enabled);
    emit_plain(&ctx, "auth");
    map_start(&ctx);
    emit_plain(&ctx, "enabled");
    emit_bool(&ctx, a->http_web.auth.enabled);
    map_end(&ctx);
    emit_plain(&ctx, "host");
    map_start(&ctx);
    emit_plain(&ctx, "address");
    emit_string(&ctx, a->http_web.host.address);
    emit_plain(&ctx, "port");
    emit_u64(&ctx, a->http_web.host.port);
    map_end(&ctx);
    emit_plain(&ctx, "skin");
    emit_string(&ctx, a->http_web.skin);
    map_end(&ctx);

    emit_plain(&ctx, "dnsProxy");
    map_start(&ctx);
    emit_plain(&ctx, "host");
    map_start(&ctx);
    emit_plain(&ctx, "address");
    emit_string(&ctx, a->dns_proxy.host.address);
    emit_plain(&ctx, "port");
    emit_u64(&ctx, a->dns_proxy.host.port);
    map_end(&ctx);
    emit_plain(&ctx, "upstream");
    map_start(&ctx);
    emit_plain(&ctx, "address");
    emit_string(&ctx, a->dns_proxy.upstream.address);
    emit_plain(&ctx, "port");
    emit_u64(&ctx, a->dns_proxy.upstream.port);
    map_end(&ctx);
    emit_plain(&ctx, "disableRemap53");
    emit_bool(&ctx, a->dns_proxy.disable_remap53);
    emit_plain(&ctx, "disableFakePTR");
    emit_bool(&ctx, a->dns_proxy.disable_fake_ptr);
    emit_plain(&ctx, "disableDropAAAA");
    emit_bool(&ctx, a->dns_proxy.disable_drop_aaaa);
    emit_plain(&ctx, "maxIdleConns");
    emit_u64(&ctx, a->dns_proxy.max_idle_conns);
    emit_plain(&ctx, "maxConcurrent");
    emit_u64(&ctx, a->dns_proxy.max_concurrent);
    emit_plain(&ctx, "timeout");
    emit_duration(&ctx, a->dns_proxy.timeout);
    map_end(&ctx);

    emit_plain(&ctx, "netfilter");
    map_start(&ctx);
    emit_plain(&ctx, "iptables");
    map_start(&ctx);
    emit_plain(&ctx, "chainPrefix");
    emit_string(&ctx, a->netfilter.iptables.chain_prefix);
    map_end(&ctx);
    emit_plain(&ctx, "ipset");
    map_start(&ctx);
    emit_plain(&ctx, "tablePrefix");
    emit_string(&ctx, a->netfilter.ipset.table_prefix);
    emit_plain(&ctx, "additionalTTL");
    emit_duration(&ctx, a->netfilter.ipset.additional_ttl);
    map_end(&ctx);
    emit_plain(&ctx, "disableIPv4");
    emit_bool(&ctx, a->netfilter.disable_ipv4);
    emit_plain(&ctx, "disableIPv6");
    emit_bool(&ctx, a->netfilter.disable_ipv6);
    emit_plain(&ctx, "startMarkTableIndex");
    emit_u64(&ctx, a->netfilter.start_mark_table_index);
    map_end(&ctx);

    emit_plain(&ctx, "link");
    seq_start(&ctx, a->n_link == 0);
    for (size_t i = 0; i < a->n_link; i++) {
        emit_string(&ctx, a->link[i]);
    }
    seq_end(&ctx);
    emit_plain(&ctx, "showAllInterfaces");
    emit_bool(&ctx, a->show_all_interfaces);
    emit_plain(&ctx, "logLevel");
    emit_string(&ctx, a->log_level);
    map_end(&ctx); /* app */

    emit_plain(&ctx, "groups");
    seq_start(&ctx, cfg->n_groups == 0);
    for (size_t i = 0; i < cfg->n_groups; i++) {
        emit_group(&ctx, cfg->groups[i]);
    }
    seq_end(&ctx);

    emit_plain(&ctx, "subscriptions");
    seq_start(&ctx, cfg->n_subscriptions == 0);
    for (size_t i = 0; i < cfg->n_subscriptions; i++) {
        emit_subscription(&ctx, cfg->subscriptions[i]);
    }
    seq_end(&ctx);

    map_end(&ctx);

    if (!yaml_document_end_event_initialize(&ev, 1)) {
        ctx.failed = true;
    } else {
        emit_event(&ctx, &ev);
    }
    if (!yaml_stream_end_event_initialize(&ev)) {
        ctx.failed = true;
    } else {
        emit_event(&ctx, &ev);
    }
    yaml_emitter_delete(&ctx.emitter);

    if (ctx.failed) {
        free(buf.data);
        return MT_ERR_SYS;
    }
    *out = buf.data != NULL ? buf.data : calloc(1, 1);
    *out_len = buf.len;
    return *out != NULL ? MT_OK : MT_ERR_NOMEM;
}

mt_err_t mt_config_save_file(const mt_config_t *cfg, const char *version,
                             const char *path)
{
    char *data = NULL;
    size_t len = 0;
    mt_err_t err = mt_config_save_buffer(cfg, version, &data, &len);
    if (err != MT_OK) {
        return err;
    }

    /* atomic write: tmp in same dir + fsync + rename + dir fsync */
    char tmp_path[4096];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path,
                     (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        free(data);
        return MT_ERR_INVAL;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        free(data);
        return mt_err_from_errno(errno);
    }
    size_t written = 0;
    while (written < len) {
        ssize_t rc = write(fd, data + written, len - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            err = mt_err_from_errno(errno);
            close(fd);
            unlink(tmp_path);
            free(data);
            return err;
        }
        written += (size_t)rc;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        err = mt_err_from_errno(errno);
        unlink(tmp_path);
        free(data);
        return err;
    }
    free(data);

    if (rename(tmp_path, path) != 0) {
        err = mt_err_from_errno(errno);
        unlink(tmp_path);
        return err;
    }

    /* fsync the directory so the rename is durable */
    char dir_buf[4096];
    snprintf(dir_buf, sizeof(dir_buf), "%s", path);
    const char *dir = dirname(dir_buf);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
    return MT_OK;
}
