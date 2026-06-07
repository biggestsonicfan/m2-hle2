/*
 * constants.h — board-level constants for Sega Model 2.
 *
 * Memory regions, IRQ flags, IAC opcodes, condition codes. These describe
 * the Model 2 hardware, NOT any particular ROM set. Per-game numbers
 * (HLE hook addresses, ROM CRCs, input maps) live in game_profile.h.
 */
#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdint.h>

/* ---- CPU / scheduling ---------------------------------------------------- */

#define FRAME_STACK_DEPTH          64
#define FRAME_ALIGN_MASK           63
#define EMU_STEPS_PER_SLICE        500000  /* sized to always reach frame boundary */

/* ---- Display ------------------------------------------------------------- */

#define VIDEO_WIDTH                496
#define VIDEO_HEIGHT               384
#define VIDEO_BPP                  4       /* RGBA8 */

/* ---- Geometry capture ---------------------------------------------------- */

/* Smaller than this wraps mid-frame and produces partial 3D snapshots. */
#define GEO_CAPTURE_SIZE           32768

/* ---- Memory map ---------------------------------------------------------- */
/* Region table is linear-scanned in declaration order in memory.h.
 * TILE must appear before H_SYNC/V_SYNC overlap regions there. */

/* ROM */
#define ROM_BASE                   0x00000000
#define ROM_SIZE                   0x00100000

/* Work RAM */
#define RAM2_BASE                  0x00200000
#define RAM2_SIZE                  0x000CF218
#define RAM_BASE                   0x00500000
#define RAM_SIZE                   0x00100000

/* Geometry / 3D engine */
#define GEO_BASE                   0x00800000
#define GEO_SIZE                   0x00004000
#define GEO_PROGRAM_BASE           0x00804000
#define GEO_PROGRAM_SIZE           0x00004000
#define GEO_CMD_BASE               0x00840000
#define GEO_CMD_SIZE               0x00000200

/* Coprocessor */
#define COPROGRAM_BASE             0x00880000
#define COPROGRAM_SIZE             0x00020000
#define COPRO_SHARC_IOP_BASE       0x008C0000
#define COPRO_SHARC_IOP_SIZE       0x00001000

#define BUFF_RAM_BASE              0x00900000
#define BUFF_RAM_SIZE              0x00020000

#define COPRO_CONTROL1_BASE        0x00980000
#define COPRO_CONTROL1_SIZE        0x00000024
#define GEO_CTL1_BASE              0x00980008
#define GEO_CTL1_SIZE              0x00000004
#define COPRO_STATUS_BASE          0x00980014
#define COPRO_STATUS_SIZE          0x00000004

/* Audio */
#define MIDI_BASE                  0x009C0000
#define MIDI_SIZE                  0x00010000

/* CPU control / IRQ */
#define CPU_CTRL_BASE              0x00E00000
#define CPU_CTRL_SIZE              0x00000056
#define IRQ_REQUEST_BASE           0x00E80000
#define IRQ_REQUEST_SIZE           0x00000004
#define IRQ_ENABLE_BASE            0x00E80004
#define IRQ_ENABLE_SIZE            0x00000004

/* Timers */
#define TIMERS_BASE                0x00F00000
#define TIMERS_SIZE                0x00000015

/* Video — TILE covers tile maps, scroll regs, video ctrl as one block.
 * H_SYNC / V_SYNC overlap and are handled inside the TILE region. */
#define TILE_BASE                  0x01000000
#define TILE_SIZE                  0x00080000
#define H_SYNC_BASE                0x01040000
#define H_SYNC_SIZE                0x00000001
#define V_SYNC_BASE                0x01060000
#define V_SYNC_SIZE                0x00000001
#define TMAPGFX_BASE               0x01080000
#define TMAPGFX_SIZE               0x00080000
/* Extended video/work RAM — fills the gap between TMAPGFX and PALETTE.
 * Used by fvipers (and likely all Model 2B titles) for collision tables,
 * display lists, and other per-frame scratch data. */
#define VID_EXT_RAM_BASE           0x01100000
#define VID_EXT_RAM_SIZE           0x00700000

#define PALETTE_BASE               0x01800000
#define PALETTE_SIZE               0x00004000
#define COLORXLAT_BASE             0x01810000
#define COLORXLAT_SIZE             0x0000C000
#define ZCLIP_3D_BASE              0x0181C000
#define ZCLIP_3D_SIZE              0x00000004

/* I/O */
#define IO_BASE                    0x01C00000
#define IO_SIZE                    0x00000044
#define IO_IDLE_FILL               0xFF      /* hardware idle state, not 0x00 */
#define SERIAL_BASE                0x01C80000
#define SERIAL_SIZE                0x00000003
#define BACK_BASE                  0x01D00000
#define BACK_SIZE                  0x00010000

