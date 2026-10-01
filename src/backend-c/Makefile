# MagiTrickle C backend — build system.
#
# Host build:        make            (-> build/host/magitrickled-c)
# Tests:             make test
# Sanitizers:        make sanitize   (ASan+UBSan build of tests, host only)
# Static analysis:   make static_analysis (clang-tidy + cppcheck if present)
# Cross build:       make CROSS_COMPILE=mipsel-linux-gnu- \
#                         SYSROOT=/path/to/sysroot BUILD=cross-mipsel
#   Per-target CFLAGS/LDFLAGS come from the toolchain descriptor
#   (docs/c-rewrite/toolchains.md); never use -march=native. SYSROOT must
#   provide the 5 feed deps (libyaml, cJSON, PCRE2, libmnl, libcurl) built
#   for that target/libc, or the link step fails -- this Makefile builds
#   the full daemon (not just the dependency-free core) for any
#   CROSS_COMPILE target given a sysroot with those 5 libs (Phase 8; see
#   decisions.md D-37/D-38 for which real targets have been validated this
#   way in this sandbox, and which remain unverified pending real
#   Entware/OpenWrt toolchain+feed provisioning).

CROSS_COMPILE ?=
CC       := $(CROSS_COMPILE)gcc
BUILD    ?= $(if $(CROSS_COMPILE),cross-$(patsubst %-,%,$(CROSS_COMPILE)),host)
BUILDDIR := build/$(BUILD)
MT_VERSION ?= unattached

# Set to any non-empty value to enable Keenetic (Entware `_kn`) platform
# behavior -- currently just the ignored-interfaces list in api/app.c
# (mirrors Go's constant.IgnoredInterfaces under its entware_kn build
# tag). Root Makefile derives this from PLATFORM/TARGET the same way it
# derives GO_TAGS' entware_kn tag.
ENTWARE_KN ?=

# Target platform (entware|openwrt), passed through by the root Makefile.
# Selects the compiled-in filesystem path defaults in paths.h (Entware
# under /opt, OpenWrt under /etc/magitrickle/state) -- the C equivalent of
# Go's entware/openwrt build tags. Empty (host builds, tests) keeps the
# default /var/... paths.
PLATFORM ?=

WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 \
            -Wundef -Wstrict-prototypes -Wmissing-prototypes
# NOTE: -Werror is enabled in CI only (see .github/workflows), not forced
# on local builds yet (spec §20).
CSTD     := -std=c11
OPT      ?= -O2
CFLAGS   ?=
# POSIX.1-2008 API surface project-wide; GNU extensions only inside
# src/platform/ (which defines _GNU_SOURCE itself).
CFLAGS   += $(CSTD) $(OPT) $(WARNINGS) -Iinclude -MMD -MP \
            -D_POSIX_C_SOURCE=200809L \
            -DMT_VERSION=\"$(MT_VERSION)\" -fno-common \
            $(if $(ENTWARE_KN),-DMT_ENTWARE_KN) \
            $(if $(filter entware,$(PLATFORM)),-DMT_PLATFORM_ENTWARE) \
            $(if $(filter openwrt,$(PLATFORM)),-DMT_PLATFORM_OPENWRT)
# CI passes CFLAGS_EXTRA=-Werror; local builds stay warning-tolerant.
CFLAGS_EXTRA ?=
CFLAGS   += $(CFLAGS_EXTRA)
LDFLAGS  ?=
LDFLAGS_EXTRA ?=
LDFLAGS  += $(LDFLAGS_EXTRA)
# -latomic: src/dns/proxy.c uses 8-byte C11 atomics (counters/deadlines);
# 32-bit targets (mipsel, arm, ...) lack native 64-bit atomic instructions
# and lower __atomic_* calls to libatomic, which glibc/musl toolchains
# ship but the linker won't pull in implicitly.
LDLIBS   := -lpthread -latomic
ifneq ($(SYSROOT),)
CFLAGS  += --sysroot=$(SYSROOT)
LDFLAGS += --sysroot=$(SYSROOT)
endif

