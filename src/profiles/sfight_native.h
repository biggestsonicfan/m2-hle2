/*
 * sfight_native.h -- Sonic the Fighters' own replay formats: each round kept
 * in them (Pinboard #602), and played back with the game's own replay code
 * (Pinboard #617). NATIVE-REPLAY.md has the formats in full.
 *
 * STF keeps three kinds of replay, and a .m2replay (core/replay.h) carries all
 * three for every round it records, byte for byte as the game lays them out:
 *
 *  - The key bank (key_bank.bin, 0x1014 bytes at 0x531000, replay_bank_adr
 *    0x500168). It is what attract's Sonic-vs-Bean fight plays: ADV_REPLAY
 *    runs whatever is there. ROUND_DSP writes its header at 0xD008-0xD084 (+4
 *    stage_x, +0xC / +0x10 the 1P / 2P character, +0xFFF / +0xFFE each rob's
 *    +0x6CC) and key_rec_disp (0x37CE0) records two bytes a frame from +0x14,
 *    each rob's +0x1200 pad byte, bits 6 and 7 swapped for a rob facing left.
 *    It stops after offset 0xFFE (2048 frames) by storing 0 into its task
 *    word; frames past 2037 have already written over the +0xFFE header bytes.
 *    ROUND_DSP only starts it when STAGE_ID (0x500054) < 9 and not_scr_bg_move
 *    bit 4 is clear (0xCFF0), so a sudden-death round is not keyed.
 *  - The round-start snapshot it plays from: key_start.bin (0x572000, the two
 *    fighters as save_key_replay 0x37E94 keeps them) and effect_start.bin
 *    (0xC80 bytes at 0x573000). load_key_replay (called from key_play_init
 *    0x37D80) puts them back.
 *  - The ending slot (ending.bin, 0x8060 bytes): what the credits replay for
 *    each stage played. save_ending_rep_data (0x38464) writes it to 0x5A0010
 *    + (STAGE_ID - 1) * 0x8060 at the end of a round the game does not take to
 *    sudden death, and never in a versus game; load_ending_rep_data (0x385B4)
 *    reads it back. +0 stage_x, +4 dword_500434, +8 the s16 at mod_fa_obj0+6
 *    less 0xEF, +0xA 1 when byte_50004D is not 0, +0xC / +0xE the 1P / 2P
 *    character, +0xD / +0xF their rounds won, +0x10 the mod_fa_record task's
 *    words 0x40..0x83, +0x60 the 0x8000 bytes of rep_ram (0x535000), the
 *    instant replay's ring.
 *
 * Those are taken at 0xDD20 in ROUND_DSP's judge (`ldob byte_50004D`): the
 * round is over, CTRL_TIMER has run out, and neither the sudden-death branch
 * nor not_sudden has run yet, so nothing the slot reads has moved. The ending
 * slot is built here the way save_ending_rep_data would build it, so a versus
 * round has one too, though the game itself never keeps it.
 *
 * The game's bank holds 2048 frames, about 34 seconds, and its own snapshot
 * misses state the fight depends on, so a round does not play back from them
 * alone. Five more parts make it whole:
 *   keys.bin      every frame's key pair from the round's start to the judge,
 *                 two bytes a frame as key_rec_disp lays them. The first 2048
 *                 are what key_rec_disp stores (read at its entry); once the
 *                 bank is full, the event loop's visit to the key_record task
 *                 (0x11418, `ld 0(g13), r4`), where key_rec_disp would have
 *                 run, takes the rest.
 *   fighters.bin  the round's first frame (version 2, 0x8CA0 bytes): +0 "STFF"
 *                 +4 version +6 start_yang (0x500432, u16 LE) +8 / +9 the
 *                 bank's +0xFFE / +0xFFF +0xC the bank's first 0x14 bytes, then
 *                 rob 0 and rob 1 (0x3400 bytes each), the COP words SFN_COP,
 *                 both robs' motion blend RAM and SFN_RAM. The header is read at
 *                 key_rec_disp's first frame, the rest at rob 0's next visit
 *                 (sfn_rec_fighters). Version 1 (#602) ends after the robs.
 *   settings.bin  the game assignments (0x42 bytes at 0x59C340): energy, the
 *                 damage type, hyper and barrier rules.
 *   draws.bin     play_motion's rand draws, u16 key frame + u16 draw each.
 *   view.bin      the camera's view toggle, a byte a key frame.
 *
 * round.bin is ours, 16 bytes saying which round it was:
 *   +0 "STFR"  +4 version (2)  +5 stage_num  +6 STAGE_ID  +7 flags
 *   +8 key frames (u16 LE, keys.bin's)  +10 curr_round_num  +11 gameprogram
 *   +12 byte_50004D
 *   flags: bit 0 the round was keyed, bit 1 it ran past the bank's 2048
 *   frames, bit 2 the game takes the round to sudden death.
 *
 * Playback (sfight_native_play) is attract's replay fight made to play our
 * rounds: ADV_REPLAY_INT (0x8F80) puts the round's bank and snapshot back and
 * pins its stage; key_play_disp (0x37DC4) gets each frame's keys from keys.bin
 * through the bank (and on the first frame the fighters, start_yang and the
 * settings); play_motion (0x1C478) and cam_mode 0x0A (0x6B408) take draws.bin
 * and view.bin; ADV_REPLAY_DSP (0x97B8) runs the round to its last key instead of
 * cutting it at a KO or attract's timer; ADV_REPLAY_PIC2 (0x98B8) goes back to
 * ADV_REPLAY_PIC for the next round instead of on to the CPU battle.
 *
 * The recording hooks only read, and keep what they read outside the board: a
 * round that nothing takes is dropped by the next replay that begins.
 */
