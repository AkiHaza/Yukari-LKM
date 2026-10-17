SHELL := /bin/sh

NDK_BUILD ?= $(if $(NDK_HOME),$(NDK_HOME)/ndk-build,ndk-build)
NDK_PROJECT_PATH := $(CURDIR)
APP_BUILD_SCRIPT := $(CURDIR)/zygisk/Android.mk
APP_PLATFORM ?= android-29
ABIS ?= arm64-v8a
OUT ?= $(CURDIR)/build/android
DIST ?= $(CURDIR)/dist
MAGISK_INCLUDE ?=
comma := ,
ABI_LIST := $(subst $(comma), ,$(ABIS))
FIRST_ABI := $(firstword $(ABI_LIST))

.PHONY: all android package package-only test clean

all: package

android:
	@test -n "$(MAGISK_INCLUDE)" && test -s "$(MAGISK_INCLUDE)/zygisk.hpp" || \
		{ echo "MAGISK_INCLUDE must contain a non-empty zygisk.hpp" >&2; exit 2; }
	@command -v "$(NDK_BUILD)" >/dev/null 2>&1 || \
		{ echo "ndk-build not found: $(NDK_BUILD)" >&2; exit 2; }
	$(NDK_BUILD) NDK_PROJECT_PATH=$(NDK_PROJECT_PATH) APP_BUILD_SCRIPT=$(APP_BUILD_SCRIPT) \
		APP_ABI="$(ABI_LIST)" APP_PLATFORM=$(APP_PLATFORM) MAGISK_INCLUDE="$(MAGISK_INCLUDE)" \
		NDK_OUT=$(OUT)/obj NDK_LIBS_OUT=$(OUT)/libs
	@for abi in $(ABI_LIST); do \
		test -s "$(OUT)/libs/$$abi/libyukari.so" || { echo "missing libyukari.so for $$abi" >&2; exit 1; }; \
		test -s "$(OUT)/libs/$$abi/service-proxy" || { echo "missing service-proxy for $$abi" >&2; exit 1; }; \
	done

package: android
	$(MAKE) --no-print-directory package-only

package-only:
	@test -n "$(FIRST_ABI)" || { echo "ABIS must contain at least one ABI" >&2; exit 2; }
	@for abi in $(ABI_LIST); do \
		test -s "$(OUT)/libs/$$abi/libyukari.so" || { echo "missing libyukari.so for $$abi" >&2; exit 1; }; \
		test -s "$(OUT)/libs/$$abi/service-proxy" || { echo "missing service-proxy for $$abi" >&2; exit 1; }; \
	done
	rm -rf "$(DIST)/yukari-service-filter" "$(DIST)/yukari-service-filter.zip"
	mkdir -p "$(DIST)/yukari-service-filter/bin" "$(DIST)/yukari-service-filter/zygisk" \
		"$(DIST)/yukari-service-filter/config"
	cp zygisk/module.prop "$(DIST)/yukari-service-filter/module.prop"
	cp zygisk/config/targets.conf "$(DIST)/yukari-service-filter/config/targets.conf"
	if [ -f zygisk/config.json ]; then cp zygisk/config.json "$(DIST)/yukari-service-filter/config.json"; fi
	chmod 0644 "$(DIST)/yukari-service-filter/module.prop" "$(DIST)/yukari-service-filter/config/targets.conf"
	cp "$(OUT)/libs/$(FIRST_ABI)/service-proxy" "$(DIST)/yukari-service-filter/bin/service-proxy"
	chmod 0755 "$(DIST)/yukari-service-filter/bin/service-proxy"
	for abi in $(ABI_LIST); do \
		cp "$(OUT)/libs/$$abi/libyukari.so" "$(DIST)/yukari-service-filter/zygisk/$$abi.so"; \
		chmod 0644 "$(DIST)/yukari-service-filter/zygisk/$$abi.so"; \
	done
	(cd "$(DIST)/yukari-service-filter" && zip -q -r "../yukari-service-filter.zip" .)
	@test -s "$(DIST)/yukari-service-filter.zip"

test:
	cmake -S zygisk -B build/host -DYUKARI_BUILD_TESTS=ON
	cmake --build build/host --target service_filter_test
	ctest --test-dir build/host --output-on-failure

clean:
	rm -rf build dist