# Core: no external library dependencies (cross-buildable everywhere),
# includes the DNS wire codec, the epoll proxy transport, and the records
# cache (no deps of its own). The iptables save/restore engine is also
# dependency-free (POSIX: posix_spawn/poll/fcntl only), so it lives here
# too, not in CONFIG_SRCS.
CORE_SRCS := \
    src/util/err.c \
    src/util/lookup.c \
    src/util/queue.c \
    src/util/bytebuf.c \
    src/util/cancel.c \
    src/util/rand.c \
    src/core/lifecycle.c \
    src/logging/log.c \
    src/platform/loop.c \
    src/dns/wire.c \
    src/dns/proxy.c \
    src/dns_cache/cache.c \
    src/iptables/rule.c \
    src/iptables/chain_patch.c \
    src/iptables/chain_override.c \
    src/iptables/chain_delete.c \
    src/iptables/engine.c \
    src/iptables/executable_real.c \
    src/crypto/md5.c \
    src/crypto/sha256.c \
    src/crypto/sha512.c \
    src/crypto/hmac.c \
    src/crypto/base64.c \
    src/crypto/crypt.c

# Config/rules/subscriptions: need libyaml + libpcre2-8 (host + feed
# sysroots; the cross skeleton target builds core only until Phase 8
# toolchain descriptors provide the dep sysroots).
CONFIG_SRCS := \
    src/config/duration.c \
    src/config/id.c \
    src/config/models.c \
    src/config/yaml_scalar.c \
    src/config/yaml_load.c \
    src/config/yaml_save.c \
    src/rules/wildcard.c \
    src/rules/match.c \
    src/rules/snapshot.c \
    src/subscriptions/subparse.c \
    src/subscriptions/runtime.c \
    src/dns/pipeline.c

# Netfilter: ipset via libmnl (NFNETLINK/NFNL_SUBSYS_IPSET). Needs libmnl
# (host + feed sysroots; same cross-build caveat as CONFIG_SRCS -- Phase 8
# provides the feed sysroots for the full cross daemon build).
NETFILTER_SRCS := \
    src/netfilter/ipset.c \
    src/netfilter/ipset_nl_real.c \
    src/netfilter/ipset_to_link.c \
    src/netfilter/port_remap.c \
    src/netfilter/cleaner.c \
    src/netfilter/committer.c \
    src/netfilter/ruleset.c \
    src/netlink/nlattr_iter.c \
    src/netlink/rtnl.c \
    src/netlink/watcher.c

# API: HTTP/Unix JSON API (Phase 6). Needs cJSON (host + feed sysroots,
# same cross-build caveat as CONFIG_SRCS/NETFILTER_SRCS). jwt.c lives here
# (not in the dependency-free CORE_SRCS crypto/ group) because it needs
# cJSON for claims (de)serialization, even though it's a "crypto" file.
API_SRCS := \
    src/api/json.c \
    src/api/httpd.c \
    src/api/auth.c \
    src/api/app.c \
    src/api/groups.c \
    src/api/system.c \
    src/api/staticfiles.c \
    src/api/subscriptions_api.c \
    src/crypto/jwt.c

# Subscriptions: fetch + runtime rule sets + sync orchestration
# (Phase 7). Needs libcurl (host + feed sysroots, same cross-build
# caveat as CONFIG_SRCS/NETFILTER_SRCS/API_SRCS).
SUBSCRIPTIONS_SRCS := \
    src/subscriptions/fetch.c \
    src/subscriptions/fetch_async.c

# Router-specific integrations. Keenetic's RCI interface-alias lookup
# needs both libcurl and cJSON (same cross-build caveat as the groups
# above); it is compiled everywhere but only talks to RCI when built
# with -DMT_ENTWARE_KN, mirroring Go's entware_kn build tag.
INTERFACES_SRCS := \
    src/interfaces/keenetic_rci.c

LIB_SRCS := $(CORE_SRCS) $(CONFIG_SRCS) $(NETFILTER_SRCS) $(API_SRCS) $(SUBSCRIPTIONS_SRCS) \
            $(INTERFACES_SRCS)

MAIN_SRC := src/main/main.c
CONFIGTOOL_SRC := src/tools/configtool.c
DNSTOOL_SRC := src/tools/dnstool.c
CACHETOOL_SRC := src/tools/cachetool.c

CORE_OBJS := $(CORE_SRCS:%.c=$(BUILDDIR)/%.o)
CONFIG_OBJS := $(CONFIG_SRCS:%.c=$(BUILDDIR)/%.o)
NETFILTER_OBJS := $(NETFILTER_SRCS:%.c=$(BUILDDIR)/%.o)
API_OBJS := $(API_SRCS:%.c=$(BUILDDIR)/%.o)
SUBSCRIPTIONS_OBJS := $(SUBSCRIPTIONS_SRCS:%.c=$(BUILDDIR)/%.o)
INTERFACES_OBJS := $(INTERFACES_SRCS:%.c=$(BUILDDIR)/%.o)
LIB_OBJS := $(CORE_OBJS) $(CONFIG_OBJS) $(NETFILTER_OBJS) $(API_OBJS) $(SUBSCRIPTIONS_OBJS) \
            $(INTERFACES_OBJS)
