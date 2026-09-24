/*
 * profiles/sfight.h — Sonic the Fighters - Arcade (sfight/schamp) profile.
 *
 * The game as the arcade board runs it. It shares its ROM set with Sonic the
 * Fighters - Console (sfight_console.h), which is the default for sfight.zip;
 * pick this one with --profile sfight or from the Game menu. MAME runs the
 * arcade game, so every grader in tools/ asks for this profile.
 *
 * MAME-equivalent ROM set: sfight (clone of schamp). load_fn extracts files
 * from sfight.zip first, falls back to schamp.zip for shared files.
 *
 * Local override: if files named sfight/epr-19001.15 and sfight/epr-19002.16
 * exist on disk, they're used INSTEAD of the zip's maincpu pair (CRC check
 * skipped) — supports working with hacked ROMs without re-zipping every time.
 *
 * Per-game install behaviour: copies romset into bus regions, then mirrors
 * romset.main_data[0x01000000..] into bus->xtra_data. The mirror was found
 * empirically (string "SNC_ZIBA" at ROM[0x01000000+0x4012FB]) — STF-specific.
 *
 * The profile carries the loader, installer, input map, quirks and the HLE
 * hook table (SFIGHT_BASE_HOOKS below; sfight_console.h adds to it).
 */
#ifndef PROFILES_SFIGHT_H
#define PROFILES_SFIGHT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "rom_loader.h"
#include "game_profile.h"
#include "memory.h"
#include "i960.h"
#include "hle_hooks.h"

/* ---- Loader ------------------------------------------------------------- */

#define SFIGHT_LOAD_PAIR_32(dest, dest_sz, name_lo, crc_lo, name_hi, crc_hi, half_sz, offset) \
    do { \
        tmp1 = zip_extract_from_set(child, parent, name_lo, &fsize, crc_lo); \
        tmp2 = zip_extract_from_set(child, parent, name_hi, &fsize, crc_hi); \
        if (!tmp1 || !tmp2) { ok = 0; goto done; } \
        interleave_32_word(dest, dest_sz, tmp1, half_sz, tmp2, half_sz, offset); \
        free(tmp1); tmp1 = NULL; \
        free(tmp2); tmp2 = NULL; \
    } while(0)

#define SFIGHT_LOAD_SAMPLE(dest, dest_sz, name, crc, offset) \
    do { \
        tmp1 = zip_extract_from_set(child, parent, name, &fsize, crc); \
        if (tmp1) { \
            load_16_word_swap(dest, dest_sz, tmp1, fsize, offset); \
            free(tmp1); tmp1 = NULL; \
        } \
    } while(0)

static inline int sfight_load(romset_t *rs, const char *child, const char *parent) {
    romset_free(rs);

    size_t   fsize = 0;
    uint8_t *tmp1 = NULL, *tmp2 = NULL;
    int      ok = 1;

    /* maincpu (2MB) — check for hacked override files first */
    rs->maincpu_size = 0x200000;
    rs->maincpu = (uint8_t *)calloc(1, rs->maincpu_size);
    {
        size_t sz1 = 0, sz2 = 0;
        uint8_t *hack1 = file_load("sfight/epr-19001.15", &sz1);
        uint8_t *hack2 = file_load("sfight/epr-19002.16", &sz2);
        if (hack1 && hack2) {
            LOG_INFO("sfight: using hacked maincpu from sfight/ (%zu, %zu bytes; CRC skipped)", sz1, sz2);
            interleave_32_word(rs->maincpu, rs->maincpu_size, hack1, sz1, hack2, sz2, 0);
            free(hack1); free(hack2);
        } else {
            if (hack1) free(hack1);
            if (hack2) free(hack2);
            SFIGHT_LOAD_PAIR_32(rs->maincpu, rs->maincpu_size,
                "epr-19001.15", 0x9b088511, "epr-19002.16", 0x46f510da, 0x80000, 0x000000);
        }
    }

    /* main_data (32MB) — large data region with a 1MB→15MB mirror */
    rs->main_data_size = 0x2000000;
    rs->main_data = (uint8_t *)calloc(1, rs->main_data_size);
    SFIGHT_LOAD_PAIR_32(rs->main_data, rs->main_data_size,
        "mpr-19007.11", 0x8b8ff751, "mpr-19008.12", 0xa94654f5, 0x400000, 0x0000000);
    SFIGHT_LOAD_PAIR_32(rs->main_data, rs->main_data_size,
        "mpr-19005.9",  0x98cd1127, "mpr-19006.10", 0xe79f0a26, 0x400000, 0x0800000);
    SFIGHT_LOAD_PAIR_32(rs->main_data, rs->main_data_size,
        "epr-19003.7",  0x63bae5c5, "epr-19004.8",  0xc10c9f39, 0x80000,  0x1000000);
    for (uint32_t d = 0x1100000; d < 0x2000000; d += 0x100000)
        rom_region_copy(rs->main_data, rs->main_data_size, 0x1000000, d, 0x100000);

    /* copro_data (8MB) */
    rs->copro_data_size = 0x800000;
    rs->copro_data = (uint8_t *)calloc(1, rs->copro_data_size);
    SFIGHT_LOAD_PAIR_32(rs->copro_data, rs->copro_data_size,
        "mpr-19015.29", 0xc74d99e3, "mpr-19016.30", 0x746ae931, 0x200000, 0x000000);

    /* polygons (16MB) */
    rs->polygons_size = 0x1000000;
    rs->polygons = (uint8_t *)calloc(1, rs->polygons_size);
    SFIGHT_LOAD_PAIR_32(rs->polygons, rs->polygons_size,
        "mpr-19009.17", 0xfd410350, "mpr-19012.21", 0x9bb7b5b6, 0x400000, 0x000000);
    SFIGHT_LOAD_PAIR_32(rs->polygons, rs->polygons_size,
        "mpr-19010.18", 0x6fd94187, "mpr-19013.22", 0x9e232fe5, 0x400000, 0x800000);

    /* textures (16MB) */
    rs->textures_size = 0x1000000;
    rs->textures = (uint8_t *)calloc(1, rs->textures_size);
    SFIGHT_LOAD_PAIR_32(rs->textures, rs->textures_size,
        "mpr-19019.27", 0x59121896, "mpr-19017.25", 0x7b298379, 0x400000, 0x000000);
    SFIGHT_LOAD_PAIR_32(rs->textures, rs->textures_size,
        "mpr-19020.28", 0x9540dba0, "mpr-19018.26", 0x3b7e7a12, 0x400000, 0x800000);

    /* audiocpu (512KB) — byte-swapped on load */
    rs->audiocpu_size = 0x80000;
    rs->audiocpu = (uint8_t *)calloc(1, rs->audiocpu_size);
    tmp1 = zip_extract_from_set(child, parent, "epr-19021.31", &fsize, 0x0b9f7583);
    if (!tmp1) { ok = 0; goto done; }
    load_16_word_swap(rs->audiocpu, rs->audiocpu_size, tmp1, fsize, 0);
    free(tmp1); tmp1 = NULL;

    /* samples (8MB) */
    rs->samples_size = 0x800000;
    rs->samples = (uint8_t *)calloc(1, rs->samples_size);
    SFIGHT_LOAD_SAMPLE(rs->samples, rs->samples_size, "mpr-19022.32", 0x4381869b, 0x000000);
    SFIGHT_LOAD_SAMPLE(rs->samples, rs->samples_size, "mpr-19023.33", 0x07c67f88, 0x200000);
    SFIGHT_LOAD_SAMPLE(rs->samples, rs->samples_size, "mpr-19024.34", 0x15ff76d3, 0x400000);
    SFIGHT_LOAD_SAMPLE(rs->samples, rs->samples_size, "mpr-19025.35", 0x6ad8fb70, 0x600000);

    rs->loaded = true;
    LOG_INFO("sfight: ROM set loaded (maincpu=%zu main_data=%zu copro=%zu poly=%zu tex=%zu audio=%zu samp=%zu)",
             rs->maincpu_size, rs->main_data_size, rs->copro_data_size,
             rs->polygons_size, rs->textures_size, rs->audiocpu_size, rs->samples_size);

done:
    free(tmp1);
    free(tmp2);
    if (!ok) { LOG_ERROR("sfight: ROM set load failed"); romset_free(rs); return -1; }
    return 0;
}

