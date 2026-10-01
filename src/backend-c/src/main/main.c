/* magitrickled-c — Phase 5 daemon.
 *
 * Loads the YAML config (defaults + overlay), wires up the full netfilter
 * layer (iptables chain patches + cleanup, per-group ipset/ipset-to-link via
 * mt_ruleset_t, port 53 DNAT remap, rtnetlink link/addr watcher), then starts
 * the epoll DNS MITM proxy and processes every upstream response through the
 * records cache + rule matching pipeline (dns.go's handleMessage, ported —
 * see dnspipeline.h), dispatching matches into the matched group's ipset
 * (rule_set.go's AddIPv4Subnet/AddIPv6Subnet, ported — see ruleset.h).
 *
 * Also wires up the HTTP/Unix-socket API (Phase 6): groups/rules CRUD,
 * system endpoints (interfaces/config-save/netfilter.d hook), auth
 * (crypt+JWT), and skin static file serving, all sharing one mt_app_t
 * that now owns the per-group mt_ruleset_t registry this file used to
 * manage inline. Subscription HTTP endpoints are not yet wired (task
 * #36). The Go backend remains authoritative until the migration
 * completes (docs/c-rewrite/).
 */
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "magitrickle/app.h"
#include "magitrickle/auth.h"
#include "magitrickle/dns_cache.h"
#include "magitrickle/dnspipeline.h"
#include "magitrickle/dnsproxy.h"
#include "magitrickle/groups.h"
#include "magitrickle/httpd.h"
#include "magitrickle/iptables.h"
#include "magitrickle/log.h"
#include "magitrickle/loop.h"
#include "magitrickle/netfilter_cleaner.h"
#include "magitrickle/netlink_watcher.h"
#include "magitrickle/paths.h"
#include "magitrickle/port_remap.h"
#include "magitrickle/rtnl.h"
#include "magitrickle/rulesnap.h"
#include "magitrickle/ruleset.h"
#include "magitrickle/staticfiles.h"
#include "magitrickle/sub_fetch.h"
#include "magitrickle/subscriptions_api.h"
#include "magitrickle/system.h"
#include "magitrickle/version.h"
#include "magitrickle/yamlio.h"

/* Matches Go's cfgFileLocation = AppStateDir + "/config.yaml", so the
 * config file follows the per-platform state dir (paths.h). */
#ifndef MT_CONFIG_PATH
#define MT_CONFIG_PATH MT_APP_STATE_DIR "/config.yaml"
#endif

#define MT_CACHE_CLEANUP_INTERVAL_MS 30000
/* Matches subscriptions.go's subscriptionAutoUpdateTick = time.Minute. */
#define MT_SUBSCRIPTION_AUTO_UPDATE_INTERVAL_MS 60000

struct daemon {
    mt_loop_t *loop;
    mt_dnsproxy_t *proxy;
    mt_cache_t *cache;
    mt_dns_pipeline_t *pipeline;
    mt_ipt_t *ipt4;
    mt_ipt_t *ipt6;
    mt_rtnl_t *rtnl;
    mt_nl_watcher_t *watcher;
    mt_port_remap_t *port_remap;
    mt_app_t *app;
    mt_sub_fetcher_t *fetcher;
    mt_httpd_t *http_tcp;
    mt_httpd_t *http_unix;

    /* Not owned by daemon_teardown (cfg is main()'s own local, config_path
     * points at argv or MT_CONFIG_PATH) -- only here so on_signal's SIGHUP
     * handler can reach them for a config reload. */
    mt_config_t *cfg;
    const char *config_path;
};

static int64_t now_unix(void)
{
    return (int64_t)time(NULL);
}

/* Reverse-order teardown of everything in `struct daemon`. Every
 * destroy/free/disable below is NULL-safe and idempotent, so this can be
 * called from any partially-constructed state (d is always zero-initialized
 * at the top of main). The HTTP servers must be destroyed while the loop is
 * still alive (mt_httpd_destroy removes their fds from it). app/port_remap
 * borrow ipt4/ipt6/rtnl/cache, so they must be torn down before those; the
 * loop must outlive the watcher/proxy fds registered on it too. */
