SHELL := /bin/bash

# Config
USE_MISE   ?= 1
VCPKG_ROOT ?= $(HOME)/vcpkg
BUILD_DIR  ?= build
DIST_DIR   ?= dist

ifeq ($(USE_MISE),1)
  RUN := mise exec --
else
  RUN :=
endif

APP_PATH := $(BUILD_DIR)/ui/macos_appkit/deed.app

.DEFAULT_GOAL := help
.PHONY: help setup build test

help:
	@echo "deed — available commands:"
	@echo ""
	@echo "  make setup   One-time: install toolchain (cmake/ninja) + bootstrap vcpkg + configure (builds deps)"
	@echo "  make build   Build the signed Release .app -> dist/deed.app"
	@echo "  make test    Fast incremental build + open the app"
	@echo "  make help    Show this message"

setup:
	mise trust
	mise install
	@if [ -x "$(VCPKG_ROOT)/vcpkg" ]; then \
	  echo "vcpkg already present at $(VCPKG_ROOT)"; \
	else \
	  git clone https://github.com/microsoft/vcpkg "$(VCPKG_ROOT)"; \
	  "$(VCPKG_ROOT)/bootstrap-vcpkg.sh" -disableMetrics; \
	fi
	$(RUN) cmake --preset default
	@echo "Setup done. Now run: make build  (or)  make test"

build:
	@test -x "$(VCPKG_ROOT)/vcpkg" || { echo "No vcpkg -> run: make setup"; exit 1; }
	$(RUN) cmake --preset release
	$(RUN) cmake --build $(BUILD_DIR) --target deed
	codesign --force --sign - "$(APP_PATH)"
	@mkdir -p "$(DIST_DIR)"
	@rm -rf "$(DIST_DIR)/deed.app"
	@cp -R "$(APP_PATH)" "$(DIST_DIR)/deed.app"
	@echo "OK -> $(DIST_DIR)/deed.app (Release, ad-hoc signed, self-contained). Drag into /Applications."

test:
	@test -f "$(BUILD_DIR)/build.ninja" || $(RUN) cmake --preset default
	$(RUN) cmake --build $(BUILD_DIR) --target deed
	open "$(APP_PATH)"
