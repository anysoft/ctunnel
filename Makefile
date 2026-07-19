PYTHON ?= python3
CMAKE ?= cmake
BUILD_DIR ?= build
CONFIG ?= .config
BUILD_TYPE ?= Release
JOBS ?=
DESTDIR ?=
PREFIX ?= /usr/local
CMAKE_EXTRA_ARGS ?=
-include .ctunnel-configure.mk
KCONFIG := $(PYTHON) scripts/kconfig/configure.py --config $(CONFIG) --build-dir $(BUILD_DIR)
MERGE_CONFIG := $(PYTHON) scripts/merge_config.py --config $(CONFIG) --build-dir $(BUILD_DIR)
PARALLEL := $(if $(JOBS),--parallel $(JOBS),--parallel)

-include $(BUILD_DIR)/generated/config.mk

.PHONY: all prepare build test install menuconfig oldconfig olddefconfig savedefconfig \
	defconfig mini_defconfig default_defconfig full_defconfig linux_defconfig \
	macos_defconfig windows_defconfig buildroot_defconfig openwrt_defconfig mini default full \
	mini_static_defconfig mini_mostly_static_defconfig static mostly-static dynamic \
	size size-check size-report size-compare config-report dist clean distclean help

all: build

prepare:
	@if test ! -f "$(CONFIG)"; then $(MAKE) default_defconfig; fi
	@$(KCONFIG) olddefconfig

build: prepare
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX) -DCTUNNEL_KCONFIG_CONFIG=$(abspath $(CONFIG)) \
		$(CMAKE_EXTRA_ARGS)
	+$(CMAKE) --build $(BUILD_DIR) $(PARALLEL)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

install: build
	DESTDIR=$(DESTDIR) $(CMAKE) --install $(BUILD_DIR)

menuconfig:
	@if test ! -f "$(CONFIG)"; then $(MAKE) default_defconfig; fi
	$(PYTHON) scripts/kconfig/menuconfig.py --config $(CONFIG) --build-dir $(BUILD_DIR)

oldconfig:
	$(KCONFIG) oldconfig

olddefconfig:
	$(KCONFIG) olddefconfig

savedefconfig:
	$(KCONFIG) savedefconfig defconfig

defconfig: default_defconfig

mini_defconfig:
	$(KCONFIG) defconfig configs/mini_defconfig

default_defconfig:
	$(KCONFIG) defconfig configs/default_defconfig

full_defconfig:
	$(KCONFIG) defconfig configs/full_defconfig

linux_defconfig:
	$(KCONFIG) --target linux defconfig configs/linux_defconfig

macos_defconfig:
	$(KCONFIG) --target darwin defconfig configs/macos_defconfig

windows_defconfig:
	$(KCONFIG) --target windows defconfig configs/windows_defconfig

buildroot_defconfig:
	$(KCONFIG) --target linux defconfig configs/buildroot_defconfig

openwrt_defconfig:
	$(KCONFIG) --target linux defconfig configs/openwrt_mini_defconfig

mini_static_defconfig:
	$(MERGE_CONFIG) configs/mini_defconfig configs/role/server.config configs/link/static.config

mini_mostly_static_defconfig:
	$(MERGE_CONFIG) configs/mini_defconfig configs/role/server.config configs/link/mostly-static.config

mini: mini_defconfig
	$(MAKE) build BUILD_DIR=$(BUILD_DIR) CONFIG=$(CONFIG) BUILD_TYPE=MinSizeRel
	$(MAKE) size-check BUILD_DIR=$(BUILD_DIR) CONFIG=$(CONFIG)

default: default_defconfig
	$(MAKE) build BUILD_DIR=$(BUILD_DIR) CONFIG=$(CONFIG) BUILD_TYPE=Release

full: full_defconfig
	$(MAKE) build BUILD_DIR=$(BUILD_DIR) CONFIG=$(CONFIG) BUILD_TYPE=Debug

static: prepare
	scripts/config --file $(CONFIG) --build-dir $(BUILD_DIR) --enable CTUNNEL_LINK_STATIC
	$(MAKE) build BUILD_DIR=$(BUILD_DIR) CONFIG=$(CONFIG) BUILD_TYPE=$(BUILD_TYPE)

