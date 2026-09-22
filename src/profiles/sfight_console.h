/*
 * profiles/sfight_console.h — Sonic the Fighters - Console.
 *
 * The same ROM set and board as Sonic the Fighters - Arcade (sfight.h), with
 * the Honey and hidden-character patches Sega's own emulator applies when it
 * runs this ROM for the console release. This is the default profile for
 * sfight.zip; --profile sfight (or the Game menu) gives the arcade game.
 *
 * WHERE THE PATCHES COME FROM. The console build does not run the arcade ROM
 * unmodified. At board bring-up its emulator DLL overwrites 76 i960
 * instructions with a trap word, each dispatching to a native handler: a
 * table of {ROM offset, handler} at DLL RVA 0x1E8870. YAMP enumerated and named
 * them (source/m2ftg/Debug/Hooks/StfHooks.inc, "LJ/HleHooks" in its spec);
 * the handlers were read in Ghidra for this port. Each handler either changes
 * registers and SKIPS the instruction it sits on (returns the instruction's
 * length), or changes registers and then RUNS it. The hooks below do exactly
 * the same, on the same addresses, with the same register semantics. The
 * numbers in brackets are the DLL's table index.
 *
 * Only the patches that concern Honey and the hidden characters are here. The
 * rest of the DLL's 76 are its own plumbing (frame yield, vsync, texture-upload
 * budget, host sound), host features (VS-cabinet stage RNG and round flow,
 * progress reporting), or content that is not about the characters: stage
 * background colours [46-48], the wind term [11], the credit scroll [62] and
 * four attract durations [49-52]. None of those are ported.
 *
 * One deliberate difference from the DLL: it unlocks Metal Sonic and Robotnik
 * (and every hidden slot) only when the cabinet is set to VS mode, and Honey
 * alone otherwise. This profile always uses the VS table, so all three are
 * selectable in single-player and in netplay lobby matches.
 *
 * Character ids are rob+0x1B0 / +0x1B1. Hidden characters: 0x03 Metal Sonic,
 * 0x0B Robotnik, 0x0F Honey, and their second colours +0x1A (0x1D, 0x25, 0x29).
 *
 * DETERMINISM. The hidden-select latch is host state the i960 cannot see, so it
 * is cleared by install_fn -- which netplay's barrier reset re-runs -- and
 * changes only on the game's own momentary-input word (0x500704), which both
 * peers compute from the same lockstepped inputs.
 */
#ifndef PROFILES_SFIGHT_CONSOLE_H
#define PROFILES_SFIGHT_CONSOLE_H

#include "sfight.h"

/* ---- Shared helpers ------------------------------------------------------ */

/* The P1 / P2 rob pointers (the fighters' work areas). */
#define SFC_ROB_P1 0x00500804u
#define SFC_ROB_P2 0x00500808u

/* Byte `off` of player p's rob. */
static inline uint32_t sfc_rob_byte(memory_bus_t *bus, uint32_t rob_ptr, uint32_t off) {
    return mem_read8(bus, mem_read32(bus, rob_ptr) + off);
}

static inline bool sfc_is_honey(uint32_t c) { return c == 0x0F || c == 0x29; }

/* Metal Sonic, Robotnik, Honey, either colour. */
static inline bool sfc_is_hidden(uint32_t c) {
    switch (c) {
    case 0x03: case 0x0B: case 0x0F: case 0x1D: case 0x25: case 0x29: return true;
    default: return false;
    }
}

/* Length of the instruction a hook skips: 8 for a MEMB with a displacement
 * word (modes 5, 0xC-0xF), 4 otherwise -- the DLL's own rule. */
static inline uint32_t sfc_instr_len(memory_bus_t *bus, uint32_t ip) {
    uint32_t w = mem_read32(bus, ip);
    if ((w >> 24) < 0x80 || !((w >> 12) & 1)) return 4;
    uint32_t mode = (w >> 10) & 0xF;
    return (mode == 5 || mode >= 0xC) ? 8 : 4;
}

/* Skip the hooked instruction: the hook has done its work. */
static inline int sfc_skip(i960_cpu_t *cpu, memory_bus_t *bus) {
    cpu->sfr.ip += sfc_instr_len(bus, cpu->sfr.ip);
    return 0;
}

/* ---- Hidden-character select --------------------------------------------- */

/* The character each select slot turns into when its hidden variant is chosen,
 * 0 = the slot has none. The DLL's VS-mode table (RVA 0x1742A0): slot 0 gives
 * Metal Sonic, slot 3 Robotnik and slot 6 Honey. The non-VS table it uses
 * otherwise (RVA 0x174298) has slot 6 alone. */
static const uint8_t k_sfc_hidden_slot[8] = { 0x03, 0, 0, 0x0B, 0, 0, 0x0F, 0 };

