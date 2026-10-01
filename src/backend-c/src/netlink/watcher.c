/* See netlink_watcher.h. */
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

struct mt_nl_watcher {
    struct mnl_socket *nl;
    mt_loop_t *loop;
    mt_nl_link_cb link_cb;
    void *link_ud;
    mt_nl_addr_cb addr_cb;
    void *addr_ud;
};

static void handle_link_msg(mt_nl_watcher_t *w, const struct nlmsghdr *h) {
    if (h->nlmsg_type != RTM_NEWLINK || !w->link_cb) { return; }

    const struct ifinfomsg *ifi = mnl_nlmsg_get_payload(h);
    bool up = (ifi->ifi_flags & IFF_UP) != 0;
    if (!up) { return; /* matches Go: linkAttrs.Flags&net.FlagUp == 0 -> no dispatch */ }

    char name[IFNAMSIZ] = {0};
    mt_nlattr_iter_t attr_it;
    const struct nlattr *attr;
    if (!mt_nlattr_iter_init_nlmsg(&attr_it, h, sizeof(struct ifinfomsg))) { return; }
    while (mt_nlattr_iter_next(&attr_it, &attr)) {
        if (mnl_attr_get_type(attr) == IFLA_IFNAME) {
            const char *v = mnl_attr_get_str(attr);
            snprintf(name, sizeof(name), "%s", v);
        }
    }
    if (name[0] == '\0') { return; }

    w->link_cb(name, true, w->link_ud);
}

static void handle_addr_msg(mt_nl_watcher_t *w, const struct nlmsghdr *h) {
    if (h->nlmsg_type != RTM_NEWADDR || !w->addr_cb) {
        return; /* matches Go: `if !event.NewAddr { return }` -- DELADDR ignored too */
    }
    const struct ifaddrmsg *ifa = mnl_nlmsg_get_payload(h);
    char name[IFNAMSIZ] = {0};
    if (!if_indextoname(ifa->ifa_index, name)) { return; }
    w->addr_cb(name, w->addr_ud);
}

static void on_readable(mt_loop_t *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)fd;
    (void)events;
    mt_nl_watcher_t *w = ud;
    uint8_t buf[MT_NL_WATCHER_RECVBUF];

    for (;;) {
        ssize_t ret = mnl_socket_recvfrom(w->nl, buf, sizeof(buf));
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { break; }
            if (errno == EINTR) { continue; }
            MT_WARN("netlink watcher recv error: %s", strerror(errno));
            break;
        }
        if (ret == 0) { break; }

        int len = (int)ret;
        struct nlmsghdr *nh = (struct nlmsghdr *)buf;
        while (mnl_nlmsg_ok(nh, len)) {
            switch (nh->nlmsg_type) {
            case RTM_NEWLINK:
                handle_link_msg(w, nh);
                break;
            case RTM_NEWADDR:
                handle_addr_msg(w, nh);
                break;
            default:
                break;
            }
            nh = mnl_nlmsg_next(nh, &len);
        }
    }
}

mt_err_t mt_nl_watcher_create(mt_loop_t *loop, mt_nl_link_cb link_cb, void *link_ud,
                              mt_nl_addr_cb addr_cb, void *addr_ud, mt_nl_watcher_t **out) {
    *out = NULL;
    mt_nl_watcher_t *w = calloc(1, sizeof(*w));
    if (!w) { return MT_ERR_NOMEM; }
    w->loop = loop;
    w->link_cb = link_cb;
    w->link_ud = link_ud;
    w->addr_cb = addr_cb;
    w->addr_ud = addr_ud;

    w->nl = mnl_socket_open(NETLINK_ROUTE);
    if (!w->nl) {
        mt_err_t err = mt_err_from_errno(errno);
        free(w);
        return err;
    }

    unsigned int groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
    if (mnl_socket_bind(w->nl, groups, MNL_SOCKET_AUTOPID) < 0) {
        mt_err_t err = mt_err_from_errno(errno);
        mnl_socket_close(w->nl);
        free(w);
        return err;
    }

    int fd = mnl_socket_get_fd(w->nl);
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        mt_err_t err = mt_err_from_errno(errno);
        mnl_socket_close(w->nl);
        free(w);
        return err;
    }

    mt_err_t err = mt_loop_add_fd(loop, fd, EPOLLIN, on_readable, w);
    if (err != MT_OK) {
        mnl_socket_close(w->nl);
        free(w);
        return err;
    }

    *out = w;
    return MT_OK;
}

void mt_nl_watcher_destroy(mt_nl_watcher_t *w) {
    if (!w) { return; }
    mt_loop_del_fd(w->loop, mnl_socket_get_fd(w->nl));
    mnl_socket_close(w->nl);
    free(w);
}
