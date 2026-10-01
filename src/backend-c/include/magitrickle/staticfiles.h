/* Skin static file serving — port of http.go's wildcard GET-fallback
 * route (compatibility-contract.md §2's static-serving row). Wired up
 * as the TCP mt_httpd_t instance's not-found handler (mt_httpd_set_not_found)
 * so it only ever runs for requests that didn't match an /api/v1/...
 * route; never registered on the Unix socket instance, matching Go
 * (unixsocket.go mounts only the v1 API router).
 *
 * mt_http_req_path() is already Go-path.Clean-equivalent (httpd.c's
 * path_clean, added alongside this module) before it ever reaches here,
 * so joining it onto skins_dir/skin cannot escape that root via "..".
 */
#ifndef MAGITRICKLE_STATICFILES_H
#define MAGITRICKLE_STATICFILES_H

#include "magitrickle/httpd.h"

/* Returns the currently configured skin name (e.g. cfg->app.http_web.skin);
 * read fresh on every request, matching Go's a.Config().HTTPWeb.Skin. */
typedef const char *(*mt_static_skin_fn)(void *ud);

typedef struct mt_static_ctx {
    /* e.g. MT_APP_SHARE_DIR "/skins" -- borrowed, must outlive the server. */
    const char *skins_dir;
    mt_static_skin_fn skin_name;
    void *ud;
} mt_static_ctx_t;

/* Serves <skins_dir>/<skin_name()>/<path>, retrying once against
 * "<path>/index.html" when <path> is a directory (matches Go's
 * up-to-2-stat loop exactly, including the pathological case in the
 * comments of staticfiles.c). Non-GET requests get a plain 404 (Go relies
 * on chi's method-specific route only ever binding GET here; other
 * methods to arbitrary paths fall through to chi's default 404, which
 * this mirrors at the status-code level, see decisions.md). Missing file
 * -> 404 JSON error, except "/" itself -> 404 with an HTML placeholder
 * page (matches Go's noSkinFoundPlaceholder). ud must be a
 * `mt_static_ctx_t *`. */
void mt_static_handler(mt_http_req_t *req, mt_http_res_t *res, void *ud);

#endif /* MAGITRICKLE_STATICFILES_H */