#undef SFIGHT_LOAD_PAIR_32
#undef SFIGHT_LOAD_SAMPLE

/* ---- Installer ---------------------------------------------------------- */

/* Reads a little-endian u32 from a flat buffer. */
static inline uint32_t sfight_read32(const uint8_t *buf, uint32_t off) {
    return  (uint32_t)buf[off]
         | ((uint32_t)buf[off + 1] << 8)
         | ((uint32_t)buf[off + 2] << 16)
         | ((uint32_t)buf[off + 3] << 24);
}

/* A profile's own work at a VS-mode rematch (sfight_hook_vs_rematch), for host
 * state the ROM's "both players continue" path cannot reset by itself: the
 * Console profile's hidden-select latch. Set by that profile's install_fn after
 * sfight_install, which clears it, so a board switched back to Arcade does not
 * keep it. */
static void (*s_sfight_on_vs_rematch)(void);

static inline void sfight_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("sfight_install: romset not loaded"); return; }
    s_sfight_on_vs_rematch = NULL;

    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);

    /* The COP reads its sine/cosine tables straight out of this ROM. */
    g_sharc_copro_rom      = rs->copro_data;
    g_sharc_copro_rom_size = rs->copro_data_size;

    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
        LOG_INFO("sfight_install: copied %zu bytes to MAIN_DATA", n);
    }

    /* STF-specific: XTRA_DATA at 0x06000000 mirrors main_data starting at
     * offset 0x01000000. Confirmed empirically — string "SNC_ZIBA" lives at
     * ROM[0x01000000+0x4012FB]. */
    if (rs->main_data && bus->xtra_data) {
        const uint32_t src_off = 0x01000000;
        if (src_off < rs->main_data_size) {
            size_t avail = rs->main_data_size - src_off;
            size_t n = avail < XTRA_DATA_SIZE ? avail : XTRA_DATA_SIZE;
            memcpy(bus->xtra_data, rs->main_data + src_off, n);
            LOG_INFO("sfight_install: mirrored %zu bytes ROM[0x%08X..] → XTRA_DATA", n, src_off);
        }
    }

    /* Read PRCB from maincpu[+0x04] → start_ip_ptr → initial IP. */
    uint32_t sat_ptr  = sfight_read32(rs->maincpu, 0x00);
    uint32_t prcb_ptr = sfight_read32(rs->maincpu, 0x04);
    LOG_INFO("sfight_install: SAT=0x%08X  PRCB=0x%08X", sat_ptr, prcb_ptr);
    if (prcb_ptr + 0x2C < rs->maincpu_size) {
        uint32_t start_ip_ptr = sfight_read32(rs->maincpu, prcb_ptr + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) {
            cpu->sfr.ip = sfight_read32(rs->maincpu, start_ip_ptr);
        }
    }
    LOG_INFO("sfight_install: initial IP = 0x%08X", cpu->sfr.ip);
}

/* ---- HLE hook functions -------------------------------------------------- */