#ifndef M2HLE_SFIGHT_NATIVE_H
#define M2HLE_SFIGHT_NATIVE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "game_profile.h"
#include "memory.h"
#include "i960.h"
#include "hle_hooks.h"
#include "savestate_reg.h"

#define SFN_BANK_PTR      0x00500168u   /* replay_bank_adr */
#define SFN_BANK          0x00531000u
#define SFN_BANK_LEN      0x1014u
#define SFN_BANK_KEYS     0x14u
#define SFN_BANK_KEYS_LEN 0x1000u       /* key_rec_disp's `off < 0x1000` */
#define SFN_KEY_START     0x00572000u
#define SFN_KEY_START_LEN 0x160u
#define SFN_EFFECT        0x00573000u
#define SFN_EFFECT_LEN    0xC80u
#define SFN_ENDING        0x005A0010u
#define SFN_ENDING_LEN    0x8060u
#define SFN_ENDING_SLOTS  10
#define SFN_REP_RAM       0x00535000u
#define SFN_REP_RAM_LEN   0x8000u
#define SFN_ROUND_LEN     16u
#define SFN_ROUND_VERSION 2

#define SFN_KEY_TASK      0x0050084Cu   /* mod_fa_key_record */
#define SFN_DEBUG_FLAG    0x00508000u   /* bit 5 stops the key tasks */
#define SFN_ROB0          0x00500804u
#define SFN_ROB1          0x00500808u
#define SFN_ROB_LEN       0x3400u
#define SFN_ROB_FROM      0x10u         /* the task header stays the board's */
#define SFN_START_YANG    0x00500432u
#define SFN_SETTINGS      0x0059C340u   /* SFIGHT_SETTINGS_WORK */
#define SFN_SETTINGS_LEN  0x42u
#define SFN_FIGHTERS_HEAD 0x20u
#define SFN_FIGHTERS_V1   (SFN_FIGHTERS_HEAD + 2u * SFN_ROB_LEN)
/* The COP's memory the fight carries from frame to frame (SHARC DM words):
 * each fighter's unit matrices and its ball table, current and previous. */
static const uint32_t SFN_COP[][2] = {
    { 0x00030420u, 0x180u }, { 0x01403E80u, 0xC0u }, { 0x01407E80u, 0xC0u },
};
#define SFN_COP_WORDS     (0x180u + 0xC0u + 0xC0u)
#define SFN_MOTION_PTR    0x0BD8u       /* rob: its motion blend RAM (action_init) */
#define SFN_MOTION_LEN    0x7C8u
/* Task data outside the robs that carries a fight from frame to frame. */
static const uint32_t SFN_RAM[][2] = {
    { 0x00519510u, 0x3F0u },   /* mod_fa_coli: the fighters' positions as collision has them */
    { 0x00519E00u, 0x80u  },   /* mod_fa_obj0: obj_center */
    { 0x00519E80u, 0x480u },   /* mod_fa_camera: heads, eyes and get_my_info_new read its angle */
};
#define SFN_RAM_LEN       (0x3F0u + 0x80u + 0x480u)
#define SFN_RAM_AT        (SFN_FIGHTERS_V1 + 4u * SFN_COP_WORDS + 2u * SFN_MOTION_LEN)
#define SFN_FIGHTERS_LEN  (SFN_RAM_AT + SFN_RAM_LEN)
#define SFN_MOTION_AT     (SFN_FIGHTERS_V1 + 4u * SFN_COP_WORDS)
#define SFN_KEYS_MAX      0x4000u       /* frames: about four and a half minutes */
#define SFN_DRAWS_MAX     0x2000u       /* play_motion's rand draws in a round */
#define SFN_DRAW_CALL     0x0001C474u   /* play_motion: call rand */
#define SFN_DRAW_TAKEN    0x0001C478u   /* ...and the draw in g0 */
#define SFN_SUB_MODE      0x00500030u   /* attract's step */
#define SFN_GAME_TIMER    0x00500028u
#define SFN_STEP_REPLAY   6             /* ADV_REPLAY_PIC */
/* 1 << the step (dword_500034). Attract's steps 8 and 9 still run the replay
 * fight's set-up: the camera at 0x1FA44 and get_frame_dat at 0x30838 take
 * their attract paths there, so the round's first frame waits for step 10. */
#define SFN_STEP_BIT      0x00500034u
#define SFN_STEPS_EARLY   0x300u
/* The camera's view toggle: cam_mode 0x0A (0x6B408) runs cam_mode_33, the
 * player's view, when mode and _sub_mode are 9 (a game's fight) and the
 * camera task's +0x41 bit 0 is up; a start button flips that bit. Attract is
 * not mode 9, so the replay takes the view from view.bin. */
#define SFN_CAM_VIEW      0x0006B408u
#define SFN_CAM_VIEW_TAKE 0x0006B478u   /* `cmpo 0, r5`: r5 is the view bit */
#define SFN_CAM_PLAYER    0x0006DCF8u   /* cam_mode_33 past its rob swap */
#define SFN_VIEW_PLAYER   1u            /* view.bin: the player's view ran */
#define SFN_VIEW_SWAP     2u            /* ...with the robs swapped (gameprogram) */

