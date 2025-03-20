APP_NAME = magitrickle
APP_DESCRIPTION = DNS-based routing application
APP_MAINTAINER = Vladimir Avtsenov <vladimir.lsk.cool@gmail.com>

COMMIT = $(shell git rev-parse --short HEAD)
UPSTREAM_VERSION = $(shell git describe --tags --abbrev=0 2> /dev/null || echo "0.0.0")
PKG_REVISION ?= 1

TAG = $(shell git describe --tags --abbrev=0 2> /dev/null)
COMMITS_SINCE_TAG = $(shell git rev-list ${TAG}..HEAD --count 2>/dev/null)
PRERELEASE_POSTFIX =
PRERELEASE_DATE = $(shell date +%Y%m%d)
ifneq ($(TAG),)
    ifneq ($(COMMITS_SINCE_TAG), 0)
        PRERELEASE_POSTFIX = ~git$(PRERELEASE_DATE).$(COMMIT)
    endif
else
    PRERELEASE_POSTFIX = ~git$(PRERELEASE_DATE).$(COMMIT)
endif

PLATFORM ?= entware
TARGET ?= mipsel-3.4
GOOS ?= linux
GOARCH ?= mipsle
GOMIPS ?= softfloat
GOARM ?=

GO_FLAGS = GOOS=$(GOOS) GOARCH=$(GOARCH) GOMIPS=$(GOMIPS) GOARM=$(GOARM)
GO_TAGS ?= kn
ifeq ($(PLATFORM),entware)
	GO_TAGS += entware
else ifeq ($(PLATFORM),openwrt)
	GO_TAGS += openwrt
endif

BUILD_DIR = ./.build
PKG_DIR = $(BUILD_DIR)/$(TARGET)
ifeq ($(PLATFORM),entware)
    BIN_DIR = $(PKG_DIR)/data/opt/bin
    SKINS_DIR = $(PKG_DIR)/data/opt/usr/share/magitrickle/skins
    DEPENDS = libc, iptables, socat
else ifeq ($(PLATFORM),openwrt)
    BIN_DIR = $(PKG_DIR)/data/usr/bin
    SKINS_DIR = $(PKG_DIR)/data/usr/share/magitrickle/skins
    DEPENDS = +libc +iptables +socat
endif

PARAMS = -v -a -trimpath -ldflags="-X 'magitrickle/constant.Version=$(UPSTREAM_VERSION)$(PRERELEASE_POSTFIX)' -X 'magitrickle/constant.Commit=$(COMMIT)' -w -s" -tags "$(GO_TAGS)"

all: clear build package

clear:
	@echo "Building on branch: $(shell git rev-parse --abbrev-ref HEAD)"
	@rm -rf $(PKG_DIR)

build_backend:
	$(GO_FLAGS) go build -C ./backend $(PARAMS) -o ../$(BIN_DIR)/magitrickled ./cmd/magitrickled
	upx -9 --lzma $(BIN_DIR)/magitrickled

build_frontend_legacy:
	cd ./frontend_legacy && npm install
	cd ./frontend_legacy && npm run build
	mkdir -p $(SKINS_DIR)/legacy
	cp -r ./frontend_legacy/dist/* $(SKINS_DIR)/legacy/

build_frontend:
	cd ./frontend && npm install
	cd ./frontend && VITE_UPSTREAM_VERSION=$(UPSTREAM_VERSION) VITE_DEV=$(if $(strip $(PRERELEASE_POSTFIX)),true,false) npm run build
	mkdir -p $(SKINS_DIR)/default
	cp -r ./frontend/dist/* $(SKINS_DIR)/default/

build: build_backend build_frontend_legacy build_frontend

package:
	mkdir -p $(PKG_DIR)/control
	echo '2.0' > $(PKG_DIR)/debian-binary
	echo 'Package: $(APP_NAME)' > $(PKG_DIR)/control/control
	echo 'Version: $(UPSTREAM_VERSION)$(PRERELEASE_POSTFIX)-$(PKG_REVISION)' >> $(PKG_DIR)/control/control
	echo 'Architecture: $(TARGET)' >> $(PKG_DIR)/control/control
	echo 'Maintainer: $(APP_MAINTAINER)' >> $(PKG_DIR)/control/control
	echo 'Description: $(APP_DESCRIPTION)' >> $(PKG_DIR)/control/control
	echo 'Section: net' >> $(PKG_DIR)/control/control
	echo 'Priority: optional' >> $(PKG_DIR)/control/control
ifeq ($(PLATFORM),entware)
	cp -r ./opt $(PKG_DIR)/data/
else ifeq ($(PLATFORM),openwrt)
	mkdir -p $(PKG_DIR)/data
	cp -r ./opt/* $(PKG_DIR)/data/
	sed -i 's|/opt||g' $(PKG_DIR)/data/etc/init.d/S99magitrickle
	sed -i 's|/opt||g' $(PKG_DIR)/data/etc/ndm/netfilter.d/100-magitrickle
endif
	tar -C $(PKG_DIR)/control -czvf $(PKG_DIR)/control.tar.gz --owner=0 --group=0 .
	tar -C $(PKG_DIR)/data -czvf $(PKG_DIR)/data.tar.gz --owner=0 --group=0 .
	tar -C $(PKG_DIR) -czvf $(BUILD_DIR)/$(APP_NAME)_$(UPSTREAM_VERSION)$(PRERELEASE_POSTFIX)-$(PKG_REVISION)_$(TARGET).ipk --owner=0 --group=0 ./debian-binary ./control.tar.gz ./data.tar.gz