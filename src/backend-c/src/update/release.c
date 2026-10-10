#include "magitrickle/update.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

static bool number(const char **text, uint32_t *out) {
    const char *p = *text;
    uint32_t n = 0;
    size_t digits = 0;
    while (*p >= '0' && *p <= '9') {
        if (++digits > 9) { return false; }
        n = n * 10U + (uint32_t)(*p++ - '0');
    }
    if (!digits) { return false; }
    *text = p;
    *out = n;
    return true;
}

bool mt_update_version_parse(const char *text, uint32_t revision, mt_update_version_t *out) {
    if (!text || !out || !revision || strlen(text) >= 128) { return false; }
    memset(out, 0, sizeof(*out));
    const char *p = text;
    if (*p == 'v') { p++; }
    unsigned count = 0;
    do {
        if (count == 4 || !number(&p, &out->parts[count++])) { return false; }
        if (*p != '.') { break; }
        p++;
    } while (true);
    if (count < 2 || (size_t)(p - text) >= sizeof(out->base)) { return false; }
    memcpy(out->base, text, (size_t)(p - text));
    out->revision = revision;
    if (!*p) { return true; }
    if (strncmp(p, "-rev", 4) == 0) {
        p += 4;
        return number(&p, &out->revision) && out->revision > 0 && !*p;
    }
    /* Build-system snapshots sort before the release with the same core.
     * Unknown formats fail closed rather than accidentally downgrading. */
    /* OpenWrt's APK SDK puts the package-safe _pre timestamp in MT_VERSION.
     * Accept that installed identity too, but never as an update target. */
    bool apk_snapshot = strncmp(p, "_pre", 4) == 0;
    if (!apk_snapshot && strncmp(p, "~git", 4) != 0) { return false; }
    p += 4;
    for (unsigned i = 0; i < 14; i++) {
        if (*p < '0' || *p > '9') { return false; }
        p++;
    }
    if (apk_snapshot) {
        if (*p) { return false; }
        out->development = true;
        return true;
    }
    if (*p++ != '.') { return false; }
    unsigned digits = 0;
    while (isxdigit((unsigned char)*p)) { p++; digits++; }
    if (*p || digits < 7 || digits > 40) { return false; }
    out->development = true;
    return true;
}

int mt_update_version_compare(const mt_update_version_t *a, const mt_update_version_t *b) {
    for (size_t i = 0; i < 4; i++) {
        if (a->parts[i] != b->parts[i]) { return a->parts[i] > b->parts[i] ? 1 : -1; }
    }
    if (a->development != b->development) { return a->development ? -1 : 1; }
    return (a->revision > b->revision) - (a->revision < b->revision);
}

bool mt_update_download_host_allowed(const char *url) {
    static const char *const prefixes[] = {
        "https://github.com/", "https://release-assets.githubusercontent.com/",
        "https://objects.githubusercontent.com/"
    };
    if (!url || strchr(url, '\r') || strchr(url, '\n') || strchr(url, '#')) { return false; }
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (strncmp(url, prefixes[i], strlen(prefixes[i])) == 0) { return true; }
    }
    return false;
}

static const char *string(const cJSON *object, const char *name) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static bool published(const cJSON *release) {
    const cJSON *draft = cJSON_GetObjectItemCaseSensitive(release, "draft");
    const cJSON *pre = cJSON_GetObjectItemCaseSensitive(release, "prerelease");
    return cJSON_IsObject(release) && cJSON_IsFalse(draft) && cJSON_IsBool(pre) &&
           strlen(string(release, "published_at")) == 20;
}

const char *mt_update_resolve(const char *json, size_t len, bool preview,
                             const char *tag, uint64_t release_id, const char *suffix,
                             const char *installed, uint32_t installed_revision,
                             mt_update_asset_t *out) {
    mt_update_version_t current, target;
    if (!out || !tag || !suffix || !mt_update_version_parse(installed, installed_revision, &current)) {
        return "Unknown installed version";
    }
    if (!mt_update_version_parse(tag, 1, &target) || target.development) { return "Invalid release tag"; }
    if (mt_update_version_compare(&target, &current) <= 0) { return "No newer version"; }
    if (!json || !len || len > MT_UPDATE_JSON_LIMIT || memchr(json, '\0', len)) { return "Invalid release metadata"; }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) { return "Invalid release metadata"; }
    const char *error = "Release changed; check again";
    const cJSON *release = NULL;
    /* Same bounded list and published_at policy as the browser, for BOTH
     * channels. /latest's designation is not necessarily this selection. */
    if (cJSON_IsArray(root)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, root) {
            if (published(item) && (preview || cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(item, "prerelease"))) &&
                (!release || strcmp(string(item, "published_at"), string(release, "published_at")) > 0)) {
                release = item;
            }
        }
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(release, "id");
    if (!release || strcmp(string(release, "tag_name"), tag) || !cJSON_IsNumber(id) ||
        release_id == 0 || release_id > UINT64_C(9007199254740991) || id->valuedouble != (double)release_id) { goto done; }
    memset(out, 0, sizeof(*out));
    snprintf(out->tag, sizeof(out->tag), "%s", tag);
    snprintf(out->version, sizeof(out->version), "%s", target.base);
    out->revision = target.revision;
    char expected[256], alternate[256];
    int n = snprintf(expected, sizeof(expected), "mt-c_%s-%u_%s", target.base, target.revision, suffix);
    if (n < 0 || (size_t)n >= sizeof(expected)) { error = "Unsupported build target"; goto done; }
    snprintf(alternate, sizeof(alternate), "mt-c_%s-r%u_%s", target.base, target.revision, suffix);
    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(release, "assets");
    const cJSON *asset, *selected = NULL;
    error = "No compatible package";
    if (!cJSON_IsArray(assets)) { goto done; }
    cJSON_ArrayForEach(asset, assets) {
        const char *name = string(asset, "name");
        bool apk = strlen(suffix) > 4 && strcmp(suffix + strlen(suffix) - 4, ".apk") == 0;
        if (strcmp(name, expected) && (!apk || strcmp(name, alternate))) { continue; }
        if (selected) { error = "Ambiguous package assets"; goto done; }
        selected = asset;
    }
    if (!selected) { goto done; }
    error = "Package verification metadata missing";
    const char *digest = string(selected, "digest");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(selected, "size");
    if (strcmp(string(selected, "state"), "uploaded") || strlen(digest) != 71 || strncmp(digest, "sha256:", 7) ||
        !cJSON_IsNumber(size) || size->valuedouble < 1 || size->valuedouble > MT_UPDATE_PACKAGE_LIMIT) { goto done; }
    for (size_t i = 7; i < 71; i++) {
        if (!(digest[i] >= '0' && digest[i] <= '9') && !(digest[i] >= 'a' && digest[i] <= 'f')) { goto done; }
    }
    out->size = (size_t)size->valuedouble;
    if ((double)out->size != size->valuedouble) { goto done; }
    snprintf(out->name, sizeof(out->name), "%s", string(selected, "name"));
    snprintf(out->sha256, sizeof(out->sha256), "%s", digest + 7);
    snprintf(out->url, sizeof(out->url), MT_UPDATE_WEB "/download/%s/%s", tag, out->name);
    error = "Invalid package URL";
    if (strcmp(out->url, string(selected, "browser_download_url"))) { goto done; }
    error = NULL;
done:
    cJSON_Delete(root);
    return error;
}