#define SFN_KEYED         1u
#define SFN_KEYS_FULL     2u
#define SFN_SUDDEN        4u

/* What the judge took, until the replay takes it. */
static struct {
    bool     pending;
    uint8_t  bank[SFN_BANK_LEN];
    uint8_t  key_start[SFN_KEY_START_LEN];
    uint8_t  effect[SFN_EFFECT_LEN];
    uint8_t  ending[SFN_ENDING_LEN];
    uint8_t  round[SFN_ROUND_LEN];
    uint8_t  keys[SFN_KEYS_MAX * 2];
    uint8_t  fighters[SFN_FIGHTERS_LEN];
    uint8_t  settings[SFN_SETTINGS_LEN];
    uint8_t  draws[SFN_DRAWS_MAX * 4];
    uint8_t  view[SFN_KEYS_MAX];
    uint32_t nkeys, ndraws;
    uint16_t energy[2];         /* each rob's +0x1AC at the judge */
} s_sfn;

/* The round being keyed. The counters are board state (savestate_extra), so
 * GGPO's rollback takes them back with the board; the keys and the round's
 * start are not, and a frame run again writes its key again. */
static struct {
    uint32_t n;                 /* keys so far */
    uint32_t live;              /* 0 none, 1 key_rec_disp runs, 2 the bank is full */
    uint32_t draws;             /* play_motion's draws so far */
    uint32_t snap;              /* fighters.bin still to take (sfn_rec_fighters) */
} s_sfr;
static uint8_t s_sfr_draws[SFN_DRAWS_MAX * 4];
static uint8_t s_sfr_view[SFN_KEYS_MAX];
static uint8_t s_sfr_keys[SFN_KEYS_MAX * 2];
static uint8_t s_sfr_fighters[SFN_FIGHTERS_LEN];
static uint8_t s_sfr_settings[SFN_SETTINGS_LEN];

/* The rounds being played: the caller's (sfight_native_play). */
static struct {
    const game_native_round_t *rounds;
    int      n;
    const uint8_t *keys, *fighters, *settings, *draws, *view;
    uint32_t fighters_len, ndraws, draw, nview;
    bool     drawing;           /* the round's first frame has run */
    uint8_t  settings_was[SFN_SETTINGS_LEN];
    bool     settings_put;
} s_sfp;

static inline void sfn_read(memory_bus_t *bus, uint32_t addr, uint8_t *out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) out[i] = mem_read8(bus, addr + i);
}

static inline void sfn_write(memory_bus_t *bus, uint32_t addr, const uint8_t *in, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) mem_write8(bus, addr + i, in[i]);
}

static inline void sfn_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline bool sfn_debug_stopped(memory_bus_t *bus) {
    return (mem_read32(bus, SFN_DEBUG_FLAG) & 0x20u) != 0;
}

/* A rob's key as key_rec_disp records it: the pad byte, bits 6 and 7 swapped
 * when the rob faces left (word 0 bit 1). key_play_disp swaps them back. */
static inline uint8_t sfn_rob_key(memory_bus_t *bus, uint32_t rob) {
    uint8_t v = mem_read8(bus, rob + 0x1200);
    if (mem_read32(bus, rob) & 2u) v = (uint8_t)((v & 0x3F) | ((v & 0x40) << 1) | ((v & 0x80) >> 1));
    return v;
}

static inline void sfn_rec_key(memory_bus_t *bus, uint32_t at) {
    if (at >= SFN_KEYS_MAX) return;
    s_sfr_keys[2 * at]     = sfn_rob_key(bus, mem_read32(bus, SFN_ROB0));
    s_sfr_keys[2 * at + 1] = sfn_rob_key(bus, mem_read32(bus, SFN_ROB1));
    s_sfr.n = at + 1;
}

