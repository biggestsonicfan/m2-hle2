/*
 * profiles/vonj.h — Cyber Troopers Virtual-On, Twin (vonj: Japan, Revision B).
 *
 * MAME ROM set: vonj, a clone of von (MAME src/mame/sega/model2.cpp,
 * ROM_START(vonj)): only the first program pair is its own, the rest comes
 * from von.zip. ROM names and CRCs are MAME's; the chips are found by CRC as
 * well, so a set that labels them otherwise still loads.
 *
 * Board: Model 2B CRX, the board STF and Fighting Vipers run on, so the
 * profile is the board's defaults, but for the COP, which runs the program
 * the game uploads (model2b_cop.h): interrupts through the program's own table
 * (irq_vectors), the frame edge at the vblank (board_vblank), no hooks.
 *
 * The comm board (cpu3, epr-18643a.7) that links the two cabinets of a Twin
 * is there with nobody on the link (m2comm.h), as in MAME: a board on its own
 * is one cabinet, a player against the CPU. The ROM's link protocol is in YAMP's notes (newyamp, docs/von-*.md).
 *
 * Controls: two levers, each with a trigger (shot) and a thumb button (dash)
 * (MAME INPUT_PORTS_START(von)). IN1 is the left lever, IN2 the right. The
 * left lever is player 1's directions and B1 / B2, the right lever player 2's
 * directions and B1 / B2 (its port is STF's player 2 port). Both levers the same
 * way walk, opposite ways turn, both out jump, both in crouch. P1 B3 is
 * both triggers (the centre weapon) and P1 B4 both dashes.
 */
#ifndef PROFILES_VONJ_H
#define PROFILES_VONJ_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "rom_loader.h"
#include "rom_recipe.h"
#include "game_profile.h"
#include "memory.h"
#include "i960.h"
#include "model2b_cop.h"
#include "m2comm.h"
#include "savestate_reg.h"

static const rom_part_t vonj_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-18664b.15", 0x27d0172c, "epr-18665b.16", 0x2f0142ee),
    ROM_PAIR32(ROM_MAINCPU,    0x0100000, "epr-18666.13",  0x66edb432, "epr-18667.14",  0xb593d31f),
    ROM_PAIR32(ROM_MAIN_DATA,  0x0000000, "mpr-18648.11",  0x2edbe489, "mpr-18649.12",  0xe68c5aa6),
    ROM_PAIR32(ROM_MAIN_DATA,  0x0800000, "mpr-18650.9",   0x89a855b9, "mpr-18651.10",  0xf4c23107),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "mpr-18662.29",  0xa33d3335, "mpr-18663.30",  0xea74a641),
    ROM_PAIR32(ROM_POLYGONS,   0x0000000, "mpr-18654.17",  0x6a0caf29, "mpr-18655.21",  0xa4293e78),
    ROM_PAIR32(ROM_POLYGONS,   0x0800000, "mpr-18656.18",  0xb4f51e76, "mpr-18657.22",  0xa9be4674),
    ROM_PAIR32(ROM_TEXTURES,   0x0000000, "mpr-18660.27",  0xe53663e5, "mpr-18658.25",  0x3d0fcd01),
    ROM_PAIR32(ROM_TEXTURES,   0x0800000, "mpr-18661.28",  0x52b50410, "mpr-18659.26",  0x27aa8ae2),
    ROM_SWAP16(ROM_AUDIOCPU,   0x0000000, "epr-18670.31",  0x3e715f76),
    ROM_SWAP16(ROM_SAMPLES,    0x0000000, "mpr-18652.32",  0x037eee53),
    ROM_SWAP16(ROM_SAMPLES,    0x0400000, "mpr-18653.34",  0x9ec3e7bf),
};