static void daemon_teardown(struct daemon *d)
{
    mt_sub_fetcher_destroy(d->fetcher);
    d->fetcher = NULL;
    /* After joining I/O workers: the committer thread reads the ruleset registry and
     * drives the iptables engines, so nothing it touches may be torn down
     * while it still runs. Stopping it also aborts a write in flight, so
     * this does not wait out a full rebuild. */
    mt_app_stop_netfilter_committer(d->app);

    mt_httpd_destroy(d->http_tcp);
    d->http_tcp = NULL;
    mt_httpd_destroy(d->http_unix);
    d->http_unix = NULL;

    mt_app_destroy(d->app);
    d->app = NULL;

    if (d->port_remap) {
        mt_port_remap_disable(d->port_remap);
        mt_port_remap_free(d->port_remap);
        d->port_remap = NULL;
    }

    mt_nl_watcher_destroy(d->watcher);
    d->watcher = NULL;
    mt_dnsproxy_destroy(d->proxy);
    d->proxy = NULL;
    mt_dns_pipeline_destroy(d->pipeline);
    d->pipeline = NULL;
    mt_cache_destroy(d->cache);
    d->cache = NULL;
    mt_ipt_free(d->ipt4);
    d->ipt4 = NULL;
    mt_ipt_free(d->ipt6);
    d->ipt6 = NULL;
    mt_rtnl_close(d->rtnl);
    d->rtnl = NULL;
    mt_loop_destroy(d->loop);
    d->loop = NULL;
}

static mt_ruleset_t *find_ruleset(struct daemon *d, mt_id_t id)
{
    mt_ruleset_t *rs = mt_app_find_group_by_id(d->app, id);
    if (rs != NULL) {
        return rs;
    }
    return mt_app_find_subscription_ruleset_by_id(d->app, id);
}

/* Reload stored state and live API settings together. DNS flags live in
 * the transport too; refresh them even on a partial group-apply failure,
 * matching Go's app-overlay-before-groups order. */
static void reload_config(struct daemon *d)
{
    mt_err_t err = mt_app_reload_config(d->app, d->config_path);
    if (d->proxy) {
        mt_dnsproxy_set_disable_flags(d->proxy,
            d->cfg->app.dns_proxy.disable_fake_ptr,
            d->cfg->app.dns_proxy.disable_drop_aaaa);
    }
    if (err == MT_ERR_NOENT) {
        MT_INFO("reload: config file %s not found, keeping current config", d->config_path);
    } else if (err != MT_OK) {
        MT_ERROR("failed to reload config %s: %s", d->config_path, mt_err_str(err));
    } else {
        MT_INFO("config reloaded from %s", d->config_path);
    }
}

static void on_signal(mt_loop_t *loop, int signo, void *ud)
{
    struct daemon *d = ud;
    if (signo == SIGHUP) {
        MT_INFO("received signal: hangup (reloading config)");
        reload_config(d);
        return;
    }
    MT_INFO("received signal: %s", strsignal(signo));
    mt_loop_stop(loop);
}

static void on_cache_cleanup_timer(mt_loop_t *loop, void *ud)
{
    (void)loop;
    struct daemon *d = ud;
    mt_cache_cleanup(d->cache, now_unix());
}

static void auto_update_done(void *ud, mt_id_t id, mt_err_t err, bool changed)
{
    (void)id;
    struct daemon *d = ud;
    if (err == MT_ERR_CANCELED || err == MT_ERR_STATE || err == MT_ERR_NOENT) { return; }
    if (err != MT_OK) { MT_ERROR("failed to sync subscription: %s", mt_err_str(err)); }
    if (changed) {
        mt_err_t serr = mt_app_save_config(d->app, d->config_path, MT_VERSION);
        if (serr != MT_OK) { MT_ERROR("failed to save config file: %s", mt_err_str(serr)); }
    }
}

/* Only enqueue network work here. Completion applies rules on this loop,
 * and stale results after an edit/reload are discarded by app.c. */
static void on_subscription_auto_update_timer(mt_loop_t *loop, void *ud)
{
    (void)loop;
    struct daemon *d = ud;
    mt_err_t err = mt_app_sync_due_subscriptions_async(d->app, d->fetcher,
                                                      now_unix(), auto_update_done, d);
    if (err != MT_OK) { MT_ERROR("failed to schedule subscription sync: %s", mt_err_str(err)); }
}

/* Match sink: dispatches into the matched group's ipset -- port of dns.go's
 * processARecord/processAAAARecord/processCNameRecord AddIPv4Subnet/
 * AddIPv6Subnet calls. */