/* The COP's carried state in or out, little-endian words. */
static inline void sfn_cop(uint8_t *p, bool put) {
    for (size_t r = 0; r < sizeof SFN_COP / sizeof SFN_COP[0]; r++)
        for (uint32_t i = 0; i < SFN_COP[r][1]; i++, p += 4) {
            if (put) sharc_dm_set(SFN_COP[r][0] + i, (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
            else     sfn_put32(p, sharc_dm_get(SFN_COP[r][0] + i));
        }
}

/* SFN_RAM in or out. */
static inline void sfn_ram(memory_bus_t *bus, uint8_t *p, bool put) {
    for (size_t r = 0; r < sizeof SFN_RAM / sizeof SFN_RAM[0]; r++, p += SFN_RAM[r - 1][1]) {
        if (put) sfn_write(bus, SFN_RAM[r][0], p, SFN_RAM[r][1]);
        else     sfn_read(bus, SFN_RAM[r][0], p, SFN_RAM[r][1]);
    }
}

/* The round as key_rec_disp sees it on its first frame (fighters.bin). */
static inline void sfn_rec_start(memory_bus_t *bus, uint32_t bank) {
    uint8_t *f = s_sfr_fighters;
    memset(f, 0, SFN_FIGHTERS_HEAD);
    memcpy(f, "STFF", 4);
    f[4] = 2;
    uint16_t yang = (uint16_t)mem_read16(bus, SFN_START_YANG);
    f[6] = (uint8_t)yang; f[7] = (uint8_t)(yang >> 8);
    f[8] = mem_read8(bus, bank + 0xFFE);
    f[9] = mem_read8(bus, bank + 0xFFF);
    sfn_read(bus, bank, f + 0xC, SFN_BANK_KEYS);
    sfn_read(bus, SFN_SETTINGS, s_sfr_settings, SFN_SETTINGS_LEN);
    memset(s_sfr_view, 0, sizeof s_sfr_view);
    s_sfr.live  = 1;
    s_sfr.snap  = 1;
    s_sfr.n     = 0;
    s_sfr.draws = 0;
}

/* The fighters, the COP's carried state and SFN_RAM, taken at rob 0's first
 * visit after the round's first key: on the board the key task runs before
 * the robs, and in attract's replay key_play_disp runs after them, so this is
 * the state the replay's first key_play_disp has to leave. */
static inline void sfn_rec_fighters(memory_bus_t *bus) {
    uint8_t *f = s_sfr_fighters;
    sfn_read(bus, mem_read32(bus, SFN_ROB0), f + SFN_FIGHTERS_HEAD, SFN_ROB_LEN);
    sfn_read(bus, mem_read32(bus, SFN_ROB1), f + SFN_FIGHTERS_HEAD + SFN_ROB_LEN, SFN_ROB_LEN);
    sfn_cop(f + SFN_FIGHTERS_V1, false);
    for (uint32_t r = 0; r < 2; r++) {
        uint32_t rob = mem_read32(bus, r ? SFN_ROB1 : SFN_ROB0);
        sfn_read(bus, mem_read32(bus, rob + SFN_MOTION_PTR), f + SFN_MOTION_AT + r * SFN_MOTION_LEN, SFN_MOTION_LEN);
    }
    sfn_ram(bus, f + SFN_RAM_AT, false);
}

/* key_rec_disp (0x37CE0), on entry: g13 is its task, +0x54 where the next
 * key goes. Observe only. */
static int sfight_hook_native_key_rec(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (g_native_play.on || sfn_debug_stopped(bus)) return 1;
    uint32_t bank = mem_read32(bus, SFN_BANK_PTR);
    uint32_t off  = mem_read32(bus, cpu->globals.g13 + 0x54) - (bank + SFN_BANK_KEYS);
    if (off == 0) sfn_rec_start(bus, bank);
    if (!s_sfr.live || off >= SFN_BANK_KEYS_LEN) return 1;
    sfn_rec_key(bus, off / 2);
    if (off == SFN_BANK_KEYS_LEN - 2) s_sfr.live = 2;
    return 1;
}

/* The event loop's visit to a task (0x11418, `ld 0(g13), r4`): rob 0's first
 * visit of a round takes fighters.bin, and once the bank is full the
 * key_record task's visit is where its frame's key is taken. */
static int sfight_hook_native_task_visit(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (s_sfr.snap && cpu->globals.g13 == mem_read32(bus, SFN_ROB0) && !g_native_play.on) {
        sfn_rec_fighters(bus);
        s_sfr.snap = 0;
    }
    if (s_sfr.live != 2) return 1;
    if (cpu->globals.g13 != mem_read32(bus, SFN_KEY_TASK) || sfn_debug_stopped(bus)) return 1;
    sfn_rec_key(bus, s_sfr.n);
    return 1;
}

/* The slot as save_ending_rep_data would write it. */
static inline void sfn_build_ending(memory_bus_t *bus, uint8_t *e) {
    uint32_t rob0 = mem_read32(bus, SFN_ROB0), rob1 = mem_read32(bus, SFN_ROB1);
    memset(e, 0, SFN_ENDING_LEN);
    sfn_put32(e + 0, mem_read32(bus, 0x0050A00C));                 /* stage_x */
    sfn_put32(e + 4, mem_read32(bus, 0x00500434));
    uint32_t obj0 = mem_read32(bus, 0x00500870);                    /* mod_fa_obj0 */
    uint16_t y = (uint16_t)((int16_t)mem_read16(bus, obj0 + 6) - 0xEF);
    e[8] = (uint8_t)y; e[9] = (uint8_t)(y >> 8);
    e[0xA] = mem_read8(bus, 0x0050004D) ? 1 : 0;
    e[0xC] = mem_read8(bus, rob0 + 0x1B0);
    e[0xD] = mem_read8(bus, 0x0050004F);
    e[0xE] = mem_read8(bus, rob1 + 0x1B0);
    e[0xF] = mem_read8(bus, 0x00500051);
    sfn_read(bus, mem_read32(bus, 0x00500854) + 0x40, e + 0x10, 0x44);   /* mod_fa_record */
    sfn_read(bus, SFN_REP_RAM, e + 0x60, SFN_REP_RAM_LEN);
}

/* Does the judge send this round to sudden death? 0xDD28-0xDD50. */
static inline bool sfn_sudden(memory_bus_t *bus) {
    uint8_t p1 = mem_read8(bus, 0x0050004F), p2 = mem_read8(bus, 0x00500051);
    return mem_read8(bus, 0x0050004D) == 2 && p1 == p2 && mem_read8(bus, 0x00500052) <= p1;
}

/* The round's keys, start and settings, as the recorder has them now. */
static inline uint32_t sfn_take_keys(void) {
    uint32_t flags = 0;
    s_sfn.nkeys = 0;
    if (s_sfr.live) {
        flags = SFN_KEYED | (s_sfr.live == 2 ? SFN_KEYS_FULL : 0);
        s_sfn.nkeys = s_sfr.n;
        memcpy(s_sfn.keys, s_sfr_keys, (size_t)s_sfr.n * 2);
        memcpy(s_sfn.fighters, s_sfr_fighters, SFN_FIGHTERS_LEN);
        memcpy(s_sfn.settings, s_sfr_settings, SFN_SETTINGS_LEN);
        s_sfn.ndraws = s_sfr.draws;
        memcpy(s_sfn.draws, s_sfr_draws, (size_t)s_sfr.draws * 4);
        memcpy(s_sfn.view, s_sfr_view, s_sfr.n);
    }
    s_sfr.live = 0;
    return flags;
}

/* round_end (0xDD20): observe only. */
static int sfight_hook_native_round(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    if (g_native_play.on) return 1;
    uint32_t flags = sfn_take_keys() | (sfn_sudden(bus) ? SFN_SUDDEN : 0);
    uint32_t frames = s_sfn.nkeys;
    sfn_read(bus, SFN_BANK, s_sfn.bank, SFN_BANK_LEN);
    sfn_read(bus, SFN_KEY_START, s_sfn.key_start, SFN_KEY_START_LEN);
    sfn_read(bus, SFN_EFFECT, s_sfn.effect, SFN_EFFECT_LEN);
    sfn_build_ending(bus, s_sfn.ending);
    uint8_t *r = s_sfn.round;
    memset(r, 0, SFN_ROUND_LEN);
    memcpy(r, "STFR", 4);
    r[4]  = SFN_ROUND_VERSION;
    r[5]  = mem_read8(bus, 0x00500064);   /* stage_num */
    r[6]  = mem_read8(bus, 0x00500054);   /* STAGE_ID */
    r[7]  = (uint8_t)flags;
    r[8]  = (uint8_t)frames;
    r[9]  = (uint8_t)(frames >> 8);
    r[10] = mem_read8(bus, 0x00500055);   /* curr_round_num */
    r[11] = mem_read8(bus, 0x0050004C);   /* gameprogram */
    r[12] = mem_read8(bus, 0x0050004D);
    s_sfn.energy[0] = mem_read16(bus, mem_read32(bus, SFN_ROB0) + 0x1AC);
    s_sfn.energy[1] = mem_read16(bus, mem_read32(bus, SFN_ROB1) + 0x1AC);
    s_sfn.pending = true;
    LOG_INFO("native replay: round %u on stage %u (STAGE_ID %u) ended, %u key frames%s%s",
             r[10], r[5], r[6], frames, (flags & SFN_KEYED) ? "" : " (not keyed)",
             (flags & SFN_SUDDEN) ? ", to sudden death" : "");
    return 1;
}

static inline uint32_t sfn_ending_addr(int stage_id) {
    return SFN_ENDING + (uint32_t)(stage_id - 1) * SFN_ENDING_LEN;
}

static bool sfight_native_take(game_native_round_t *out) {
    if (!s_sfn.pending) return false;
    s_sfn.pending = false;
    const uint8_t *r = s_sfn.round;
    int stage_id = r[6];
    bool keyed = (r[7] & SFN_KEYED) != 0;
    uint32_t ending = stage_id >= 1 && stage_id <= SFN_ENDING_SLOTS ? sfn_ending_addr(stage_id) : 0;
    const game_native_part_t parts[] = {
        { "round.bin",        0,             s_sfn.round,     SFN_ROUND_LEN     },
        { "key_bank.bin",     SFN_BANK,      s_sfn.bank,      SFN_BANK_LEN      },
        { "key_start.bin",    SFN_KEY_START, s_sfn.key_start, SFN_KEY_START_LEN },
        { "effect_start.bin", SFN_EFFECT,    s_sfn.effect,    SFN_EFFECT_LEN    },
        { "ending.bin",       ending,        s_sfn.ending,    SFN_ENDING_LEN    },
        { "keys.bin",         0,             s_sfn.keys,      s_sfn.nkeys * 2   },
        { "fighters.bin",     0,             s_sfn.fighters,  SFN_FIGHTERS_LEN  },
        { "settings.bin",     SFN_SETTINGS,  s_sfn.settings,  SFN_SETTINGS_LEN  },
        { "draws.bin",        0,             s_sfn.draws,     s_sfn.ndraws * 4  },
        { "view.bin",         0,             s_sfn.view,      s_sfn.nkeys       },
    };
    out->nparts = keyed ? 10 : 5;
    memcpy(out->part, parts, (size_t)out->nparts * sizeof parts[0]);
    snprintf(out->json, sizeof out->json,
             "{\"game\":\"stf\",\"round\":%u,\"stage\":%u,\"stage_id\":%d,\"keyed\":%d,"
             "\"key_frames\":%u,\"sudden\":%d,\"p1\":%u,\"p2\":%u,\"p1_wins\":%u,\"p2_wins\":%u,"
             "\"energy\":[%u,%u]}",
             r[10], r[5], stage_id, keyed ? 1 : 0, r[8] | (r[9] << 8),
             (r[7] & SFN_SUDDEN) ? 1 : 0, s_sfn.ending[0xC], s_sfn.ending[0xE],
             s_sfn.ending[0xD], s_sfn.ending[0xF], s_sfn.energy[0], s_sfn.energy[1]);
    return true;
}

/* A part by name, of `len` bytes (0: any length). */
static inline const game_native_part_t *sfn_find(const game_native_round_t *in, const char *name,
                                                 uint32_t len) {
    for (int i = 0; i < in->nparts; i++)
        if (!strcmp(in->part[i].name, name) && (!len || in->part[i].len == len)) return &in->part[i];
    return NULL;
}

static inline const game_native_part_t *sfn_round_part(const game_native_round_t *in) {
    const game_native_part_t *r = sfn_find(in, "round.bin", SFN_ROUND_LEN);
    if (!r || memcmp(r->data, "STFR", 4) || r->data[4] < 1 || r->data[4] > SFN_ROUND_VERSION) return NULL;
    return r;
}

/*
 * The way back in: the key bank and its round-start snapshot go where attract
 * plays them from, and the round's stage pins attract's replay stage
 * (sfight_hook_replay_stage), so the next attract replay is this round. The
 * ending slot goes to the stage the round was played as (STAGE_ID), where the
 * credits read it. A part that is missing is left as the board has it. A
 * version 1 bank is the 0x1000 bytes #602 kept, short of the last 0x14.
 */
static const char *sfight_native_put(memory_bus_t *bus, const game_native_round_t *in) {
    const game_native_part_t *r = sfn_round_part(in);
    if (!r) return "the round has no round.bin (version 1 or 2)";
    const game_native_part_t *bank = sfn_find(in, "key_bank.bin", 0);
    const game_native_part_t *ks   = sfn_find(in, "key_start.bin", SFN_KEY_START_LEN);
    const game_native_part_t *eff  = sfn_find(in, "effect_start.bin", SFN_EFFECT_LEN);
    const game_native_part_t *end  = sfn_find(in, "ending.bin", SFN_ENDING_LEN);
    if (bank && bank->len > SFN_BANK_LEN) bank = NULL;
    if (bank) sfn_write(bus, SFN_BANK, bank->data, bank->len);
    if (ks)   sfn_write(bus, SFN_KEY_START, ks->data, SFN_KEY_START_LEN);
    if (eff)  sfn_write(bus, SFN_EFFECT, eff->data, SFN_EFFECT_LEN);
    int stage_id = r->data[6];
    if (end && stage_id >= 1 && stage_id <= SFN_ENDING_SLOTS)
        sfn_write(bus, sfn_ending_addr(stage_id), end->data, SFN_ENDING_LEN);
    if (bank) g_replay_stage_pin = r->data[5];
    return NULL;
}

/* ---- Playback ------------------------------------------------------------ */

/* Can the round be played whole: keyed, with its keys, start and settings? */
static inline bool sfn_playable(const game_native_round_t *in) {
    const game_native_part_t *k = sfn_find(in, "keys.bin", 0);
    const game_native_part_t *f = sfn_find(in, "fighters.bin", SFN_FIGHTERS_LEN);
    if (!f) f = sfn_find(in, "fighters.bin", SFN_FIGHTERS_V1);
    return sfn_round_part(in) && k && k->len >= 4 && f && !memcmp(f->data, "STFF", 4)
        && sfn_find(in, "settings.bin", SFN_SETTINGS_LEN) && sfn_find(in, "key_bank.bin", SFN_BANK_LEN);
}

/* Play `n` rounds through attract's replay, from the next ADV_REPLAY_INT.
 * n = 0 stops. The rounds stay the caller's until g_native_play.on drops. */
static const char *sfight_native_play(const game_native_round_t *rounds, int n) {
    for (int i = 0; i < n; i++)
        if (!sfn_playable(&rounds[i])) return "a round cannot be played: it was not keyed";
    memset(&s_sfp, 0, sizeof s_sfp);
    memset(&g_native_play, 0, sizeof g_native_play);
    if (n <= 0) return NULL;
    s_sfp.rounds = rounds;
    s_sfp.n      = n;
    g_native_play.on     = true;
    g_native_play.rounds = n;
    for (int i = 0; i < n; i++)
        g_native_play.total_frames += sfn_find(&rounds[i], "keys.bin", 0)->len / 2 - 1;
    return NULL;
}

/* ADV_REPLAY_INT (0x8F80): the round goes in before attract sets up its fight. */
static int sfight_hook_native_adv_int(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    if (!g_native_play.on) return 1;
    const game_native_round_t *in = &s_sfp.rounds[g_native_play.round];
    sfight_native_put(bus, in);
    const game_native_part_t *k = sfn_find(in, "keys.bin", 0);
    s_sfp.keys     = k->data;
    const game_native_part_t *d = sfn_find(in, "draws.bin", 0);
    s_sfp.draws    = d ? d->data : NULL;
    s_sfp.ndraws   = d ? d->len / 4 : 0;
    s_sfp.draw     = 0;
    s_sfp.drawing  = false;
    const game_native_part_t *v = sfn_find(in, "view.bin", 0);
    s_sfp.view     = v ? v->data : NULL;
    s_sfp.nview    = v ? v->len : 0;
    const game_native_part_t *f = sfn_find(in, "fighters.bin", SFN_FIGHTERS_LEN);
    if (!f) f = sfn_find(in, "fighters.bin", SFN_FIGHTERS_V1);
    s_sfp.fighters     = f->data;
    s_sfp.fighters_len = f->len;
    s_sfp.settings = sfn_find(in, "settings.bin", SFN_SETTINGS_LEN)->data;
    /* The bank's header as the round began: its keys wrote over +0xFFE. */
    sfn_write(bus, SFN_BANK, s_sfp.fighters + 0xC, SFN_BANK_KEYS);
    mem_write8(bus, SFN_BANK + 0xFFE, s_sfp.fighters[8]);
    mem_write8(bus, SFN_BANK + 0xFFF, s_sfp.fighters[9]);
    g_native_play.frame  = 0;
    g_native_play.frames = k->len / 2 - 1;
    return 1;
}

/* The round's first frame: the fighters, start_yang and the settings as the
 * recording had them. The board's own settings go back at the round's end. */
static inline void sfn_play_start(memory_bus_t *bus) {
    const uint8_t *f = s_sfp.fighters;
    for (uint32_t r = 0; r < 2; r++) {
        uint32_t rob = mem_read32(bus, r ? SFN_ROB1 : SFN_ROB0);
        const uint8_t *src = f + SFN_FIGHTERS_HEAD + r * SFN_ROB_LEN;
        sfn_write(bus, rob + SFN_ROB_FROM, src + SFN_ROB_FROM, SFN_ROB_LEN - SFN_ROB_FROM);
        /* +0's low three bytes are the fighter's flags (bit 1 facing, bit 23
         * sub_39E78's); the top byte is the task's. */
        sfn_write(bus, rob, src, 3);
        if (s_sfp.fighters_len == SFN_FIGHTERS_LEN)
            sfn_write(bus, mem_read32(bus, rob + SFN_MOTION_PTR), f + SFN_MOTION_AT + r * SFN_MOTION_LEN, SFN_MOTION_LEN);
    }
    mem_write16(bus, SFN_START_YANG, (uint16_t)(f[6] | (f[7] << 8)));
    if (s_sfp.fighters_len == SFN_FIGHTERS_LEN) {
        sfn_cop((uint8_t *)f + SFN_FIGHTERS_V1, true);
        sfn_ram(bus, (uint8_t *)f + SFN_RAM_AT, true);
    }
    if (!s_sfp.settings_put) sfn_read(bus, SFN_SETTINGS, s_sfp.settings_was, SFN_SETTINGS_LEN);
    sfn_write(bus, SFN_SETTINGS, s_sfp.settings, SFN_SETTINGS_LEN);
    s_sfp.settings_put = true;
    s_sfp.drawing      = true;
}

/* play_motion's draw (0x1C478, g0 = rand()): a motion script's chance step
 * (`rand < chance`). rand mixes in the board's timers, which attract does not
 * run as the game did, so the draws are kept with the round and given back:
 * draws.bin, per draw the key frame (u16 LE) and the draw (u16 LE). */
static int sfight_hook_native_draw(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    if (g_native_play.on) {
        if (!s_sfp.drawing || s_sfp.draw >= s_sfp.ndraws) return 1;
        const uint8_t *e = s_sfp.draws + 4 * s_sfp.draw++;
        cpu->globals.g[0] = (uint32_t)(e[2] | (e[3] << 8));
        return 1;
    }
    if (!s_sfr.live || s_sfr.draws >= SFN_DRAWS_MAX) return 1;
    uint8_t *e = s_sfr_draws + 4 * s_sfr.draws++;
    e[0] = (uint8_t)s_sfr.n; e[1] = (uint8_t)(s_sfr.n >> 8);
    e[2] = (uint8_t)cpu->globals.g[0]; e[3] = (uint8_t)(cpu->globals.g[0] >> 8);
    return 1;
}

/* cam_mode 0x0A's `cmpo 0, r5` (0x6B478), reached only in a game's fight:
 * which view the camera takes this frame. Observe only. */
static int sfight_hook_native_view_rec(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (g_native_play.on || !s_sfr.live || s_sfr.n >= SFN_KEYS_MAX) return 1;
    s_sfr_view[s_sfr.n] = (uint8_t)((cpu->locals.r[5] ? SFN_VIEW_PLAYER : 0)
                                  | (mem_read8(bus, 0x0050004C) ? SFN_VIEW_SWAP : 0));
    return 1;
}

/* cam_mode 0x0A (0x6B408) in the replay: the player's view where the round
 * had it, with the robs as cam_mode_33 would have swapped them. */
static int sfight_hook_native_view_play(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!g_native_play.on || !s_sfp.view || !s_sfp.drawing) return 1;
    uint32_t f = g_native_play.frame;
    uint8_t v = f < s_sfp.nview ? s_sfp.view[f] : 0;
    if (!(v & SFN_VIEW_PLAYER)) return 1;
    bool swap = (v & SFN_VIEW_SWAP) != 0;
    cpu->globals.g[7] = mem_read32(bus, swap ? SFN_ROB1 : SFN_ROB0);
    cpu->globals.g[8] = mem_read32(bus, swap ? SFN_ROB0 : SFN_ROB1);
    cpu->sfr.ip = SFN_CAM_PLAYER;
    return 0;
}

/* key_play_disp (0x37DC4; key_play_init falls into it once at the round's
 * start): it reads its frame's two keys at the bank pointer and moves it on,
 * so the pointer is put back at +0x14 with the next keys there every frame.
 * The key recorded on frame f + 1 is the one played on frame f. */
static int sfight_hook_native_key_play(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!g_native_play.on || !s_sfp.keys || sfn_debug_stopped(bus)) return 1;
    uint32_t f = g_native_play.frame;
    if (f == 0) {
        sfn_play_start(bus);
        if (mem_read32(bus, SFN_STEP_BIT) & SFN_STEPS_EARLY) return 1;
    }
    uint32_t bank = mem_read32(bus, SFN_BANK_PTR);
    bool have = f + 1 <= g_native_play.frames;
    mem_write8(bus, bank + SFN_BANK_KEYS,     have ? s_sfp.keys[2 * (f + 1)]     : 0);
    mem_write8(bus, bank + SFN_BANK_KEYS + 1, have ? s_sfp.keys[2 * (f + 1) + 1] : 0);
    mem_write32(bus, cpu->globals.g13 + 0x54, bank + SFN_BANK_KEYS);
    if (have) { g_native_play.frame++; g_native_play.total++; }
    return 1;
}