static const rom_recipe_t vonj_recipe = {
    .set  = "vonj",
    .size = { [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000,
              [ROM_COPRO_DATA] = 0x800000,
              [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x1000000,
              [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000 },
    .parts = vonj_parts, .count = sizeof vonj_parts / sizeof vonj_parts[0],
};

static inline int vonj_load(romset_t *rs, const char *child, const char *parent) {
    return rom_recipe_load(rs, child, parent, &vonj_recipe);
}

static inline uint32_t vonj_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

static inline void vonj_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("vonj_install: romset not loaded"); return; }

    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);

    /* The COP runs Virtual-On's own program, not STF's (model2b_cop.h), and
     * reads its sine/cosine tables straight out of copro_data. */
    model2b_cop_install(bus, rs->copro_data, rs->copro_data_size);
    savestate_extra("vonj", "M2BCOP", &g_m2bc, sizeof g_m2bc);
    savestate_extra("vonj", "SHARC", &g_shc, sizeof g_shc);
    /* A comm board with no other cabinet on the link: without one the
     * machine select waits for a link state forever (m2comm.h). */
    m2comm_install(bus, 0x1C0u);
    savestate_extra("vonj", "M2COMM", &g_m2comm, sizeof g_m2comm);

    /* main_data at 0x02000000; the "extra" window at 0x06000000 is main_data
     * from 0x1000000 (MAME model2_base_mem), which Virtual-On leaves empty. */
    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
    }
    if (rs->main_data && bus->xtra_data && rs->main_data_size > 0x1000000u) {
        size_t n = rs->main_data_size - 0x1000000u;
        memcpy(bus->xtra_data, rs->main_data + 0x1000000u, n < XTRA_DATA_SIZE ? n : XTRA_DATA_SIZE);
    }

    /* The processor starts on the PRCB's interrupt stack, at priority 31,
     * interrupted, supervisor (MAME i960 device_reset), as sfight_install. */
    uint32_t prcb = vonj_read32(rs->maincpu, 0x04);
    cpu->prcb = prcb;
    if (prcb + 0x2C < rs->maincpu_size) {
        uint32_t isp = vonj_read32(rs->maincpu, prcb + PRCB_INTR_STACK);
        cpu->globals.fp = isp;
        cpu->locals.sp  = isp + 64;
        cpu->sfr.pc     = 0x001F2002u;
        uint32_t start_ip_ptr = vonj_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = vonj_read32(rs->maincpu, start_ip_ptr);
    }
    LOG_INFO("vonj_install: PRCB=0x%08X initial IP=0x%08X", prcb, cpu->sfr.ip);
}

/* The PS3 shell's ARCADE rows written into the cabinet's GAME ASSIGNMENTS:
 * the backup-RAM bytes its test menu edits. The game reads them when a game
 * starts, so a write during the attract applies to the next game. The PS3 port
 * does the same (NPJB00321 FUN_0019f0e4, through its board-write call
 * FUN_0018f6ac).
 *   0x1D00016..1A  PLAY TIME 1P (stage 1~5) / penalty / stage 6~8 / last stage /
 *                  versus: (n + 1) * 5 seconds; 0xFF is DEATH-MATCH. The PS3
 *                  writes 0x16, 0x18 and 0x1A and leaves the penalty and
 *                  last-stage times alone.
 *   0x1D0001B..1F  MATCH COUNT: rounds to win - 1 (the PS3 skips 0x1E).
 *   0x1D00021      GAME DIFFICULTY: 0 NORMAL, 1 HARD, 2 VERY HARD, 3 EASY.
 *   0x1D00034      free play: the shell has no coins, so START starts a game.
 * s[] is ps3ui_shell's record: s[0] difficulty (Easy..Very Hard), s[3] play
 * time (30, 60, 90 s, death-match), s[4] rounds to win - 1. */
#define VONJ_BACKUP_PLAY_TIME  0x01D00016u
#define VONJ_BACKUP_MATCHES    0x01D0001Bu
#define VONJ_BACKUP_DIFFICULTY 0x01D00021u
#define VONJ_BACKUP_FREE_PLAY  0x01D00034u

static inline void vonj_apply_menu_settings(memory_bus_t *bus, const uint8_t s[8]) {
    static const uint8_t difficulty[4] = { 3, 0, 1, 2 };
    static const uint8_t play_time[4]  = { 5, 11, 17, 0xFF };
    mem_write8(bus, VONJ_BACKUP_DIFFICULTY, difficulty[s[0] < 4 ? s[0] : 1]);
    for (uint32_t i = 0; i < 5; i += 2)
        mem_write8(bus, VONJ_BACKUP_PLAY_TIME + i, play_time[s[3] < 4 ? s[3] : 2]);
    for (uint32_t i = 0; i < 5; i++)
        if (i != 3) mem_write8(bus, VONJ_BACKUP_MATCHES + i, s[4] < 5 ? s[4] : 0);
    mem_write8(bus, VONJ_BACKUP_FREE_PLAY, 1);
}

/* The cabinet's role on the link (Pinboard #969): the backup-RAM byte
 * InitNetwork (0x18A10) reads at every boot, 0x1D00028: 0 SLAVE (link id 2),
 * 1 MASTER (link id 1), anything else a cabinet alone (link id 3; the factory
 * value is 2).
 *
 * It is in the settings block InitBackup (0x2440) checks at boot: the ROM's 16
 * bytes from 0x2420, the block's size 0x20C at +0x10, and at +0x14 GetCRC
 * (0x3120: seed 0xDEBDEB00, a u16 table at ROM 0x2F20) of +0x16..+0x37. The
 * block is kept twice, the copy at +0x20C (BackupBKRAM 0x2330); a block that
 * fails both is factory-filled, in the same frame InitNetwork reads the role
 * (frame 285 from blank RAM). So the role goes into a block the ROM has made,
 * with its CRC, and the board boots into it (net/comm_role.h). */
