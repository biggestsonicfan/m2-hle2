/*
 * build_features.h -- the optional parts a build carries (Pinboard #220).
 *
 * One tree makes four programs (CMakeLists.txt, M2HLE_FRONTEND): the desktop
 * debugger (sokol + ImGui + the MCP bridge), the handheld (sdl3), the libretro
 * core and the browser (web). Each frontend includes only the headers it uses,
 * but the board is shared, and some of what sits in the board's hot paths is
 * there for the desktop's tools alone. A switch here says whether a build has
 * them; CMake sets it per frontend.
 *
 * M2HLE_DEV_TOOLS -- the debugger's hooks inside the board: code breakpoints
 *   (breakpoint.h), data watchpoints (watchpoint.h), the display-list / COP
 *   write capture (memory.h, g_dl) and the sound-board capture (sound.h,
 *   g_sndcap). Only the MCP bridge and the desktop's windows arm them.
 *   1 unless the build says otherwise: the desktop and the tests. CMake sets 0
 *   for sdl3, libretro and web, whose frontends have nothing that arms them.
 *   With 0 the state and the functions stay, so shared code and the tests built
 *   in those trees (det_digest under web) compile unchanged, but every "is it
 *   armed" test is a constant false: the bus's writes stop testing g_wp and
 *   g_dl, and the run loop stops testing g_bp. Nothing the board computes
 *   changes -- an unarmed hook never did anything.
 *
 * Platform code is gated where it is used, by the compiler's own macros
 * (_WIN32, __linux__, __EMSCRIPTEN__, __ANDROID__), not here.
 */
#ifndef BUILD_FEATURES_H
#define BUILD_FEATURES_H

#ifndef M2HLE_DEV_TOOLS
#define M2HLE_DEV_TOOLS 1
#endif

#endif /* BUILD_FEATURES_H */