static void on_match(const mt_match_action_t *action, void *ud)
{
    struct daemon *d = ud;
    mt_ruleset_t *rs = find_ruleset(d, action->group_id);
    if (rs == NULL) {
        return;
    }

    uint32_t ttl = action->ttl_seconds;
    mt_err_t err;
    if (action->addr_len == 4) {
        mt_ipv4_subnet_t subnet = {.cidr = 0};
        memcpy(subnet.addr, action->addr, 4);
        err = mt_ruleset_add_ipv4(rs, subnet, &ttl);
    } else {
        mt_ipv6_subnet_t subnet = {.cidr = 0};
        memcpy(subnet.addr, action->addr, 16);
        err = mt_ruleset_add_ipv6(rs, subnet, &ttl);
    }

    if (err != MT_OK) {
        MT_ERROR(
            "failed to add to routing: kind=%s domain=%s matched=%s "
            "group=%s err=%s",
            action->record_kind, action->record_domain, action->matched_name,
            action->group_name, mt_err_str(err));
        return;
    }
    MT_INFO(
        "added to routing: kind=%s domain=%s matched=%s group=%s "
        "ttl=%u",
        action->record_kind, action->record_domain, action->matched_name,
        action->group_name, action->ttl_seconds);
}

static void on_response(const mt_dns_msg_t *msg, const char *client_addr,
                        const char *network, void *ud)
{
    (void)client_addr;
    struct daemon *d = ud;
    MT_DEBUG("upstream response: id=%u answers=%zu network=%s", msg->id,
             msg->n_answers, network);
    mt_dns_pipeline_handle_message(d->pipeline, msg, now_unix());
}

static void on_link_up(const char *iface_name, bool up, void *ud)
{
    (void)up; /* the watcher only calls this when the link is up (see
               * netlink_watcher.h), matching Go's net.FlagUp filter */
    struct daemon *d = ud;
    MT_DEBUG("interface up: %s", iface_name);
    for (size_t i = 0; i < mt_app_user_group_count(d->app); i++) {
        mt_ruleset_t *rs = mt_app_user_group_at(d->app, i);
        const mt_group_t *g = mt_ruleset_group(rs);
        if (g->iface == NULL || strcmp(g->iface, iface_name) != 0) {
            continue;
        }
        mt_err_t err = mt_ruleset_on_link_up(rs);
        if (err != MT_OK) {
            MT_ERROR("error while handling interface up: group=%s err=%s",
                     g->name, mt_err_str(err));
        }
    }
    for (size_t i = 0; i < mt_app_subscription_ruleset_count(d->app); i++) {
        mt_ruleset_t *rs = mt_app_subscription_ruleset_at(d->app, i);
        const mt_group_t *g = mt_ruleset_group(rs);
        if (g->iface == NULL || strcmp(g->iface, iface_name) != 0) {
            continue;
        }
        mt_err_t err = mt_ruleset_on_link_up(rs);
        if (err != MT_OK) {
            MT_ERROR("error while handling interface up: group=%s err=%s",
                     g->name, mt_err_str(err));
        }
    }
}

static void on_addr_change(const char *iface_name, void *ud)
{
    struct daemon *d = ud;
    MT_DEBUG("interface address changed: %s", iface_name);
    for (size_t i = 0; i < mt_app_user_group_count(d->app); i++) {
        mt_ruleset_t *rs = mt_app_user_group_at(d->app, i);
        const mt_group_t *g = mt_ruleset_group(rs);
        if (g->iface == NULL || strcmp(g->iface, iface_name) != 0) {
            continue;
        }
        mt_err_t err = mt_ruleset_on_addr_change(rs);
        if (err != MT_OK) {
            MT_ERROR(
                "error while handling interface addr change: group=%s "
                "err=%s",
                g->name, mt_err_str(err));
        }
    }
    for (size_t i = 0; i < mt_app_subscription_ruleset_count(d->app); i++) {
        mt_ruleset_t *rs = mt_app_subscription_ruleset_at(d->app, i);
        const mt_group_t *g = mt_ruleset_group(rs);
        if (g->iface == NULL || strcmp(g->iface, iface_name) != 0) {
            continue;
        }
        mt_err_t err = mt_ruleset_on_addr_change(rs);
        if (err != MT_OK) {
            MT_ERROR(
                "error while handling interface addr change: group=%s "
                "err=%s",
                g->name, mt_err_str(err));
        }
    }
}

