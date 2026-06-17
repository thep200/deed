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
  src/app/AppController.mm
  src/app/DeedConfig.mm
  src/theme/OS9Theme.mm
  src/icons/OS9Glyphs.mm
  src/widgets/OS9BevelButton.mm
  src/widgets/OS9PopupButton.mm
  src/widgets/OS9Window.mm
  src/widgets/OS9TitleBar.mm
  src/widgets/OS9BackgroundView.mm
  src/widgets/OS9Divider.mm
  src/widgets/OS9Scroller.mm
  src/widgets/OS9InsetView.mm
  src/widgets/OS9SerratedInset.mm
  src/widgets/OS9Toast.mm
  src/widgets/OS9Label.mm
  src/widgets/OS9StyleMenu.mm
  src/widgets/OS9Dropdown.mm
  src/editor/SciTextView.mm
  src/editor/JsonEditorBehavior.mm
  src/windows/MainWindowController.mm
  src/windows/EnvWindowController.mm
  ${DEED_ENV_SRC}
)

# .env -> Contents/Resources/.env
set_source_files_properties(${DEED_ENV_SRC} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

# Import có tiền tố thư mục (vd "widgets/OS9Button.h", "theme/OS9Theme.h").
target_include_directories(ApiClientApp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(ApiClientApp PRIVATE core scintilla "-framework Cocoa" "-framework QuartzCore")

# ARC cho Objective-C++ (quản lý retain/release + __weak trong CoreBridge).
target_compile_options(ApiClientApp PRIVATE -fobjc-arc -fobjc-weak)

set_target_properties(ApiClientApp PROPERTIES
  OUTPUT_NAME "ApiClient"
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist
)
