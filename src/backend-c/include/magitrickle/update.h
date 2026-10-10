/* Browser-discovered GitHub releases; the independent installer revalidates
 * everything. No release polling or network I/O runs in the daemon loop. */
#ifndef MAGITRICKLE_UPDATE_H
#define MAGITRICKLE_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MT_UPDATE_REPO "dan0102dan/mt-c"
#define MT_UPDATE_API "https://api.github.com/repos/" MT_UPDATE_REPO "/releases"
#define MT_UPDATE_WEB "https://github.com/" MT_UPDATE_REPO "/releases"
#define MT_UPDATE_JSON_LIMIT (4U * 1024U * 1024U)
#define MT_UPDATE_PACKAGE_LIMIT (64U * 1024U * 1024U)
#ifndef MT_BUILD_TARGET
#define MT_BUILD_TARGET ""
#endif
#ifndef MT_PACKAGE_REVISION
#define MT_PACKAGE_REVISION 1
#endif

typedef struct mt_update_version {
    uint32_t parts[4];
    uint32_t revision;
    bool development;
    char base[64];
} mt_update_version_t;

typedef struct mt_update_asset {
    char tag[80];
    char version[64];
    uint32_t revision;
    char name[256];
    char url[1024];
    char sha256[65];
    size_t size;
} mt_update_asset_t;

bool mt_update_version_parse(const char *text, uint32_t revision, mt_update_version_t *out);
int mt_update_version_compare(const mt_update_version_t *a, const mt_update_version_t *b);
bool mt_update_download_host_allowed(const char *url);
/* suffix is compiled target + validated local OS identity, never browser input.
 * Returns a stable error key, or NULL on success. */
const char *mt_update_resolve(const char *json, size_t len, bool preview,
                             const char *tag, uint64_t release_id, const char *suffix,
                             const char *installed, uint32_t installed_revision,
                             mt_update_asset_t *out);

struct mt_httpd;
struct mt_system_ctx;
void mt_update_register_routes(struct mt_httpd *http, struct mt_system_ctx *ctx);
/* Only called by the separately packaged mt-c-updater, with inherited lock fd 3. */
int mt_update_worker(void);
#endif
