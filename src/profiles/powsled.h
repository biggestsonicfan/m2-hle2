/*
 * profiles/powsled.h — Power Sled (powsled: the Slave unit, Revision A).
 *
 * MAME ROM set: powsled, a parent set (MAME src/mame/sega/model2.cpp,
 * ROM_START(powsled)). ROM names and CRCs are MAME's.
 *
 * Board: Model 2B CRX, the board STF, Fighting Vipers and Virtual-On run on.
 * The COP runs the program the game uploads (model2b_cop.h; copro_data is
 * empty, MAME's ROMREGION_ERASE00), interrupts go through the program's own
 * table (irq_vectors) and the frame edge is the vblank (board_vblank).
 *
 * Power Sled is a linked game: a Slave cabinet (this set, two riders) on a
 * comm-board ring with a Main unit (and a Relay one for the big screen), all
 * running the same program with a mode byte of their own (MAME init_powsledm
 * patches the relay program into the main one). The comm board is MAME's
 * (m2comm.h) with its frame offset 0x180.
 */
#ifndef PROFILES_POWSLED_H
#define PROFILES_POWSLED_H

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
#include "io_5649.h"
#include "savestate_reg.h"

static const rom_part_t powsled_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,   0x0000000, "epr-19470a.15", 0x8f28cc09, "epr-19471a.16", 0x01a013e3),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "fpr-19468.11",  0x56fae4e2, "fpr-19469.12",  0x5579c922),
    ROM_PAIR32(ROM_POLYGONS,  0x0000000, "fpr-19455.17",  0x165ee345, "fpr-19456.21",  0xc3b2e2c5),
    ROM_PAIR32(ROM_TEXTURES,  0x0000000, "fpr-19458.27",  0xf24acca2, "fpr-19457.25",  0x79d7e6fa),
    ROM_SWAP16(ROM_AUDIOCPU,  0x0000000, "epr-19466.31",  0xc42892a5),
    ROM_SWAP16(ROM_SAMPLES,   0x0000000, "fpr-19459.32",  0xa424743f),
};