/* Collects the local addresses of the configured `link` interfaces for the
 * port-53 DNAT remap -- port of start.go's netlink.LinkByName/AddrList loop.
 * A configured interface that doesn't exist is a startup error, matching
 * Go's "failed to find link %s". */
static mt_err_t collect_link_addrs(char *const *link_names, size_t n_link,
                                   mt_remap_addr_t **out, size_t *out_n)
{
    *out = NULL;
    *out_n = 0;
    if (n_link == 0) {
        return MT_OK;
    }

    for (size_t i = 0; i < n_link; i++) {
        if (if_nametoindex(link_names[i]) == 0) {
            MT_ERROR("failed to find link %s: %s", link_names[i],
                     strerror(errno));
            return mt_err_from_errno(errno);
        }
    }

    struct ifaddrs *ifap;
    if (getifaddrs(&ifap) != 0) {
        return mt_err_from_errno(errno);
    }

    size_t cap = 0;
    size_t n = 0;
    mt_remap_addr_t *arr = NULL;
    for (struct ifaddrs *p = ifap; p != NULL; p = p->ifa_next) {
        if (p->ifa_addr == NULL) {
            continue;
        }
        bool matched = false;
        for (size_t i = 0; i < n_link; i++) {
            if (strcmp(p->ifa_name, link_names[i]) == 0) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }

        mt_remap_addr_t a = {0};
        if (p->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in sin;
            memcpy(&sin, p->ifa_addr, sizeof(sin));
            a.family = AF_INET;
            a.iplen = 4;
            memcpy(a.ip, &sin.sin_addr, 4);
        } else if (p->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6 sin6;
            memcpy(&sin6, p->ifa_addr, sizeof(sin6));
            a.family = AF_INET6;
            a.iplen = 16;
            memcpy(a.ip, &sin6.sin6_addr, 16);
        } else {
            continue;
        }

        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            mt_remap_addr_t *np = realloc(arr, ncap * sizeof(*np));
            if (!np) {
                free(arr);
                freeifaddrs(ifap);
                return MT_ERR_NOMEM;
            }
            arr = np;
            cap = ncap;
        }
        arr[n++] = a;
    }
    freeifaddrs(ifap);
    *out = arr;
    *out_n = n;
    return MT_OK;
}

/* HTTP glue callbacks -- ud is always the daemon's mt_config_t (see main()),
 * matching what auth.h/staticfiles.h expect. */
static bool auth_enabled_fn(void *ud)
{
    const mt_config_t *cfg = ud;
    return cfg->app.http_web.auth.enabled;
}

static const char *auth_state_dir_fn(void *ud)
{
    (void)ud;
    return MT_APP_STATE_DIR;
}

