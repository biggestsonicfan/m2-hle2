/*
 * profiles/motoraid.h — Motor Raid - Twin (Sega, 1997), Model 2A (Pinboard #1627).
 *
 * MAME ROM set: motoraid, a parent set (src/mame/sega/model2.cpp,
 * ROM_START(motoraid), machine manxtt). ROM names and CRCs are MAME's. A CRX
 * board, the TGP running the program the i960 uploads (board/tgp.h,
 * board/model2a.h), its collision and height data in the copro ROMs.
 *
 * Controls (MAME INPUT_PORTS_START(motoraid)): the 315-5649's analog channels
 * 0 the throttle, 1 the brake and 2 the bank (io_5649.h, quirks.wheel.bike),
 * IN1 0x10 Punch and 0x20 Kick, IN0 the cabinet's (COIN1 0x01, COIN2 0x02,
 * TEST 0x04, SERVICE 0x08, START / VR 0x40).
 *
 * The link. Motor Raid is a ring of up to eight cabinets (and a LIVE MONITOR
 * spectator) on the comm board MAME's m2comm.h is, at its 0x01A10000 mirror:
 * a 0x1C0-byte packet a node a frame, the frame received one slot up from the
 * sender's (slot k the node k hops downstream, the node's own echo the last
 * slot). YAMP's mr-comm-packet.md decoded it from the ROM's symbols; the
 * addresses below are this ROM's.
 *   - The role is the cabinet's GAME ASSIGNMENTS (link_ID): _InitGameSet
 *     (0x13BB0) takes the EEPROM mirror's byte 0x51900E & 0xF into
 *     _GAMEASSIGN+0xC (0x518EFC): 0 alone, 1 MASTER, 2-8 a SLAVE, 0xF the
 *     LIVE MONITOR. _InitCommunication (0x4B150) reads it at boot, and a
 *     slave runs _InitGameSet again once it has taken the master's settings.
 *     The EEPROM is erased at every board reset here (eeprom_93c46.h), so the
 *     role is put into _InitGameSet's register (motoraid_hook_link_id), every
 *     time it runs, from g_motoraid_link: the room's role (net/comm_role.h).
 *   - Every node's CABINET ID must differ: the check's exit (0x4A280)
 *     compares the others' (packet +0x12 bits 5-8) with its own
 *     (_GAMEASSIGN+0x1C, 0x518F0C), and a match is NETWORK ERROR (_SubMode
 *     9) some seconds into every linked attract. The EEPROM default is 0 on
 *     both, so the ID is put into _InitGameSet's r11 too
 *     (motoraid_hook_cab_id): the MASTER 0, the SLAVE 1.
 *   - The check needs every RX slot's status nibble (+0x12) at 1, the echo's
 *     too: MAME's whole frame (m2comm_set_whole_frame) puts this node's own
 *     frame in slot 1 of two.
 *   - A linked cabinet with one node re-runs the check: no ring of one, so no
 *     virtual TOSlink cable (link_cable).
 */
#ifndef PROFILES_MOTORAID_H
#define PROFILES_MOTORAID_H

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
#include "model2a.h"
#include "m2comm.h"
#include "io_5649.h"
#include "savestate_reg.h"

static const rom_part_t motoraid_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-20007.12", 0xf040c108, "epr-20008.13", 0x78976e1a),
    ROM_PAIR32(ROM_MAIN_DATA,  0x0000000, "mpr-20019.10", 0x49053727, "mpr-20020.11", 0xcc5ddb15),
    ROM_PAIR32(ROM_MAIN_DATA,  0x0800000, "mpr-20017.8",  0x4e206acd, "mpr-20018.9",  0xe7ed0e85),
    ROM_PAIR32(ROM_MAIN_DATA,  0x1000000, "mpr-20015.6",  0x23427339, "mpr-20016.7",  0xc99a83f4),
    ROM_PAIR32(ROM_MAIN_DATA,  0x1800000, "epr-20013.4",  0xa4478f52, "epr-20014.5",  0x1aa541be),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1900000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1a00000, 0x200000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1c00000, 0x400000),
    ROM_PAIR32(ROM_POLYGONS,   0x0000000, "mpr-20023.16", 0x016be8d6, "mpr-20026.20", 0x20044a30),
    ROM_PAIR32(ROM_POLYGONS,   0x0800000, "mpr-20024.17", 0x62fd2d5b, "mpr-20027.21", 0xb2504ea6),
    ROM_PAIR32(ROM_POLYGONS,   0x1000000, "mpr-20025.18", 0xd4ecd0be, "mpr-20028.22", 0x3147e0e1),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "epr-20011.28", 0x794c026c, "epr-20012.29", 0xf53db4e3),
    ROM_PAIR32(ROM_TEXTURES,   0x0000000, "mpr-20022.25", 0x9e47b3c2, "mpr-20021.24", 0x3cbf36cb),
    ROM_SWAP16(ROM_AUDIOCPU,   0x0000000, "epr-20029.30", 0x927d31b9),
    ROM_SWAP16(ROM_SAMPLES,    0x0000000, "mpr-20030.31", 0xb70ab686),
    ROM_SWAP16(ROM_SAMPLES,    0x0200000, "mpr-20031.32", 0x84da70e4),
    ROM_SWAP16(ROM_SAMPLES,    0x0400000, "mpr-20032.36", 0x15516d35),
    ROM_SWAP16(ROM_SAMPLES,    0x0600000, "mpr-20033.37", 0x8c8ed187),
    /* MODEL2_CPU_BOARD: the TGP's tables, on the 2A's CPU board. */
    ROM_PAIR32(ROM_TGP_TABLES, 0x0000000, "opr-14742a.45", 0x90c6b117, "opr-14743a.46", 0xae7f446b),
};