static const rom_recipe_t powsled_recipe = {
    .set  = "powsled",
    .size = { [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000,
              [ROM_COPRO_DATA] = 0x800000,
              [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x800000,
              [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000 },
    .parts = powsled_parts, .count = sizeof powsled_parts / sizeof powsled_parts[0],
};

static inline int powsled_load(romset_t *rs, const char *child, const char *parent) {
    return rom_recipe_load(rs, child, parent, &powsled_recipe);
}

static inline uint32_t powsled_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

/* The unit the program runs as. One program is every unit: the byte it
 * stores at 0x529429 at boot is the mode (0 Relay, 1 Main, 2 Slave), and
 * FUN_15840 takes the node ID from the settings (rides 1-4; 0x100 the Relay,
 * 0x200 the Main). The Main's program is not dumped: MAME init_powsledm makes
 * one out of the Relay's with three bytes. The Slave's has the same mode
 * store (mov 2,g13 at 0x1571C), so its literal makes the Main mode, and the
 * Main's ID goes where the Slave masks the settings byte (and g4,g6,g4 at
 * 0x1584C becomes lda 0x200,g4; its inverted copy follows from the not). */
typedef enum { POWSLED_SLAVE, POWSLED_MAIN } powsled_unit_t;

static powsled_unit_t g_powsled_unit = POWSLED_SLAVE;

#define POWSLED_MODE_AT  0x1571Cu   /* mov <mode>,g13: the literal */
#define POWSLED_ID_AT    0x1584Cu   /* the node ID into g4 */
/* The race's camera: the setup at 0x37DD4 gives node 0x100 the Relay's big
 * screen camera 0x372D0 (it follows the riders) and every other node the
 * ride's own 0x366C0, which takes its rider's comm slot from 0x505530[ID].
 * For ID 0x200 that entry is null, the camera reads ROM 0x10 and goes NaN:
 * the Main drew only its HUD. setbit 8 -> setbit 9 makes the compare 0x200,
 * so the Main gets the Relay's camera; the comm frame keeps ID 0x200. */
#define POWSLED_CAM_AT   0x37DDCu   /* setbit <n>,0,g13: the literal */

static inline void powsled_set_unit(memory_bus_t *bus, powsled_unit_t unit) {
    static const uint8_t slave_id[4] = { 0x94, 0x80, 0xA5, 0x58 };   /* and g4,g6,g4 */
    static const uint8_t main_id[4]  = { 0x00, 0x02, 0xA0, 0x8C };   /* lda 0x200,g4 */
    if (!bus->rom || bus->rom_size < POWSLED_CAM_AT + 4u) return;
    bool m = unit == POWSLED_MAIN;
    bus->rom[POWSLED_MODE_AT] = m ? 0x01 : 0x02;
    memcpy(bus->rom + POWSLED_ID_AT, m ? main_id : slave_id, 4);
    bus->rom[POWSLED_CAM_AT] = m ? 0x89 : 0x88;
}

/* The settings (pedal calibration, ride colour, ...) are 0x1C bytes kept three
 * times in backup RAM, at 0x000, 0x100 and 0x200; FUN_edc30 takes two copies
 * that agree, or the defaults at 0xEDC00 when none do. A blank board's three
 * zeroed copies agree, so it would run with every pedal calibrated to 0 and
 * none of them steering: the operator's BACKUP DATA CLEAR is what writes the
 * defaults, and this does it for a blank board. */
#define POWSLED_SETTINGS_DEFAULTS 0xEDC00u
#define POWSLED_SETTINGS_SIZE     0x1Cu

static inline void powsled_seed_settings(const romset_t *rs, memory_bus_t *bus) {
    if (rs->maincpu_size < POWSLED_SETTINGS_DEFAULTS + POWSLED_SETTINGS_SIZE) return;
    for (uint32_t copy = 0; copy < 3; copy++)
        for (uint32_t i = 0; i < POWSLED_SETTINGS_SIZE; i++)
            if (mem_read8(bus, 0x01D00000u + copy * 0x100u + i)) return;
    for (uint32_t copy = 0; copy < 3; copy++)
        for (uint32_t i = 0; i < POWSLED_SETTINGS_SIZE; i++)
            mem_write8(bus, 0x01D00000u + copy * 0x100u + i, rs->maincpu[POWSLED_SETTINGS_DEFAULTS + i]);
}

static inline void powsled_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("powsled_install: romset not loaded"); return; }

    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    powsled_set_unit(bus, g_powsled_unit);
    powsled_seed_settings(rs, bus);
    /* The unit is the program's bytes, so they go with the board: a GGPO
     * match swaps the Main and the ride through the one bus (emu_twin.h). */
    if (bus->rom && bus->rom_size >= POWSLED_CAM_AT + 4u) {
        savestate_extra("powsled", "UNIT", &g_powsled_unit, sizeof g_powsled_unit);
        savestate_extra("powsled", "UNIT.mode", bus->rom + POWSLED_MODE_AT, 1);
        savestate_extra("powsled", "UNIT.id", bus->rom + POWSLED_ID_AT, 4);
        savestate_extra("powsled", "UNIT.cam", bus->rom + POWSLED_CAM_AT, 1);
    }

    model2b_cop_install(bus, rs->copro_data, rs->copro_data_size);
    savestate_extra("powsled", "M2BCOP", &g_m2bc, sizeof g_m2bc);
    savestate_extra("powsled", "SHARC", &g_shc, sizeof g_shc);
    m2comm_install(bus, 0x180u);
    m2comm_set_whole_frame(true);
    savestate_extra("powsled", "M2COMM", &g_m2comm, sizeof g_m2comm);
    io5649_reset();
    savestate_extra("powsled", "IO5649", &g_io5649, sizeof g_io5649);

    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
    }
    if (rs->main_data && bus->xtra_data && rs->main_data_size > 0x1000000u) {
        size_t n = rs->main_data_size - 0x1000000u;
        memcpy(bus->xtra_data, rs->main_data + 0x1000000u, n < XTRA_DATA_SIZE ? n : XTRA_DATA_SIZE);
    }

    /* The processor starts on the PRCB's interrupt stack, as vonj_install. */
    uint32_t prcb = powsled_read32(rs->maincpu, 0x04);
    cpu->prcb = prcb;
    if (prcb + 0x2C < rs->maincpu_size) {
        uint32_t isp = powsled_read32(rs->maincpu, prcb + PRCB_INTR_STACK);
        cpu->globals.fp = isp;
        cpu->locals.sp  = isp + 64;
        cpu->sfr.pc     = 0x001F2002u;
        uint32_t start_ip_ptr = powsled_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = powsled_read32(rs->maincpu, start_ip_ptr);
    }
    LOG_INFO("powsled_install: PRCB=0x%08X initial IP=0x%08X", prcb, cpu->sfr.ip);
}