/* Per-player "the hidden variant is chosen" latch (the DLL's 0x6C1A5C/5D). */
static uint8_t s_sfc_hidden[2];

/*
 * Start pressed on a slot with a hidden variant toggles it; moving the cursor
 * left or right clears it (DLL FUN_1800530A0). Returns true when it toggled.
 * A player cannot take the hidden variant of the slot the other player is on
 * when the other has already taken Metal Sonic or Robotnik (either colour).
 * Honey is not in that test, so both players can be her.
 */
static inline bool sfc_hidden_toggle(i960_cpu_t *cpu, memory_bus_t *bus, int p) {
    uint32_t sel = cpu->globals.g[13];                    /* the select screen's work */
    uint32_t mom = mem_read32(bus, 0x00500704);           /* momentary input */
    if ((mom >> (p + 4)) & 1) {                           /* this player's Start */
        uint32_t mine   = mem_read8(bus, sel + (p ? 0x78 : 0x5C));
        uint32_t theirs = mem_read8(bus, sel + (p ? 0x5C : 0x78));
        bool refused = false;
        if (mine == theirs) {
            uint32_t c = sfc_rob_byte(bus, p ? SFC_ROB_P1 : SFC_ROB_P2, 0x1B0);
            refused = c < 0x26 && ((0x2020000808ull >> c) & 1);   /* 0x03 0x0B 0x1D 0x25 */
        }
        if (!refused && mine < 8 && k_sfc_hidden_slot[mine]) {
            s_sfc_hidden[p] = !s_sfc_hidden[p];
            return true;
        }
    }
    if (mom & (p ? 0x00C00000u : 0x0000C000u))            /* left / right */
        s_sfc_hidden[p] = 0;
    return false;
}

/* [35] char_add2_pass_p1+0x810 (0x366F0) / [37] _p2 (0x3771C):
 * `ld 0x500248 / 0x50024C, r15` feeds `bbs 3, r15` into the ROM's dormant
 * hidden-select path. r15 = 8 exactly when Start toggled the latch. */
static int sfc_hook_hidden_flag_p1(i960_cpu_t *cpu, memory_bus_t *bus) {
    cpu->locals.r[15] = sfc_hidden_toggle(cpu, bus, 0) ? 8 : 0;
    return sfc_skip(cpu, bus);
}
static int sfc_hook_hidden_flag_p2(i960_cpu_t *cpu, memory_bus_t *bus) {
    cpu->locals.r[15] = sfc_hidden_toggle(cpu, bus, 1) ? 8 : 0;
    return sfc_skip(cpu, bus);
}

/* [36] char_add2_pass_p1+0x86C (0x3674C) / [38] _p2 (0x37778):
 * `ldib 0xDACAC(r4), r15`, slot -> character. With the latch set the hidden
 * character comes from the table (r15 left alone for a slot with none, as the
 * DLL does). Without it the ROM's own table, which the DLL's copy matches. */
static inline int sfc_slot_lookup(i960_cpu_t *cpu, memory_bus_t *bus, int p) {
    if (!s_sfc_hidden[p]) return 1;
    uint32_t slot = cpu->locals.r[4];
    if (slot < 8 && k_sfc_hidden_slot[slot]) cpu->locals.r[15] = k_sfc_hidden_slot[slot];
    return sfc_skip(cpu, bus);
}
static int sfc_hook_slot_p1(i960_cpu_t *cpu, memory_bus_t *bus) { return sfc_slot_lookup(cpu, bus, 0); }
static int sfc_hook_slot_p2(i960_cpu_t *cpu, memory_bus_t *bus) { return sfc_slot_lookup(cpu, bus, 1); }

/* [39] char_add2_pass_p1+0xA94 (0x36974) / [40] _p2 (0x379A0):
 * `bbc 4, r3` on a backup-RAM setting byte. A hidden character takes the
 * branch: bit 4 is cleared, then the instruction runs. */
static int sfc_hook_hidden_branch_p1(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (sfc_is_hidden(sfc_rob_byte(bus, SFC_ROB_P1, 0x1B0))) cpu->locals.r[3] &= ~0x10u;
    return 1;
}
static int sfc_hook_hidden_branch_p2(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (sfc_is_hidden(sfc_rob_byte(bus, SFC_ROB_P2, 0x1B0))) cpu->locals.r[3] &= ~0x10u;
    return 1;
}

/* [64] pl1_skp+0x9C (0xA9E8) / [65] pl1_skp+0x1E0 (0xAB2C):
 * `stib r15, 0x1B1(g7)` after the colour fold. Unless the opponent is the
 * same character in either colour, rob+0x1B0 takes the folded id too, so a
 * hidden pick is not left with its unfolded one. Then the store runs. */