static const rom_recipe_t motoraid_recipe = {
    .set = "motoraid",
    .size = {
        [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2400000, [ROM_COPRO_DATA] = 0x800000,
        [ROM_POLYGONS] = 0x2000000, [ROM_TEXTURES] = 0x1000000,
        [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000, [ROM_TGP_TABLES] = 0x40000,
    },
    .parts = motoraid_parts, .count = sizeof motoraid_parts / sizeof motoraid_parts[0],
};

static inline int motoraid_load(romset_t *rs, const char *child, const char *parent) {
    return rom_recipe_load(rs, child, parent, &motoraid_recipe);
}

static inline uint32_t motoraid_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

/* The cabinet's link_ID while a room links it (see the top): 0 none (the
 * EEPROM's own), 1 MASTER, 2 SLAVE. A savestate extra, as it goes with the
 * board when a GGPO match swaps the two cabinets through one bus. */
static uint32_t g_motoraid_link;

#define MOTORAID_LINK_ID_AT 0x00013C50u   /* _InitGameSet: g3 is the assign, about to be kept */
#define MOTORAID_CAB_ID_AT  0x00013CC4u   /* _InitGameSet: r11 the CABINET ID, kept at 0x13CCC */

static int motoraid_hook_link_id(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    if (g_motoraid_link) cpu->globals.g3 = g_motoraid_link;
    return 1;
}

/* The CABINET ID (EEPROM byte 0x51900F & 7, the menu shows it plus one) must
 * differ between the cabinets: once the handshake is through, the exit from
 * _InitCommunication (0x4A420) compares every other node's ID (packet +0x12
 * bits 5-8) with its own (_GAMEASSIGN+0x1C, 0x518F0C) and a match is a
 * NETWORK ERROR. Two boards with the EEPROM's defaults are both ID 0, so a
 * linked cabinet takes its node's: the MASTER 0, the SLAVE 1. */
static int motoraid_hook_cab_id(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    if (g_motoraid_link) cpu->locals.r11 = g_motoraid_link - 1u;
    return 1;
}

/*
 * The region (Pinboard #1628). The settings are the EEPROM block the boot's
 * loader (0x1F030) reads into 0x518FF0: "SEGA", a check word at +8 over the
 * GAME ASSIGNMENTS, kept twice (the copy at 0x51901C). COUNTRY is the byte at
 * 0x519006 (0 JAPAN, 1 USA, 2 EXP, 3 AUSTRALIAN, 4 KOREA: the strings at
 * 0x96980; the test menu edits it as 0x5194BE), g_region's numbers for the
 * first three. _InitGameSet takes it into _GAMEASSIGN (0x518EF0), and the
 * coin settings (0x1A510) go by it too.
 *
 * country_default (0x1F138): a block that fails took the ROM's defaults
 * (0x1F200 copies them from 0x21F1C0, COUNTRY JAPAN) and is about to be
 * kept (0x1F150). Put g_region there first. Japan (0) is the ROM's
 * default and MAME's, so --region japan (the graders') leaves the board as it
 * was.
 *
 * settings_checked (0x1F124, the loader's ret, where every path ends): with
 * g_region_over_saved, put g_region over the COUNTRY a saved block brought.
 * The EEPROM is erased at every board reset here (eeprom_93c46.h), so today
 * every boot takes the defaults and this writes what country_default did.
 */
#define MOTORAID_COUNTRY 0x00519006u

static int motoraid_hook_country_default(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    mem_write8(bus, MOTORAID_COUNTRY, (uint8_t)g_region);
    return 1;
}

static uint32_t motoraid_country(memory_bus_t *bus) { (void)bus; return MOTORAID_COUNTRY; }

static int motoraid_hook_settings_checked(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    if (g_region_over_saved) mem_write8(bus, MOTORAID_COUNTRY, (uint8_t)g_region);
    return 1;
}

static inline bool motoraid_link_role(memory_bus_t *bus, game_link_role_t role) {
    (void)bus;
    g_motoraid_link = role == GAME_LINK_MASTER ? 1u : role == GAME_LINK_SLAVE ? 2u : 0u;
    return true;
}

static inline void motoraid_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("motoraid_install: romset not loaded"); return; }
    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2a_install(bus, rs->tgp_tables, rs->copro_data, rs->copro_data_size);
    savestate_extra("motoraid", "TGP", &g_tgp, sizeof g_tgp);
    savestate_extra("motoraid", "I960FRAC", &g_i960_cyc_frac, sizeof g_i960_cyc_frac);
    savestate_extra("motoraid", "I960BT", &g_i960_bt, sizeof g_i960_bt);
    io5649_reset();
    savestate_extra("motoraid", "IO5649", &g_io5649, sizeof g_io5649);
    m2comm_install(bus, 0x1C0u);
    m2comm_set_whole_frame(true);
    savestate_extra("motoraid", "M2COMM", &g_m2comm, sizeof g_m2comm);
    savestate_extra("motoraid", "LINK", &g_motoraid_link, sizeof g_motoraid_link);

    /* main_data at 0x02000000; the "extra" window at 0x06000000 is main_data
     * from 0x1000000 (MAME model2_base_mem). */
    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
    }
    if (rs->main_data && bus->xtra_data && rs->main_data_size > 0x1000000u) {
        size_t n = rs->main_data_size - 0x1000000u;
        memcpy(bus->xtra_data, rs->main_data + 0x1000000u, n < XTRA_DATA_SIZE ? n : XTRA_DATA_SIZE);
    }

    /* MAME i960 device_reset: the PRCB's interrupt stack, priority 31,
     * interrupted, supervisor. */
    uint32_t prcb = motoraid_read32(rs->maincpu, 0x04);
    cpu->prcb = prcb;
    if (prcb + 0x2C < rs->maincpu_size) {
        uint32_t isp = motoraid_read32(rs->maincpu, prcb + PRCB_INTR_STACK);
        cpu->globals.fp = isp;
        cpu->locals.sp  = isp + 64;
        cpu->sfr.pc     = 0x001F2002u;
        uint32_t start_ip_ptr = motoraid_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = motoraid_read32(rs->maincpu, start_ip_ptr);
    }
    LOG_INFO("motoraid_install: PRCB=0x%08X initial IP=0x%08X", prcb, cpu->sfr.ip);
}

