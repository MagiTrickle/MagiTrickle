/* libyaml emit spike: reproduce the byte shape of the Go backend's
 * SaveConfig output (go.yaml.in/yaml/v2) for a representative config.
 * Compared against the fixture produced by gen_fixture_go (same data).
 * Goal: verify block style, indentation (2 spaces), sequence dash style,
 * quoting decisions, and where shims are needed (durations, quoted strings).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

static void die(const char *msg)
{
    fprintf(stderr, "yaml_emit: %s\n", msg);
    exit(1);
}

static void emit(yaml_emitter_t *e, yaml_event_t *ev)
{
    if (!yaml_emitter_emit(e, ev)) {
        die("emit failed");
    }
}

static void scalar(yaml_emitter_t *e, const char *value,
                   yaml_scalar_style_t style)
{
    yaml_event_t ev;
    if (!yaml_scalar_event_initialize(&ev, NULL, NULL,
                                      (yaml_char_t *)value,
                                      (int)strlen(value), 1, 1, style)) {
        die("scalar init");
    }
    emit(e, &ev);
}

static void plain(yaml_emitter_t *e, const char *v)
{
    scalar(e, v, YAML_PLAIN_SCALAR_STYLE);
}

static void map_start(yaml_emitter_t *e)
{
    yaml_event_t ev;
    yaml_mapping_start_event_initialize(&ev, NULL, NULL, 1,
                                        YAML_BLOCK_MAPPING_STYLE);
    emit(e, &ev);
}

static void map_end(yaml_emitter_t *e)
{
    yaml_event_t ev;
    yaml_mapping_end_event_initialize(&ev);
    emit(e, &ev);
}

static void seq_start(yaml_emitter_t *e, yaml_sequence_style_t style)
{
    yaml_event_t ev;
    yaml_sequence_start_event_initialize(&ev, NULL, NULL, 1, style);
    emit(e, &ev);
}

static void seq_end(yaml_emitter_t *e)
{
    yaml_event_t ev;
    yaml_sequence_end_event_initialize(&ev);
    emit(e, &ev);
}

int main(void)
{
    yaml_emitter_t e;
    if (!yaml_emitter_initialize(&e)) {
        die("init");
    }
    yaml_emitter_set_output_file(&e, stdout);
    yaml_emitter_set_indent(&e, 2);
    yaml_emitter_set_width(&e, -1); /* yaml.v2 does not wrap lines */

    yaml_event_t ev;
    yaml_stream_start_event_initialize(&ev, YAML_UTF8_ENCODING);
    emit(&e, &ev);
    yaml_document_start_event_initialize(&ev, NULL, NULL, NULL, 1);
    emit(&e, &ev);

    map_start(&e);
    plain(&e, "configVersion");
    plain(&e, "0.99.0");

    plain(&e, "app");
    map_start(&e);
    {
        plain(&e, "httpWeb");
        map_start(&e);
        plain(&e, "enabled");
        plain(&e, "true");
        plain(&e, "auth");
        map_start(&e);
        plain(&e, "enabled");
        plain(&e, "false");
        map_end(&e);
        plain(&e, "host");
        map_start(&e);
        plain(&e, "address");
        /* yaml.v2 single-quotes strings starting with [ */
        scalar(&e, "[::]", YAML_SINGLE_QUOTED_SCALAR_STYLE);
        plain(&e, "port");
        plain(&e, "8080");
        map_end(&e);
        plain(&e, "skin");
        plain(&e, "default");
        map_end(&e);

        plain(&e, "dnsProxy");
        map_start(&e);
        plain(&e, "host");
        map_start(&e);
        plain(&e, "address");
        scalar(&e, "[::]", YAML_SINGLE_QUOTED_SCALAR_STYLE);
        plain(&e, "port");
        plain(&e, "3553");
        map_end(&e);
        plain(&e, "upstream");
        map_start(&e);
        plain(&e, "address");
        plain(&e, "127.0.0.1");
        plain(&e, "port");
        plain(&e, "53");
        map_end(&e);
        plain(&e, "disableRemap53");
        plain(&e, "false");
        plain(&e, "disableFakePTR");
        plain(&e, "false");
        plain(&e, "disableDropAAAA");
        plain(&e, "false");
        plain(&e, "maxIdleConns");
        plain(&e, "10");
        plain(&e, "maxConcurrent");
        plain(&e, "100");
        plain(&e, "timeout");
        plain(&e, "5s"); /* duration shim: emit Go duration string */
        map_end(&e);

        plain(&e, "netfilter");
        map_start(&e);
        plain(&e, "iptables");
        map_start(&e);
        plain(&e, "chainPrefix");
        plain(&e, "MT_");
        map_end(&e);
        plain(&e, "ipset");
        map_start(&e);
        plain(&e, "tablePrefix");
        plain(&e, "mt_");
        plain(&e, "additionalTTL");
        plain(&e, "1h0m0s");
        map_end(&e);
        plain(&e, "disableIPv4");
        plain(&e, "false");
        plain(&e, "disableIPv6");
        plain(&e, "false");
        plain(&e, "startMarkTableIndex");
        plain(&e, "1298229097");
        map_end(&e);

        plain(&e, "link");
        seq_start(&e, YAML_BLOCK_SEQUENCE_STYLE);
        plain(&e, "br0");
        seq_end(&e);
        plain(&e, "showAllInterfaces");
        plain(&e, "false");
        plain(&e, "logLevel");
        plain(&e, "info");
    }
    map_end(&e);

    plain(&e, "groups");
    seq_start(&e, YAML_BLOCK_SEQUENCE_STYLE);
    map_start(&e);
    plain(&e, "id");
    plain(&e, "d663876a");
    plain(&e, "name");
    plain(&e, "Example");
    plain(&e, "color");
    scalar(&e, "#ffffff", YAML_SINGLE_QUOTED_SCALAR_STYLE);
    plain(&e, "interface");
    plain(&e, "nwg0");
    plain(&e, "enable");
    plain(&e, "false");
    plain(&e, "rules");
    seq_start(&e, YAML_BLOCK_SEQUENCE_STYLE);
    map_start(&e);
    plain(&e, "id");
    plain(&e, "6f34ee91");
    plain(&e, "name");
    plain(&e, "Wildcard Example");
    plain(&e, "type");
    plain(&e, "wildcard");
    plain(&e, "rule");
    scalar(&e, "*wildcard.example.com", YAML_SINGLE_QUOTED_SCALAR_STYLE);
    plain(&e, "enable");
    plain(&e, "true");
    map_end(&e);
    seq_end(&e);
    map_end(&e);
    seq_end(&e);

    plain(&e, "subscriptions");
    seq_start(&e, YAML_FLOW_SEQUENCE_STYLE); /* empty list -> [] */
    seq_end(&e);

    map_end(&e);

    yaml_document_end_event_initialize(&ev, 1);
    emit(&e, &ev);
    yaml_stream_end_event_initialize(&ev);
    emit(&e, &ev);
    yaml_emitter_delete(&e);
    return 0;
}