/* The round's keys have run out: is the fight the one recorded? Its energies
 * against round.json's "energy" (a round from before #617 has none). */
static inline void sfn_play_check(memory_bus_t *bus) {
    const game_native_round_t *in = &s_sfp.rounds[g_native_play.round];
    unsigned e0 = mem_read16(bus, mem_read32(bus, SFN_ROB0) + 0x1AC);
    unsigned e1 = mem_read16(bus, mem_read32(bus, SFN_ROB1) + 0x1AC);
    unsigned r0 = 0, r1 = 0;
    const char *at = strstr(in->json, "\"energy\":[");
    if (!at || sscanf(at + 10, "%u,%u", &r0, &r1) != 2) {
        LOG_INFO("native replay: played round %d, energies %u / %u", g_native_play.round + 1, e0, e1);
        return;
    }
    bool same = e0 == r0 && e1 == r1;
    log_msg(same ? LOG_LVL_INFO : LOG_LVL_WARN, "native replay: played round %d, energies %u / %u, %s (%u / %u)",
            g_native_play.round + 1, e0, e1, same ? "as recorded" : "NOT as recorded", r0, r1);
}

/* ADV_REPLAY_DSP (0x97B8): on the board a KO or attract's timer ends the
 * replay. Here the round's keys do: the timer is held at 2 until they run out
 * and then left to fall to 0 through the routine's own count (0x97D8). */
