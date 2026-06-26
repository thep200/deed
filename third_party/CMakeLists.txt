# Scintilla 5.5 + Lexilla 5.2 (JSON lexer only) -> one static lib "scintilla". The upstream sources are
# NOT vendored in git; CMake FetchContent downloads them into ${CMAKE_BINARY_DIR}/_deps at configure time
# (pinned + verified by SHA256). Our macOS-12 deprecation fixes are re-applied via deed-scintilla.patch.
# macOS only (Cocoa port). See docs/RENDERING_AND_ASSETS.md §3.
include(FetchContent)

# `patch -i <file>` (not `< file`): FetchContent runs PATCH_COMMAND directly, NOT through a shell, so a
# `<` redirect would not work. FetchContent applies it once after extraction (stamp-guarded -> no re-apply).
FetchContent_Declare(scintilla
  URL      https://www.scintilla.org/scintilla550.tgz
  URL_HASH SHA256=e553e95509f01f92aa157fa02d06a712642e13d69a11ec1a02a7ddf22c406231
  PATCH_COMMAND patch -p1 -i "${CMAKE_CURRENT_SOURCE_DIR}/deed-scintilla.patch"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(lexilla
  URL      https://www.scintilla.org/lexilla520.tgz
  URL_HASH SHA256=a45766e0753cfeb6707ee4b0abb7f796de4deebe1b1f46d17128c122a6e107c5
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
# Tarballs ship no CMakeLists -> MakeAvailable just downloads/extracts (no add_subdirectory); we build the
# static lib ourselves below from the populated source dirs.
FetchContent_MakeAvailable(scintilla lexilla)

set(SCI ${scintilla_SOURCE_DIR})
set(LEX ${lexilla_SOURCE_DIR})

file(GLOB SCI_SRC ${SCI}/src/*.cxx)
set(SCI_COCOA
  ${SCI}/cocoa/PlatCocoa.mm
  ${SCI}/cocoa/ScintillaCocoa.mm
  ${SCI}/cocoa/ScintillaView.mm
  ${SCI}/cocoa/InfoBar.mm)
file(GLOB LEX_LEXLIB ${LEX}/lexlib/*.cxx)

add_library(scintilla STATIC
  ${SCI_SRC}
  ${SCI_COCOA}
  ${LEX_LEXLIB}
  ${LEX}/lexers/LexJSON.cxx
  ${CMAKE_CURRENT_SOURCE_DIR}/lexilla_json.cxx)

target_include_directories(scintilla PUBLIC
  ${SCI}/include ${SCI}/cocoa ${LEX}/include ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(scintilla PRIVATE
  ${SCI}/src ${LEX}/lexlib ${LEX}/lexers)

target_compile_features(scintilla PUBLIC cxx_std_17)
target_compile_definitions(scintilla PRIVATE SCI_LEXER=1)

# File Cocoa của Scintilla BẮT BUỘC ARC.
set_source_files_properties(${SCI_COCOA} PROPERTIES COMPILE_FLAGS "-fobjc-arc")

target_link_libraries(scintilla PUBLIC "-framework Cocoa" "-framework QuartzCore")