mostly-static: prepare
	scripts/config --file $(CONFIG) --build-dir $(BUILD_DIR) --enable CTUNNEL_LINK_MOSTLY_STATIC
	$(MAKE) build BUILD_DIR=$(BUILD_DIR) CONFIG=$(CONFIG) BUILD_TYPE=$(BUILD_TYPE)

dynamic: prepare
	scripts/config --file $(CONFIG) --build-dir $(BUILD_DIR) --enable CTUNNEL_LINK_DYNAMIC
	$(MAKE) build BUILD_DIR=$(BUILD_DIR) CONFIG=$(CONFIG) BUILD_TYPE=$(BUILD_TYPE)

size: build
	@command -v size >/dev/null 2>&1 && size $(BUILD_DIR)/ctunnel || wc -c $(BUILD_DIR)/ctunnel

size-check: prepare
	@CTUNNEL_MAX_MINI_BINARY_SIZE_KIB=$(CONFIG_MINI_MAX_BINARY_SIZE_KIB) \
		CTUNNEL_LINK_MODE=$(if $(CONFIG_CTUNNEL_LINK_DYNAMIC),dynamic,$(if $(CONFIG_CTUNNEL_LINK_STATIC),static,mostly-static)) \
		CTUNNEL_STRIP="$${STRIP:-strip}" scripts/size-report.sh $(BUILD_DIR)/ctunnel

size-report: build
	@mkdir -p $(BUILD_DIR)/reports
	@CTUNNEL_STRIP="$${STRIP:-strip}" scripts/size-report.sh $(BUILD_DIR)/ctunnel | \
		tee $(BUILD_DIR)/reports/size-report.txt
	@cp $(CONFIG) $(BUILD_DIR)/reports/ctunnel.config
	@if test -f "$(BUILD_DIR)/ctunnel.link-report.txt"; then \
		cp "$(BUILD_DIR)/ctunnel.link-report.txt" "$(BUILD_DIR)/reports/link-report.txt"; \
	fi

size-compare:
	./scripts/size-compare.sh

config-report: prepare
	@if test -x "$(BUILD_DIR)/ctunnel" && test "$(CONFIG_FEATURE_BUILD_INFO)" = y; then \
		$(BUILD_DIR)/ctunnel build-config; \
	else \
		sed -n '1,240p' $(BUILD_DIR)/generated/config.mk; \
	fi

dist: build size-report
	@$(CMAKE) -E make_directory dist
	@$(CMAKE) -E copy $(BUILD_DIR)/ctunnel dist/ctunnel
	@$(CMAKE) -E copy $(CONFIG) dist/ctunnel.config
	@if test -f "$(BUILD_DIR)/ctunnel.link-report.txt"; then \
		$(CMAKE) -E copy "$(BUILD_DIR)/ctunnel.link-report.txt" dist/ctunnel.link-report.txt; \
	fi
	@if test -f "$(BUILD_DIR)/reports/size-report.txt"; then \
		$(CMAKE) -E copy "$(BUILD_DIR)/reports/size-report.txt" dist/ctunnel.size-report.txt; \
	fi
	@strip_tool="$${STRIP:-strip}"; if command -v "$$strip_tool" >/dev/null 2>&1; then "$$strip_tool" dist/ctunnel; fi
	@shasum -a 256 dist/ctunnel > dist/SHA256SUMS 2>/dev/null || sha256sum dist/ctunnel > dist/SHA256SUMS

clean:
	@if test -f "$(BUILD_DIR)/CMakeCache.txt"; then $(CMAKE) --build $(BUILD_DIR) --target clean; fi

distclean:
	$(CMAKE) -E remove_directory build
	$(CMAKE) -E remove_directory dist
	$(CMAKE) -E rm -f .config include/generated/autoconf.h .ctunnel-configure.mk

help:
	@echo "Configuration: menuconfig defconfig oldconfig olddefconfig savedefconfig"
	@echo "Presets: mini_defconfig mini_static_defconfig mini_mostly_static_defconfig default_defconfig full_defconfig"
	@echo "Build: all build test mini default full static mostly-static dynamic install dist size size-check size-report size-compare"
	@echo "Cleanup: clean distclean"
