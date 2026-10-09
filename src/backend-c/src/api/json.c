/* See json.h. */
#include "magitrickle/json.h"

#include <math.h>
#include <string.h>

#include "magitrickle/models.h"

cJSON *mt_json_error(const char *msg) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) { return NULL; }
    if (!cJSON_AddStringToObject(obj, "error", msg)) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

char *mt_json_dump(const cJSON *obj) {
    return cJSON_PrintUnformatted(obj);
}

mt_err_t mt_json_parse_priority(const cJSON *obj, uint16_t *priority) {
    const cJSON *value = NULL;
    const cJSON *item;
    cJSON_ArrayForEach(item, obj) {
        if (!item->string || strcmp(item->string, "priority") != 0) { continue; }
        if (value) { return MT_ERR_INVAL; }
        value = item;
    }
    if (!value) { return MT_OK; }
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < MT_PRIORITY_MIN || value->valuedouble > MT_PRIORITY_MAX) {
        return MT_ERR_INVAL;
    }
    /* Cast only after the range check, avoiding undefined conversion of an
     * overflowing double. JSON integral numbers (including 3e2) are valid. */
    uint16_t parsed = (uint16_t)value->valuedouble;
    if ((double)parsed != value->valuedouble) { return MT_ERR_INVAL; }
    *priority = parsed;
    return MT_OK;
}