MAIN_OBJ := $(MAIN_SRC:%.c=$(BUILDDIR)/%.o)
CONFIGTOOL_OBJ := $(CONFIGTOOL_SRC:%.c=$(BUILDDIR)/%.o)
DNSTOOL_OBJ := $(DNSTOOL_SRC:%.c=$(BUILDDIR)/%.o)
CACHETOOL_OBJ := $(CACHETOOL_SRC:%.c=$(BUILDDIR)/%.o)

PCRE2_LIB ?= -lpcre2-8
CJSON_LIB ?= -lcjson
DEP_LIBS := -lyaml $(PCRE2_LIB) -lmnl $(CJSON_LIB) -lm -lcurl

# fake_iptables.c/fake_ipset_nl.c are test *support* (the former a port of
# executable-fake.go, gated behind Go's `testing` build tag there; the
# latter a mock ipset transport standing in for the untestable-here real
# libmnl backend) -- not test binaries of their own, linked into whichever
# test binaries need them (see TEST_SUPPORT_OBJS).
TEST_SUPPORT_SRCS := tests/unit/fake_iptables.c tests/unit/fake_ipset_nl.c
TEST_SUPPORT_OBJS := $(TEST_SUPPORT_SRCS:tests/unit/%.c=$(BUILDDIR)/tests/support/%.o)

