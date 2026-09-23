/*
 * sokol_gfx + sokol_log + sokol_debugtext implementation for the SDL3 frontend
 * (main_sdl.c), which owns its window, GL context, input and timing through SDL
 * and so needs none of sokol_app, sokol_audio or sokol_imgui. The backend
 * (SOKOL_GLES3) is set by CMake.
 */
#define SOKOL_GFX_IMPL
#define SOKOL_LOG_IMPL
#define SOKOL_DEBUGTEXT_IMPL
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_debugtext.h"
/* The PS3-style menus draw through sokol_gl (ui/ps3ui_gpu.h). */
#define SOKOL_GL_IMPL
#include "sokol_gl.h"
