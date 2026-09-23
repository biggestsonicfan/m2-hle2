/*
 * The sokol implementation unit for the web frontend (main_web.c): sokol_app for
 * the canvas, the WebGL2 context, input and the frame callback, sokol_gfx on
 * GLES 3, and sokol_audio. No sokol_imgui -- the web build has no ImGui, and so
 * no cimgui to generate and no Python in its build. The backend (SOKOL_GLES3) is
 * set by CMake.
 */
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_audio.h"
/* The PS3-style menus draw through sokol_gl (ui/ps3ui_gpu.h). */
#define SOKOL_GL_IMPL
#include "sokol_gl.h"