/* cop_initialize_l1 (0x0F3C): boot-time COP-ready spin loop. Reads
 * COPRO_CONTROL1+4 bit 0 until set. Set the bit and let the instruction run
 * normally — the next iteration sees ready=1 and exits. Without this hook STF
 * hangs on the BACKUP RAM screen forever. */
static int sfight_hook_cop_init_l1(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    uint32_t cur = mem_read32(bus, COPRO_CONTROL1_BASE + 4);
    mem_write32(bus, COPRO_CONTROL1_BASE + 4, cur | 0x01);
    return 1;
}

/* check_timer_4 (0x4A55C): spin loop waiting for a timer interrupt.
 * Skip the whole function — return 0 to the caller. */
static int sfight_hook_check_timer_4(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    cpu->globals.g[0] = 0;
    hle_ret(cpu);
    return 0;
}

/* check_timer_4_spin (0x4A58C): inner polling loop reading byte_50008C.
 * Write 0x01 so the loop exits on its own next iteration. */
static int sfight_hook_check_timer_4_spin(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    mem_write8(bus, 0x0050008C, 0x01);
    return 1;
}

/* interrupt_wait (0x1768): inject VsyncScr then skip the spin loop entirely.
 * The real loop spins until RAM_BASE >= 2 and bit 0 is clear. We skip it by
 * returning to 0x1778 (the instruction after the loop) and use hle_call to run
 * VsyncScr (0x0C40) first so per-frame 2D work fires. */
static int sfight_hook_interrupt_wait(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    hle_call(cpu, 0x00000C40, 0x00001778);
    return 0;
}

/* interrupt_wait_b (0x11580): clear RAM_BASE to 0 so _idle's spin loop runs and
 * execution falls through to the _idle hook (rather than jumping past it). */
static int sfight_hook_interrupt_wait_b(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    mem_write8(bus, RAM_BASE, 0x00);
    return 1;
}

/* _idle (0x11610): inject VsyncScr on first entry; run the ldob normally after.
 * First call: push a VsyncScr frame returning to 0x11610 so the hook fires again.
 * Second call: let ldob execute so the cmpibe at 0x11618 sees the value and exits. */
static int sfight_hook_idle(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    static int s_vsync_fired = 0;
    if (!s_vsync_fired) {
        s_vsync_fired = 1;
        hle_call(cpu, 0x00000C40, 0x00011610);
        return 0;
    }
    s_vsync_fired = 0;
    return 1;
}

/* _700000_loop (0x7264): sound-init delay loop — zero r3 so cmpdeco exits. */
static int sfight_hook_700000_loop(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    cpu->locals.r[3] = 0;
    return 1;
}

/* variable_diff_calc (0x11A04): fires once per game frame at the end of the
 * main loop. Setting g_frame_done lets the emu thread pace to the next 60Hz
 * tick (and ends the slice early, freeing the mutex for the UI); the geo
 * capture ring's frame boundary is marked so the scanner reads exactly one
 * game frame's draw commands. */
static void sfight_xplay_frame(memory_bus_t *bus);

static int sfight_hook_frame_pace(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    sfight_xplay_frame(bus);
    cop_geo_frame_edge();
    g_frame_done = 1;
    emu_attn_bump();
    return 1;
}

/* co_processor_error_hang (0x77F8): infinite self-branch on COP self-test
 * failure. Halt the CPU and log g4's error code so the UI stays responsive. */
static int sfight_hook_cop_err_hang(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    LOG_ERROR("COP self-test failed — error code 0x%08X  (IP=0x%08X)",
              cpu->globals.g[4], cpu->sfr.ip);
    cpu->halted = 1;
    return 0;
}

/*
 * versus_result (0xDC3C, in ROUND_DSP): a two-player match has been decided.
 * OBSERVE ONLY -- it records the result and lets the instruction run.
 *
 * 0xDC3C is `stib r15, _sub_mode`, the step past the arcade's win-streak
 * bookkeeping (byte_500066 / word_5000A2) that runs once per decided match.
 * The PS3 port hooks the same instruction for the same reason
 * (i960hook_DC3C_JUDGE_postResult, which posts its OnMatchResult event), and
 * gates it on the same two facts the arcade's own streak code tests just before:
 * not_scr_bg_move (0x500068) bit 1, the versus flag, and gameprogram (0x50004C)
 * == 2, both players. The winning side is `winner` (0x500065): 0 = 1P.
 */
static int sfight_hook_versus_result(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    if ((mem_read8(bus, 0x00500068) & 2u) && mem_read8(bus, 0x0050004C) == 2u) {
        g_versus_result = mem_read8(bus, 0x00500065) ? 2 : 1;
        LOG_INFO("versus match decided: %s won", g_versus_result == 1 ? "1P" : "2P");
    }
    return 1;
}

/*
 * vs_rematch (0xE584, next_round+0x1A4): VS mode's rematch. Sega's console
 * emulator traps this instruction (its table index 26, handler RVA 0x52EC0).
 *
 * 0xE584 is `ldob winner, r14`, which a decided versus match reaches on the way
 * to 0xF5D4 (1P won) or 0xF620 (2P won). Both of those keep the winner on and
 * send the loser out, so the next fight is the winner against the CPU. With VS
 * mode set, the DLL ORs 5 into both players' flag words (0x500248 / 0x50024C,
 * bits 0 and 2) and jumps to 0xF524 instead. That is the ROM's own "both players
 * continue" path: it sets bit 2 of both flags again, clears the round state and
 * branches to SEL_INT with mode 6. So the board is back at character select
 * with both players already in.
 *
 * With VS mode off the instruction runs as it always has, so the Arcade profile
 * and every grader see the ROM's own flow.
 *
 * s_sfight_on_vs_rematch runs first when a profile set one (Console: clear the
 * hidden-select latch, which the select screen does not show).
 */
