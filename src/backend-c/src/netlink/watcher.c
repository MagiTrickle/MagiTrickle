#include "magitrickle/netlink_watcher.h"
#include "magitrickle/log.h"
#include "magitrickle/nlattr_iter.h"

#include <errno.h>
#include <fcntl.h>
#include <libmnl/libmnl.h>
#include <linux/if.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>

#define MT_NL_WATCHER_RECVBUF 8192
#define MT_NL_RECOVERY_MAX_MS 30000u

struct mt_nl_watcher {
    struct mnl_socket *nl;
    mt_loop_t *loop;
    mt_nl_link_cb link_cb;
    void *link_ud;
    mt_nl_addr_cb addr_cb;
    void *addr_ud;
    mt_nl_change_cb route_cb;
    void *route_ud;
    mt_nl_change_cb resync_cb;
    void *resync_ud;
    int reconnect_timer;
    unsigned reconnect_delay_ms;
};

static void notify_resync(mt_nl_watcher_t *w)
{
    if (w->resync_cb) { w->resync_cb(w->resync_ud); }
}

static bool handle_link_msg(mt_nl_watcher_t *w, const struct nlmsghdr *h)
{
    if (mnl_nlmsg_get_payload_len(h) < sizeof(struct ifinfomsg)) { return false; }
    const struct ifinfomsg *ifi = mnl_nlmsg_get_payload(h);
    bool up = h->nlmsg_type != RTM_DELLINK && (ifi->ifi_flags & IFF_UP) != 0;
    char name[IFNAMSIZ] = {0};
    mt_nlattr_iter_t it;
    const struct nlattr *attr;
    if (!mt_nlattr_iter_init_nlmsg(&it, h, sizeof(*ifi))) { return false; }
    while (mt_nlattr_iter_next(&it, &attr)) {
        if (mnl_attr_get_type(attr) == IFLA_IFNAME) {
            if (mnl_attr_validate(attr, MNL_TYPE_NUL_STRING) != 0) { return false; }
            snprintf(name, sizeof(name), "%s", mnl_attr_get_str(attr));
        }
    }
    if (!name[0]) { return false; }
    if (w->link_cb) { w->link_cb(name, up, w->link_ud); }
    return true;
}

static bool handle_addr_msg(mt_nl_watcher_t *w, const struct nlmsghdr *h)
{
    if (mnl_nlmsg_get_payload_len(h) < sizeof(struct ifaddrmsg)) { return false; }
    const struct ifaddrmsg *ifa = mnl_nlmsg_get_payload(h);
    char name[IFNAMSIZ] = {0};
    if (!if_indextoname(ifa->ifa_index, name)) {
        /* A DELADDR can race with DELLINK. Name lookup is not authoritative. */
        return false;
    }
    if (w->addr_cb) { w->addr_cb(name, w->addr_ud); }
    return true;
}

static void watcher_drop_socket(mt_nl_watcher_t *w)
{
    if (!w->nl) { return; }
    (void)mt_loop_del_fd(w->loop, mnl_socket_get_fd(w->nl));
    mnl_socket_close(w->nl);
    w->nl = NULL;
}

static void on_readable(mt_loop_t *loop, int fd, uint32_t events, void *ud);

static mt_err_t watcher_open_socket(mt_nl_watcher_t *w)
{
    struct mnl_socket *nl = mnl_socket_open(NETLINK_ROUTE);
    if (!nl) { return mt_err_from_errno(errno); }
    unsigned int groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR |
                          RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
    if (mnl_socket_bind(nl, groups, MNL_SOCKET_AUTOPID) < 0) {
        mt_err_t err = mt_err_from_errno(errno);
        mnl_socket_close(nl);
        return err;
    }
    int fd = mnl_socket_get_fd(nl);
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        mt_err_t err = mt_err_from_errno(errno);
        mnl_socket_close(nl);
        return err;
    }
    mt_err_t err = mt_loop_add_fd(w->loop, fd, EPOLLIN | EPOLLERR | EPOLLHUP, on_readable, w);
    if (err != MT_OK) {
        mnl_socket_close(nl);
        return err;
    }
    w->nl = nl;
    return MT_OK;
}

static void on_reconnect(mt_loop_t *loop, void *ud);

static void watcher_schedule_reconnect(mt_nl_watcher_t *w)
{
    if (w->reconnect_timer) { return; }
    unsigned delay = w->reconnect_delay_ms ? w->reconnect_delay_ms : 100u;
    if (delay > MT_NL_RECOVERY_MAX_MS) { delay = MT_NL_RECOVERY_MAX_MS; }
    if (mt_loop_add_timer(w->loop, delay, 0, on_reconnect, w, &w->reconnect_timer) != MT_OK) {
        MT_ERROR("cannot schedule netlink watcher recovery; stopping daemon");
        mt_loop_stop(w->loop);
        return;
    }
    w->reconnect_delay_ms = delay >= MT_NL_RECOVERY_MAX_MS / 2u ?
                            MT_NL_RECOVERY_MAX_MS : delay * 2u;
}

