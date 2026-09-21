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

# The board's arithmetic has to come out the same on every host: the COP's
# low bits feed back into the next frame (sharc.h). GCC and Clang fuse a*b+c
# into one FMA instruction wherever the target has it — AArch64 always does,
# x86-64 only with -march — and a fused result differs in the last bit, so an
# ARM build's RAM left an x86 build's two frames after boot.
#
# -fno-strict-aliasing for the same reason. Somewhere the board reads memory
# through a pointer of another type, which C leaves undefined; MSVC never
# optimises on that rule and Clang does. The wasm build (Clang) left the
# desktop build (MSVC) at frame 2948 of attract, and computed the same frames
# again with this flag and no other change (tests/det_digest.c, WEB-NETPLAY.md
# "Cross-play"). It costs nothing measurable there.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-ffp-contract=off -fno-strict-aliasing)
endif()

set(VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
set(SRC    ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Version stamp. CI (.github/workflows/canary.yml) passes -DM2HLE_VERSION=rNNN-<sha>;
# a local build just reports "dev".
set(M2HLE_VERSION "dev" CACHE STRING "Version string baked into the binary")
add_compile_definitions(M2HLE_VERSION="${M2HLE_VERSION}")

# How many 68000 clock periods before the end of an instruction the sound
# board samples the SCSP interrupt lines (board/sound.h, SOUND_IPL_LEAD). It is
# a fitted constant standing in for the real chip sampling IPL at the microcode
# step that loads the instruction register, which falls a different number of
# cycles before the end of each instruction -- so it is settable here in order
# to be swept against MAME rather than left as a literal nobody can grade.
set(M2HLE_SOUND_IPL_LEAD "" CACHE STRING "68000 IPL sampling lead, in clock periods (blank = the default in sound.h)")
if(M2HLE_SOUND_IPL_LEAD)
    add_compile_definitions(SOUND_IPL_LEAD=${M2HLE_SOUND_IPL_LEAD})
endif()

# Which host to build:
#   sokol  the ImGui debugger (sokol_app window, D3D11 / GL core / Metal)
#   sdl3   the fullscreen handheld frontend (src/main_sdl.c): SDL3 window and
#          input, sokol_gfx on GLES 3, no ImGui — and so no Python either
#   web    the browser build (src/main_web.c, Emscripten only): sokol_app on a
#          canvas, sokol_gfx on WebGL2, no ImGui; the page in web/ is its UI.
#          See WEB-PORT.md. Configure with emcmake.
#   libretro  a libretro core (src/main_libretro.c): m2hle_libretro.dll/.so for
#          RetroArch, which owns the window, input, audio and netplay lobby.
#          GL 4.1 core; -DM2HLE_LIBRETRO_GLES=ON for GLES 3 (the handheld).
set(M2HLE_FRONTEND "sokol" CACHE STRING "Host frontend: sokol (ImGui debugger), sdl3 (fullscreen, GLES 3), web (Emscripten) or libretro (core)")
set_property(CACHE M2HLE_FRONTEND PROPERTY STRINGS sokol sdl3 web libretro)
set(M2HLE_SDL3 OFF)
set(M2HLE_WEB  OFF)
set(M2HLE_LIBRETRO OFF)
if(M2HLE_FRONTEND STREQUAL "sdl3")
    set(M2HLE_SDL3 ON)
elseif(M2HLE_FRONTEND STREQUAL "libretro")
    set(M2HLE_LIBRETRO ON)
elseif(M2HLE_FRONTEND STREQUAL "web")
    set(M2HLE_WEB ON)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "M2HLE_FRONTEND=web is the Emscripten build. Configure it with emcmake:\n"
                            "  emcmake cmake -S . -B build_web -DM2HLE_FRONTEND=web")
    endif()
elseif(NOT M2HLE_FRONTEND STREQUAL "sokol")
    message(FATAL_ERROR "M2HLE_FRONTEND must be sokol, sdl3, web or libretro, not '${M2HLE_FRONTEND}'")
endif()

# ---------------------------------------------------------------------------
# Vendored dependencies are git submodules. Fail early and usefully if the
# tree was cloned without them.
# ---------------------------------------------------------------------------
if(M2HLE_SDL3 OR M2HLE_WEB OR M2HLE_LIBRETRO)
    set(M2HLE_SUBMODULE_PATHS vendor/sokol vendor/miniz)
    set(M2HLE_SUBMODULE_PROBES sokol/sokol_gfx.h miniz/miniz.c)
else()
    set(M2HLE_SUBMODULE_PATHS vendor/imgui vendor/dear_bindings vendor/sokol vendor/miniz vendor/ImGuiFileDialog vendor/imgui_club)
    set(M2HLE_SUBMODULE_PROBES imgui/imgui.h dear_bindings/dear_bindings.py sokol/sokol_gfx.h miniz/miniz.c ImGuiFileDialog/ImGuiFileDialog.cpp
                               imgui_club/imgui_memory_editor/imgui_memory_editor.h)
endif()
foreach(probe ${M2HLE_SUBMODULE_PROBES})
    if(NOT EXISTS ${VENDOR}/${probe})
        # Joined with spaces: interpolating the list directly renders it
        # semicolon-separated, so the command the message told you to run was one
        # a shell splits into six broken commands.
        string(JOIN " " M2HLE_SUBMODULE_LIST ${M2HLE_SUBMODULE_PATHS})
        message(FATAL_ERROR
            "vendor/${probe} is missing. The vendored dependencies are git submodules:\n"
            "  git submodule update --init ${M2HLE_SUBMODULE_LIST}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# net/tls.h opens the system's OpenSSL with dlopen on Linux rather than linking
# it (the comment there says why). glibc has had dlopen in libc itself since
# 2.34, so libdl is linked only where it is still separate: an unconditional
# -ldl would add a NEEDED entry the ROCKNIX binary is not meant to carry.
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    include(CheckSymbolExists)
    check_symbol_exists(dlopen "dlfcn.h" M2HLE_DLOPEN_IN_LIBC)
    if(NOT M2HLE_DLOPEN_IN_LIBC)
        link_libraries(${CMAKE_DL_LIBS})
    endif()
endif()

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
# sdl3 frontend: m2hle from src/main_sdl.c, then stop — nothing below (ImGui,
# sokol_app, the unit tests) is part of this build.
# ---------------------------------------------------------------------------
if(M2HLE_SDL3)
    find_package(SDL3 REQUIRED CONFIG)
    find_library(M2HLE_GLESV2 GLESv2 REQUIRED)
    find_package(Threads REQUIRED)

    add_library(sokol_gfx STATIC ${SRC}/sokol_gfx_impl.c)
    target_compile_definitions(sokol_gfx PUBLIC SOKOL_GLES3)
    target_include_directories(sokol_gfx PUBLIC ${VENDOR}/sokol ${VENDOR}/sokol/util)
    target_link_libraries(sokol_gfx PUBLIC ${M2HLE_GLESV2})

    add_executable(m2hle ${SRC}/main_sdl.c)
    target_link_libraries(m2hle PRIVATE sokol_gfx miniz SDL3::SDL3 Threads::Threads m)
    target_include_directories(m2hle PRIVATE
        ${SRC}
        ${SRC}/board
        ${SRC}/core
        ${SRC}/ui
        ${SRC}/profiles
    )
    install(TARGETS m2hle RUNTIME DESTINATION bin)
    return()
endif()

# ---------------------------------------------------------------------------
# libretro core: m2hle_libretro from src/main_libretro.c, then stop. A shared
# library RetroArch loads; everything static inside it, so position-independent.
# ---------------------------------------------------------------------------
if(M2HLE_LIBRETRO)
    option(M2HLE_LIBRETRO_GLES "Build the core for GLES 3 (ARM handhelds) instead of GL 4.1 core" OFF)
    # Android's RetroArch only has GLES.
    if(ANDROID)
        set(M2HLE_LIBRETRO_GLES ON CACHE BOOL "" FORCE)
    endif()
    # Only the retro_* API leaves the library (libretro.h marks it default).
    set_target_properties(miniz PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

    add_library(sokol_gfx STATIC ${SRC}/sokol_gfx_impl.c)
    set_target_properties(sokol_gfx PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
    target_include_directories(sokol_gfx PUBLIC ${VENDOR}/sokol ${VENDOR}/sokol/util)
    if(M2HLE_LIBRETRO_GLES)
        # Android keeps the GLES 3 entry points in libGLESv3; everywhere else
        # (Mesa, libglvnd, the Mali blobs) libGLESv2 carries them all.
        if(ANDROID)
            find_library(M2HLE_GLES GLESv3 REQUIRED)
        else()
            find_library(M2HLE_GLES GLESv2 REQUIRED)
        endif()
        target_compile_definitions(sokol_gfx PUBLIC SOKOL_GLES3 M2HLE_LIBRETRO_GLES)
        target_link_libraries(sokol_gfx PUBLIC ${M2HLE_GLES})
        if(ANDROID)
            target_link_libraries(sokol_gfx PUBLIC log)   # sokol_log writes to logcat
        endif()
    else()
        # Windows: sokol loads the GL entry points itself from the current
        # context (opengl32 + wglGetProcAddress), so there is nothing to link.
        target_compile_definitions(sokol_gfx PUBLIC SOKOL_GLCORE)
        if(APPLE)
            target_compile_definitions(sokol_gfx PUBLIC GL_SILENCE_DEPRECATION)
        endif()
        if(NOT WIN32)
            find_package(OpenGL REQUIRED)
            target_link_libraries(sokol_gfx PUBLIC OpenGL::GL)
        endif()
    endif()

    add_library(m2hle_libretro SHARED ${SRC}/main_libretro.c)
    set_target_properties(m2hle_libretro PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
    # RetroArch's names: m2hle_libretro.dll / .so / .dylib, and on Android
    # m2hle_libretro_android.so.
    if(ANDROID)
        set_target_properties(m2hle_libretro PROPERTIES OUTPUT_NAME m2hle_libretro_android)
    endif()
    # A shared library may leave symbols undefined, so a missing piece (a
    # renderer that did not compile, a library not linked) only shows when
    # RetroArch loads it. Make it a link error instead, where the linker can.
    if(NOT WIN32 AND NOT APPLE)
        target_link_options(m2hle_libretro PRIVATE -Wl,-z,defs)
    endif()
    target_link_libraries(m2hle_libretro PRIVATE sokol_gfx miniz)
    target_include_directories(m2hle_libretro PRIVATE
        ${SRC}
        ${SRC}/board
        ${SRC}/core
        ${SRC}/net
        ${SRC}/ui
        ${SRC}/profiles
    )
    if(WIN32)
        # RPCN netplay (src/net): the transport and Schannel, as for m2hle.exe.
        target_link_libraries(m2hle_libretro PRIVATE ws2_32 secur32 crypt32 bcrypt winmm)
        # wglGetCurrentDC: the window's size, for "Full screen" (main_libretro.c).
        target_link_libraries(m2hle_libretro PRIVATE opengl32)
        target_compile_definitions(m2hle_libretro PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(m2hle_libretro PRIVATE Threads::Threads m)
    endif()
    install(TARGETS m2hle_libretro LIBRARY DESTINATION lib/libretro RUNTIME DESTINATION lib/libretro)
    return()
endif()

# ---------------------------------------------------------------------------
# web frontend: build_web/site/ is the whole website — m2hle.js + m2hle.wasm from
# src/main_web.c, beside the page from web/. Then stop: nothing below (ImGui,
# the desktop sokol TU, the unit tests) is part of this build.
#
# One thread, on purpose: GitHub Pages cannot send the COOP/COEP headers that
# SharedArrayBuffer (and so -pthread) needs. WEB-PORT.md, "Threading".
# ---------------------------------------------------------------------------
if(M2HLE_WEB)
    add_library(sokol STATIC ${SRC}/sokol_web_impl.c)
    target_compile_definitions(sokol PUBLIC SOKOL_GLES3)
    target_include_directories(sokol PUBLIC ${VENDOR}/sokol ${VENDOR}/sokol/util)

    add_executable(m2hle ${SRC}/main_web.c)
    target_compile_definitions(m2hle PRIVATE M2HLE_WEB)
    target_link_libraries(m2hle PRIVATE sokol miniz)
    target_include_directories(m2hle PRIVATE
        ${SRC}
        ${SRC}/board
        ${SRC}/core
        ${SRC}/net
        ${SRC}/ui
        ${SRC}/profiles
    )
    set(M2HLE_SITE_DIR ${CMAKE_BINARY_DIR}/site)
    set_target_properties(m2hle PROPERTIES
        SUFFIX ".js"
        RUNTIME_OUTPUT_DIRECTORY ${M2HLE_SITE_DIR}
    )
    target_link_options(m2hle PRIVATE
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        # The ROM regions alone are 82.5 MB and the bus copies 48 MB of them again;
        # the player's zip (18 MB) is alive only while it is being extracted.
        "-sINITIAL_MEMORY=268435456"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sSTACK_SIZE=8388608"
        "-sEXPORTED_FUNCTIONS=_main,_malloc,_free"
        "-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF32,UTF8ToString,stringToUTF8,lengthBytesUTF8"
    )
    # The page: copied and version-stamped on every build. A target of its own and
    # not a POST_BUILD step of m2hle, which only runs when m2hle relinks -- so an
    # edit to web/site alone left the old page in the site directory.
    add_custom_target(m2hle_site ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/web/site ${M2HLE_SITE_DIR}
        COMMAND ${CMAKE_COMMAND} -DSITE=${M2HLE_SITE_DIR} -DVERSION=${M2HLE_VERSION}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/web/stamp-site.cmake
        DEPENDS m2hle
        COMMENT "web: page -> ${M2HLE_SITE_DIR} (${M2HLE_VERSION})"
        VERBATIM)
    # The board as the page runs it, under Node, one digest line per frame: the
    # wasm half of the cross-play check (tests/det_digest.c). Built only when
    # asked for (--target det_digest), with every flag the page's build has.
    add_executable(det_digest EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/tests/det_digest.c)
    target_compile_definitions(det_digest PRIVATE M2HLE_WEB)
    target_link_libraries(det_digest PRIVATE sokol miniz)
    target_include_directories(det_digest PRIVATE
        ${SRC} ${SRC}/board ${SRC}/core ${SRC}/net ${SRC}/ui ${SRC}/profiles)
    target_link_options(det_digest PRIVATE
        "-sNODERAWFS=1" "-sINITIAL_MEMORY=536870912" "-sALLOW_MEMORY_GROWTH=1" "-sSTACK_SIZE=8388608")
    return()
endif()

# ---------------------------------------------------------------------------
# cimgui + imgui (single static lib)
#
# Dear ImGui (vendor/imgui) and the dear_bindings generator
# (vendor/dear_bindings) are submodules; the C bindings are generated into the
# build tree at build time, so no generated source is committed.
#
# The bindings MUST come from dear_bindings and not from the cimgui/cimgui
# repo — that is a different generator, and its C structs did not line up with
# the imgui C++ build here: an ImGuiIO ABI mismatch where MousePos updated but
# MouseDown stayed 0.
#
# '--replace-prefix ImGui_=ig' keeps the ig* spelling that src/ uses and that
# sokol_imgui.h detects as the 'original cimgui' flavour.
#
# Needs Python 3 with ply:  python -m pip install ply==3.11
# ---------------------------------------------------------------------------
set(IMGUI_DIR         ${VENDOR}/imgui)
set(DEAR_BINDINGS_DIR ${VENDOR}/dear_bindings)
set(CIMGUI_GEN_DIR    ${CMAKE_BINARY_DIR}/cimgui-gen)

find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import ply"
    RESULT_VARIABLE CIMGUI_PLY_RESULT
    OUTPUT_QUIET ERROR_QUIET)
if(NOT CIMGUI_PLY_RESULT EQUAL 0)
    message(FATAL_ERROR
        "dear_bindings needs the Python 'ply' module, which ${Python3_EXECUTABLE} cannot import.\n"
        "  \"${Python3_EXECUTABLE}\" -m pip install ply==3.11")
endif()

file(MAKE_DIRECTORY ${CIMGUI_GEN_DIR})

add_custom_command(
    OUTPUT ${CIMGUI_GEN_DIR}/cimgui.h ${CIMGUI_GEN_DIR}/cimgui.cpp
    COMMAND ${Python3_EXECUTABLE} ${DEAR_BINDINGS_DIR}/dear_bindings.py
            -o ${CIMGUI_GEN_DIR}/cimgui
            --replace-prefix ImGui_=ig
            ${IMGUI_DIR}/imgui.h
    DEPENDS ${IMGUI_DIR}/imgui.h ${IMGUI_DIR}/imconfig.h ${DEAR_BINDINGS_DIR}/dear_bindings.py
    COMMENT "dear_bindings: cimgui.{h,cpp} from imgui.h"
    VERBATIM)

add_custom_command(
    OUTPUT ${CIMGUI_GEN_DIR}/cimgui_internal.h ${CIMGUI_GEN_DIR}/cimgui_internal.cpp
    COMMAND ${Python3_EXECUTABLE} ${DEAR_BINDINGS_DIR}/dear_bindings.py
            -o ${CIMGUI_GEN_DIR}/cimgui_internal
            --include ${IMGUI_DIR}/imgui.h
            --replace-prefix ImGui_=ig
            ${IMGUI_DIR}/imgui_internal.h
    DEPENDS ${IMGUI_DIR}/imgui_internal.h ${IMGUI_DIR}/imgui.h ${IMGUI_DIR}/imconfig.h
            ${DEAR_BINDINGS_DIR}/dear_bindings.py
    COMMENT "dear_bindings: cimgui_internal.{h,cpp} from imgui_internal.h"
    VERBATIM)

add_library(cimgui STATIC
    ${CIMGUI_GEN_DIR}/cimgui.h
    ${CIMGUI_GEN_DIR}/cimgui.cpp
    ${CIMGUI_GEN_DIR}/cimgui_internal.h
    ${CIMGUI_GEN_DIR}/cimgui_internal.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
# cimgui.h includes imconfig.h, and the generated .cpp files include imgui.h,
# so both directories are public.
target_include_directories(cimgui PUBLIC ${CIMGUI_GEN_DIR} ${IMGUI_DIR})

# ---------------------------------------------------------------------------
# ImGuiFileDialog
# ---------------------------------------------------------------------------
add_library(imguifiledialog STATIC
    ${VENDOR}/ImGuiFileDialog/ImGuiFileDialog.cpp
)
target_include_directories(imguifiledialog PUBLIC ${VENDOR}/ImGuiFileDialog)
target_link_libraries(imguifiledialog PUBLIC cimgui)

# ---------------------------------------------------------------------------
# imgui_club — the hex editor behind src/ui/memview.h. Header-only C++ against
# the ImGui C++ API, so src/ui/mem_edit.cpp is the one translation unit that
# includes it and hands the rest of src/ a C handle.
# ---------------------------------------------------------------------------
set(IMGUI_CLUB_MEMEDIT_DIR ${VENDOR}/imgui_club/imgui_memory_editor)

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
    # asound: sokol_audio's Linux backend is ALSA (audio_out.h plays the sound board).
    target_link_libraries(sokol PUBLIC X11 Xi Xcursor GL asound dl m Threads::Threads)
endif()
target_include_directories(sokol PUBLIC ${VENDOR}/sokol ${VENDOR}/sokol/util)
# sokol_imgui pulls in cimgui; sokol's TU must see the cimgui headers.
target_link_libraries(sokol PUBLIC cimgui)

# ---------------------------------------------------------------------------
# m2hle executable
# ---------------------------------------------------------------------------
if(WIN32)
    add_executable(m2hle WIN32 ${SRC}/main.c ${SRC}/ui/mem_edit.cpp)
else()
    add_executable(m2hle ${SRC}/main.c ${SRC}/ui/mem_edit.cpp)
endif()
target_link_libraries(m2hle PRIVATE sokol miniz imguifiledialog)
target_include_directories(m2hle PRIVATE
    ${SRC}
    ${SRC}/board
    ${SRC}/core
    ${SRC}/net
    ${SRC}/ui
    ${SRC}/profiles
    ${IMGUI_CLUB_MEMEDIT_DIR}
)

# ---------------------------------------------------------------------------
# Netplay (src/net) — RPCN matchmaking + peer-to-peer lockstep.
#
# No new dependency: the sockets and the TLS client are the platform's own.
# ws2_32 is the transport; secur32/crypt32/bcrypt are Schannel, which is the
# only TLS backend implemented (src/net/tls.h explains the seam and what a
# POSIX build does instead — netplay compiles there and reports that it has no
# backend rather than failing to link).
# ---------------------------------------------------------------------------
if(WIN32)
    target_link_libraries(m2hle PRIVATE ws2_32 secur32 crypt32 bcrypt)
endif()
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
    set(TEST_INCLUDES ${SRC} ${SRC}/board ${SRC}/core ${SRC}/net ${SRC}/ui ${SRC}/profiles)

    add_executable(mem_test  ${CMAKE_CURRENT_SOURCE_DIR}/tests/mem_test.c)
    add_executable(i960_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/i960_test.c)
    add_executable(rom_test  ${CMAKE_CURRENT_SOURCE_DIR}/tests/rom_test.c)
    add_executable(emu_test  ${CMAKE_CURRENT_SOURCE_DIR}/tests/emu_test.c)
    add_executable(boot_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/boot_test.c)
    add_executable(cop_test  ${CMAKE_CURRENT_SOURCE_DIR}/tests/cop_test.c)
    add_executable(geo_test  ${CMAKE_CURRENT_SOURCE_DIR}/tests/geo_test.c)
    add_executable(m68k_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/m68k_opcode_test.c)
    add_executable(input_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/input_test.c)
    # Netplay: the lockstep engine, the protobuf codec and the ComId standard.
    # No sockets and no server — see the header of tests/net_test.c.
    add_executable(net_test  ${CMAKE_CURRENT_SOURCE_DIR}/tests/net_test.c)
    # Not a ctest: replays a MAME capture of the COP conversation (needs a capture file).
    add_executable(cop_replay ${CMAKE_CURRENT_SOURCE_DIR}/tests/cop_replay.c)
    target_include_directories(cop_replay PRIVATE ${TEST_INCLUDES})
    if(MSVC)
        target_compile_definitions(cop_replay PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    # Not a ctest: replays a MAME sound capture's MIDI stream through the sound board.
    add_executable(snd_replay ${CMAKE_CURRENT_SOURCE_DIR}/tests/snd_replay.c)
    target_include_directories(snd_replay PRIVATE ${TEST_INCLUDES})
    target_link_libraries(snd_replay PRIVATE miniz)
    if(MSVC)
        target_compile_definitions(snd_replay PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    # Not a ctest: a per-frame digest of the board for holding two builds against
    # each other (the wasm-vs-desktop cross-play gate). Needs a merged ROM zip.
    add_executable(det_digest ${CMAKE_CURRENT_SOURCE_DIR}/tests/det_digest.c)
    target_include_directories(det_digest PRIVATE ${TEST_INCLUDES})
    target_link_libraries(det_digest PRIVATE miniz sokol)
    if(MSVC)
        target_compile_definitions(det_digest PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    target_link_libraries(rom_test   PRIVATE miniz)
    target_link_libraries(boot_test  PRIVATE miniz)
    target_link_libraries(geo_test   PRIVATE miniz)
    # emu_test/input_test pull input.h -> sokol_app.h (and sokol_gfx through the
    # UI headers); linking sokol propagates the includes.
    target_link_libraries(emu_test   PRIVATE sokol)
    target_link_libraries(input_test PRIVATE miniz sokol)

    foreach(t mem_test i960_test rom_test emu_test boot_test cop_test geo_test m68k_test input_test net_test)
        target_include_directories(${t} PRIVATE ${TEST_INCLUDES})
        if(MSVC)
            target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
    # The board headers call sqrtf/sincosf/floor..., which live in libm off
    # Windows. m2hle, emu_test and input_test get it through sokol; the rest link
    # nothing that carries it.
    if(NOT WIN32)
        foreach(t mem_test i960_test rom_test emu_test boot_test cop_test geo_test m68k_test
                  input_test net_test cop_replay snd_replay)
            target_link_libraries(${t} PRIVATE m)
        endforeach()
    endif()
endif()
