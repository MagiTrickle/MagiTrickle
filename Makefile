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

TARGET ?= mipsel-3.4
GOOS ?= linux
GOARCH ?= mipsle
GOMIPS ?= softfloat
GOARM ?=

BUILD_DIR = ./.build
PKG_DIR = $(BUILD_DIR)/$(TARGET)
BIN_DIR = $(PKG_DIR)/data/opt/bin
PARAMS = -v -a -trimpath -ldflags="-X 'magitrickle/constant.Version=$(UPSTREAM_VERSION)$(PRERELEASE_POSTFIX)' -X 'magitrickle/constant.Commit=$(COMMIT)' -w -s"

all: clear build_daemon package

clear:
	echo $(shell git rev-parse --abbrev-ref HEAD)
	rm -rf $(PKG_DIR)

build_daemon:
	GOOS=$(GOOS) GOARCH=$(GOARCH) GOMIPS=$(GOMIPS) GOARM=$(GOARM) go build $(PARAMS) -o $(BIN_DIR)/magitrickled ./cmd/magitrickled

package:
	@mkdir -p $(PKG_DIR)/control
	@echo '2.0' > $(PKG_DIR)/debian-binary
	@echo 'Package: $(APP_NAME)' > $(PKG_DIR)/control/control
	@echo 'Version: $(UPSTREAM_VERSION)$(PRERELEASE_POSTFIX)-$(PKG_REVISION)' >> $(PKG_DIR)/control/control
	@echo 'Architecture: $(TARGET)' >> $(PKG_DIR)/control/control
	@echo 'Maintainer: $(APP_MAINTAINER)' >> $(PKG_DIR)/control/control
	@echo 'Description: $(APP_DESCRIPTION)' >> $(PKG_DIR)/control/control
	@echo 'Section: net' >> $(PKG_DIR)/control/control
	@echo 'Priority: optional' >> $(PKG_DIR)/control/control
	@echo 'Depends: libc, iptables, socat' >> $(PKG_DIR)/control/control
	@cp -r ./opt $(PKG_DIR)/data/
	@fakeroot sh -c "tar -C $(PKG_DIR)/control -czvf $(PKG_DIR)/control.tar.gz ."
	@fakeroot sh -c "tar -C $(PKG_DIR)/data -czvf $(PKG_DIR)/data.tar.gz ."
	@tar -C $(PKG_DIR) -czvf $(BUILD_DIR)/$(APP_NAME)_$(UPSTREAM_VERSION)$(PRERELEASE_POSTFIX)-$(PKG_REVISION)_$(TARGET).ipk ./debian-binary ./control.tar.gz ./data.tar.gz
