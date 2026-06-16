# Scintilla 5.5 + Lexilla 5.2 (chỉ JSON lexer) -> 1 static lib, build từ source vendored.
# Theo docs/RENDERING_AND_ASSETS.md §3. Chỉ build trên macOS (dùng port Cocoa).
set(SCI ${CMAKE_CURRENT_SOURCE_DIR}/scintilla)
set(LEX ${CMAKE_CURRENT_SOURCE_DIR}/lexilla)

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
