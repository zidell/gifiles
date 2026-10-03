# Builds the vendored libvterm 0.3.3 (C99, MIT; third_party/libvterm, release tarball's include/ and
# src/ with the generated .inc tables and our resize fix) as a static library `vterm_static`.
enable_language(C)
set(_vt ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvterm)
add_library(vterm_static STATIC
    ${_vt}/src/encoding.c ${_vt}/src/keyboard.c ${_vt}/src/mouse.c ${_vt}/src/parser.c
    ${_vt}/src/pen.c ${_vt}/src/screen.c ${_vt}/src/state.c ${_vt}/src/unicode.c ${_vt}/src/vterm.c)
set_target_properties(vterm_static PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
target_include_directories(vterm_static PUBLIC ${_vt}/include PRIVATE ${_vt}/src)
if(MSVC)
    target_compile_options(vterm_static PRIVATE /wd4244 /wd4267 /wd4996)
endif()
