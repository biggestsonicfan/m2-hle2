/* macOS Objective-C translation unit — same body as sokol_impl.c. */
#define SOKOL_IMPL
#define SOKOL_METAL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"

#include "cimgui.h"
#ifndef ImDrawCallback_ResetRenderState
#define ImDrawCallback_ResetRenderState ((ImDrawCallback)(intptr_t)(-8))
#endif
#include "sokol_imgui.h"