static inline int sfc_sync_char(i960_cpu_t *cpu, memory_bus_t *bus, uint32_t other_rob) {
    uint32_t c     = cpu->locals.r[15];
    uint32_t other = sfc_rob_byte(bus, other_rob, 0x1B0);
    if (other != c && other != c + 0x1A)
        mem_write8(bus, cpu->globals.g[7] + 0x1B0, c & 0xFF);
    return 1;
}
static int sfc_hook_sync_char_p1(i960_cpu_t *cpu, memory_bus_t *bus) { return sfc_sync_char(cpu, bus, SFC_ROB_P2); }
static int sfc_hook_sync_char_p2(i960_cpu_t *cpu, memory_bus_t *bus) { return sfc_sync_char(cpu, bus, SFC_ROB_P1); }

/* [66] name_init+0x88 (0x4F41C): `st r7, 0x1D03280[r3*8]` writes the ranking
 * record, r3 being ROM 0xDC0E4[character] & 7. The hidden characters' entries
 * are 8 and up, so the mask aliased them onto a real character's record. For
 * those the store is skipped and name entry gets mode 1 instead of 9. */
static int sfc_hook_name_rank(i960_cpu_t *cpu, memory_bus_t *bus) {
    uint32_t rank = mem_read8(bus, 0x000DC0E4 + mem_read8(bus, cpu->globals.g[7] + 0x1B1));
    if (rank < 8) { cpu->locals.r[15] = 9; return 1; }
    cpu->locals.r[15] = 1;
    return sfc_skip(cpu, bus);
}

/* [67] name_init+0x90 (0x4F424): `mov 9, r15` is deleted; [66] set r15. */
static int sfc_hook_name_mode(i960_cpu_t *cpu, memory_bus_t *bus) {
    return sfc_skip(cpu, bus);
}

/* ---- Honey: art ---------------------------------------------------------- */

/* [68] rm_wipe_in+0x44 (0x7D6E4) / [69] +0x5C (0x7D6FC): `ldos table[r3*2], g0`,
 * the VS portrait by rob+0x1B1. Honey's entries point at Robotnik's art. */
static int sfc_hook_vs_portrait_p1(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!sfc_is_honey(cpu->locals.r[3])) return 1;
    cpu->globals.g[0] = 0x96;
    return sfc_skip(cpu, bus);
}
static int sfc_hook_vs_portrait_p2(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!sfc_is_honey(cpu->locals.r[3])) return 1;
    cpu->globals.g[0] = 0x98;
    return sfc_skip(cpu, bus);
}

/* [70-72] rm_char_move+0x38, rm_char_disp_int+0x68, rm_char_move2+0x4C and
 * [73-75] their +0x6C / +0xB8 / +0x80 twins: the P1 / P2 character card. */
static int sfc_hook_card_p1(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!sfc_is_honey(cpu->locals.r[3])) return 1;
    cpu->globals.g[0] = 0x19A;
    return sfc_skip(cpu, bus);
}
static int sfc_hook_card_p2(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!sfc_is_honey(cpu->locals.r[3])) return 1;
    cpu->globals.g[0] = 0x19C;
    return sfc_skip(cpu, bus);
}

/* [44] md_deathegg_sekkin_init+0xB8 (0x58054): `ld 0x97588[r3*4], r3`, the
 * Death Egg approach cutscene's model by character. Honey's (0x0F) entry is
 * the default 0xBD3; the DLL gives her 0xDBD, and character 0x10 0xEA9. */
static int sfc_hook_deathegg(i960_cpu_t *cpu, memory_bus_t *bus) {
    uint32_t c = cpu->locals.r[3];
    if (c == 0x0F)      cpu->locals.r[3] = 0xDBD;
    else if (c == 0x10) cpu->locals.r[3] = 0xEA9;
    else return 1;
    return sfc_skip(cpu, bus);
}

/* ---- Honey: animation ---------------------------------------------------- */

/* The 16 words calc_rob_angle_int copies for Honey's second angle table, ROM
 * 0xC49C4 (her record at 0xC4D54 points at 0xC4984; +0x40 is this one). DLL
 * RVA 0x17C8B0. The ROM's are 0x50C..0x68A; these sit beside her second
 * colour's (0x29, table 0xC4A04: 0xAA8..0xAE1). */
static const uint32_t k_sfc_honey_angles[16] = {
    0x000, 0x5DD, 0xA99, 0xA9B, 0xAA5, 0xAA3, 0xA9A, 0xAA4,
    0xAA2, 0x000, 0xA9D, 0xA98, 0xA89, 0xA9C, 0xA8A, 0xA88,
};

