include $(TOPDIR)/rules.mk

PKG_NAME:=libcjson
PKG_VERSION:=1.7.18
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/DaveGamble/cJSON.git
PKG_SOURCE_VERSION:=v$(PKG_VERSION)
PKG_MIRROR_HASH:=skip

PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE

PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/libcjson
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=Ultralightweight JSON parser in ANSI C
  URL:=https://github.com/DaveGamble/cJSON
endef

define Package/libcjson/description
 cJSON, vendored as an in-tree SDK package because it isn't in any
 OpenWrt feed for this release (confirmed via CI: "WARNING: No feed for
 package 'libcjson' found" -- see docs/c-rewrite/decisions.md D-50).
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -c -o $(PKG_BUILD_DIR)/cJSON.o $(PKG_BUILD_DIR)/cJSON.c
	$(TARGET_AR) rc $(PKG_BUILD_DIR)/libcjson.a $(PKG_BUILD_DIR)/cJSON.o
	$(TARGET_RANLIB) $(PKG_BUILD_DIR)/libcjson.a
endef

define Package/libcjson/install
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/cjson
	$(CP) $(PKG_BUILD_DIR)/cJSON.h $(1)/usr/include/cjson/
	$(INSTALL_DIR) $(1)/usr/lib
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/libcjson.a $(1)/usr/lib/
endef

$(eval $(call BuildPackage,libcjson))