static int sfight_hook_native_adv_dsp(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!g_native_play.on || !s_sfp.keys) return 1;
    bool more = g_native_play.frame < g_native_play.frames;
    mem_write16(bus, SFN_GAME_TIMER, more ? 2 : 1);
    if (!more && s_sfp.settings_put) {
        sfn_play_check(bus);
        sfn_write(bus, SFN_SETTINGS, s_sfp.settings_was, SFN_SETTINGS_LEN);
        s_sfp.settings_put = false;
    }
    cpu->sfr.ip = 0x000097D8u;
    return 0;
}

/* ADV_REPLAY_PIC2 (0x98B8): the next round, or attract carries on. */
static int sfight_hook_native_adv_pic2(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!g_native_play.on) return 1;
    if (g_native_play.round + 1 < s_sfp.n) {
        g_native_play.round++;
        s_sfp.keys = NULL;
        mem_write8(bus, SFN_SUB_MODE, SFN_STEP_REPLAY);
        hle_ret(cpu);
        return 0;
    }
    g_native_play.on    = false;
    g_native_play.ended = true;
    g_replay_stage_pin  = -1;
    return 1;
}

/* Is a player's game on the board? The main mode (0x50002A): 0-3 the warning
 * and attract, 16-17 the test menu; the rest is a game, from INFO_INT to the
 * name entry and the ranking. */