static int sfight_hook_vs_rematch(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!g_vs_mode) return 1;
    if (s_sfight_on_vs_rematch) s_sfight_on_vs_rematch();
    mem_write8(bus, 0x00500248, (uint8_t)(mem_read8(bus, 0x00500248) | 5u));
    mem_write8(bus, 0x0050024C, (uint8_t)(mem_read8(bus, 0x0050024C) | 5u));
    cpu->sfr.ip = 0x0000F524;
    return 0;
}

/*
 * country_default (0x62688, init_game_assignments+0x1A8): the factory default
 * of the region setting. The instruction is `stob r15, country_val_bk`
 * (backup RAM 0x1D03352) after `mov 0, r15`, and the next one stores r15 again
 * to the working copy, country_val (0x59C352). Load g_region into r15 and let
 * both stores run. Backup RAM starts blank on every boot here, so the game
 * takes this path on every cold boot, as well as from the test menu's
 * INITIALIZE. Japan (0) shows the "only in Japan" warning; USA (1) skips it,
 * shows the FBI picture before the Sega logo in attract, and uses the US
 * names and credit limit.
 */
static int sfight_hook_country_default(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    cpu->locals.r[15] = (uint32_t)g_region;
    return 1;
}

/*
 * damage_default (0x62674, init_game_assignments+0x194): the factory default of
 * the GAME ASSIGNMENTS flag byte, `stob r15, game_assign_byte_flag_bk` after
 * `mov 0, r15`, with the store to the working copy (0x59C353) next. Like
 * country_default, the game takes this path on every cold boot here. With
 * g_damage_real set, bit 7 goes in: the test menu's DAMAGE row reads it through
 * DAMAGE_TYPE {NORMAL, REAL}. Every other bit keeps its factory 0.
 *
 * What the bit does: `ketchup` (0x19740, called by damage_calculation) scales
 * each hit by 1 + 0.01 * the energy gap, clamped, when bit 7 is CLEAR (NORMAL)
 * and returns the damage as it is when it is set (REAL); ACT_RC_DOWN_ATTACK
 * tests it the same way. So REAL is the setting without the catch-up.
 */
static int sfight_hook_damage_default(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    if (g_damage_real) cpu->locals.r[15] = 0x80u;
    return 1;
}

/*
 * Cross-play with the PS3 port (net/ps3_link.h, hle_hooks.h g_xplay_*). The PS3
 * build's emulator runs a network match through traps on these three
 * instructions, and a board playing against it has to reach the same points:
 *
 * xplay_force_start (0x83F4, ADV_DSP `call player_entry`): the PS3 writes
 * START1|START2 into INTERUPT_FLAGS_MOMENTARY (0x500704) and lets the call run,
 * so attract starts a two-player game by itself (on free play), and opens a new
 * lockstep generation (i960hook_83F4_player_entry_forceStart).
 */
static int sfight_hook_xplay_force_start(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    if (!g_xplay_match || !g_xplay_ready) return 1;
    mem_write32(bus, 0x00500704, 0x30);
    g_xplay_events |= XPLAY_EV_NEW_GENERATION;
    return 1;
}

/*
 * xplay_barrier (0xA218, SEL_INT entry): character select waits here, skipped
 * as an i960 `ret`, until both machines' inputs are flowing. On the frame it is
 * released it is skipped once more and frame_counter (0x500020) is zeroed, and
 * from the next frame on every input comes out of the lockstep
 * (i960hook_A218_SEL_INT_syncBarrier; the PS3 also reseeds its MT19937 `rand`
 * there, which this board does not use).
 */
static int sfight_hook_xplay_barrier(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!g_xplay_match || g_xplay_barrier == 2) return 1;
    if (g_xplay_barrier == 1) {
        mem_write32(bus, 0x00500020, 0);
        g_xplay_barrier = 2;
        g_xplay_events |= XPLAY_EV_BARRIER;
    }
    hle_ret(cpu);
    return 0;
}

/*
 * Once a frame, at the frame boundary: note mode / also_mode for ps3_link, and
 * put a PS3 match's rules into the game's settings once the board is past
 * WARNING (Settings_ApplyRoomRules: the RAM copy, the backup copy, and the
 * barrier count at 0x50A424 that game_engine_setup copied at boot).
 */
static void sfight_xplay_frame(memory_bus_t *bus) {
    g_xplay_mode      = mem_read8(bus, 0x0050002A);
    g_xplay_also_mode = mem_read8(bus, 0x0050002B);
    if (!g_xplay_match || !g_xplay_rules_pending) return;
    if (g_xplay_also_mode < 2 && g_xplay_mode != 2) return;   /* still booting */
    static const uint8_t offs[5] = { 0x01, 0x04, 0x11, 0x13, 0x18 };
    for (int i = 0; i < 5; i++) {
        mem_write8(bus, 0x0059C340u + offs[i], g_xplay_rules[i]);
        mem_write8(bus, 0x01D03340u + offs[i], g_xplay_rules[i]);
    }
    mem_write32(bus, 0x0050A424, g_xplay_rules[4]);
    g_xplay_rules_pending = 0;
    g_xplay_ready = 1;
    LOG_INFO("xplay: settings in place (rounds %u, energy %u, time %u, flags 0x%02X, barrier %u)",
             g_xplay_rules[0], g_xplay_rules[1], g_xplay_rules[2], g_xplay_rules[3], g_xplay_rules[4]);
}

/* xplay_stage (0xAF84, set_vs_cnt_and_stage_num_sel+0x58 `stob r3, stage_num`):
 * a network match is played on stage seed % 9 (i960hook_AF84_stage_fromSeed;
 * its table at 0x3766D0+0x70 is 0..8). */