static void on_reconnect(mt_loop_t *loop, void *ud)
{
    (void)loop;
    mt_nl_watcher_t *w = ud;
    w->reconnect_timer = 0;
    mt_err_t err = watcher_open_socket(w);
    if (err != MT_OK) {
        MT_WARN("netlink watcher reconnect failed: %s", mt_err_str(err));
        watcher_schedule_reconnect(w);
        return;
    }
    w->reconnect_delay_ms = 0;
    notify_resync(w); /* State may have changed while the socket was absent. */
}

static void watcher_socket_failure(mt_nl_watcher_t *w)
{
    watcher_drop_socket(w);
    notify_resync(w);
    watcher_schedule_reconnect(w);
}

static void on_readable(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)loop;
    (void)fd;
    mt_nl_watcher_t *w = ud;
    if (events & (EPOLLERR | EPOLLHUP)) {
        watcher_socket_failure(w);
        return;
    }
    uint8_t buf[MT_NL_WATCHER_RECVBUF];
    for (;;) {
        ssize_t ret = mnl_socket_recvfrom(w->nl, buf, sizeof(buf));
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { return; }
            if (errno == EINTR) { continue; }
            MT_WARN("netlink watcher recv error: %s", strerror(errno));
            if (errno == ENOBUFS) {
                notify_resync(w);
                continue;
            }
            watcher_socket_failure(w);
            return;
        }
        if (ret == 0) {
            watcher_socket_failure(w);
            return;
        }
        int len = (int)ret;
        struct nlmsghdr *nh = (struct nlmsghdr *)buf;
        while (mnl_nlmsg_ok(nh, len)) {
            switch (nh->nlmsg_type) {
            case RTM_NEWLINK:
            case RTM_DELLINK:
                if (!handle_link_msg(w, nh)) { notify_resync(w); }
                break;
            case RTM_NEWADDR:
            case RTM_DELADDR:
                if (!handle_addr_msg(w, nh)) { notify_resync(w); }
                break;
            case RTM_NEWROUTE:
            case RTM_DELROUTE:
                if (mnl_nlmsg_get_payload_len(nh) < sizeof(struct rtmsg)) {
                    notify_resync(w);
                } else if (w->route_cb) {
                    w->route_cb(w->route_ud);
                }
                break;
            case NLMSG_ERROR:
                if (mnl_nlmsg_get_payload_len(nh) < sizeof(struct nlmsgerr) ||
                    ((const struct nlmsgerr *)mnl_nlmsg_get_payload(nh))->error != 0) {
                    notify_resync(w);
                }
                break;
            case NLMSG_OVERRUN:
                notify_resync(w);
                break;
            default:
                break;
            }
            nh = mnl_nlmsg_next(nh, &len);
        }
        if (len != 0) { notify_resync(w); }
    }
}

mt_err_t mt_nl_watcher_create_with_routes(mt_loop_t *loop, mt_nl_link_cb link_cb,
                                          void *link_ud, mt_nl_addr_cb addr_cb,
                                          void *addr_ud, mt_nl_change_cb route_cb,
                                          void *route_ud, mt_nl_change_cb resync_cb,
                                          void *resync_ud, mt_nl_watcher_t **out)
{
    if (!loop || !out) { return MT_ERR_INVAL; }
    *out = NULL;
    mt_nl_watcher_t *w = calloc(1, sizeof(*w));
    if (!w) { return MT_ERR_NOMEM; }
    w->loop = loop;
    w->link_cb = link_cb;
    w->link_ud = link_ud;
    w->addr_cb = addr_cb;
    w->addr_ud = addr_ud;
    w->route_cb = route_cb;
    w->route_ud = route_ud;
    w->resync_cb = resync_cb;
    w->resync_ud = resync_ud;
    mt_err_t err = watcher_open_socket(w);
    if (err != MT_OK) {
        free(w);
        return err;
    }
    *out = w;
    return MT_OK;
}

mt_err_t mt_nl_watcher_create(mt_loop_t *loop, mt_nl_link_cb link_cb, void *link_ud,
                              mt_nl_addr_cb addr_cb, void *addr_ud, mt_nl_watcher_t **out)
{
    return mt_nl_watcher_create_with_routes(loop, link_cb, link_ud, addr_cb, addr_ud,
                                           NULL, NULL, NULL, NULL, out);
}

void mt_nl_watcher_destroy(mt_nl_watcher_t *w)
{
    if (!w) { return; }
    if (w->reconnect_timer) { (void)mt_loop_del_timer(w->loop, w->reconnect_timer); }
    watcher_drop_socket(w);
    free(w);
}
