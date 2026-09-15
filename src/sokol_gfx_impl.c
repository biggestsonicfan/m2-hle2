/*
 * sokol_gfx + sokol_log implementation for the SDL3 frontend (main_sdl.c),
 * which owns its window, GL context, input and timing through SDL and so needs
 * none of sokol_app, sokol_audio or sokol_imgui. The backend (SOKOL_GLES3) is
 * set by CMake.
 */
#define SOKOL_GFX_IMPL
#define SOKOL_LOG_IMPL
#include "sokol_gfx.h"
#include "sokol_log.h"