static int sfight_hook_xplay_stage(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    if (!g_xplay_match) return 1;
    cpu->locals.r[3] = (cpu->locals.r[3] & ~0xFFu) | (g_xplay_seed % 9u);
    return 1;
}

/* replay_stage (0x941C, ADV_REPLAY_INT `call change_scene`): the attract
 * replay has just stored its stage (byte_50005B + 1, wrapping past 7) in both
 * byte_50005B and stage_num, and change_scene loads it next, so a stage written
 * to both now is the one the replay is played on (stage_num alone still loads
 * the natural stage). match_replay's stage pin (g_replay_stage_pin). MAME's
 * side substitutes the value of those two stores with write taps, which leaves
 * memory the same at this instruction. */
static int sfight_hook_replay_stage(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    if (g_replay_stage_pin < 0) return 1;
    mem_write8(bus, 0x0050005B, (uint8_t)g_replay_stage_pin);
    mem_write8(bus, 0x00500064, (uint8_t)g_replay_stage_pin);
    return 1;
}

/* xplay_game_time (0xB0F8, GAME_INT+4): `time` (0x500090) is the round-time
 * setting, settings byte +0x11 (0x59C351), before the instruction runs. */
static int sfight_hook_xplay_game_time(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    if (g_xplay_match) mem_write8(bus, 0x00500090, mem_read8(bus, 0x0059C351));
    return 1;
}

/* xplay_replay_timer (0x96AC, ADV_REPLAY_WAIT1A+0x128 `stis r15, game_timer`):
 * the attract replay's timer is the round time, at most 30, times 64, and the
 * store is skipped. */
static int sfight_hook_xplay_replay_timer(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!g_xplay_match) return 1;
    uint32_t t = mem_read8(bus, 0x0059C351);
    if (t > 30) t = 30;
    mem_write16(bus, 0x00500028, (uint16_t)(t << 6));
    uint32_t w = mem_read32(bus, cpu->sfr.ip);
    uint32_t mode = (w >> 10) & 0xFu;
    bool two = ((w >> 12) & 1u) && (mode == 5 || mode >= 12);
    cpu->sfr.ip += two ? 8u : 4u;
    return 0;
}

/* xplay_match_over (0xE6EC, VIC_INT entry): a versus match is over, when the
 * versus flag (0x500068 bit 1) is set, unless somebody is watching
 * (i960hook_E6EC_VIC_INT_matchOver). Observe only. */
static int sfight_hook_xplay_match_over(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    if (g_xplay_match && !g_xplay_spectators && (mem_read8(bus, 0x00500068) & 2u))
        g_xplay_events |= XPLAY_EV_MATCH_OVER;
    return 1;
}

/* xplay_vic_dsp (0xE93C, VIC_DSP `ld INTERUPT_FLAGS_MOMENTARY, r8`): in a
 * network match START does not cut the victory screen short, the load is
 * skipped with r8 = 0; and with somebody watching, the match ends here, once
 * CTRL_TIMER (0x500024) has counted down to 60 (i960hook_E93C_VIC_DSP_
 * blockStart). */
static int sfight_hook_xplay_vic_dsp(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!g_xplay_match || !(mem_read8(bus, 0x00500068) & 2u)) return 1;
    if (g_xplay_spectators && mem_read32(bus, 0x00500024) == 0x3C) g_xplay_events |= XPLAY_EV_MATCH_OVER;
    cpu->locals.r[8] = 0;
    uint32_t w = mem_read32(bus, cpu->sfr.ip);
    uint32_t mode = (w >> 10) & 0xFu;
    bool two = ((w >> 12) & 1u) && (mode == 5 || mode >= 12);
    cpu->sfr.ip += two ? 8u : 4u;
    return 0;
}

/* NOTE: there is intentionally NO read_sw (0x17CC) hook. Inputs are delivered
 * the authentic way — input.h serves the active-low I/O ports (0x1C00000) and
 * the game's own read_sw, called from the VsyncScr vblank interrupt, reads them
 * and builds 0x500700/0x500704 itself. (Phase 12.) */

/* NOTE: there is intentionally NO hook on clip_point_check_yoko (0x28188) or
 * clip_point_check (0x28250). They return nothing in g0: they write one outcode
 * byte per point to 0x50E000, and area_clip ANDs a chunk's four corners from
 * there to decide whether to draw it. 0x50E000 is scratch that rob_spd_control
 * and smooth_int also write floats to, so skipping the routine made the ground
 * chunks follow the low bytes of fighter positions. tools/grade-cull.mjs holds
 * the chunks drawn against the ROM's own rule. */