static bool sfight_in_play(memory_bus_t *bus) {
    uint8_t mode = mem_read8(bus, 0x0050002A);
    return mode >= 4 && mode <= 19 && mode != 16 && mode != 17;
}

/* The recorder's counters ride in savestates and GGPO's rollback. */
static inline void sfight_native_install(const char *profile_id) {
    memset(&s_sfr, 0, sizeof s_sfr);
    savestate_extra(profile_id, "SFN_REC", &s_sfr, sizeof s_sfr);
}

#define SFIGHT_NATIVE_HOOK_COUNT 10
#define SFIGHT_NATIVE_HOOKS                                                           \
    { 0x0000DD20, sfight_hook_native_round,      "round_dsp judge (native replay)" }, \
    { 0x00037CE0, sfight_hook_native_key_rec,    "key_rec_disp (native replay)"    }, \
    { 0x00011418, sfight_hook_native_task_visit, "task loop (native replay)"       }, \
    { 0x00008F80, sfight_hook_native_adv_int,    "ADV_REPLAY_INT (native replay)"  }, \
    { 0x00037DC4, sfight_hook_native_key_play,   "key_play_disp (native replay)"   }, \
    { 0x000097B8, sfight_hook_native_adv_dsp,    "ADV_REPLAY_DSP (native replay)"  }, \
    { 0x000098B8, sfight_hook_native_adv_pic2,   "ADV_REPLAY_PIC2 (native replay)" }, \
    { SFN_DRAW_TAKEN, sfight_hook_native_draw,   "play_motion draw (native replay)" }, \
    { SFN_CAM_VIEW_TAKE, sfight_hook_native_view_rec,  "camera view (native replay)" }, \
    { SFN_CAM_VIEW,   sfight_hook_native_view_play, "camera view play (native replay)" }, \

#endif /* M2HLE_SFIGHT_NATIVE_H */
