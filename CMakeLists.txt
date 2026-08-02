cmake_minimum_required(VERSION 3.21)
if(APPLE)
  project(api_client LANGUAGES C CXX OBJCXX)
else()
  project(api_client LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # compile_commands.json for clang-tidy (.clang-tidy / SPEC_refactor §7)

# --- Sanitizers for the stress harness (STRESS_TEST.md §2). Run passes SEPARATELY. ---
# macOS lacks a reliable LeakSanitizer -> use `leaks`/Instruments for leaks; ASan for
# use-after-free/overflow; TSan for races. ASan and TSan NOT enabled at the same time.
option(ENABLE_ASAN  "AddressSanitizer"   OFF)
option(ENABLE_TSAN  "ThreadSanitizer"    OFF)   # do NOT enable at the same time as ASan
option(ENABLE_UBSAN "UB Sanitizer"       OFF)
if(ENABLE_ASAN AND ENABLE_TSAN)
  message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN are mutually exclusive — run two separate passes.")
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

# --- Libraries from vcpkg (manifest mode auto-installs at configure time) ---
find_package(cpr CONFIG REQUIRED)
find_package(CURL REQUIRED)                 # WebSocket transport (curl_ws_* — SPEC_websocket §3)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(RdKafka CONFIG REQUIRED)       # Kafka producer+consumer (SPEC_kafka §1) — PLAINTEXT only, ssl/sasl off
find_package(OpenSSL REQUIRED)              # env-value encrypt-at-rest (already in tree via curl/grpc)
# Kafka Avro value serde (SPEC_kafka Avro v1) — confined to src/infra/transport/kafka.
# NOT find_package(avro-cpp): the port's config unconditionally aliases the SHARED target
# (avro-cpp::avrocpp), which a static triplet doesn't export -> hard CMake error. Include the
# generated targets file directly and alias the static lib under the documented name.
find_package(ZLIB REQUIRED)                 # avro-cpp interface dependency
include("${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/avro-cpp/avro-cpp-targets.cmake")
if(NOT TARGET avro-cpp::avrocpp)
  add_library(avro-cpp::avrocpp INTERFACE IMPORTED)
  target_link_libraries(avro-cpp::avrocpp INTERFACE avro-cpp::avrocpp_s)
endif()

# --- Real targets (README §3) ---
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
  add_subdirectory(third_party)      # Scintilla + Lexilla (editor for the 2 panes)
  add_subdirectory(ui/macos_appkit)  # Phase 2: AppKit + Objective-C++ (.mm)
endif()