TEST_SRCS := $(filter-out $(TEST_SUPPORT_SRCS), $(wildcard tests/unit/*.c))
TEST_BINS := $(TEST_SRCS:tests/unit/%.c=$(BUILDDIR)/tests/%)

FUZZ_SRCS := $(wildcard tests/fuzz/*.c)
FUZZ_BINS := $(FUZZ_SRCS:tests/fuzz/%.c=$(BUILDDIR)/fuzz/%)

.PHONY: all core clean test sanitize static_analysis fuzz

# The full daemon loads YAML config, so it links the config layer + deps
# (libyaml/cJSON/PCRE2/libmnl/libcurl) -- cross builds need those 5 libs
# present in SYSROOT (Phase 8; `core` below remains available for anyone
# who only wants the dependency-free subset, e.g. an early sysroot-less
# cross smoke build).
#
# A CROSS_COMPILE build with no SYSROOT normally has nowhere to find those
# 5 libs for the target arch, so it stays core-only, exactly as before
# Phase 8. Feed SDKs already configure the compiler sysroot and expose
# dependency headers/libs through their target flags; they opt into the
# full daemon with WITH_DEPS=1 instead of adding a second --sysroot.
# This keeps the existing CI cross_build job (mipsel-linux-gnu, no feed)
# core-only. Passing SYSROOT also opts a target into the full daemon (see
# root Makefile's BACKEND/C_CROSS_COMPILE/C_SYSROOT).
# mt-dnstool/mt-cachetool link against CORE_OBJS only (no DEP_LIBS), so
# they are real, fully-linked binaries a sysroot-less cross build can
# still produce -- CI's cross_build job checks one of these (not
# magitrickled-c, which core-only cannot produce) for real target-arch
# code, not just successful .o compilation.
ifneq ($(and $(CROSS_COMPILE),$(if $(or $(SYSROOT),$(WITH_DEPS)),,1)),)
all: core $(BUILDDIR)/mt-dnstool $(BUILDDIR)/mt-cachetool
else
all: $(BUILDDIR)/magitrickled-c $(BUILDDIR)/mt-configtool $(BUILDDIR)/mt-dnstool $(BUILDDIR)/mt-cachetool
endif

core: $(CORE_OBJS)

$(BUILDDIR)/magitrickled-c: $(LIB_OBJS) $(MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(DEP_LIBS)

$(BUILDDIR)/mt-configtool: $(LIB_OBJS) $(CONFIGTOOL_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(DEP_LIBS)

$(BUILDDIR)/mt-dnstool: $(CORE_OBJS) $(DNSTOOL_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/mt-cachetool: $(CORE_OBJS) $(CACHETOOL_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- tests ------------------------------------------------------------

# Without this, make treats tests/support/%.o as an intermediate of the
# tests/% chain (built only to satisfy another pattern rule) and deletes
# it after each run, forcing a needless rebuild for every test binary.
.PRECIOUS: $(BUILDDIR)/tests/support/%.o

$(BUILDDIR)/tests/support/%.o: tests/unit/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests/unit -c -o $@ $<

$(BUILDDIR)/tests/%: tests/unit/%.c $(LIB_OBJS) $(TEST_SUPPORT_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests/vendor -Itests/unit $(LDFLAGS) -o $@ $< $(LIB_OBJS) $(TEST_SUPPORT_OBJS) $(LDLIBS) $(DEP_LIBS)

# Inject entropy errors into the real parser without production test hooks.
$(BUILDDIR)/tests/test_large_subscriptions: tests/unit/test_large_subscriptions.c $(LIB_OBJS) $(TEST_SUPPORT_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests/vendor -Itests/unit $(LDFLAGS) -Wl,--wrap=mt_random_bytes -o $@ $< $(LIB_OBJS) $(TEST_SUPPORT_OBJS) $(LDLIBS) $(DEP_LIBS)

# Inspect reconciliation helpers without exposing them in the production API.
$(BUILDDIR)/tests/test_ruleset_indexed: tests/unit/test_ruleset_indexed.c $(LIB_OBJS) $(TEST_SUPPORT_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests/vendor -Itests/unit $(LDFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/src/netfilter/ruleset.o,$(LIB_OBJS)) $(TEST_SUPPORT_OBJS) $(LDLIBS) $(DEP_LIBS)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; $$t; done

sanitize:
	$(MAKE) BUILD=sanitize \
	    OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	    LDFLAGS="-fsanitize=address,undefined" test

# libFuzzer targets (host, clang). Each runs a short smoke by default in CI
# via `make fuzz FUZZ_RUNS=...`; without args they run interactively.
FUZZ_CC ?= clang
FUZZ_RUNS ?= 100000
$(BUILDDIR)/fuzz/%: tests/fuzz/%.c $(CORE_SRCS)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) -std=c11 -g -O1 -Iinclude -D_POSIX_C_SOURCE=200809L \
	    -fsanitize=fuzzer,address,undefined -o $@ $< $(CORE_SRCS)

# corpus/ is a working directory (gitignored); it is seeded from the small
# committed seeds/ set so a fresh checkout gets useful starting inputs.
fuzz: $(FUZZ_BINS)
	@set -e; for f in $(FUZZ_BINS); do \
	    name=$$(basename $$f); \
	    echo "== $$f ($(FUZZ_RUNS) runs)"; \
	    mkdir -p tests/fuzz/corpus/$$name; \
	    [ -d tests/fuzz/seeds/$$name ] && \
	        cp -n tests/fuzz/seeds/$$name/* tests/fuzz/corpus/$$name/ 2>/dev/null || true; \
	    $$f -runs=$(FUZZ_RUNS) -max_len=1600 \
	        tests/fuzz/corpus/$$name 2>&1 | tail -3 || exit 1; \
	done

# Reproducible CPU benchmark, intentionally not a wall-clock CI pass/fail gate.
.PHONY: bench_large_rules
bench_large_rules: $(BUILDDIR)/bench-large-rules
$(BUILDDIR)/bench-large-rules: ../../tools/bench/large-rules.c $(LIB_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJS) $(LDLIBS) $(DEP_LIBS)

# ---- analysis ---------------------------------------------------------

static_analysis:
	@if command -v clang-tidy >/dev/null; then \
	    clang-tidy --quiet $(LIB_SRCS) $(MAIN_SRC) $(CONFIGTOOL_SRC) $(DNSTOOL_SRC) $(CACHETOOL_SRC) $(TEST_SUPPORT_SRCS) -- $(CSTD) -Iinclude -Itests/unit \
	        -D_POSIX_C_SOURCE=200809L -DMT_VERSION=\"$(MT_VERSION)\"; \
	else echo "clang-tidy not found (skipped)"; fi
	@if command -v cppcheck >/dev/null; then \
	    cppcheck --std=c11 --enable=warning,performance,portability \
	        --error-exitcode=1 --inline-suppr -Iinclude -Itests/unit \
	        -D_POSIX_C_SOURCE=200809L \
	        $(LIB_SRCS) $(MAIN_SRC) $(CONFIGTOOL_SRC) $(DNSTOOL_SRC) $(CACHETOOL_SRC) $(TEST_SUPPORT_SRCS); \
	else echo "cppcheck not found (skipped)"; fi

clean:
	rm -rf build

-include $(LIB_OBJS:.o=.d) $(MAIN_OBJ:.o=.d)
