/*
 * board.mjs — the addresses the graders read, in one place.
 *
 * Every constant here is either a Model 2 board region out of
 * src/board/constants.h or a Sonic The Fighters variable out of the
 * disassembly. They are written down once so a tool never carries a bare hex
 * literal, and so a region that moves is corrected in one file rather than in
 * however many graders happened to read it.
 *
 * The sizes are not incidental: TEXRAM_BYTES, LUMA_BYTES and CXLAT_BYTES are
 * the same numbers the explorer's texture.js and colors.js export, which is
 * what lets a dump of the emulator's region be compared against the explorer's
 * build with no reshaping at all. If one side ever changes, the graders should
 * fail loudly rather than compare a prefix — see assertSizes below.
 */
import { nc } from './noclip.mjs';

/* ---- board regions (src/board/constants.h) ------------------------------- */

export const RAM_BASE       = 0x00500000;
export const COPROGRAM_BASE = 0x00880000;
export const GEO_PROGRAM_BASE = 0x00804000;
export const BUFF_RAM_BASE  = 0x00900000;

export const PALETTE_BASE   = 0x01800000;
export const PALETTE_BYTES  = 0x00004000;

export const COLORXLAT_BASE = 0x01810000;
export const CXLAT_BYTES    = 0x0000c000;

export const TEXRAM0_BASE   = 0x11000000;
export const TEXRAM1_BASE   = 0x11200000;
export const TEXRAM_BYTES   = 0x00100000;

export const LUMA_BASE      = 0x11400000;
export const LUMA_BYTES     = 0x00020000;

export const MAIN_DATA_BASE = 0x02000000;

/* ---- Sonic The Fighters variables ---------------------------------------- */

/* The scene the draw routines branch on. A byte, and the same one the MAME
 * capture Lua pins to walk the game into a chosen arena. */
export const STAGE_NUM = 0x00500064;

/* The game's own frame counter. */
export const FRAME_COUNTER = 0x00500020;

/* The stage record table in the program ROM, and the 64-word copy change_scene
 * makes of whichever record it chose.
 *
 * This pair is what makes a capture trustworthy. Pinning stage_num only sets
 * what the draw routines branch on; the scene itself was chosen the last time
 * change_scene ran, which may well have been before the pin. Comparing the
 * loaded copy against the ROM record is the only way to know the game really is
 * in the arena that was asked for.
 *
 * The comparison is made on the record's two texture-set words rather than on
 * the whole record: change_scene copies the record and then the game goes on
 * writing to the copy — cage_clip_m clears panels it has destroyed, the Flying
 * Carpet rewrites its own vector every frame — so a scene that is genuinely the
 * right one still differs in a couple of dozen words. What it cannot differ in
 * is which texture sets it asked for. */
export const STAGE_DATA = 0x0008f3d0;
export const STAGE_STRIDE = 256;
export const STAGE_LOADED = 0x00504800;
export const STAGE_TEX_A = 0x0c;
export const STAGE_TEX_B = 0x0e;

/* ---- input bits (through the real 315-5649 I/O ports) -------------------- */

export const IN = {
    P1_UP: 0x00002000, P1_DOWN: 0x00001000, P1_LEFT: 0x00008000, P1_RIGHT: 0x00004000,
    P1_B1: 0x00000100, P1_B2: 0x00000200, P1_B3: 0x00000400,
    P2_UP: 0x00200000, P2_DOWN: 0x00100000, P2_LEFT: 0x00800000, P2_RIGHT: 0x00400000,
    P2_B1: 0x00010000, P2_B2: 0x00020000, P2_B3: 0x00040000,
    START1: 0x00000010, START2: 0x00000020, SERVICE: 0x00000004,
};

/* ---- the regions a grader dumps ------------------------------------------ */

/** name -> {base, size}. `dump-board.mjs` writes one file per entry. */
export const REGIONS = {
    texram0:   { base: TEXRAM0_BASE,   size: TEXRAM_BYTES },
    texram1:   { base: TEXRAM1_BASE,   size: TEXRAM_BYTES },
    lumaram:   { base: LUMA_BASE,      size: LUMA_BYTES },
    colorxlat: { base: COLORXLAT_BASE, size: CXLAT_BYTES },
    /* A face's colour comes from palram[colorbase + 0x1000], not from the table
     * in the data ROM, and the two do not agree — so this is the only place the
     * shading the board actually uses can be read. */
    palram:    { base: PALETTE_BASE,   size: PALETTE_BYTES },
};

/**
 * Fail loudly if the emulator's region sizes and the explorer's buffer sizes
 * have drifted apart. Comparing a 1 MB dump against a 512 kB build by way of a
 * prefix would report a clean pass on half a sheet, which is exactly the sort
 * of quiet wrong answer a grading harness exists to prevent.
 */
export async function assertSizes() {
    const tex = await nc('texture.js');
    const col = await nc('colors.js');
    const pairs = [
        ['texture sheet', TEXRAM_BYTES, tex.SHEET_BYTES],
        ['luma RAM',      LUMA_BYTES,   col.LUMA_BYTES],
        ['colorxlat',     CXLAT_BYTES,  col.CXLAT_BYTES],
    ];
    for (const [what, ours, theirs] of pairs) {
        if (ours !== theirs) {
            throw new Error(
                `${what}: the emulator's region is 0x${ours.toString(16)} bytes but the ` +
                `explorer builds 0x${theirs.toString(16)} — the two have drifted apart, ` +
                'and comparing them would grade a prefix. Fix board.mjs or the profile.');
        }
    }
}