/* ---- Pre-game menu settings (the PS3 port's ARCADE / OFFLINE VERSUS menus) --
 *
 * The PS3 port (NPUB30927) runs this same i960 program and applies its menus by
 * rewriting the arcade's own GAME ASSIGNMENTS block, which is what the test
 * menu edits. Decide calls NetGameMode_Set (PS3 0x57110) with eight bytes, and
 * that calls Settings_ApplyArcade (0x11E20C, mode 0) or Settings_ApplyRoomRules
 * (0x11DFF0, versus). Both copy the 0x42-byte block at add_BACKUP_RAM_TO_RAM
 * (0x50016C) + 0x3340, patch it from the lookup table at PS3 0x377AB0, write it
 * to backup RAM 0x1D03340 AND its work-RAM copy 0x59C340, and store the barrier
 * count to barrier_default_num (0x50A424).
 *
 * The block (offsets from 0x3340; the test menu's GAME ASSIGNMENTS rows):
 *   +0x00 MATCH COUNT(1P)  rounds to win, 2..5          arcade only
 *   +0x01 MATCH COUNT(VS)  rounds to win, 2..5          both
 *   +0x02 ENEMY RANK       match_enemy_rank, 0..3       arcade only
 *   +0x03 ENERGY(1P)       max_energy_1p_list index     arcade only
 *   +0x04 ENERGY(VS)       max_energy_vs_list index     both
 *   +0x11 TIME             time_vars index (see below)
 *   +0x12 COUNTRY          left alone
 *   +0x13 flag byte        game_assign_byte_flag, rebuilt whole:
 *                          b7 DAMAGE (1 = REAL), b6 HYPER MODE (1 = OFF),
 *                          b4 AUTOMATIC (1 = ON), b3 BARRIER RESET (1 = ON);
 *                          b0 ADVERTISE SOUND, b1 CONTINUE, b2 DISPLAY TYPE
 *                          and b5 VS FINISH come out 0 (ON, ON, C.R.T., OFF)
 *   +0x18 BARRIER          barriers per match, 1..10
 *   +0x19..+0x1F           monitor colour settings, left alone
 *
 * WHEN THE BOARD READS THEM. The game runs on the work-RAM copy: after the boot
 * check add_BACKUP_RAM_TO_RAM holds 0x599000 (BACKUP_RAM_TO_RAM), and backup RAM
 * is only read at power-on (backup_ram_check copies it down).
 * - Rounds: re-read at the start of every game (just before loc_A670).
 * - Enemy rank and energy: set_game_setting, called from SEL_INT and on
 *   continue; the AI reads rank per fight (sub_3B22C).
 * - Flag byte: tested live all over the game.
 * - Barriers: only game_engine_setup (boot) copies +0x18 to barrier_default_num,
 *   so the PS3 writes 0x50A424 itself, and so does this.
 * - Time: only main (boot, ROM 0x71F8) turns +0x11 into `time` (0x500090), and
 *   there +0x11 is an INDEX into time_vars {10,20,..,90,99}. The PS3 stores the
 *   seconds there instead (10/30/60/99) and has its own trap on GAME_INT (i960
 *   0xB0F8, PS3 FUN_00081ab8) copy +0x11 into `time` at every game start. This
 *   profile has no such hook, so this function writes `time` itself and stores
 *   the time_vars index in +0x11: the test menu then shows the right TIME, and a
 *   boot that ever read the block back would get the same seconds, where the
 *   PS3's byte would index past the end of time_vars.
 * So everything can be applied while the board sits in attract or at the title
 * and holds for the next game. Nothing after boot writes these back, except the
 * test menu (every change) and init_game_assignments (INITIALIZE, or a cold
 * boot whose backup CRC fails -- every cold boot here, since backup RAM starts
 * blank). A cold board reset therefore needs the settings applied again once
 * add_BACKUP_RAM_TO_RAM reads 0x599000.
 *
 * CHECKSUM. Backup RAM 0x1D03302 (crc_value_bk) is make_crc (
 * CRC-16/XMODEM: poly 0x1021, init 0, MSB first) over the block's first 0x20
 * bytes. Only power-on checks it; a mismatch resets the block to the factory
 * defaults. The PS3 never recomputes it (its boot trap at i960 0x3B44,
 * i960hook_3B44_applyBootSettings, skips the check altogether). This does, as
 * the test menu does after every change (set_game_assign_byte_flag, 0x62300),
 * so backup RAM always holds a block the ROM itself would accept.
 */
#define SFIGHT_SETTINGS_PTR      0x0050016Cu  /* add_BACKUP_RAM_TO_RAM          */
#define SFIGHT_SETTINGS_WORK     0x0059C340u  /* BACKUP_RAM_TO_RAM + 0x3340     */
#define SFIGHT_SETTINGS_BACKUP   0x01D03340u  /* backup RAM + 0x3340            */
#define SFIGHT_SETTINGS_CRC      0x01D03302u  /* crc_value_bk                   */
#define SFIGHT_SETTINGS_LEN      0x42u        /* what the PS3 copies            */
#define SFIGHT_SETTINGS_CRC_LEN  0x20u        /* what make_crc covers           */
#define SFIGHT_BARRIER_DEFAULT   0x0050A424u  /* barrier_default_num (.long)    */
#define SFIGHT_ROUND_TIME        0x00500090u  /* time, seconds (.byte)          */

/* The eight bytes the PS3 menus hand NetGameMode_Set, in that order. Indices
 * are menu rows; out of range falls back to index 0, as the PS3 does. */
typedef struct {
    uint8_t difficulty;   /* s[0] Easy/Normal/Hard/Hardest, 0..3 (arcade only)  */
    uint8_t not_trial;    /* s[1] !trial -- not read by the apply functions     */
    uint8_t reserved;     /* s[2] always 0                                      */
    uint8_t time_idx;     /* s[3] 10/30/60/99 s                                 */
    uint8_t rounds_idx;   /* s[4] 2/3/4/5 rounds                                */
    uint8_t attack_idx;   /* s[5] -1/Normal/+1/+2/+3                            */
    uint8_t barriers_idx; /* s[6] 1..10 barriers                                */
    uint8_t type_idx;     /* s[7] game type A..D                                */
} sfight_menu_settings_t;

/* The menus' defaults (PS3 TaskMenuArcade / TaskMenuVersus Init). */
static const sfight_menu_settings_t SFIGHT_MENU_DEFAULT_ARCADE = { 1, 1, 0, 1, 0, 1, 4, 0 };
static const sfight_menu_settings_t SFIGHT_MENU_DEFAULT_VERSUS = { 1, 1, 0, 1, 1, 1, 4, 0 };

