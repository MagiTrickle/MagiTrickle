-include .config

PKG_NAME := magitrickle
PKG_DESCRIPTION := DNS-based routing application
PKG_MAINTAINER := Vladimir Avtsenov <vladimir.lsk.cool@gmail.com>

ifeq ($(strip $(PKG_VERSION)),)
	TAG := $(shell git describe --tags --abbrev=0 2> /dev/null)
	PKG_VERSION := $(shell echo "$(TAG)" | sed 's/-rev[0-9]*$$//' 2> /dev/null || echo "0.0.0")
	TAG_RELEASE := $(shell echo "$(TAG)" | grep -oE 'rev[0-9]+$$' | sed 's/rev//')
	ifneq ($(strip $(TAG_RELEASE)),)
		PKG_REVISION ?= $(TAG_RELEASE)
	endif

	COMMITS_SINCE_TAG := $(shell [ -n "$(TAG)" ] && git rev-list $(TAG)..HEAD --count 2>/dev/null || echo 0)
	ifneq ($(or $(COMMITS_SINCE_TAG),$(if $(TAG),,1)),0)
		PKG_VERSION_PRERELEASE := $(shell v=$(PKG_VERSION); echo $${v%.*}.$$(( $${v##*.} + 1 )) )
		PRERELEASE_DATE := $(shell date +%Y%m%d%H%M%S)
		COMMIT := $(shell git rev-parse --short HEAD)

		PKG_VERSION := $(PKG_VERSION_PRERELEASE)~git$(PRERELEASE_DATE).$(COMMIT)
	endif
endif

PKG_REVISION ?= 1

BUILDS_DIR := ./.build

BUILD_DIR := $(BUILDS_DIR)/$(PLATFORM)_$(TARGET)

DATA_DIR := $(BUILD_DIR)/data
CONTROL_DIR := $(BUILD_DIR)/control

BIN_DIR := $(DATA_DIR)/bin
ETC_DIR := $(DATA_DIR)/etc
USRSHARE_DIR := $(DATA_DIR)/usr/share
STATE_DIR := $(DATA_DIR)/var/lib/magitrickle

ifeq ($(PLATFORM),entware)
	BIN_DIR := $(DATA_DIR)/opt/bin
	ETC_DIR := $(DATA_DIR)/opt/etc
	USRSHARE_DIR := $(DATA_DIR)/opt/usr/share
	STATE_DIR := $(DATA_DIR)/opt/var/lib/magitrickle

	GO_TAGS += entware
	ifeq ($(filter %_kn,$(TARGET)),$(TARGET))
		GO_TAGS += entware_kn
	endif
endif

ifeq ($(PLATFORM),openwrt)
	BIN_DIR := $(DATA_DIR)/usr/bin
	ETC_DIR := $(DATA_DIR)/etc
	USRSHARE_DIR := $(DATA_DIR)/usr/share
	STATE_DIR := $(DATA_DIR)/etc/magitrickle/state

	GO_TAGS += openwrt
endif

GO_FLAGS := \
	$(if $(GOOS),GOOS="$(GOOS)") \
	$(if $(GOARCH),GOARCH="$(GOARCH)") \
	$(if $(GOMIPS),GOMIPS="$(GOMIPS)") \
	$(if $(GOARM),GOARM="$(GOARM)") \
	$(if $(GO386),GO386="$(GO386)") \

GO_PARAMS = -v -trimpath -ldflags="-X 'magitrickle/constant.Version=$(PKG_VERSION)' -w -s" $(if $(GO_TAGS),-tags "$(GO_TAGS)")

all: clear build package

clear:
	rm -rf ./src/frontend/dist
	rm -rf "$(BUILD_DIR)"

build: build_backend build_frontend

build_backend:
	cd ./src/backend && go mod tidy
	mkdir -p "$(BIN_DIR)"
	cd ./src/backend && $(GO_FLAGS) go build $(GO_PARAMS) -o "../../$(BIN_DIR)/magitrickled" ./cmd/magitrickled
ifneq ($(filter $(GOARCH),riscv64 mips64 mips64le loong64),$(GOARCH))
	upx -9 --lzma "$(BIN_DIR)/magitrickled"
endif

build_frontend:
	cd ./src/frontend && npm install
	cd ./src/frontend && VITE_PKG_VERSION="$(PKG_VERSION)" VITE_PKG_VERSION_IS_DEV=$(if $(PKG_VERSION_PRERELEASE),true,false) npm run build
	mkdir -p "$(USRSHARE_DIR)/magitrickle/skins/default"
	cp -r ./src/frontend/dist/* "$(USRSHARE_DIR)/magitrickle/skins/default"

define _copy_files
	if [ -d $(1)/_ipk/control ]; then mkdir -p $(BUILD_DIR)/control; cp -r $(1)/_ipk/control/* $(BUILD_DIR)/control; fi
	if [ -d $(1)/bin ]; then mkdir -p $(BIN_DIR); cp -r $(1)/bin/* $(BIN_DIR); fi
	if [ -d $(1)/etc ]; then mkdir -p $(ETC_DIR); cp -r $(1)/etc/* $(ETC_DIR); fi
	if [ -d $(1)/usr/share ]; then mkdir -p $(USRSHARE_DIR); cp -r $(1)/usr/share/* $(USRSHARE_DIR); fi
	if [ -d $(1)/var/lib/magitrickle ]; then mkdir -p $(STATE_DIR); cp -r $(1)/var/lib/magitrickle/* $(STATE_DIR); fi
endef

package:
ifeq ($(PLATFORM),openwrt)
	$(MAKE) package_ipk
endif
ifeq ($(PLATFORM),entware)
	$(MAKE) package_ipk
endif

package_ipk:
	echo '2.0' > $(BUILD_DIR)/debian-binary

	mkdir -p $(BUILD_DIR)/control
	echo 'Package: $(PKG_NAME)' > $(BUILD_DIR)/control/control
	echo 'Version: $(PKG_VERSION)-$(PKG_REVISION)' >> $(BUILD_DIR)/control/control
	echo 'Architecture: $(TARGET)' >> $(BUILD_DIR)/control/control
	echo 'Maintainer: $(PKG_MAINTAINER)' >> $(BUILD_DIR)/control/control
	echo 'Description: $(PKG_DESCRIPTION)' >> $(BUILD_DIR)/control/control
	echo 'Section: net' >> $(BUILD_DIR)/control/control
	echo 'Priority: optional' >> $(BUILD_DIR)/control/control
ifeq ($(PLATFORM),entware)
	@DEPS="libc, iptables"; \
	if echo "$(TARGET)" | grep -q '_kn$$'; then \
		DEPS="$$DEPS, socat"; \
	fi; \
	echo "Depends: $$DEPS" >> $(BUILD_DIR)/control/control
endif
ifeq ($(PLATFORM),openwrt)
	echo "Depends: libc, iptables-nft, iptables-mod-conntrack-extra, kmod-ipt-nat, kmod-ipt-ipset, ip6tables-nft" >> $(BUILD_DIR)/control/control
endif

ifeq ($(PLATFORM),entware)
	echo "/opt/var/lib/magitrickle/config.yaml" >> $(BUILD_DIR)/control/conffiles
endif
ifeq ($(PLATFORM),openwrt)
	echo "/etc/config/magitrickle" >> $(BUILD_DIR)/control/conffiles
	echo "/etc/magitrickle/state/config.yaml" >> $(BUILD_DIR)/control/conffiles
endif

	$(call _copy_files,./files/common)
	$(if $(filter entware,$(PLATFORM)), $(call _copy_files,./files/entware))
	$(if $(filter entware,$(PLATFORM)), $(if $(filter %_kn,$(TARGET)), $(call _copy_files,./files/entware_kn)))
	$(if $(filter openwrt,$(PLATFORM)), $(call _copy_files,./files/openwrt))

	tar -C "$(BUILD_DIR)/control" -czvf "$(BUILD_DIR)/control.tar.gz" --owner=0 --group=0 .
	tar -C "$(BUILD_DIR)/data" -czvf "$(BUILD_DIR)/data.tar.gz" --owner=0 --group=0 .
	tar -C "$(BUILD_DIR)" -czvf "$(BUILDS_DIR)/$(PKG_NAME)_$(PKG_VERSION)-$(PKG_REVISION)_$(PLATFORM)_$(TARGET).ipk" --owner=0 --group=0 ./debian-binary ./control.tar.gz ./data.tar.gz