/* Cartridge / extra ROM data */
#define MAIN_DATA_BASE             0x02000000
#define MAIN_DATA_SIZE             0x02000000
#define XTRA_DATA_BASE             0x06000000
#define XTRA_DATA_SIZE             0x01000000

/* ---- 68000 sound CPU address space (Model 2B/2C, 68K-side map) ---------- */
/* Verified against MAME model2_state::model2_snd() address map and
 * schamp IDA disassembly (epr-19021).
 * The MIDI region (0x9C0000 in i960 space) is the i960's view of the
 * SCSP interface; the 68K sees those same registers at M68K_SCSP_BASE. */
#define M68K_WAVE_BASE     0x00000000u  /* sound / wave RAM (512 KB)        */
#define M68K_WAVE_SIZE     0x00080000u  /* 68K uses this for track+channel data */
#define M68K_SCSP_BASE     0x00100000u  /* SCSP registers (4 KB)            */
#define M68K_SCSP_SIZE     0x00001000u
#define M68K_SNDCTL_BASE   0x00400000u  /* sound control register (2 bytes) */
#define M68K_SNDCTL_SIZE   0x00000002u
#define M68K_ROM_BASE      0x00600000u  /* 68K program ROM (512 KB)         */
#define M68K_ROM_SIZE      0x00080000u
#define M68K_SAMPLE_BASE   0x00800000u  /* sample ROM window (up to 2 MB)   */
#define M68K_SAMPLE_SIZE   0x00200000u  /* 68K window covers first 2 MB     */

/* Unknown video MMIO */
#define UNKNOWN_VID_BASE           0x10000000
#define UNKNOWN_VID_SIZE           0x00000004

/* Texture RAM: 1MB each, mirrored.  0x11000000 & 0x11100000 alias texram0,
 * 0x11200000 & 0x11300000 alias texram1 (matches MAME model2b map).  The game
 * uploads textures via the 0x111xxxxx mirror; the rasterizer/atlas reads the
 * same 1MB buffer, so the mirror MUST alias or texel offsets are wrong.
 * STF also keeps a THIRD texram0 alias at 0x10F00000 (the i960 var `texram_0`
 * = 0x10F00000; IDA notes "MAME says this is 0x11000000").  The game uploads
 * stage/object textures through it, so it MUST alias texram0 or those textures
 * are dropped (object 3545's sheet-0 tiles came up blank without this). */
#define TEXRAM0_ALIAS_BASE         0x10F00000
#define TEXRAM0_BASE               0x11000000
#define TEXRAM0_SIZE               0x00100000
#define TEXRAM0_MIRROR_BASE        0x11100000
#define TEXRAM1_BASE               0x11200000
#define TEXRAM1_SIZE               0x00100000
#define TEXRAM1_MIRROR_BASE        0x11300000
#define LUMA_BASE                  0x11400000
#define LUMA_SIZE                  0x00020000
#define LUMA2_BASE                 0x12800000
#define LUMA2_SIZE                 0x00020000

/* Framebuffer */
#define FRAMEBUFFER_BASE           0x12C00000
#define FRAMEBUFFER_SIZE           0x00080000

/* IAC */
#define IAC_BASE                   0xFF000000
#define IAC_SIZE                   0x00000020
#define IAC_MSG_PORT               0xFF000010

/* ---- PRCB offsets -------------------------------------------------------- */

#define PRCB_START_IP              0x04
#define PRCB_INTR_TABLE            0x14
#define PRCB_INTR_STACK            0x18
#define PRCB_FAULT_TABLE           0x28

/* ---- IAC message types --------------------------------------------------- */

#define IAC_REINITIALIZE           0x93
#define IAC_PURGE_CACHE            0x89

/* ---- Condition codes (AC register bits 0-2) ------------------------------ */
/* chkbit sets CC_NO (0x0) when the tested bit is 0, NOT CC_NE.
 * bno depends on this — see CLAUDE.md i960 invariants. */

#define AC_CC_MASK                 0x7
#define CC_NO                      0x0
#define CC_G                       0x1
#define CC_E                       0x2
#define CC_GE                      0x3
#define CC_L                       0x4
#define CC_NE                      0x5
#define CC_LE                      0x6
#define CC_O                       0x7

/* Last i960 IP that issued a memory store — set in i960_exec.h before each
 * mem_write call so COP / MMIO handlers can log which instruction fired them. */
static volatile uint32_t g_last_store_ip = 0;

#endif /* CONSTANTS_H */