#define VONJ_BACKUP_BLOCK  0x20Cu
#define VONJ_BACKUP_CRC    0x14u
#define VONJ_BACKUP_ROLE   0x28u
#define VONJ_ROM_HEADER    0x2420u
#define VONJ_ROM_CRC_TABLE 0x2F20u

static inline uint32_t vonj_rom16(const memory_bus_t *bus, uint32_t off) {
    return (uint32_t)bus->rom[off] | (uint32_t)bus->rom[off + 1] << 8;
}

/* GetCRC (0x3120) of n bytes. */
static inline uint16_t vonj_backup_crc(const memory_bus_t *bus, const uint8_t *p, uint32_t n) {
    uint32_t c = 0xDEBDEB00u;
    while (n--) c = ((c + *p++) << 8) ^ (vonj_rom16(bus, VONJ_ROM_CRC_TABLE + (c >> 24) * 2u) << 16);
    return (uint16_t)(vonj_rom16(bus, VONJ_ROM_CRC_TABLE + (c >> 24) * 2u) ^ (c >> 8));
}

/* A copy the ROM made: its header and size. */
static inline bool vonj_backup_made(const memory_bus_t *bus, const uint8_t *b) {
    return bus->rom && bus->rom_size > VONJ_ROM_CRC_TABLE + 0x200u
        && memcmp(b, bus->rom + VONJ_ROM_HEADER, 16) == 0
        && vonj_read32(b, 0x10) == VONJ_BACKUP_BLOCK;
}

/* One copy of the block, as InitBackup checks it. */
static inline bool vonj_backup_valid(const memory_bus_t *bus, const uint8_t *b) {
    return vonj_backup_made(bus, b)
        && (uint32_t)(b[VONJ_BACKUP_CRC] | b[VONJ_BACKUP_CRC + 1u] << 8) == vonj_backup_crc(bus, b + 0x16, 0x22);
}

/* The role into each copy the ROM made, with a fresh CRC: the PS3 shell's
 * Arcade settings (vonj_apply_menu_settings) go into the first copy without
 * one, and the boot into the role keeps them. False before the ROM made it. */
static inline bool vonj_link_role(memory_bus_t *bus, game_link_role_t role) {
    static const uint8_t role_byte[3] = { 2, 1, 0 };   /* ALONE, MASTER, SLAVE */
    bool any = false;
    for (uint32_t at = 0; at <= VONJ_BACKUP_BLOCK; at += VONJ_BACKUP_BLOCK) {
        uint8_t *b = bus->back + at;
        if (!vonj_backup_made(bus, b)) continue;
        b[VONJ_BACKUP_ROLE] = role_byte[role <= GAME_LINK_SLAVE ? role : GAME_LINK_ALONE];
        uint16_t crc = vonj_backup_crc(bus, b + 0x16, 0x22);
        b[VONJ_BACKUP_CRC]      = (uint8_t)crc;
        b[VONJ_BACKUP_CRC + 1u] = (uint8_t)(crc >> 8);
        any = true;
    }
    return any;
}

/* IN0 is the cabinet's: COIN1 0x01, COIN2 0x02, SERVICE 0x04, START 0x10
 * (MAME INPUT_PORTS_START(model2crx)). IN1 (byte 1) the left lever, IN2
 * (byte 2) the right: shot 0x01, dash 0x02, down 0x10, up 0x20, right 0x40,
 * left 0x80 (INPUT_PORTS_START(von)). */
#define VONJ_L_SHOT 0x00000100u
#define VONJ_L_DASH 0x00000200u
#define VONJ_R_SHOT 0x00010000u
#define VONJ_R_DASH 0x00020000u

/* The game's state, for replays (core/replay.h). Names are the PS3 build's
 * symbol table (rokuYAMP docs/von-rom-symbols.md). MainMode indexes
 * __mainloop__'s table at ROM 0x18680: 1 Advertize (attract), 3 InitGameMain,
 * 4 GameMain (select, the rounds, continue), 5 test. SubMode 10 is a round on
 * (Pinboard #995, watched over the bridge). machine_P / machine_E are the
 * player's machine and the other side's, in the order of the name table at ROM
 * 0x181F0. NumOfSets is MATCH COUNT, rounds to win - 1. */
