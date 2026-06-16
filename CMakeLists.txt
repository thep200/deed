cmake_minimum_required(VERSION 3.21)
if(APPLE)
  project(api_client LANGUAGES C CXX OBJCXX)
else()
  project(api_client LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Thu vien tu vcpkg (manifest mode tu cai khi configure) ---
find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)

# --- Smoke test: verify toolchain link duoc (throwaway) ---
option(BUILD_SMOKE "Build the toolchain smoke-test executable" OFF)
if(BUILD_SMOKE)
  add_subdirectory(tools/smoke)
endif()

# --- Cac target that (README muc 3) ---
add_subdirectory(core)              # -> static lib "core" (pure C++)

option(BUILD_CLI "Build the headless CLI driver (dev/CI)" ON)
if(BUILD_CLI)
  add_subdirectory(ui/cli)          # -> executable, link "core"
endif()

option(BUILD_TESTS "Build core unit tests" ON)
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)           # -> core_tests
endif()

option(BUILD_MACOS_UI "Build the macOS AppKit app (Phase 2)" ON)
if(BUILD_MACOS_UI AND APPLE)
  add_subdirectory(ui/macos_appkit) # Phase 2: AppKit + Objective-C++ (.mm)
endif()
