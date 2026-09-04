cmake_minimum_required(VERSION 3.20)
project(m2hle C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
set(SRC    ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Version stamp. CI (.github/workflows/canary.yml) passes -DM2HLE_VERSION=rNNN-<sha>;
# a local build just reports "dev".
set(M2HLE_VERSION "dev" CACHE STRING "Version string baked into the binary")
add_compile_definitions(M2HLE_VERSION="${M2HLE_VERSION}")

# ---------------------------------------------------------------------------
# cimgui + imgui (single static lib)
#
# We use the dear_bindings-generated cimgui (flat layout, imgui sources alongside
# cimgui.cpp). The earlier setup with cimgui/cimgui as a submodule produced an
# ImGuiIO ABI mismatch — MousePos updated but MouseDown stayed 0, suggesting
# field offsets in the generated C header didn't align with the imgui C++ build.
# These files come from a known-working dear_bindings drop (imgui 1.92.6).
# ---------------------------------------------------------------------------
add_library(cimgui STATIC
    ${VENDOR}/cimgui/cimgui.cpp
    ${VENDOR}/cimgui/cimgui_internal.cpp
    ${VENDOR}/cimgui/imgui.cpp
    ${VENDOR}/cimgui/imgui_demo.cpp
    ${VENDOR}/cimgui/imgui_draw.cpp
    ${VENDOR}/cimgui/imgui_tables.cpp
    ${VENDOR}/cimgui/imgui_widgets.cpp
)
target_include_directories(cimgui PUBLIC ${VENDOR}/cimgui)

# ---------------------------------------------------------------------------
# ImGuiFileDialog
# ---------------------------------------------------------------------------
add_library(imguifiledialog STATIC
    ${VENDOR}/ImGuiFileDialog/ImGuiFileDialog.cpp
)
target_include_directories(imguifiledialog PUBLIC ${VENDOR}/ImGuiFileDialog)
target_link_libraries(imguifiledialog PUBLIC cimgui)

# ---------------------------------------------------------------------------
# miniz (C)
# miniz's normal CMake build generates miniz_export.h via
# GenerateExportHeader. We build it as a plain static lib instead, so
# stub the export macros in our binary dir and put it on the include path.
# ---------------------------------------------------------------------------
set(MINIZ_GEN_DIR ${CMAKE_BINARY_DIR}/miniz-gen)
file(WRITE ${MINIZ_GEN_DIR}/miniz_export.h
"#ifndef MINIZ_EXPORT_H\n#define MINIZ_EXPORT_H\n#define MINIZ_EXPORT\n#define MINIZ_NO_EXPORT\n#endif\n")
add_library(miniz STATIC
    ${VENDOR}/miniz/miniz.c
    ${VENDOR}/miniz/miniz_tdef.c
    ${VENDOR}/miniz/miniz_tinfl.c
    ${VENDOR}/miniz/miniz_zip.c
)
target_include_directories(miniz PUBLIC ${VENDOR}/miniz ${MINIZ_GEN_DIR})

# ---------------------------------------------------------------------------
# sokol implementation TU
# Sokol is header-only; one translation unit must define SOKOL_IMPL.
# On macOS this must be compiled as Objective-C.
# ---------------------------------------------------------------------------
if(APPLE)
    add_library(sokol STATIC ${SRC}/sokol_impl.m)
    set_source_files_properties(${SRC}/sokol_impl.m PROPERTIES COMPILE_FLAGS "-x objective-c -fobjc-arc")
    target_link_libraries(sokol PUBLIC
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework MetalKit"
        "-framework AudioToolbox"
    )
elseif(EMSCRIPTEN)
    add_library(sokol STATIC ${SRC}/sokol_impl.c)
    target_compile_definitions(sokol PUBLIC SOKOL_GLES3)
elseif(WIN32)
    add_library(sokol STATIC ${SRC}/sokol_impl.c)
    target_compile_definitions(sokol PUBLIC SOKOL_D3D11)
    target_link_libraries(sokol PUBLIC kernel32 user32 gdi32 ole32 d3d11 dxgi)
else() # Linux / BSD
    add_library(sokol STATIC ${SRC}/sokol_impl.c)
    target_compile_definitions(sokol PUBLIC SOKOL_GLCORE)
    find_package(Threads REQUIRED)
    target_link_libraries(sokol PUBLIC X11 Xi Xcursor GL dl m Threads::Threads)
endif()
target_include_directories(sokol PUBLIC ${VENDOR}/sokol ${VENDOR}/sokol/util)
# sokol_imgui pulls in cimgui; sokol's TU must see the cimgui headers.
target_link_libraries(sokol PUBLIC cimgui)

# ---------------------------------------------------------------------------
# m2hle executable
# ---------------------------------------------------------------------------
if(WIN32)
    add_executable(m2hle WIN32 ${SRC}/main.c)
else()
    add_executable(m2hle ${SRC}/main.c)
endif()
target_link_libraries(m2hle PRIVATE sokol miniz imguifiledialog)
target_include_directories(m2hle PRIVATE
    ${SRC}
    ${SRC}/board
    ${SRC}/core
    ${SRC}/ui
    ${SRC}/profiles
)
set_target_properties(m2hle PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_BINARY_DIR}/Debug
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Release
)

if(EMSCRIPTEN)
    set_target_properties(m2hle PROPERTIES SUFFIX ".html")
    target_link_options(m2hle PRIVATE
        "-sUSE_WEBGL2=1"
        "-sALLOW_MEMORY_GROWTH=1"
    )
endif()

# Dead-strip on Clang/Apple
if(CMAKE_C_COMPILER_ID STREQUAL "AppleClang" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    if(APPLE)
        target_link_options(m2hle PRIVATE "-Wl,-dead_strip")
    endif()
endif()

if(WIN32)
    target_link_libraries(m2hle PRIVATE winmm)
endif()

# ---------------------------------------------------------------------------
# Unit tests (standalone, header-driven). Run via ctest or run_tests.ps1.
# Each test #defines NDEBUG itself; we also quiet MSVC CRT deprecation noise.
# ---------------------------------------------------------------------------
option(M2HLE_BUILD_TESTS "Build the standalone unit tests" ON)
# The tests are native console programs (some load ROMs from absolute dev-machine
# paths) — not meaningful for the Emscripten/web build, so skip them there.
if(M2HLE_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    set(TEST_INCLUDES ${SRC} ${SRC}/board ${SRC}/core ${SRC}/ui ${SRC}/profiles)

    add_executable(mem_test  ${CMAKE_CURRENT_SOURCE_DIR}/tests/mem_test.c)
    add_executable(i960_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/i960_test.c)
    add_executable(rom_test  ${CMAKE_CURRENT_SOURCE_DIR}/tests/rom_test.c)
    add_executable(emu_test  ${CMAKE_CURRENT_SOURCE_DIR}/tests/emu_test.c)
    add_executable(boot_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/boot_test.c)
    add_executable(cop_test  ${CMAKE_CURRENT_SOURCE_DIR}/tests/cop_test.c)
    add_executable(geo_test  ${CMAKE_CURRENT_SOURCE_DIR}/tests/geo_test.c)
    add_executable(m68k_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/m68k_opcode_test.c)
    add_executable(input_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/input_test.c)
    target_link_libraries(rom_test   PRIVATE miniz)
    target_link_libraries(boot_test  PRIVATE miniz)
    target_link_libraries(geo_test   PRIVATE miniz)
    # emu_test/input_test pull emu_thread.h -> sound.h -> scsp_hle.h (sokol
    # headers) and input.h -> sokol_app.h; linking sokol propagates the includes.
    target_link_libraries(emu_test   PRIVATE sokol)
    target_link_libraries(input_test PRIVATE miniz sokol)

    foreach(t mem_test i960_test rom_test emu_test boot_test cop_test geo_test m68k_test input_test)
        target_include_directories(${t} PRIVATE ${TEST_INCLUDES})
        if(MSVC)
            target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
endif()
