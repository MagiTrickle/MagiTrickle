/* Internal: yaml.v2 scalar resolution (resolve.go port) shared by the
 * loader (typing) and the saver (quoting decisions). */
#ifndef MT_YAML_SCALAR_H
#define MT_YAML_SCALAR_H

#include <stdbool.h>
#include <stdint.h>

typedef enum mt_scalar_kind {
    MT_SCALAR_STR = 0,
    MT_SCALAR_NULL,
    MT_SCALAR_BOOL,
    MT_SCALAR_INT,   /* fits int64 or uint64 */
    MT_SCALAR_FLOAT,
    MT_SCALAR_TIMESTAMP,
    MT_SCALAR_MERGE, /* "<<" */
} mt_scalar_kind_t;

typedef struct mt_scalar_value {
    mt_scalar_kind_t kind;
    bool b;          /* BOOL */
    bool is_uint;    /* INT: value only fits uint64 */
    int64_t i;       /* INT (signed) */
    uint64_t u;      /* INT (unsigned, when is_uint) */
    double f;        /* FLOAT */
} mt_scalar_value_t;

/* Resolve a PLAIN scalar exactly like yaml.v2 resolve("", s). */
mt_scalar_value_t mt_yaml_resolve_plain(const char *s);

/* encode.go isBase60Float port. */
bool mt_yaml_is_base60_float(const char *s);

/* True when a string value must be double-quoted on emit to stay a string
 * (resolves to a non-string type or is a base-60 float). */
bool mt_yaml_string_needs_quote(const char *s);

#endif /* MT_YAML_SCALAR_H */