#define VONJ_MAIN_MODE   0x005039F4u
#define VONJ_SUB_MODE    0x00503A00u
#define VONJ_SET_WIN     0x00503A6Cu
#define VONJ_SET_LOSE    0x00503A70u
#define VONJ_NUM_OF_SETS 0x00503A78u
#define VONJ_VERSUS_MODE 0x00503A7Cu
#define VONJ_MACHINE_P   0x00503A98u
#define VONJ_MACHINE_E   0x00503A9Cu
#define VONJ_FIELD_NO    0x005770F0u

static inline bool vonj_in_play(memory_bus_t *bus) {
    uint32_t mode = mem_read32(bus, VONJ_MAIN_MODE);
    return mode == 3 || mode == 4;
}

static inline void vonj_match_info(memory_bus_t *bus, game_match_info_t *out) {
    static const char *const machines[10] = {
        "Temjin", "Viper II", "Belgdor", "Raiden", "Dorkas",
        "Fei-Yen", "Apharmd", "Bal-Bas-Bow", "Jaguarandi", "Z-Gradt",
    };
    static const char *const difficulty[4] = { "Normal", "Hard", "Very Hard", "Easy" };
    uint32_t side[2] = { mem_read32(bus, VONJ_MACHINE_P), mem_read32(bus, VONJ_MACHINE_E) };
    bool versus = mem_read32(bus, VONJ_VERSUS_MODE) != 0;
    uint8_t d   = mem_read8(bus, VONJ_BACKUP_DIFFICULTY);
    out->fighting = mem_read32(bus, VONJ_MAIN_MODE) == 4 && mem_read32(bus, VONJ_SUB_MODE) == 10;
    out->players  = 2;
    for (int i = 0; i < 2; i++) {
        out->chara[i]      = side[i] < 10 ? (int)side[i] : -1;
        out->chara_name[i] = side[i] < 10 ? machines[side[i]] : NULL;
        out->cpu[i]        = i == 1 && !versus;
    }
    out->difficulty      = versus || d > 3 ? -1 : d;
    out->difficulty_name = versus || d > 3 ? NULL : difficulty[d];
    out->stage           = (int)mem_read32(bus, VONJ_FIELD_NO);
    out->stage_name      = NULL;
    out->rounds[0]       = (int)mem_read32(bus, VONJ_SET_WIN);
    out->rounds[1]       = (int)mem_read32(bus, VONJ_SET_LOSE);
    out->rounds_to_win   = (int)mem_read32(bus, VONJ_NUM_OF_SETS) + 1;
}

static const game_profile_t vonj_profile = {
    .id              = "vonj",
    .display_name    = "Cyber Troopers Virtual-On (Japan, Rev B)",
    .parent_zip_name = "von.zip",
    .board           = BOARD_MODEL2B_CRX,
    .load_fn         = vonj_load,
    .install_fn      = vonj_install,
    .hook_count      = 0,
    .link_role       = vonj_link_role,
    .match_info      = vonj_match_info,
    .in_play         = vonj_in_play,
    .input = {
        .bits = {
            [GAME_INPUT_P1_COIN]  = 0x00000001,
            [GAME_INPUT_P2_COIN]  = 0x00000002,
            [GAME_INPUT_SERVICE]  = 0x00000004,
            [GAME_INPUT_P1_START] = 0x00000010,
            [GAME_INPUT_P1_DOWN]  = 0x00001000,
            [GAME_INPUT_P1_UP]    = 0x00002000,
            [GAME_INPUT_P1_RIGHT] = 0x00004000,
            [GAME_INPUT_P1_LEFT]  = 0x00008000,
            [GAME_INPUT_P1_B1]    = VONJ_L_SHOT,
            [GAME_INPUT_P1_B2]    = VONJ_L_DASH,
            [GAME_INPUT_P1_B3]    = VONJ_L_SHOT | VONJ_R_SHOT,
            [GAME_INPUT_P1_B4]    = VONJ_L_DASH | VONJ_R_DASH,
            [GAME_INPUT_P2_DOWN]  = 0x00100000,
            [GAME_INPUT_P2_UP]    = 0x00200000,
            [GAME_INPUT_P2_RIGHT] = 0x00400000,
            [GAME_INPUT_P2_LEFT]  = 0x00800000,
            [GAME_INPUT_P2_B1]    = VONJ_R_SHOT,
            [GAME_INPUT_P2_B2]    = VONJ_R_DASH,
        },
    },
    .quirks = {
        .enable_68k_sound = true,
        .irq_vectors      = true,
        .board_vblank     = true,
        /* vonj is the Japanese program: its test menu has no country, and
         * MAME's von and vonu (the other countries) are other program ROMs, not a setting. */
        .region           = { .kind = REGION_PROGRAM_ROM,
                              .why  = "the program ROM is Japan's (von, vonu are other sets)" },
    },
};

#endif /* PROFILES_VONJ_H */
