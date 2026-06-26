# macos_appkit — macOS adapter (Objective-C++ + AppKit), links "core". README §3, UI spec.
# Build only on macOS.
if(NOT APPLE)
  message(STATUS "Skipping ui/macos_appkit: macOS-only build")
  return()
endif()

# Embed .env (at repo root) as a compile-time constant: generate a header holding the .env content,
# the app reads it from the binary so NO .env file is needed at runtime. Editing .env -> auto reconfigure.
set(DEED_ENV_SRC ${CMAKE_SOURCE_DIR}/.env)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DEED_ENV_SRC})
file(READ ${DEED_ENV_SRC} DEED_ENV_CONTENT)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/app/DeedConfigData.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/app/DeedConfigData.h
  @ONLY)

# App icon: embed assets/logo/deed.icns (transparent background) into Contents/Resources.
set(DEED_APP_ICON ${CMAKE_SOURCE_DIR}/assets/logo/deed.icns)
set_source_files_properties(${DEED_APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

add_executable(deed MACOSX_BUNDLE
  ${DEED_APP_ICON}
  src/main.mm
  src/app/AppController.mm
  src/app/AppStrings.mm
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
  src/widgets/OS9Toggle.mm
  src/widgets/OS9Label.mm
  src/widgets/OS9StyleMenu.mm
  src/widgets/OS9Dropdown.mm
  src/widgets/OS9EnvGrid.mm
  src/editor/SciTextView.mm
  src/editor/JsonEditorBehavior.mm
  src/dialogs/OS9Dialog.mm
  src/windows/TreeViews.mm
  src/windows/MainWindowController.mm
  src/windows/MainWindowControllerTree.mm
  src/windows/MainWindowControllerEditor.mm
  src/windows/MainWindowControllerSend.mm
  src/windows/MainWindowControllerConfig.mm
  src/windows/EnvWindowController.mm
)

# --- Debug tools (StressRunner — STRESS_TEST.md §5/§9). Do NOT ship in release. ---
option(DEED_DEBUG_TOOLS "Compile in-app debug tools (StressRunner)" OFF)
if(DEED_DEBUG_TOOLS)
  target_sources(deed PRIVATE
    src/debug/StressRunner.mm
    src/windows/MainWindowControllerStress.mm)
  target_compile_definitions(deed PRIVATE DEED_DEBUG_TOOLS=1)
endif()

# Imports use a directory prefix (e.g. "widgets/OS9Button.h", "theme/OS9Theme.h").
# generated/ holds DeedConfigData.h (embedded .env) -> import "app/DeedConfigData.h".
target_include_directories(deed PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_link_libraries(deed PRIVATE core scintilla "-framework Cocoa" "-framework QuartzCore" "-framework UniformTypeIdentifiers")

# ARC for Objective-C++ (manages retain/release + __weak in CoreBridge).
target_compile_options(deed PRIVATE -fobjc-arc -fobjc-weak)

set_target_properties(deed PROPERTIES
  OUTPUT_NAME "deed"     # -> deed.app + Contents/MacOS/deed (matches CFBundleExecutable)
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist
)
