/*
 * Single C TU compiling all sokol header-only modules + sokol_imgui.
 * Backend selection (SOKOL_D3D11 / SOKOL_GLCORE / SOKOL_GLES3) is set
 * by CMake per-platform.
 *
 * sokol_imgui auto-detects the active C binding: cimgui/cimgui defines
 * CIMGUI_INCLUDED and uses *_Nil suffixes; dear_bindings does not, and
 * uses the unsuffixed names. We use dear_bindings.
 */
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"
#include "sokol_audio.h"

#include "cimgui.h"
#include "sokol_imgui.h"
