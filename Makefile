# Makefile — wraps setup/build/package for the API Client (REST + gRPC) on macOS.
# Invokes tools via `mise exec --` to ensure the pinned versions are used + VCPKG_ROOT/env
# (even when mise is not activated in the shell). See SETUP.md for details.

SHELL := /bin/bash

# --- Config (override: e.g. `make build USE_MISE=0`) ---
USE_MISE   ?= 1
VCPKG_ROOT ?= $(HOME)/vcpkg
BUILD_DIR  ?= build
DIST_DIR   ?= dist

ifeq ($(USE_MISE),1)
  RUN := mise exec --
else
  RUN :=
endif

# --- Variables for packaging (passed when running `make package`) ---
# App is built under the ui/macos_appkit/ subtree (per CMake target deed).
APP_PATH       ?= $(BUILD_DIR)/ui/macos_appkit/deed.app
DEV_ID_APP     ?=
NOTARY_PROFILE ?=

.DEFAULT_GOAL := help

.PHONY: help doctor tools bootstrap baseline configure configure-release \
        build build-all app release dist smoke test core-test run-ui run-ui-smoke \
        setup notary-store package clean distclean

help: ## List all targets
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
	  | awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-18s\033[0m %s\n", $$1, $$2}'

doctor: ## Check that tools are ready
	@echo "xcode  : $$(xcode-select -p 2>/dev/null || echo 'NOT INSTALLED -> xcode-select --install')"
	@command -v mise >/dev/null && echo "mise   : $$(mise --version)" || echo "mise   : NOT INSTALLED"
	@$(RUN) cmake --version 2>/dev/null | head -1 | sed 's/^/cmake  : /' || echo "cmake  : MISSING (make tools)"
	@$(RUN) ninja --version 2>/dev/null | sed 's/^/ninja  : /' || echo "ninja  : MISSING (make tools)"
	@test -x "$(VCPKG_ROOT)/vcpkg" && echo "vcpkg  : $(VCPKG_ROOT)" || echo "vcpkg  : NOT BOOTSTRAPPED -> make bootstrap"

tools: ## mise install (cmake/ninja per pinned versions)
	mise trust
	mise install

bootstrap: ## Clone + bootstrap vcpkg into $(VCPKG_ROOT) if missing
	@if [ -x "$(VCPKG_ROOT)/vcpkg" ]; then \
	  echo "vcpkg already present at $(VCPKG_ROOT)"; \
	else \
	  git clone https://github.com/microsoft/vcpkg "$(VCPKG_ROOT)"; \
	  "$(VCPKG_ROOT)/bootstrap-vcpkg.sh" -disableMetrics; \
	fi

baseline: ## Write builtin-baseline into vcpkg.json (reproducible build)
	$(RUN) "$(VCPKG_ROOT)/vcpkg" x-update-baseline --add-initial-baseline

configure: ## CMake configure Debug (first run vcpkg builds deps from source)
	@test -x "$(VCPKG_ROOT)/vcpkg" || { echo "No vcpkg -> make bootstrap"; exit 1; }
	$(RUN) cmake --preset default

configure-release: ## CMake configure Release
	@test -x "$(VCPKG_ROOT)/vcpkg" || { echo "No vcpkg -> make bootstrap"; exit 1; }
	$(RUN) cmake --preset release

# build = produces the .app on macOS (target deed; pulls in core).
build: configure ## Build the macOS app (.app bundle)
	$(RUN) cmake --build $(BUILD_DIR) --target deed
	@echo "App: $(APP_PATH)"

app: build ## Alias for `build`

release: configure-release ## Build Release (entire project)
	$(RUN) cmake --build $(BUILD_DIR)

# dist = ONE COMMAND TO A USABLE APP: build Release (.app) + ad-hoc sign + copy to dist/deed.app.
# App is statically linked (no external dylibs) so it runs by dragging into /Applications. No notarization
# (own machine only); to distribute to other machines -> use `make package` (Developer ID + notarize).
dist: configure-release ## One command: signed Release .app -> dist/deed.app (drag into /Applications)
	$(RUN) cmake --build $(BUILD_DIR) --target deed
	codesign --force --sign - "$(APP_PATH)"          # re-sign ad-hoc (macdeployqtfix can break the signature)
	@mkdir -p "$(DIST_DIR)"
	@rm -rf "$(DIST_DIR)/deed.app"
	@cp -R "$(APP_PATH)" "$(DIST_DIR)/deed.app"
	@echo "OK -> $(DIST_DIR)/deed.app (Release, ad-hoc signed, self-contained)."
	@echo "    Drag $(DIST_DIR)/deed.app into /Applications to use."

build-all: configure ## Build everything (core + cli + tests + app)
	$(RUN) cmake --build $(BUILD_DIR)

smoke: ## Build + run toolchain smoke test (cpr/grpc/json link)
	$(RUN) cmake --preset default -DBUILD_SMOKE=ON
	$(RUN) cmake --build $(BUILD_DIR) --target smoke
	./$(BUILD_DIR)/tools/smoke/smoke

# test = FAST app build (incremental, no re-configure if already present) then open to test the UI.
test: ## Fast app build + open to test (incremental)
	@test -f "$(BUILD_DIR)/build.ninja" || $(RUN) cmake --preset default
	$(RUN) cmake --build $(BUILD_DIR) --target deed
	open "$(APP_PATH)"

run-ui: test ## Alias for `test` (fast build + open app)

core-test: configure ## Build + run Core unit tests (ctest)
	$(RUN) cmake --build $(BUILD_DIR)
	$(RUN) ctest --test-dir $(BUILD_DIR) --output-on-failure

# Smoke UI with no clicking: opens COLLECTION, auto-presses Send, prints result to stdout.
#   make run-ui-smoke COLLECTION=/path/to/collection
run-ui-smoke: build ## Build + run app headless: open COLLECTION, auto Send (prints [smoke] log)
	@test -n "$(COLLECTION)" || { echo "Missing COLLECTION=<collection path>"; exit 1; }
	APICLIENT_OPEN="$(COLLECTION)" APICLIENT_SEND=1 \
	  "$(APP_PATH)/Contents/MacOS/deed"

setup: ## Run all first-time install steps + verify (sequential)
	$(MAKE) tools
	$(MAKE) bootstrap
	$(MAKE) core-test

notary-store: ## Print instructions to store notary credentials (run once)
	@echo 'Run (substitute your own details), credentials saved to Keychain:'
	@echo '  xcrun notarytool store-credentials "api-client-notary" \'
	@echo '    --apple-id you@example.com --team-id TEAMID --password <app-specific-password>'

package: ## .dmg: codesign + notarize + staple (needs DEV_ID_APP, NOTARY_PROFILE)
	@test -n "$(DEV_ID_APP)"     || { echo "Missing DEV_ID_APP='Developer ID Application: Name (TEAMID)'"; exit 1; }
	@test -n "$(NOTARY_PROFILE)" || { echo "Missing NOTARY_PROFILE (see: make notary-store)"; exit 1; }
	APP_PATH="$(APP_PATH)" DEV_ID_APP="$(DEV_ID_APP)" NOTARY_PROFILE="$(NOTARY_PROFILE)" \
	  DIST_DIR="$(DIST_DIR)" ./scripts/package_macos.sh

clean: ## Remove build/
	rm -rf $(BUILD_DIR)

distclean: clean ## Also remove dist/ vcpkg_installed/ .cache/
	rm -rf $(DIST_DIR) vcpkg_installed .cache
