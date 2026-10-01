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
PKG_BUILD_DEPENDS:=pcre2 libcjson

include $(INCLUDE_DIR)/package.mk

define Package/magitrickle
  SECTION:=net
  CATEGORY:=Network
  TITLE:=DNS-based routing application
  URL:=https://magitrickle.dev
  DEPENDS:=+iptables-nft +iptables-mod-conntrack-extra +kmod-ipt-nat +kmod-ipt-ipset +ip6tables-nft +libatomic +libyaml +libpcre2 +libmnl +libcurl
endef

define Package/magitrickle/description
 DNS-based routing application.
endef

define Package/magitrickle/conffiles
/etc/config/magitrickle
/etc/magitrickle/state/config.yaml
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR)/src/backend-c \
		BUILD=openwrt-$(PKGARCH) \
		MT_VERSION="$(PKG_VERSION)" \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		PLATFORM=openwrt \
		WITH_DEPS=1 \
		CFLAGS_EXTRA="$(TARGET_CFLAGS)" \
		LDFLAGS_EXTRA="$(TARGET_LDFLAGS)" \
		CJSON_LIB="$(STAGING_DIR)/usr/lib/libcjson.a"
endef

define Package/magitrickle/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) \
		$(PKG_BUILD_DIR)/src/backend-c/build/openwrt-$(PKGARCH)/magitrickled-c \
		$(1)/usr/bin/magitrickled

	$(INSTALL_DIR) $(1)/usr/share/magitrickle/skins/default
	$(CP) $(PKG_BUILD_DIR)/src/frontend/dist/. \
		$(1)/usr/share/magitrickle/skins/default/

	$(INSTALL_DIR) $(1)/etc/magitrickle/state
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/files/common/var/lib/magitrickle/config.yaml \
		$(1)/etc/magitrickle/state/config.yaml

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/files/openwrt/etc/config/magitrickle \
		$(1)/etc/config/magitrickle

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/files/openwrt/etc/init.d/magitrickle \
		$(1)/etc/init.d/magitrickle
endef

$(eval $(call BuildPackage,magitrickle))
