/* Platform filesystem paths — C equivalent of Go's build-tag-conditional
 * constant/path_{default,entware,openwrt}.go.
 *
 * The root Makefile derives the platform from PLATFORM/TARGET and passes
 * -DMT_PLATFORM_ENTWARE or -DMT_PLATFORM_OPENWRT to the C build; without
 * either (host builds, tests) the default set below is used. Every macro
 * keeps its own #ifndef guard so an individual path can still be pinned
 * from the command line via -D (that -D wins over these defaults, same
 * override hook already used for MT_CONFIG_PATH in main.c).
 *
 * Entware installs everything under /opt, so its paths carry that prefix;
 * OpenWrt keeps state under /etc/magitrickle/state. Keep these in sync
 * with the packaging tree the root Makefile assembles (BIN_DIR/ETC_DIR/
 * STATE_DIR per PLATFORM) and with the per-platform payload under files/
 * (conffiles, and the entware_kn netfilter.d hook SOCKET_PATH).
 */
#ifndef MAGITRICKLE_PATHS_H
#define MAGITRICKLE_PATHS_H

#if defined(MT_PLATFORM_ENTWARE)

#ifndef MT_APP_SHARE_DIR
#define MT_APP_SHARE_DIR "/opt/usr/share/magitrickle"
#endif
#ifndef MT_APP_STATE_DIR
#define MT_APP_STATE_DIR "/opt/var/lib/magitrickle"
#endif
#ifndef MT_SOCK_PATH
#define MT_SOCK_PATH "/opt/var/run/magitrickle.sock"
#endif
#ifndef MT_PASSWD_FILE
#define MT_PASSWD_FILE "/opt/etc/passwd"
#endif
#ifndef MT_SHADOW_FILE
#define MT_SHADOW_FILE "/opt/etc/shadow"
#endif

#elif defined(MT_PLATFORM_OPENWRT)

#ifndef MT_APP_SHARE_DIR
#define MT_APP_SHARE_DIR "/usr/share/magitrickle"
#endif
#ifndef MT_APP_STATE_DIR
#define MT_APP_STATE_DIR "/etc/magitrickle/state"
#endif
#ifndef MT_SOCK_PATH
#define MT_SOCK_PATH "/var/run/magitrickle.sock"
#endif
#ifndef MT_PASSWD_FILE
#define MT_PASSWD_FILE "/etc/passwd"
#endif
#ifndef MT_SHADOW_FILE
#define MT_SHADOW_FILE "/etc/shadow"
#endif

#else /* default / host-native */

#ifndef MT_APP_SHARE_DIR
#define MT_APP_SHARE_DIR "/usr/share/magitrickle"
#endif
#ifndef MT_APP_STATE_DIR
#define MT_APP_STATE_DIR "/var/lib/magitrickle"
#endif
#ifndef MT_SOCK_PATH
#define MT_SOCK_PATH "/var/run/magitrickle.sock"
#endif
#ifndef MT_PASSWD_FILE
#define MT_PASSWD_FILE "/etc/passwd"
#endif
#ifndef MT_SHADOW_FILE
#define MT_SHADOW_FILE "/etc/shadow"
#endif

#endif

#endif /* MAGITRICKLE_PATHS_H */