/* make_crc with its table crc_variables: CRC-16/XMODEM. */
static inline uint16_t sfight_make_crc(const uint8_t *p, uint32_t n) {
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

/*
 * Apply the PS3 menu settings s[8] (sfight_menu_settings_t's layout) to the
 * board: versus = 0 is Settings_ApplyArcade, non-zero Settings_ApplyRoomRules.
 * Call it between frames with the board past its boot check (attract, title).
 * The two differences from the PS3 are the checksum and the time byte, both
 * explained above.
 */
static inline void sfight_apply_menu_settings(memory_bus_t *bus, const uint8_t s[8], int versus) {
    /* PS3 0x377AB0. +0x00: attack {flag, energy index} pairs; +0x0A: game type
     * {barrier reset, hyper} pairs; +0x18 rounds; +0x20 seconds; +0x28 barriers. */
    static const uint8_t attack[5][2] = { {0,0}, {0,1}, {0,2}, {0,3}, {1,3} };
    static const uint8_t type[4][2]   = { {0,1}, {0,0}, {1,1}, {1,0} };
    static const uint8_t rounds[4]    = { 2, 3, 4, 5 };
    static const uint8_t seconds[4]   = { 10, 30, 60, 99 };
    /* time_vars is {10,20,30,40,50,60,70,80,90,99}. */
    static const uint8_t time_var[4]  = { 0, 2, 5, 9 };

    unsigned a = s[5] < 5 ? s[5] : 0;
    unsigned t = s[7] < 4 ? s[7] : 0;
    unsigned r = s[4] < 4 ? s[4] : 0;
    unsigned m = s[3] < 4 ? s[3] : 0;
    unsigned b = s[6] < 10 ? s[6] : 0;
    uint8_t  barriers = (uint8_t)(b + 1);

    /* Read the block the game runs on, through its own pointer. Before the boot
     * check sets it this is not a usable address; take the work copy then. */
    uint32_t base = mem_read32(bus, SFIGHT_SETTINGS_PTR);
    uint32_t src  = (base == 0x00599000u || base == 0x01D00000u)
                  ? base + 0x3340u : SFIGHT_SETTINGS_WORK;
    uint8_t blk[SFIGHT_SETTINGS_LEN];
    for (uint32_t i = 0; i < SFIGHT_SETTINGS_LEN; i++)
        blk[i] = (uint8_t)mem_read8(bus, src + i);

    if (!versus) {
        blk[0x00] = rounds[r];
        blk[0x02] = s[0] < 4 ? s[0] : 1;  /* the PS3 stores it unchecked */
        blk[0x03] = attack[a][1];
    }
    blk[0x01] = rounds[r];
    blk[0x04] = attack[a][1];
    blk[0x11] = time_var[m];              /* the PS3 stores seconds[m] */
    blk[0x13] = (uint8_t)((attack[a][0] << 7) | ((type[t][1] ^ 1u) << 6)
                        | 0x10u | (type[t][0] << 3));
    blk[0x18] = barriers;

    for (uint32_t i = 0; i < SFIGHT_SETTINGS_LEN; i++) {
        mem_write8(bus, SFIGHT_SETTINGS_BACKUP + i, blk[i]);
        mem_write8(bus, SFIGHT_SETTINGS_WORK + i, blk[i]);
    }
    mem_write16(bus, SFIGHT_SETTINGS_CRC, sfight_make_crc(blk, SFIGHT_SETTINGS_CRC_LEN));
    mem_write32(bus, SFIGHT_BARRIER_DEFAULT, barriers);
    mem_write8(bus, SFIGHT_ROUND_TIME, seconds[m]);  /* PS3: its GAME_INT trap */
}

/* ---- Profile object ----------------------------------------------------- */

/* Two profiles run this ROM set: this one, and Sonic the Fighters - Console
 * (sfight_console.h), which adds the official console emulator's Honey and
 * hidden-character patches on top. What the two share is spelled once, in the
 * macros below, so a fix to one reaches both. */

/* The hooks every STF profile needs to boot and pace frames, the versus hook
 * netplay rooms read the result from, VS mode's rematch, and the region
 * default. */
#define SFIGHT_BASE_HOOK_COUNT 21
#define SFIGHT_BASE_HOOKS                                                      \
    { 0x00000F3C, sfight_hook_cop_init_l1,        "cop_initialize_l1"       }, \
    { 0x0004A55C, sfight_hook_check_timer_4,      "check_timer_4"           }, \
    { 0x0004A58C, sfight_hook_check_timer_4_spin, "check_timer_4_spin"      }, \
    { 0x00001768, sfight_hook_interrupt_wait,     "interrupt_wait"          }, \
    { 0x00011580, sfight_hook_interrupt_wait_b,   "interrupt_wait_b"        }, \
    { 0x00011610, sfight_hook_idle,               "_idle"                   }, \
    { 0x00007264, sfight_hook_700000_loop,        "_700000_loop"            }, \
    { 0x00011A04, sfight_hook_frame_pace,         "frame_pace"              }, \
    { 0x000077F8, sfight_hook_cop_err_hang,       "co_processor_error_hang" }, \
    { 0x0000DC3C, sfight_hook_versus_result,      "versus_result"           }, \
    { 0x0000E584, sfight_hook_vs_rematch,         "next_round+0x1a4"        }, \
    { 0x00062688, sfight_hook_country_default,    "country_default"         }, \
    { 0x00062674, sfight_hook_damage_default,     "damage_default"          }, \
    { 0x000083F4, sfight_hook_xplay_force_start,  "xplay_force_start"       }, \
    { 0x0000A218, sfight_hook_xplay_barrier,      "xplay_sel_int_barrier"   }, \
    { 0x0000E6EC, sfight_hook_xplay_match_over,   "xplay_vic_int"           }, \
    { 0x0000E93C, sfight_hook_xplay_vic_dsp,      "xplay_vic_dsp"           }, \
    { 0x0000AF84, sfight_hook_xplay_stage,        "xplay_stage"             }, \
    { 0x0000B0F8, sfight_hook_xplay_game_time,    "xplay_game_time"         }, \
    { 0x000096AC, sfight_hook_xplay_replay_timer, "xplay_replay_timer"      }, \
    { 0x0000941C, sfight_hook_replay_stage,       "replay_stage"            },

/* read_sw's copies of the pad: held 0x500700, momentary 0x500704; the
 * credits are at 0x59C388 (P1) and 0x59C38C (P2). */
#define SFIGHT_INPUT_MAP                                                        \
    .bits = {                                                                   \
        [GAME_INPUT_P1_UP]    = 0x00002000,                                     \
        [GAME_INPUT_P1_DOWN]  = 0x00001000,                                     \
        [GAME_INPUT_P1_LEFT]  = 0x00008000,                                     \
        [GAME_INPUT_P1_RIGHT] = 0x00004000,                                     \
        [GAME_INPUT_P1_B1]    = 0x00000100,                                     \
        [GAME_INPUT_P1_B2]    = 0x00000200,                                     \
        [GAME_INPUT_P1_B3]    = 0x00000400,                                     \
        [GAME_INPUT_P1_START] = 0x00000010,                                     \
        [GAME_INPUT_P2_UP]    = 0x00200000,                                     \
        [GAME_INPUT_P2_DOWN]  = 0x00100000,                                     \
        [GAME_INPUT_P2_LEFT]  = 0x00800000,                                     \
        [GAME_INPUT_P2_RIGHT] = 0x00400000,                                     \
        [GAME_INPUT_P2_B1]    = 0x00010000,                                     \
        [GAME_INPUT_P2_B2]    = 0x00020000,                                     \
        [GAME_INPUT_P2_B3]    = 0x00040000,                                     \
        [GAME_INPUT_P2_START] = 0x00000020,                                     \
        [GAME_INPUT_SERVICE]  = 0x00000004,                                     \
        /* Coins map to IN0 (held byte0) COIN1/COIN2 — active-low at IO+0x02. \
         * The game's read_sw edge-detects these and updates credits. */        \
        [GAME_INPUT_P1_COIN]  = 0x00000001,                                     \
        [GAME_INPUT_P2_COIN]  = 0x00000002,                                     \
    },

/* advertise_steps[_sub_mode]: 5 is ADV_MOVIE_DSP (the ~2200-frame intro
 * movie), 6 ADV_REPLAY_PIC, which leads into the Sonic vs Bean replay on
 * stage 1 (replay_bank_init_data, ROM 0xDC9B0). The movie controller
 * (am_cntr .. 0x5004E7) keeps running through the replay, so it is set to
 * the state a natural boot has at fc 2435, where MOVIE_DSP hands over:
 * animation 3, frame 0x1A7. prep_adv_movie writes adv_movie_cont_ex
 * (0x5004CC) in the frame the step becomes 5, which is what "ready" waits
 * for. Captured off this emulator and checked bit for bit against the
 * natural boot's fight (1097 frames, both fighters). */
#define SFIGHT_QUIRKS                                                                 \
    .mesh_ptr_subtract  = 0x02000010,                                                 \
    .mesh_ptr_add       = 0x10,                                                       \
    .model_table_offset = 0x000E0004,                                                 \
    .model_table_count  = 5103,                                                       \
    .camera_struct_addr = 0x00519E98,                                                 \
    .enable_68k_sound   = true,                                                       \
    /* Real interrupt handlers (dispatch table @0x46b4): pin0 VsyncScr,               \
     * pin1 VsyncObj, pin2 Timer, pin3 Other(sound). */                               \
    .irq_handler        = { 0x00000C40, 0x00000D10, 0x00000D30, 0x00000DF0 },         \
    .sound_queue_count_addr = 0x00504001,   /* byte_504001 */                         \
    .sound_queue_state_addr = 0x00504014,   /* byte_504014 */                         \
    .warning_skip_addr      = 0x00500410,   /* poke 1 → skip boot warning screen */ \
    .vs_rematch             = true,         /* sfight_hook_vs_rematch */             \
    /* Aurora Icefield's ice pillars (aurora_ice_pillar_init, records at        \
     * 0x754F8, field +0x18) stand on the ice (issue #78; geo3d.h says why     \
     * the walruses, 1601, are not listed). */                                   \
    .zsort_standing_count   = 1,                                                      \
    .zsort_standing         = { 4278 },                                               \
    .attract_replay = {                                                             \
        .step_addr   = 0x00500030,           /* _sub_mode */                          \
        .from_step   = 5,                                                             \
        .to_step     = 6,                                                             \
        .ready_addr  = 0x005004CC,           /* adv_movie_cont_ex */                  \
        .state_addr  = 0x005004C4,           /* am_cntr, am_num, dword_5004C8, ... */ \
        .state_count = 9,                                                             \
        .state = { 0x000301A7, 0x00000028, 0x00055DDC, 0x000562D0, 0xC1200000,        \
                   0x433A8000, 0x43810000, 0xC1200000, 0x43398000 },                  \
    },

static const game_profile_t sfight_profile = {
    .id               = "sfight",
    .display_name     = "Sonic the Fighters - Arcade",
    .rom_set          = "sfight",
    .parent_zip_name  = "schamp.zip",
    .board            = BOARD_MODEL2B_CRX,
    .load_fn      = sfight_load,
    .install_fn   = sfight_install,
    .hook_count   = SFIGHT_BASE_HOOK_COUNT,
    .hooks        = { SFIGHT_BASE_HOOKS },
    .input        = { SFIGHT_INPUT_MAP },
    .quirks       = { SFIGHT_QUIRKS },
};

#endif /* PROFILES_SFIGHT_H */