/* A link's MASTER is the Main unit, its SLAVE a ride: the unit is the
 * program's bytes, which the reset into the role (comm_role.h) installs. The
 * ride's colour is its settings' byte 8 (0 SILVER, 1 RED, 2 BLACK, 3 YELLOW;
 * three copies in backup RAM at 0x000, 0x100, 0x200), set in the test menu: a
 * Main with one ride wants the SILVER one, a blank board's. */
static inline bool powsled_link_role(memory_bus_t *bus, game_link_role_t role) {
    (void)bus;
    g_powsled_unit = role == GAME_LINK_MASTER ? POWSLED_MAIN : POWSLED_SLAVE;
    return true;
}

/* The Main's operator, for a ride played alone (emu_ggpo.h, SOLO; Pinboard
 * #1110). The race state, 0x53E744 on every unit, is 4 between races, 5 with
 * entry open, 6 once it is closed, 7 racing and 8 on the results, then 4
 * again. The operator opens entry between races (P1 Call), waits for a rider
 * to press Entry (either seat), closes entry (P2 Call twice: the Main's own
 * entry, then the close) and starts the race (the Main's IN1 bit 4). Entry
 * stays open with nobody in it: the ride waits for its player. */
#define POWSLED_RACE_STATE 0x0053E744u
#define POWSLED_OP_PRESS   10   /* frames a button is held */
#define POWSLED_OP_GAP     60   /* frames between the operator's presses */

static struct {
    uint8_t  state;      /* the race state last seen */
    uint32_t in_state;   /* frames in it */
    uint32_t entered;    /* frames since a rider pressed Entry, 0 none */
    int      steps;      /* P2 Calls given in this entry */
    uint32_t since;      /* frames since the last press began */
} g_powsled_op;

static inline uint32_t powsled_solo_op(memory_bus_t *bus, uint32_t held) {
    enum { CALL = 0x200, ENTRY = 0x100 | 0x400, STEP = 0x800, GO = 0x20000 };
    uint8_t st = mem_read8(bus, POWSLED_RACE_STATE);
    if (st != g_powsled_op.state) {
        memset(&g_powsled_op, 0, sizeof g_powsled_op);
        g_powsled_op.state = st;
        g_powsled_op.since = POWSLED_OP_GAP;
    }
    g_powsled_op.in_state++;
    g_powsled_op.since++;
    uint32_t press = 0;
    switch (st) {
    case 4: press = CALL; break;
    case 5:
        if (!g_powsled_op.entered && (held & ENTRY)) g_powsled_op.entered = 1;
        else if (g_powsled_op.entered) g_powsled_op.entered++;
        if (g_powsled_op.entered > POWSLED_OP_GAP && g_powsled_op.steps < 2) press = STEP;
        break;
    case 6: press = GO; break;
    default: return 0;
    }
    if (!press) return 0;
    if (g_powsled_op.in_state < POWSLED_OP_GAP) return 0;   /* let the state settle */
    if (g_powsled_op.since >= POWSLED_OP_GAP) {
        g_powsled_op.since = 0;
        if (press == STEP) g_powsled_op.steps++;
    }
    return g_powsled_op.since < POWSLED_OP_PRESS ? press : 0;
}

