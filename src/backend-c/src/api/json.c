/* See json.h. */
#include "magitrickle/json.h"

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
