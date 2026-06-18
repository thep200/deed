cmake_minimum_required(VERSION 3.21)
if(APPLE)
  project(api_client LANGUAGES C CXX OBJCXX)
else()
  project(api_client LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Sanitizer cho stress harness (STRESS_TEST.md §2). Chạy các pass RIÊNG. ---
# macOS không có LeakSanitizer tin cậy -> dùng `leaks`/Instruments cho rò rỉ; ASan cho
# use-after-free/overflow; TSan cho race. ASan và TSan KHÔNG bật cùng lúc.
option(ENABLE_ASAN  "AddressSanitizer"   OFF)
option(ENABLE_TSAN  "ThreadSanitizer"    OFF)   # KHONG bat cung luc voi ASan
option(ENABLE_UBSAN "UB Sanitizer"       OFF)
if(ENABLE_ASAN AND ENABLE_TSAN)
  message(FATAL_ERROR "ENABLE_ASAN và ENABLE_TSAN loại trừ nhau — chạy hai pass riêng.")
endif()
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address)
endif()
if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -g)
  add_link_options(-fsanitize=thread)
endif()
if(ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=undefined)
endif()

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
  add_subdirectory(third_party)      # Scintilla + Lexilla (editor cho 2 pane)
  add_subdirectory(ui/macos_appkit)  # Phase 2: AppKit + Objective-C++ (.mm)
endif()
