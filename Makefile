# Makefile — wrap setup/build/package cho API Client (REST + gRPC) tren macOS.
# Goi tools qua `mise exec --` de chac chan dung version da pin + co VCPKG_ROOT/env
# (ke ca khi chua activate mise trong shell). Xem SETUP.md de biet chi tiet.

SHELL := /bin/bash

# --- Cau hinh (override: vd `make build USE_MISE=0`) ---
USE_MISE   ?= 1
VCPKG_ROOT ?= $(HOME)/vcpkg
BUILD_DIR  ?= build
DIST_DIR   ?= dist

ifeq ($(USE_MISE),1)
  RUN := mise exec --
else
  RUN :=
endif

# --- Bien cho packaging (truyen khi chay `make package`) ---
# App build ra trong cay con ui/macos_appkit/ (theo CMake target ApiClientApp).
APP_PATH       ?= $(BUILD_DIR)/ui/macos_appkit/ApiClient.app
DEV_ID_APP     ?=
NOTARY_PROFILE ?=

.DEFAULT_GOAL := help

.PHONY: help doctor tools bootstrap baseline configure configure-release \
        build build-all app release smoke test core-test run-ui run-ui-smoke \
        setup notary-store package clean distclean

help: ## Liet ke cac target
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
	  | awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-18s\033[0m %s\n", $$1, $$2}'

doctor: ## Kiem tra cong cu da san sang
	@echo "xcode  : $$(xcode-select -p 2>/dev/null || echo 'CHUA CAI -> xcode-select --install')"
	@command -v mise >/dev/null && echo "mise   : $$(mise --version)" || echo "mise   : CHUA CAI"
	@$(RUN) cmake --version 2>/dev/null | head -1 | sed 's/^/cmake  : /' || echo "cmake  : CHUA CO (make tools)"
	@$(RUN) ninja --version 2>/dev/null | sed 's/^/ninja  : /' || echo "ninja  : CHUA CO (make tools)"
	@test -x "$(VCPKG_ROOT)/vcpkg" && echo "vcpkg  : $(VCPKG_ROOT)" || echo "vcpkg  : CHUA BOOTSTRAP -> make bootstrap"

tools: ## mise install (cmake/ninja theo version pin)
	mise trust
	mise install

bootstrap: ## Clone + bootstrap vcpkg vao $(VCPKG_ROOT) neu chua co
	@if [ -x "$(VCPKG_ROOT)/vcpkg" ]; then \
	  echo "vcpkg da co tai $(VCPKG_ROOT)"; \
	else \
	  git clone https://github.com/microsoft/vcpkg "$(VCPKG_ROOT)"; \
	  "$(VCPKG_ROOT)/bootstrap-vcpkg.sh" -disableMetrics; \
	fi

baseline: ## Ghi builtin-baseline vao vcpkg.json (build tai lap)
	$(RUN) "$(VCPKG_ROOT)/vcpkg" x-update-baseline --add-initial-baseline

configure: ## CMake configure Debug (lan dau vcpkg build deps tu nguon)
	@test -x "$(VCPKG_ROOT)/vcpkg" || { echo "Chua co vcpkg -> make bootstrap"; exit 1; }
	$(RUN) cmake --preset default

configure-release: ## CMake configure Release
	@test -x "$(VCPKG_ROOT)/vcpkg" || { echo "Chua co vcpkg -> make bootstrap"; exit 1; }
	$(RUN) cmake --preset release

# build = ra file .app tren macOS (target ApiClientApp; keo theo core).
build: configure ## Build ra file app macOS (.app bundle)
	$(RUN) cmake --build $(BUILD_DIR) --target ApiClientApp
	@echo "App: $(APP_PATH)"

app: build ## Alias cua `build`

release: configure-release ## Build Release (toan bo project)
	$(RUN) cmake --build $(BUILD_DIR)

build-all: configure ## Build toan bo (core + cli + tests + app)
	$(RUN) cmake --build $(BUILD_DIR)

smoke: ## Build + chay smoke test toolchain (cpr/grpc/json link duoc)
	$(RUN) cmake --preset default -DBUILD_SMOKE=ON
	$(RUN) cmake --build $(BUILD_DIR) --target smoke
	./$(BUILD_DIR)/tools/smoke/smoke

# test = build NHANH app (incremental, khong configure lai neu da co) roi mo de test UI.
test: ## Build nhanh app + mo de test (incremental)
	@test -f "$(BUILD_DIR)/build.ninja" || $(RUN) cmake --preset default
	$(RUN) cmake --build $(BUILD_DIR) --target ApiClientApp
	open "$(APP_PATH)"

run-ui: test ## Alias cua `test` (build nhanh + mo app)

core-test: configure ## Build + chay unit test cua Core (ctest)
	$(RUN) cmake --build $(BUILD_DIR)
	$(RUN) ctest --test-dir $(BUILD_DIR) --output-on-failure

# Smoke UI khong can click: mo san COLLECTION roi tu bam Send, in ket qua ra stdout.
#   make run-ui-smoke COLLECTION=/duong/dan/collection
run-ui-smoke: build ## Build + chay app headless: mo COLLECTION, tu Send (in [smoke] log)
	@test -n "$(COLLECTION)" || { echo "Thieu COLLECTION=<duong dan collection>"; exit 1; }
	APICLIENT_OPEN="$(COLLECTION)" APICLIENT_SEND=1 \
	  "$(APP_PATH)/Contents/MacOS/ApiClient"

setup: ## Chay tat ca buoc cai dat lan dau + verify (tuan tu)
	$(MAKE) tools
	$(MAKE) bootstrap
	$(MAKE) core-test

notary-store: ## In huong dan luu notary credential (chay 1 lan)
	@echo 'Chay (thay thong tin cua ban), credential luu vao Keychain:'
	@echo '  xcrun notarytool store-credentials "api-client-notary" \'
	@echo '    --apple-id you@example.com --team-id TEAMID --password <app-specific-password>'

package: ## .dmg: codesign + notarize + staple (can DEV_ID_APP, NOTARY_PROFILE)
	@test -n "$(DEV_ID_APP)"     || { echo "Thieu DEV_ID_APP='Developer ID Application: Ten (TEAMID)'"; exit 1; }
	@test -n "$(NOTARY_PROFILE)" || { echo "Thieu NOTARY_PROFILE (xem: make notary-store)"; exit 1; }
	APP_PATH="$(APP_PATH)" DEV_ID_APP="$(DEV_ID_APP)" NOTARY_PROFILE="$(NOTARY_PROFILE)" \
	  DIST_DIR="$(DIST_DIR)" ./scripts/package_macos.sh

clean: ## Xoa build/
	rm -rf $(BUILD_DIR)

distclean: clean ## Xoa them dist/ vcpkg_installed/ .cache/
	rm -rf $(DIST_DIR) vcpkg_installed .cache