/* [41] calc_rob_angle_int+0xB4 (0x2EFEC): `ld (r14), r15` in the copy loop. */
static int sfc_hook_honey_angles(i960_cpu_t *cpu, memory_bus_t *bus) {
    uint32_t off = cpu->locals.r[14] - 0x000C49C4u;
    if (off >= 0x40) return 1;
    cpu->locals.r[15] = k_sfc_honey_angles[off >> 2];
    return sfc_skip(cpu, bus);
}

/* [42] snc_eye_thd_set+0x148 (0x215B8) / [43] +0x188 (0x215F8): `lda 0, g3`
 * before the eye-tracking call. Honey's eyes do not track: g1/g2, the
 * target, are zeroed, then the instruction runs. */
static int sfc_hook_honey_eyes(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (sfc_is_honey(mem_read8(bus, cpu->globals.g[7] + 0x1B0))) {
        cpu->globals.g[1] = 0;
        cpu->globals.g[2] = 0;
    }
    return 1;
}

/* [45] get_frame_dat+0x140 (0x30608): the motion blend, r11 the delta the
 * weight multiplies, r12 the loop count down from 24. The head tilt is the
 * delta at counts 7..9 (elements 15-17); it is zeroed, then the multiply runs.
 * The DLL does this for every character, and so does this profile. */
static int sfc_hook_head_tilt(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    if (cpu->locals.r[12] - 7u < 3u) cpu->locals.r[11] = 0;
    return 1;
}

/* ---- Profile object ------------------------------------------------------ */

static inline void sfight_console_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    s_sfc_hidden[0] = s_sfc_hidden[1] = 0;
    sfight_install(rs, cpu, bus);
}

static const game_profile_t sfight_console_profile = {
    .id               = "sfight_console",
    .display_name     = "Sonic the Fighters - Console",
    .rom_set          = "sfight",
    .parent_zip_name  = "schamp.zip",
    .board            = BOARD_MODEL2B_CRX,
    .load_fn      = sfight_load,
    .install_fn   = sfight_console_install,
    .hook_count   = SFIGHT_BASE_HOOK_COUNT + 23,
    .hooks        = {
        SFIGHT_BASE_HOOKS
        /* hidden-character select */
        { 0x000366F0, sfc_hook_hidden_flag_p1,   "char_add2_pass_p1+0x810" },
        { 0x0003674C, sfc_hook_slot_p1,          "char_add2_pass_p1+0x86c" },
        { 0x00036974, sfc_hook_hidden_branch_p1, "char_add2_pass_p1+0xa94" },
        { 0x0003771C, sfc_hook_hidden_flag_p2,   "char_add2_pass_p2+0x810" },
        { 0x00037778, sfc_hook_slot_p2,          "char_add2_pass_p2+0x86c" },
        { 0x000379A0, sfc_hook_hidden_branch_p2, "char_add2_pass_p2+0xa94" },
        { 0x0000A9E8, sfc_hook_sync_char_p1,     "pl1_skp+0x9c"            },
        { 0x0000AB2C, sfc_hook_sync_char_p2,     "pl1_skp+0x1e0"           },
        { 0x0004F41C, sfc_hook_name_rank,        "name_init+0x88"          },
        { 0x0004F424, sfc_hook_name_mode,        "name_init+0x90"          },
        /* Honey's art */
        { 0x0007D6E4, sfc_hook_vs_portrait_p1,   "rm_wipe_in+0x44"         },
        { 0x0007D6FC, sfc_hook_vs_portrait_p2,   "rm_wipe_in+0x5c"         },
        { 0x0007D750, sfc_hook_card_p1,          "rm_char_move+0x38"       },
        { 0x0007D824, sfc_hook_card_p1,          "rm_char_disp_int+0x68"   },
        { 0x0007D944, sfc_hook_card_p1,          "rm_char_move2+0x4c"      },
        { 0x0007D784, sfc_hook_card_p2,          "rm_char_move+0x6c"       },
        { 0x0007D874, sfc_hook_card_p2,          "rm_char_disp_int+0xb8"   },
        { 0x0007D978, sfc_hook_card_p2,          "rm_char_move2+0x80"      },
        { 0x00058054, sfc_hook_deathegg,         "md_deathegg_sekkin_init+0xb8" },
        /* Honey's animation */
        { 0x0002EFEC, sfc_hook_honey_angles,     "calc_rob_angle_int+0xb4" },
        { 0x000215B8, sfc_hook_honey_eyes,       "snc_eye_thd_set+0x148"   },
        { 0x000215F8, sfc_hook_honey_eyes,       "snc_eye_thd_set+0x188"   },
        { 0x00030608, sfc_hook_head_tilt,        "get_frame_dat+0x140"     },
    },
    .input        = { SFIGHT_INPUT_MAP },
    .quirks       = { SFIGHT_QUIRKS },
};

#endif /* PROFILES_SFIGHT_CONSOLE_H */
