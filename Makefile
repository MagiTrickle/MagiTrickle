-include .config

#
# Environment
#

# Package info

PKG_NAME := magitrickle
PKG_DESCRIPTION := DNS-based routing application
PKG_MAINTAINER := Vladimir Avtsenov <vladimir.lsk.cool@gmail.com>

ifeq ($(strip $(PKG_VERSION)),)
	TAG := $(shell git describe --tags --abbrev=0 2> /dev/null)
	PKG_VERSION := $(if $(TAG),$(shell echo "$(TAG)" | sed 's/-rev[0-9]*$$//'),0.0.0)
	TAG_RELEASE := $(shell echo "$(TAG)" | grep -oE 'rev[0-9]+$$' | sed 's/rev//')
	ifneq ($(strip $(TAG_RELEASE)),)
		PKG_REVISION ?= $(TAG_RELEASE)
	endif

	COMMITS_SINCE_TAG := $(shell [ -n "$(TAG)" ] && git rev-list $(TAG)..HEAD --count 2>/dev/null || echo 0)
	ifneq ($(or $(filter-out 0,$(COMMITS_SINCE_TAG)),$(if $(TAG),,1)),)
		PKG_VERSION_PRERELEASE := $(shell v=$(PKG_VERSION); echo $${v%.*}.$$(( $${v##*.} + 1 )) )
		PRERELEASE_DATE := $(shell date +%Y%m%d%H%M%S)
		COMMIT := $(shell git rev-parse --short HEAD)

		PKG_VERSION := $(PKG_VERSION_PRERELEASE)~git$(PRERELEASE_DATE).$(COMMIT)
	endif
endif
PKG_REVISION ?= 1

# Directories

BUILDS_DIR := ./.build
STAMPS_DIR := $(BUILDS_DIR)/.stamps

UNIQUE_NAME := $(PLATFORM)_$(TARGET)
BUILD_DIR := $(BUILDS_DIR)/$(UNIQUE_NAME)
COMPILE_DIR := $(BUILD_DIR)/compile

ROOT_DIR := $(BUILD_DIR)/root
BIN_DIR := $(ROOT_DIR)/bin
ETC_DIR := $(ROOT_DIR)/etc
USRSHARE_DIR := $(ROOT_DIR)/usr/share
STATE_DIR := $(ROOT_DIR)/var/lib/magitrickle

ifeq ($(PLATFORM),entware)
	BIN_DIR := $(ROOT_DIR)/opt/bin
	ETC_DIR := $(ROOT_DIR)/opt/etc
	USRSHARE_DIR := $(ROOT_DIR)/opt/usr/share
	STATE_DIR := $(ROOT_DIR)/opt/var/lib/magitrickle

	GO_TAGS += entware
	ifeq ($(filter %_kn,$(TARGET)),$(TARGET))
		GO_TAGS += entware_kn
	endif
endif

ifeq ($(PLATFORM),openwrt)
	BIN_DIR := $(ROOT_DIR)/usr/bin
	ETC_DIR := $(ROOT_DIR)/etc
	USRSHARE_DIR := $(ROOT_DIR)/usr/share
	STATE_DIR := $(ROOT_DIR)/etc/magitrickle/state

	GO_TAGS += openwrt
endif

IPK_DIR := $(BUILD_DIR)/ipk
IPK_CONTROL_DIR := $(IPK_DIR)/control

APK_DIR := $(BUILD_DIR)/apk

# Build properties

GO_FLAGS := \
	$(if $(GOOS),GOOS="$(GOOS)") \
	$(if $(GOARCH),GOARCH="$(GOARCH)") \
	$(if $(GOMIPS),GOMIPS="$(GOMIPS)") \
	$(if $(GOARM),GOARM="$(GOARM)") \
	$(if $(GO386),GO386="$(GO386)") \

GO_PARAMS = -v -trimpath -ldflags="-X 'magitrickle/constant.Version=$(PKG_VERSION)' -w -s" $(if $(GO_TAGS),-tags "$(GO_TAGS)")

# Incremental data

BACKEND_DEPENDENCIES := ./src/backend/go.mod ./src/backend/go.sum
BACKEND_SOURCES := $(shell find ./src/backend -type f -name '*.go' 2>/dev/null)
BACKEND_SOURCES += $(BACKEND_DEPENDENCIES)
BACKEND_BUILD_PROPERTIES := PLATFORM=$(PLATFORM) TARGET=$(TARGET) GOOS=$(GOOS) GOARCH=$(GOARCH) GOMIPS=$(GOMIPS) GOARM=$(GOARM) GO386=$(GO386) GO_TAGS=$(GO_TAGS) PKG_VERSION=$(PKG_VERSION)

FRONTEND_DEPENDENCIES := ./src/frontend/package.json ./src/frontend/package-lock.json
FRONTEND_SOURCES := $(shell find ./src/frontend/src -type f 2>/dev/null)
FRONTEND_SOURCES += ./src/frontend/vite.config.ts ./src/frontend/tsconfig.json
FRONTEND_SOURCES += $(FRONTEND_DEPENDENCIES)
FRONTEND_BUILD_PROPERTIES := PKG_VERSION=$(PKG_VERSION) PKG_VERSION_PRERELEASE=$(PKG_VERSION_PRERELEASE)

#
# Targets
#

.PHONY: all clear clean download download_backend download_frontend redownload redownload_backend redownload_frontend build build_backend build_frontend rebuild rebuild_backend rebuild_frontend prepare_files package package_ipk FORCE

all: download build package

clear:
	rm -rf ./src/frontend/dist
	rm -rf "$(BUILD_DIR)"

clean:
	rm -rf "$(BUILDS_DIR)"

redownload: redownload_backend redownload_frontend

download: download_backend download_frontend

rebuild: rebuild_backend rebuild_frontend

build: build_backend build_frontend

# Backend

$(STAMPS_DIR)/download-backend: $(BACKEND_DEPENDENCIES)
	cd ./src/backend && go mod tidy

	@mkdir -p $(STAMPS_DIR)
	@touch "$(STAMPS_DIR)/download-backend"

download_backend: $(STAMPS_DIR)/download-backend

redownload_backend:
	@rm -f "$(STAMPS_DIR)/download-backend"
	$(MAKE) download_backend

$(STAMPS_DIR)/build-properties-backend-$(UNIQUE_NAME): FORCE
	@mkdir -p $(STAMPS_DIR)
	@echo "$(BACKEND_BUILD_PROPERTIES)" | cmp -s - $@ || echo "$(BACKEND_BUILD_PROPERTIES)" > $@

$(STAMPS_DIR)/build-backend-$(UNIQUE_NAME): $(STAMPS_DIR)/download-backend $(BACKEND_SOURCES) $(STAMPS_DIR)/build-properties-backend-$(UNIQUE_NAME)
	mkdir -p "$(COMPILE_DIR)"
	cd ./src/backend && $(GO_FLAGS) go build $(GO_PARAMS) -o "../../$(COMPILE_DIR)/magitrickled" ./cmd/magitrickled
ifneq ($(filter $(GOARCH),riscv64 mips64 mips64le loong64),$(GOARCH))
	upx -9 --lzma "$(COMPILE_DIR)/magitrickled"
endif

	@mkdir -p $(STAMPS_DIR)
	@touch "$(STAMPS_DIR)/build-backend-$(UNIQUE_NAME)"

build_backend: $(STAMPS_DIR)/build-backend-$(UNIQUE_NAME)

rebuild_backend:
	@rm -f "$(STAMPS_DIR)/build-backend"
	$(MAKE) build_backend

# Frontend

$(STAMPS_DIR)/download-frontend: $(FRONTEND_DEPENDENCIES)
	cd ./src/frontend && npm install

	@mkdir -p $(STAMPS_DIR)
	@touch "$(STAMPS_DIR)/download-frontend"

download_frontend: $(STAMPS_DIR)/download-frontend

redownload_frontend:
	@rm -f "$(STAMPS_DIR)/download-frontend"
	$(MAKE) download_frontend

$(STAMPS_DIR)/build-properties-frontend: FORCE
	@mkdir -p $(STAMPS_DIR)
	@echo "$(FRONTEND_BUILD_PROPERTIES)" | cmp -s - $@ || echo "$(FRONTEND_BUILD_PROPERTIES)" > $@

$(STAMPS_DIR)/build-frontend: $(STAMPS_DIR)/download-frontend $(FRONTEND_SOURCES) $(STAMPS_DIR)/build-properties-frontend
	cd ./src/frontend && VITE_PKG_VERSION="$(PKG_VERSION)" VITE_PKG_VERSION_IS_DEV=$(if $(PKG_VERSION_PRERELEASE),true,false) npm run build

	@mkdir -p $(STAMPS_DIR)
	@touch "$(STAMPS_DIR)/build-frontend"

build_frontend: $(STAMPS_DIR)/build-frontend

rebuild_frontend:
	@rm -f "$(STAMPS_DIR)/build-frontend"
	$(MAKE) build_frontend

# Packaging

define _copy_files
	if [ -d $(1)/_ipk/control ]; then mkdir -p $(IPK_CONTROL_DIR); cp -r $(1)/_ipk/control/* $(IPK_CONTROL_DIR); fi
	if [ -d $(1)/bin ]; then mkdir -p $(BIN_DIR); cp -r $(1)/bin/* $(BIN_DIR); fi
	if [ -d $(1)/etc ]; then mkdir -p $(ETC_DIR); cp -r $(1)/etc/* $(ETC_DIR); fi
	if [ -d $(1)/usr/share ]; then mkdir -p $(USRSHARE_DIR); cp -r $(1)/usr/share/* $(USRSHARE_DIR); fi
	if [ -d $(1)/var/lib/magitrickle ]; then mkdir -p $(STATE_DIR); cp -r $(1)/var/lib/magitrickle/* $(STATE_DIR); fi
endef

prepare_files: build
	rm -rf "$(ROOT_DIR)"
	mkdir -p "$(BIN_DIR)"
	cp "$(COMPILE_DIR)/magitrickled" "$(BIN_DIR)/magitrickled"
	mkdir -p "$(USRSHARE_DIR)/magitrickle/skins/default"
	cp -r ./src/frontend/dist/* "$(USRSHARE_DIR)/magitrickle/skins/default"
	$(call _copy_files,./files/common)
	$(if $(filter entware,$(PLATFORM)), $(call _copy_files,./files/entware))
	$(if $(filter entware,$(PLATFORM)), $(if $(filter %_kn,$(TARGET)), $(call _copy_files,./files/entware_kn)))
	$(if $(filter openwrt,$(PLATFORM)), $(call _copy_files,./files/openwrt))

package:
ifeq ($(PLATFORM),openwrt)
	$(MAKE) package_ipk
endif
ifeq ($(PLATFORM),entware)
	$(MAKE) package_ipk
endif

package_ipk: prepare_files
	mkdir -p "$(IPK_DIR)"
	echo '2.0' > $(IPK_DIR)/debian-binary

	mkdir -p $(IPK_CONTROL_DIR)
	echo 'Package: $(PKG_NAME)' > $(IPK_CONTROL_DIR)/control
	echo 'Version: $(PKG_VERSION)-$(PKG_REVISION)' >> $(IPK_CONTROL_DIR)/control
	echo 'Architecture: $(TARGET)' >> $(IPK_CONTROL_DIR)/control
	echo 'Maintainer: $(PKG_MAINTAINER)' >> $(IPK_CONTROL_DIR)/control
	echo 'Description: $(PKG_DESCRIPTION)' >> $(IPK_CONTROL_DIR)/control
	echo 'Section: net' >> $(IPK_CONTROL_DIR)/control
	echo 'Priority: optional' >> $(IPK_CONTROL_DIR)/control
ifeq ($(PLATFORM),entware)
	@DEPS="libc, iptables"; \
	if echo "$(TARGET)" | grep -q '_kn$$'; then \
		DEPS="$$DEPS, socat"; \
	fi; \
	echo "Depends: $$DEPS" >> $(IPK_CONTROL_DIR)/control
endif
ifeq ($(PLATFORM),openwrt)
	echo "Depends: libc, iptables-nft, iptables-mod-conntrack-extra, kmod-ipt-nat, kmod-ipt-ipset, ip6tables-nft" >> $(IPK_CONTROL_DIR)/control
endif

ifeq ($(PLATFORM),entware)
	echo "/opt/var/lib/magitrickle/config.yaml" >> $(IPK_CONTROL_DIR)/conffiles
endif
ifeq ($(PLATFORM),openwrt)
	echo "/etc/config/magitrickle" >> $(IPK_CONTROL_DIR)/conffiles
	echo "/etc/magitrickle/state/config.yaml" >> $(IPK_CONTROL_DIR)/conffiles
endif

	tar -C "$(IPK_CONTROL_DIR)" -czvf "$(IPK_DIR)/control.tar.gz" --owner=0 --group=0 .
	tar -C "$(ROOT_DIR)" -czvf "$(IPK_DIR)/data.tar.gz" --owner=0 --group=0 .
	tar -C "$(IPK_DIR)" -czvf "$(BUILDS_DIR)/$(PKG_NAME)_$(PKG_VERSION)-$(PKG_REVISION)_$(UNIQUE_NAME).ipk" --owner=0 --group=0 ./debian-binary ./control.tar.gz ./data.tar.gz

FORCE:
