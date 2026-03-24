include $(TOPDIR)/rules.mk

include pkg-info.mk

PKG_BUILD_DEPENDS:=golang/host
PKG_BUILD_PARALLEL:=1
PKG_BUILD_FLAGS:=no-mips16

GO_PKG:=$(PKG_NAME)
GO_PKG_LDFLAGS_X:=$(GO_PKG)/constant.Version=$(PKG_VERSION)
GO_PKG_TAGS:=$(PLATFORM)

INST_BIN_DIR:=/usr/bin
INST_ETC_DIR:=/etc
INST_USRSHARE_DIR:=/usr/share
INST_STATE_DIR:=/etc/magitrickle/state

include $(INCLUDE_DIR)/package.mk
include $(TOPDIR)/feeds/packages/lang/golang/golang-package.mk

ifneq (,$(filter $(PLATFORM),entware entware_kn))
	INST_BIN_DIR:=/opt/bin
	INST_ETC_DIR:=/opt/etc
	INST_USRSHARE_DIR:=/opt/usr/share
	INST_STATE_DIR:=/opt/var/lib/magitrickle

	GO_PKG_TAGS:=entware
ifeq ($(PLATFORM),entware_kn)
	GO_PKG_TAGS+=entware_kn
endif
endif

define Package/$(PKG_NAME)
	URL:=$(PKG_URL)
	MAINTAINER:=$(PKG_MAINTAINER)
	LICENSE:=$(PKG_LICENSE)
	SECTION:=net
	CATEGORY:=Network
	TITLE:=$(PKG_NAME)
ifeq ($(CONFIG_USE_APK),)
	VERSION:=$(PKG_VERSION)-$(PKG_REVISION)
else
	VERSION:=$(PKG_VERSION_APK)-r$(PKG_REVISION)
endif
	DEPENDS:=$(GO_ARCH_DEPENDS)
ifeq ($(PLATFORM),openwrt)
	DEPENDS+= iptables-nft iptables-mod-conntrack-extra kmod-ipt-nat kmod-ipt-ipset ip6tables-nft
endif
ifneq (,$(filter $(PLATFORM),entware entware_kn))
	DEPENDS+= iptables
ifeq ($(PLATFORM),entware_kn)
	DEPENDS+= socat
endif
endif
endef

define Package/$(PKG_NAME)/description
	$(PKG_DESCRIPTION)
endef

ifeq ($(PLATFORM),openwrt)
define Package/$(PKG_NAME)/conffiles
$(INST_STATE_DIR)/config.yaml
$(INST_ETC_DIR)/config/magitrickle
endef
else
define Package/$(PKG_NAME)/conffiles
$(INST_STATE_DIR)/config.yaml
endef
endif

define Package/$(PKG_NAME)/install
	$(call GoPackage/Package/Install/Bin,$(PKG_INSTALL_DIR))

	$(INSTALL_DIR) $(1)/$(INST_BIN_DIR)
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/magitrickled $(1)/$(INST_BIN_DIR)/

	$(INSTALL_DIR) $(1)/$(INST_STATE_DIR)
	$(INSTALL_DIR) $(1)/$(INST_ETC_DIR)/init.d

ifeq ($(PLATFORM),openwrt)
	$(INSTALL_CONF) $(CURDIR)/files/openwrt/var/lib/magitrickle/config.yaml $(1)/$(INST_STATE_DIR)/

	$(INSTALL_DIR) $(1)/$(INST_ETC_DIR)/config
	$(INSTALL_DATA) $(CURDIR)/files/openwrt/etc/config/magitrickle $(1)/$(INST_ETC_DIR)/config/

	$(INSTALL_BIN) $(CURDIR)/files/openwrt/etc/init.d/magitrickle $(1)/$(INST_ETC_DIR)/init.d/
endif

ifneq (,$(filter $(PLATFORM),entware entware_kn))
	$(INSTALL_CONF) $(CURDIR)/files/common/var/lib/magitrickle/config.yaml $(1)/$(INST_STATE_DIR)/

	$(INSTALL_BIN) $(CURDIR)/files/$(PLATFORM)/etc/init.d/S99magitrickle $(1)/$(INST_ETC_DIR)/init.d/
ifeq ($(PLATFORM),entware_kn)
	$(INSTALL_DIR) $(1)/$(INST_ETC_DIR)/ndm/netfilter.d/
	$(INSTALL_BIN) $(CURDIR)/files/$(PLATFORM)/etc/ndm/netfilter.d/100-magitrickle $(1)/$(INST_ETC_DIR)/ndm/netfilter.d/
endif
endif

	$(INSTALL_DIR) $(1)/$(INST_USRSHARE_DIR)/magitrickle/skins/default
	$(CP) $(CURDIR)/frontend/* $(1)/$(INST_USRSHARE_DIR)/magitrickle/skins/default/
endef

ifneq (,$(filter $(PLATFORM),entware entware_kn))
define Package/$(PKG_NAME)/postinst
#!/bin/sh

$(INST_ETC_DIR)/init.d/S99magitrickle restart

exit 0
endef
endif

$(eval $(call GoBinPackage,$(PKG_NAME)))
$(eval $(call BuildPackage,$(PKG_NAME)))