static const game_profile_t motoraid_profile = {
    .id = "motoraid", .display_name = "Motor Raid - Twin",
    .board = BOARD_MODEL2A_CRX, .load_fn = motoraid_load, .install_fn = motoraid_install,
    .hook_count = 4,
    .hooks = {
        { MOTORAID_LINK_ID_AT, motoraid_hook_link_id, "link_id" },
        { MOTORAID_CAB_ID_AT,  motoraid_hook_cab_id,  "cab_id" },
        { 0x0001F138u, motoraid_hook_country_default,  "country_default" },
        { 0x0001F124u, motoraid_hook_settings_checked, "settings_checked" },
    },
    .link_role = motoraid_link_role,
    .link_twin = true,   /* GGPO: both cabinets on each machine, one rider on each */
    .link_setting = "GAME ASSIGNMENTS > NETWORK TYPE: STAND ALONE",
    .input = { .bits = {
        /* IN0: the cabinet's. IN1 (byte 1): Punch 0x10, Kick 0x20. Byte 2 is
         * no port: io_5649.h reads it as the throttle, brake and bank. */
        [GAME_INPUT_P1_COIN]  = 0x00000001, [GAME_INPUT_P2_COIN]  = 0x00000002,
        [GAME_INPUT_TEST]     = 0x00000004, [GAME_INPUT_SERVICE]  = 0x00000008,
        [GAME_INPUT_P1_START] = 0x00000040,
        [GAME_INPUT_P1_B3]    = 0x00001000, [GAME_INPUT_P1_B4]    = 0x00002000,
        [GAME_INPUT_P1_B1]    = 0x00010000, [GAME_INPUT_P1_B2]    = 0x00020000,
        [GAME_INPUT_P1_RIGHT] = 0x00040000, [GAME_INPUT_P1_LEFT]  = 0x00080000,
    } },
    .quirks = {
        .enable_68k_sound = true,
        .irq_vectors      = true,
        .board_vblank     = true,
        .io_5649          = true,
        .wheel = { .left = 0x80000, .right = 0x40000, .accel = 0x10000, .brake = 0x20000,
                   .bike = true },
        .region = { .kind = REGION_HOOKED, .factory_default = "country_default",
                     .over_saved = "settings_checked", .country = motoraid_country },
    },
};

#endif /* PROFILES_MOTORAID_H */
