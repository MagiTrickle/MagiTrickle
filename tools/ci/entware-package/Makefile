include $(TOPDIR)/rules.mk

include ./version.mk

PKG_NAME:=magitrickle
PKG_VERSION:=$(MAGITRICKLE_VERSION)
PKG_RELEASE:=$(MAGITRICKLE_RELEASE)
PKG_LICENSE:=GPL-3.0-or-later
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=Vladimir Avtsenov <vladimir.lsk.cool@gmail.com>
PKG_SOURCE:=magitrickle-source.tar.gz
PKG_SOURCE_URL:=file://$(CURDIR)
PKG_HASH:=skip
PKG_MIRROR_HASH:=skip
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
PKG_BUILD_PARALLEL:=1
PKGARCH:=$(MAGITRICKLE_TARGET)

include $(INCLUDE_DIR)/package.mk

# ownik/entware-sdk contains the completed target toolchain but drops its
# completion stamps together with generated IPKs. Entware's package/check
# removes .built unless the matching configured/prepared checks also exist.
# Restore the stamp chain used by the pinned GCC 8.4/glibc 2.27 SDK so the
# package phase can create libc/libgcc IPKs from staging without rebuilding
# GCC. A future SDK with different stamp hashes will fail visibly here.
MAGITRICKLE_TOOLCHAIN_DIR:=$(BUILD_DIR)/toolchain
MAGITRICKLE_TOOLCHAIN_PREPARED:=.prepared_ad8c94d059fbcd3e4fe92dada0211bba_18f1e190c5d53547fed41a3eaa76e9e9
MAGITRICKLE_TOOLCHAIN_CONFIGURED:=.configured_68b329da9893e34099c7d8ad5cb9c940
$(shell mkdir -p $(MAGITRICKLE_TOOLCHAIN_DIR) && \
	touch \
		$(MAGITRICKLE_TOOLCHAIN_DIR)/$(MAGITRICKLE_TOOLCHAIN_PREPARED)_check \
		$(MAGITRICKLE_TOOLCHAIN_DIR)/$(MAGITRICKLE_TOOLCHAIN_PREPARED) \
		$(MAGITRICKLE_TOOLCHAIN_DIR)/$(MAGITRICKLE_TOOLCHAIN_CONFIGURED) \
		$(MAGITRICKLE_TOOLCHAIN_DIR)/.built)

MAGITRICKLE_DYNAMIC_LINKER:=ld.so.1
ifneq ($(findstring aarch64,$(MAGITRICKLE_TARGET)),)
MAGITRICKLE_DYNAMIC_LINKER:=ld-linux-aarch64.so.1
endif

# Mirrors the root Makefile's own $(filter %_kn,$(TARGET)) gate: only
# Keenetic (_kn) targets get ENTWARE_KN=1, the NDM netfilter.d hook, and
# the self-contained (no /opt/etc/init.d/rc.func) init script. Plain
# Entware targets use the generic init script that sources rc.func, same
# as the non-CI ipk packaging path.
MAGITRICKLE_IS_KN:=$(filter %_kn,$(MAGITRICKLE_TARGET))

define Package/magitrickle
  SECTION:=net
  CATEGORY:=Network
  TITLE:=DNS-based routing application
  URL:=https://magitrickle.dev
  DEPENDS:=+iptables +libyaml +libpcre2 +libmnl +libcurl +cJSON $(if $(MAGITRICKLE_IS_KN),+socat)
endef

define Package/magitrickle/description
 DNS-based routing application for Entware.
endef

define Package/magitrickle/conffiles
/opt/var/lib/magitrickle/config.yaml
endef

define Package/magitrickle/postinst
#!/bin/sh
/opt/etc/init.d/S99magitrickle restart
exit 0
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR)/src/backend-c \
		BUILD=entware-$(MAGITRICKLE_TARGET) \
		MT_VERSION="$(PKG_VERSION)" \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		PLATFORM=entware \
		$(if $(MAGITRICKLE_IS_KN),ENTWARE_KN=1) \
		WITH_DEPS=1 \
		CFLAGS_EXTRA="$(TARGET_CFLAGS) -I$(STAGING_DIR)/opt/include" \
		LDLIBS="-lpthread -Wl,-Bstatic -latomic -Wl,-Bdynamic" \
		LDFLAGS_EXTRA="$(TARGET_LDFLAGS) \
			-Wl,--dynamic-linker=/opt/lib/$(MAGITRICKLE_DYNAMIC_LINKER) \
			-Wl,-rpath,/opt/lib \
			-L$(STAGING_DIR)/opt/lib \
			-Wl,-rpath-link,$(STAGING_DIR)/opt/lib"
endef

define Package/magitrickle/install
	$(INSTALL_DIR) $(1)/opt/bin
	$(INSTALL_BIN) \
		$(PKG_BUILD_DIR)/src/backend-c/build/entware-$(MAGITRICKLE_TARGET)/magitrickled-c \
		$(1)/opt/bin/magitrickled

	$(INSTALL_DIR) $(1)/opt/usr/share/magitrickle/skins/default
	$(CP) $(PKG_BUILD_DIR)/src/frontend/dist/. \
		$(1)/opt/usr/share/magitrickle/skins/default/

	$(INSTALL_DIR) $(1)/opt/var/lib/magitrickle
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/files/common/var/lib/magitrickle/config.yaml \
		$(1)/opt/var/lib/magitrickle/config.yaml

	$(INSTALL_DIR) $(1)/opt/etc/init.d
ifneq ($(MAGITRICKLE_IS_KN),)
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/files/entware_kn/etc/init.d/S99magitrickle \
		$(1)/opt/etc/init.d/S99magitrickle

	$(INSTALL_DIR) $(1)/opt/etc/ndm/netfilter.d
	$(INSTALL_BIN) \
		$(PKG_BUILD_DIR)/files/entware_kn/etc/ndm/netfilter.d/100-magitrickle \
		$(1)/opt/etc/ndm/netfilter.d/100-magitrickle
else
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/files/entware/etc/init.d/S99magitrickle \
		$(1)/opt/etc/init.d/S99magitrickle
endif
endef

$(eval $(call BuildPackage,magitrickle))
