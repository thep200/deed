# macos_appkit — adapter macOS (Objective-C++ + AppKit), link "core". README §3, UI spec.
# Chỉ build trên macOS.
if(NOT APPLE)
  message(STATUS "Bỏ qua ui/macos_appkit: chỉ build trên macOS")
  return()
endif()

# Đưa .env (ở gốc repo) vào bundle Resources để app đọc cấu hình khi chạy.
set(DEED_ENV_SRC ${CMAKE_SOURCE_DIR}/.env)

add_executable(ApiClientApp MACOSX_BUNDLE
  src/main.mm
  src/AppController.mm
  src/MainWindowController.mm
  src/EnvWindowController.mm
  src/DeedConfig.mm
  src/OS9Theme.mm
  src/OS9Widgets.mm
  src/SciTextView.mm
  src/JsonEditorBehavior.mm
  ${DEED_ENV_SRC}
)

# .env -> Contents/Resources/.env
set_source_files_properties(${DEED_ENV_SRC} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

target_link_libraries(ApiClientApp PRIVATE core scintilla "-framework Cocoa" "-framework QuartzCore")

# ARC cho Objective-C++ (quản lý retain/release + __weak trong CoreBridge).
target_compile_options(ApiClientApp PRIVATE -fobjc-arc -fobjc-weak)

set_target_properties(ApiClientApp PROPERTIES
  OUTPUT_NAME "ApiClient"
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist
)