/* MAME INPUT_PORTS_START(powsled): IN0 the cabinet (COIN1 0x01, COIN2 0x02,
 * TEST 0x04, SERVICE1 0x08, START 0x10 / 0x20, SERVICE2 0x40, Cancel Error
 * 0x80), IN1 the riders' buttons (P1 Entry 0x01, P1 Call 0x02, P2 Entry 0x04,
 * P2 Call 0x08), IN3 (port E) Cancel Network Check 0x02, and the pedals on
 * the 315-5649's A/D channels 1, 3, 5 and 7 (quirks.pedal). MAME reads a pedal
 * 0x00 up and 0xFF down; the program's default calibration (0xEDC00) has a
 * right pedal at 0x70 up and 0x90 down and a left one the other way round, and
 * FUN_15c30 reads anything outside the two as the far side, so those are what
 * the pedals read here.
 *
 * On the Main unit IN1 is the operator's (FUN_bd4b0): P1 Entry sends the rides
 * to the attract, P1 Call opens entry, P2 Call starts the race, P2 Entry the
 * test menu. In entry the Main's first press is its own entry (FUN_33260), so
 * entry closes on a second P2 Call. The Main's IN1 has three more buttons
 * MAME leaves out (its high nibble, FUN_bd4b0's commands 5, 0x100 and 0x300):
 * bit 4, the first, starts the race (state 6 -> 7). */
static const game_profile_t powsled_profile = {
    .id              = "powsled",
    .display_name    = "Power Sled (Slave, Rev A)",
    .parent_zip_name = NULL,
    .board           = BOARD_MODEL2B_CRX,
    .load_fn         = powsled_load,
    .install_fn      = powsled_install,
    .hook_count      = 0,
    .link_role       = powsled_link_role,
    .link_twin       = true,   /* GGPO: the Main (1P) and a ride (2P) on each machine */
    .link_solo       = powsled_solo_op,   /* alone: a Main beside the ride, its panel worked */
    .input = {
        .bits = {
            [GAME_INPUT_P1_COIN]  = 0x00000001,
            [GAME_INPUT_P2_COIN]  = 0x00000002,
            [GAME_INPUT_TEST]     = 0x00000004,
            [GAME_INPUT_SERVICE]  = 0x00000008,
            [GAME_INPUT_P1_START] = 0x00000010,
            [GAME_INPUT_P2_START] = 0x00000020,
            [GAME_INPUT_P1_B1]    = 0x00000100,
            [GAME_INPUT_P1_B2]    = 0x00000200,
            [GAME_INPUT_P2_B1]    = 0x00000400,
            [GAME_INPUT_P2_B2]    = 0x00000800,
            [GAME_INPUT_P1_B4]    = 0x00000080,   /* Cancel Error */
            [GAME_INPUT_P1_LEFT]  = 0x00001000,   /* the pedals (quirks.pedal) */
            [GAME_INPUT_P1_RIGHT] = 0x00002000,
            [GAME_INPUT_P2_LEFT]  = 0x00004000,
            [GAME_INPUT_P2_RIGHT] = 0x00008000,
            [GAME_INPUT_P1_B3]    = 0x00010000,   /* Cancel Network Check */
            [GAME_INPUT_P2_B3]    = 0x00020000,   /* the Main's IN1 bit 4: start the race */
            [GAME_INPUT_P2_B4]    = 0x00040000,   /* ... bit 5 (command 0x100) */
            [GAME_INPUT_P1_UP]    = 0x00100000,   /* ... bit 7 (command 0x300) */
        },
    },
    .quirks = {
        .enable_68k_sound = true,
        .irq_vectors      = true,
        .board_vblank     = true,
        .sled             = true,
        .pedal            = { .p1_left = 0x1000, .p1_right = 0x2000, .p2_left = 0x4000,
                              .p2_right = 0x8000, .cancel_net = 0x10000,
                              .right_up = 0x70, .right_down = 0x90, .left_up = 0x90, .left_down = 0x70 },
        .region           = { .kind = REGION_TODO, .why = "the country setting is not found yet" },
    },
};

#endif /* PROFILES_POWSLED_H */
