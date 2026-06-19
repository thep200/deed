# macos_appkit — adapter macOS (Objective-C++ + AppKit), link "core". README §3, UI spec.
# Chỉ build trên macOS.
if(NOT APPLE)
  message(STATUS "Bỏ qua ui/macos_appkit: chỉ build trên macOS")
  return()
endif()

# Nhúng .env (ở gốc repo) thành compile-time constant: sinh header chứa nội dung .env,
# app đọc từ binary nên KHÔNG cần file .env khi chạy. Sửa .env -> tự reconfigure.
set(DEED_ENV_SRC ${CMAKE_SOURCE_DIR}/.env)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DEED_ENV_SRC})
file(READ ${DEED_ENV_SRC} DEED_ENV_CONTENT)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/app/DeedConfigData.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/app/DeedConfigData.h
  @ONLY)

add_executable(deed MACOSX_BUNDLE
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
  src/dialogs/OS9Dialog.mm
  src/windows/TreeViews.mm
  src/windows/MainWindowController.mm
  src/windows/MainWindowController+Tree.mm
  src/windows/MainWindowController+Editor.mm
  src/windows/MainWindowController+Send.mm
  src/windows/MainWindowController+Config.mm
  src/windows/EnvWindowController.mm
)

# --- Debug tools (StressRunner — STRESS_TEST.md §5/§9). KHÔNG ship release. ---
option(DEED_DEBUG_TOOLS "Compile in-app debug tools (StressRunner)" OFF)
if(DEED_DEBUG_TOOLS)
  target_sources(deed PRIVATE
    src/debug/StressRunner.mm
    src/windows/MainWindowController+Stress.mm)
  target_compile_definitions(deed PRIVATE DEED_DEBUG_TOOLS=1)
endif()

# Import có tiền tố thư mục (vd "widgets/OS9Button.h", "theme/OS9Theme.h").
# generated/ chứa DeedConfigData.h (nhúng .env) -> import "app/DeedConfigData.h".
target_include_directories(deed PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_link_libraries(deed PRIVATE core scintilla "-framework Cocoa" "-framework QuartzCore")

# ARC cho Objective-C++ (quản lý retain/release + __weak trong CoreBridge).
target_compile_options(deed PRIVATE -fobjc-arc -fobjc-weak)

set_target_properties(deed PROPERTIES
  OUTPUT_NAME "deed"     # -> deed.app + Contents/MacOS/deed (khớp CFBundleExecutable)
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist
)