static const char *static_skin_fn(void *ud)
{
    const mt_config_t *cfg = ud;
    return cfg->app.http_web.skin;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("%s\n", MT_VERSION);
        return 0;
    }
    const char *config_path = MT_CONFIG_PATH;
    if (argc > 2 && strcmp(argv[1], "--config") == 0) {
        config_path = argv[2];
    }

    MT_INFO("starting MagiTrickle daemon (C) version=%s", MT_VERSION);

    /* libcurl's documented contract: call this once, non-concurrently,
     * before any thread performs a transfer (D-31) -- must happen before
     * any subscription fetch, whether HTTP-triggered (subscriptions_api.c)
     * or from the auto-update timer below. No other thread exists yet at
     * this point in startup. */
    mt_sub_fetch_global_init();

    mt_config_t cfg;
    if (mt_config_init_defaults(&cfg) != MT_OK) {
        MT_ERROR("failed to init config defaults");
        return 1;
    }
    mt_err_t err = mt_config_load_file(&cfg, config_path);
    if (err == MT_ERR_NOENT) {
        MT_INFO("config file %s not found, using defaults", config_path);
    } else if (err != MT_OK) {
        MT_ERROR("failed to load config %s: %s", config_path,
                 mt_err_str(err));
        mt_config_clear(&cfg);
        return 1;
    }
    mt_log_set_level(mt_log_level_from_str(cfg.app.log_level));

    struct daemon d = {0};
    d.cfg = &cfg;
    d.config_path = config_path;

    if (mt_loop_create(&d.loop) != MT_OK) {
        MT_ERROR("failed to create event loop");
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }

    d.cache = mt_cache_create(0);
    if (d.cache == NULL) {
        MT_ERROR("failed to create records cache");
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }

    uint32_t additional_ttl = (uint32_t)(cfg.app.netfilter.ipset.additional_ttl /
                                         MT_DURATION_SEC);
    d.pipeline = mt_dns_pipeline_create(d.cache, additional_ttl, on_match, &d);
    if (d.pipeline == NULL) {
        MT_ERROR("failed to create DNS pipeline");
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }
    mt_dns_pipeline_set_snapshot(d.pipeline, mt_ruleset_snapshot_build(&cfg));

    int cleanup_timer = 0;
    if (mt_loop_add_timer(d.loop, MT_CACHE_CLEANUP_INTERVAL_MS,
                          MT_CACHE_CLEANUP_INTERVAL_MS,
                          on_cache_cleanup_timer, &d,
                          &cleanup_timer) != MT_OK) {
        MT_ERROR("failed to schedule cache cleanup");
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }

    /* ---- netfilter: iptables engines, chain patches, cleanup ---------- */

    if (!cfg.app.netfilter.disable_ipv4) {
        mt_ipt_executable_t *exe = mt_ipt_executable_real_new(MT_IPT_PROTO_IPV4);
        if (!exe) {
            MT_ERROR("failed to create iptables executable (ipv4)");
            daemon_teardown(&d);
            mt_config_clear(&cfg);
            return 1;
        }
        d.ipt4 = mt_ipt_new(exe);
        if (!d.ipt4) {
            mt_ipt_executable_free(exe);
            MT_ERROR("netfilter helper init fail: out of memory");
            daemon_teardown(&d);
            mt_config_clear(&cfg);
            return 1;
        }
    }
    if (!cfg.app.netfilter.disable_ipv6) {
        mt_ipt_executable_t *exe = mt_ipt_executable_real_new(MT_IPT_PROTO_IPV6);
        if (!exe) {
            MT_ERROR("failed to create iptables executable (ipv6)");
            daemon_teardown(&d);
            mt_config_clear(&cfg);
            return 1;
        }
        d.ipt6 = mt_ipt_new(exe);
        if (!d.ipt6) {
            mt_ipt_executable_free(exe);
            MT_ERROR("netfilter helper init fail: out of memory");
            daemon_teardown(&d);
            mt_config_clear(&cfg);
            return 1;
        }
    }

    err = mt_netfilter_register_base_chains(d.ipt4, d.ipt6);
    if (err != MT_OK) {
        MT_ERROR("failed to register chain patches: %s", mt_err_str(err));
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }

    err = mt_netfilter_clean_iptables(d.ipt4, d.ipt6,
                                      cfg.app.netfilter.iptables.chain_prefix);
    if (err != MT_OK) {
        MT_ERROR("failed to clear iptables: %s", mt_err_str(err));
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }

    d.rtnl = mt_rtnl_open();
    if (d.rtnl == NULL) {
        MT_ERROR("failed to open rtnetlink");
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }

    if (mt_nl_watcher_create(d.loop, on_link_up, &d, on_addr_change, &d,
                             &d.watcher) != MT_OK) {
        MT_ERROR("failed to subscribe to link/addr updates");
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }

    /* ---- DNS proxy ------------------------------------------------------ */

    mt_dnsproxy_config_t pcfg = {
        .listen_addr = cfg.app.dns_proxy.host.address,
        .listen_port = cfg.app.dns_proxy.host.port,
        .upstream_addr = cfg.app.dns_proxy.upstream.address,
        .upstream_port = cfg.app.dns_proxy.upstream.port,
        .timeout_ms = (uint32_t)(cfg.app.dns_proxy.timeout / MT_DURATION_MS),
        .max_concurrent = (uint32_t)cfg.app.dns_proxy.max_concurrent,
        .max_idle_conns = (uint32_t)cfg.app.dns_proxy.max_idle_conns,
        .disable_fake_ptr = cfg.app.dns_proxy.disable_fake_ptr,
        .disable_drop_aaaa = cfg.app.dns_proxy.disable_drop_aaaa,
    };
    if (pcfg.max_concurrent == 0) {
        pcfg.max_concurrent = 100;
    }
    if (pcfg.timeout_ms == 0) {
        pcfg.timeout_ms = 5000;
    }

    if (mt_dnsproxy_create(&pcfg, d.loop, on_response, &d, &d.proxy) !=
        MT_OK) {
        MT_ERROR("failed to create DNS proxy");
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }
    err = mt_dnsproxy_start(d.proxy);
    if (err != MT_OK) {
        MT_ERROR("failed to start DNS proxy: %s", mt_err_str(err));
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    if (mt_loop_add_signals(d.loop, &set, on_signal, &d) != MT_OK) {
        MT_ERROR("failed to subscribe to signals");
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }

    /* ---- port 53 DNAT remap ---------------------------------------------- */

    if (!cfg.app.dns_proxy.disable_remap53) {
        mt_remap_addr_t *addrs = NULL;
        size_t n_addrs = 0;
        err = collect_link_addrs(cfg.app.link, cfg.app.n_link, &addrs, &n_addrs);
        if (err != MT_OK) {
            daemon_teardown(&d);
            mt_config_clear(&cfg);
            return 1;
        }
        d.port_remap =
            mt_port_remap_new(cfg.app.netfilter.iptables.chain_prefix, 53,
                              cfg.app.dns_proxy.host.port, addrs, n_addrs, d.ipt4, d.ipt6);
        free(addrs);
        if (d.port_remap == NULL) {
            MT_ERROR("failed to override DNS: out of memory");
            daemon_teardown(&d);
            mt_config_clear(&cfg);
            return 1;
        }
        err = mt_port_remap_enable(d.port_remap);
        if (err != MT_OK) {
            MT_ERROR("failed to override DNS: %s", mt_err_str(err));
            daemon_teardown(&d);
            mt_config_clear(&cfg);
            return 1;
        }
    }

    /* ---- app layer: per-group netfilter rulesets + HTTP mutation API -------- */

    mt_app_deps_t app_deps = {
        .cfg = &cfg,
        .cache = d.cache,
        .pipeline = d.pipeline,
        .ipt4 = d.ipt4,
        .ipt6 = d.ipt6,
        .rtnl = d.rtnl,
        .start_idx = cfg.app.netfilter.start_mark_table_index,
    };
    d.app = mt_app_create(&app_deps);
    if (d.app == NULL) {
        MT_ERROR("failed to create app");
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }
    for (size_t i = 0; i < mt_app_user_group_count(d.app); i++) {
        mt_ruleset_t *rs = mt_app_user_group_at(d.app, i);
        err = mt_ruleset_enable(rs);
        if (err != MT_OK) {
            MT_ERROR("failed to enable group: %s", mt_err_str(err));
            daemon_teardown(&d);
            mt_config_clear(&cfg);
            return 1;
        }
        err = mt_ruleset_sync(rs, d.cache, now_unix());
        if (err != MT_OK) {
            MT_ERROR("failed to sync group: %s", mt_err_str(err));
            daemon_teardown(&d);
            mt_config_clear(&cfg);
            return 1;
        }
    }
    for (size_t i = 0; i < mt_app_subscription_ruleset_count(d.app); i++) {
        mt_ruleset_t *rs = mt_app_subscription_ruleset_at(d.app, i);
        err = mt_ruleset_enable(rs);
        if (err != MT_OK) {
            MT_ERROR("failed to enable subscription: %s", mt_err_str(err));
            daemon_teardown(&d);
            mt_config_clear(&cfg);
            return 1;
        }
        err = mt_ruleset_sync(rs, d.cache, now_unix());
        if (err != MT_OK) {
            MT_ERROR("failed to sync subscription: %s", mt_err_str(err));
            daemon_teardown(&d);
            mt_config_clear(&cfg);
            return 1;
        }
    }
    /* Only now mark the app "running" -- matches Go's Start() CAS
     * happening before this same enable/sync loop, so a group added later
     * via the HTTP API (mt_app_add_group) gets its own immediate
     * enable+sync, while the initial set was already brought up above. */
    mt_app_set_running(d.app, true);

    /* Signals were blocked above; worker threads inherit that mask. */
    if (mt_sub_fetcher_create(d.loop, &d.fetcher) != MT_OK) {
        MT_ERROR("failed to create subscription fetch workers");
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }

    /* ---- subscription auto-update: 1-minute tick, fires once now too ------- */

    int sub_auto_update_timer = 0;
    if (mt_loop_add_timer(d.loop, 0, MT_SUBSCRIPTION_AUTO_UPDATE_INTERVAL_MS,
                          on_subscription_auto_update_timer, &d,
                          &sub_auto_update_timer) != MT_OK) {
        MT_ERROR("failed to schedule subscription auto-update");
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }

    /* ---- HTTP API: Unix socket (always on) + TCP WebUI (config-gated) ------ */

    mt_groups_ctx_t groups_ctx = {
        .app = d.app,
        .config_path = config_path,
        .config_version = MT_VERSION,
    };
    mt_system_ctx_t system_ctx = {
        .app = d.app,
        .config_path = config_path,
        .config_version = MT_VERSION,
    };
    mt_subs_ctx_t subs_ctx = {
        .app = d.app,
        .fetcher = d.fetcher,
        .config_path = config_path,
        .config_version = MT_VERSION,
    };
    mt_auth_ctx_t auth_ctx = {
        .enabled = auth_enabled_fn,
        .state_dir = auth_state_dir_fn,
        .ud = &cfg,
    };

    if (mt_httpd_create(d.loop, &d.http_unix) != MT_OK) {
        MT_ERROR("failed to create Unix socket server");
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }
    mt_groups_register_routes(d.http_unix, &groups_ctx);
    mt_system_register_routes(d.http_unix, &system_ctx);
    mt_subs_register_routes(d.http_unix, &subs_ctx);
    mt_httpd_route(d.http_unix, "GET", "/api/v1/auth", mt_auth_status_handler, &auth_ctx);
    mt_httpd_route(d.http_unix, "POST", "/api/v1/auth", mt_auth_login_handler, &auth_ctx);
    err = mt_httpd_listen_unix(d.http_unix, MT_SOCK_PATH);
    if (err != MT_OK) {
        MT_ERROR("failed to listen on Unix socket %s: %s", MT_SOCK_PATH, mt_err_str(err));
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }
    MT_INFO("Unix socket API listening on %s", MT_SOCK_PATH);

    mt_static_ctx_t static_ctx = {
        .skins_dir = MT_APP_SHARE_DIR "/skins",
        .skin_name = static_skin_fn,
        .ud = &cfg,
    };
    if (cfg.app.http_web.enabled) {
        if (mt_httpd_create(d.loop, &d.http_tcp) != MT_OK) {
            MT_ERROR("failed to create HTTP server");
            daemon_teardown(&d);
            mt_config_clear(&cfg);
            return 1;
        }
        mt_groups_register_routes(d.http_tcp, &groups_ctx);
        mt_system_register_routes(d.http_tcp, &system_ctx);
        mt_subs_register_routes(d.http_tcp, &subs_ctx);
        mt_httpd_route(d.http_tcp, "GET", "/api/v1/auth", mt_auth_status_handler, &auth_ctx);
        mt_httpd_route(d.http_tcp, "POST", "/api/v1/auth", mt_auth_login_handler, &auth_ctx);
        mt_httpd_set_middleware(d.http_tcp, mt_auth_middleware, &auth_ctx);
        mt_httpd_set_not_found(d.http_tcp, mt_static_handler, &static_ctx);
        err = mt_httpd_listen_tcp(d.http_tcp, cfg.app.http_web.host.address,
                                  cfg.app.http_web.host.port);
        if (err != MT_OK) {
            MT_ERROR("failed to listen HTTP %s:%u: %s", cfg.app.http_web.host.address,
                     cfg.app.http_web.host.port, mt_err_str(err));
            daemon_teardown(&d);
            mt_config_clear(&cfg);
            return 1;
        }
        MT_INFO("HTTP WebUI listening on %s:%u", cfg.app.http_web.host.address,
                cfg.app.http_web.host.port);
    } else {
        MT_INFO("HTTP WebUI disabled by configuration");
    }

    /* Started last, once everything a rebuild reaches is up: the port
     * remap, every group's chains, and the API that can change them. On
     * builds without a committer this is a no-op and the netfilter.d hook
     * keeps committing in place. */
    mt_app_set_port_remap(d.app, d.port_remap);
    err = mt_app_start_netfilter_committer(d.app);
    if (err != MT_OK) {
        MT_ERROR("failed to start netfilter committer: %s", mt_err_str(err));
        daemon_teardown(&d);
        mt_config_clear(&cfg);
        return 1;
    }

    MT_INFO("service started");
    err = mt_loop_run(d.loop);
    if (err != MT_OK) {
        MT_ERROR("event loop failed: %s", mt_err_str(err));
    }

    MT_INFO("service stopped");
    daemon_teardown(&d);
    mt_config_clear(&cfg);
    mt_sub_fetch_global_cleanup();
    return err == MT_OK ? 0 : 1;
}
