/*
 * profiles/daytona.h — Daytona USA (daytona, Japan, Revision A). Runs on the
 * desktop builds (Pinboard #1035) and the web (#1047): attract, the test menu, coin to race, with
 * the wheel, pedals and lever through the Model 1 I/O board, and the Model 1
 * sound board's music and effects (board/model1_sound.h).
 *
 * Pinboard #660 (parity with noclip): the explorer reads this set and its
 * clones (vendor/noclip js/games.js, `daytona`), and the polygon decoder was
 * first worked out on it (CLAUDE.md, "3D Polygon Decoder"). The profile loads
 * the same ROMs with the same model table, ready for the board work. ROM names,
 * CRCs and offsets are MAME's (src/mame/sega/model2.cpp, ROM_START(daytona)).
 *
 * The board: an original Model 2 (MAME model2o), like Virtua Cop. The i960
 * talks to an MB86234 TGP running the game's program (board/tgp.h, as Sega
 * Rally's; Daytona sends some arguments with halfword stores, model2a.h), the
 * model2o memory map is board/model2o.h's, the Model 1 I/O board is
 * board/model1io.h (its Z80 runs model1io.zip's program), and M2COMM answers
 * as a link of one. The sound board is MAME's segam1audio: a 68000, two
 * MultiPCMs and a YM3438 whose timers run but whose FM voices are not made
 * (the game keys none on in attract or a race; a key-on is logged). The
 * drive board's force feedback is its commands decoded (board/drive_board.h)
 * and played on the host's pad or wheel (core/ffb.h, Pinboard #1047). What it
 * lacks: a second cabinet on the link. The comm board's Z80 program
 * (epr-16726) and the drive board's ROM are not loaded: nothing here would
 * run them.
 */
#ifndef PROFILES_DAYTONA_H
#define PROFILES_DAYTONA_H

#include "game_profile.h"
#include "rom_recipe.h"
#include "memory.h"
#include "model2o.h"
#include "model1io.h"
#include "sound.h"
#include "m2comm.h"
#include "i960.h"
#include "io_5649.h"
#include "savestate_reg.h"

/* The chips every build but the 1993 one shares: the data ROMs but the pair
 * at 0x800000, the TGP's course data, the polygons, the textures and the
 * sound board. */
#define DAYTONA_DATA_LO \
    ROM_PAIR32(ROM_MAIN_DATA,  0x000000, "mpr-16528.10",   0x9ce591f6, "mpr-16529.11",   0xf7095eaf)
#define DAYTONA_DATA8(lo, lo_crc, hi, hi_crc) \
    ROM_PAIR32(ROM_MAIN_DATA,  0x800000, lo, lo_crc, hi, hi_crc), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0x900000, 0x100000), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xa00000, 0x100000), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xb00000, 0x100000), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xc00000, 0x100000), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xd00000, 0x100000), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xe00000, 0x100000), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xf00000, 0x100000)
/* Course collision and height, read by the TGP (as Sega Rally's). */
#define DAYTONA_COPRO_POLY \
    ROM_PAIR32(ROM_COPRO_DATA, 0x000000, "mpr-16537.ic28", 0x36b7c35a, "mpr-16536.ic29", 0x6d6afed9), \
    ROM_PAIR32(ROM_POLYGONS,   0x000000, "mpr-16523.ic16", 0x2f484d42, "mpr-16518.ic20", 0xdf683bf7), \
    ROM_PAIR32(ROM_POLYGONS,   0x400000, "mpr-16524.ic17", 0x34658bd7, "mpr-16519.ic21", 0xfacd1c81), \
    ROM_PAIR32(ROM_POLYGONS,   0x800000, "mpr-16525.ic18", 0xfb517521, "mpr-16520.ic22", 0xd66bd9bd)
#define DAYTONA_TEX0 \
    ROM_PAIR32(ROM_TEXTURES,   0x000000, "mpr-16522.25",   0x55d39a57, "mpr-16521.24",   0xaf1934fb)
/* Model 1 sound board: MultiPCM 1 at 0, MultiPCM 2 at 0x400000 of `samples`;
 * then the TGP's tables (MAME ROM_REGION32_LE "copro_tgp_tables") and the
 * Model 1 I/O board's program, MAME's model1io.zip (BIOS epr14869c). */
#define DAYTONA_SAMPLES_ON \
    ROM_BYTES(ROM_SAMPLES,     0x000000, "mpr-16491.32",   0x89920903), \
    ROM_BYTES(ROM_SAMPLES,     0x200000, "mpr-16492.33",   0x459e701b), \
    ROM_BYTES(ROM_SAMPLES,     0x400000, "mpr-16493.4",    0x9990db15), \
    ROM_BYTES(ROM_SAMPLES,     0x600000, "mpr-16494.5",    0x600e1d6c), \
    ROM_PAIR32(ROM_TGP_TABLES, 0x000000, "opr-14742a.45",  0x90c6b117, "opr-14743a.46",  0xae7f446b), \
    ROM_BYTES(ROM_IOCPU,       0x000000, "epr-14869c.25",  0x24b68e64)
#define DAYTONA_COMMON(d8lo, d8lo_crc, d8hi, d8hi_crc) \
    DAYTONA_DATA_LO, \
    ROM_PAIR32(ROM_MAIN_DATA,  0x400000, "mpr-16808.8",    0x44f1f5a0, "mpr-16809.9",    0x37a2dd12), \
    DAYTONA_DATA8(d8lo, d8lo_crc, d8hi, d8hi_crc), \
    DAYTONA_COPRO_POLY, \
    ROM_PAIR32(ROM_POLYGONS,   0xc00000, "mpr-16772.ic19", 0x770ed912, "mpr-16771.ic23", 0xa2205124), \
    DAYTONA_TEX0, \
    ROM_PAIR32(ROM_TEXTURES,   0x800000, "mpr-16770.27",   0xf9fa7bfb, "mpr-16769.26",   0xe57429e9), \
    ROM_SWAP16(ROM_AUDIOCPU,   0x000000, "epr-16720.7",    0x8e73cffd), \
    ROM_SWAP16(ROM_AUDIOCPU,   0x020000, "epr-16721.8",    0x1bb3b7b7), \
    DAYTONA_SAMPLES_ON
#define DAYTONA_COMMON_REV_A DAYTONA_COMMON("epr-16724a.6", 0x469f10fd, "epr-16725a.7", 0xba0df8db)
#define DAYTONA_PROGRAM(lo, lo_crc, hi, hi_crc) \
    ROM_PAIR32(ROM_MAINCPU,    0x000000, lo, lo_crc, hi, hi_crc)

static const rom_part_t daytona_parts[] = {
    DAYTONA_PROGRAM("epr-16722a.12", 0x48b94318, "epr-16723a.13", 0x8af8b32d),
    DAYTONA_COMMON_REV_A,
};

/* The clones (Pinboard #1286), MAME's ROM_START lines. Only the program and
 * the data pair at 0x800000 change, but in the 1993 build. */
static const rom_part_t daytonase_parts[] = {
    DAYTONA_PROGRAM("epr-17369a.12", 0x3bc6ca62, "epr-17370a.13", 0x5d1c74e4),
    DAYTONA_COMMON("epr-17371.6", 0x7478f0d2, "epr-17372.7", 0x308a06a9),
};
static const rom_part_t daytonas_parts[] = {
    DAYTONA_PROGRAM("epr-17965.ic12", 0xf022b3da, "epr-17966.ic13", 0xf9e4ece5),
    DAYTONA_COMMON("epr-17967.ic6", 0xa94d8690, "epr-17968.ic7", 0x9d5a92c6),
};
static const rom_part_t daytonat_parts[] = {
    DAYTONA_PROGRAM("turbo1.12", 0x4b41a341, "turbo2.13", 0x6ca580fa),
    DAYTONA_COMMON_REV_A,
};
static const rom_part_t daytonata_parts[] = {
    DAYTONA_PROGRAM("dayturbo.12", 0xaec6857a, "dayturbo.13", 0xcb657edc),
    DAYTONA_COMMON_REV_A,
};
static const rom_part_t daytonagtx_parts[] = {
    DAYTONA_PROGRAM("gtx.12", 0x08283a6f, "gtx.13", 0xf9b356ae),
    DAYTONA_COMMON_REV_A,
};
/* The 1993 build: its own data pair (twice as long, the second half
 * mirrored), fourth polygon pair, second texture pair and sound program. */
static const rom_part_t daytona93_parts[] = {
    DAYTONA_PROGRAM("epr-16530a.12", 0x39e962b5, "epr-16531a.13", 0x693126eb),
    DAYTONA_DATA_LO,
    ROM_PAIR32(ROM_MAIN_DATA,  0x400000, "mpr-16526.8",    0x5273b8b5, "mpr-16527.9",    0xfc4cb0ef),
    ROM_PAIR32(ROM_MAIN_DATA,  0x800000, "epr-16534a.6",   0x1bb0d72d, "epr-16535a.7",   0x459a8bfb),
    ROM_MIRROR(ROM_MAIN_DATA,  0x900000, 0xa00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x900000, 0xb00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x900000, 0xc00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x900000, 0xd00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x900000, 0xe00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x900000, 0xf00000, 0x100000),
    DAYTONA_COPRO_POLY,
    ROM_PAIR32(ROM_POLYGONS,   0xc00000, "epr-16646.ic19", 0x7ba9fd6b, "epr-16645.ic23", 0x78fe0b8a),
    DAYTONA_TEX0,
    ROM_PAIR32(ROM_TEXTURES,   0x800000, "mpr-16517.27",   0x4705d3dd, "mpr-16516.26",   0xa260d45d),
    ROM_SWAP16(ROM_AUDIOCPU,   0x000000, "epr-16489.7",    0xc20e543e),
    ROM_SWAP16(ROM_AUDIOCPU,   0x020000, "epr-16490.8",    0xc24edaab),
    DAYTONA_SAMPLES_ON,
};

/* A clone's parent is daytona.zip, so the I/O board's program comes out of
 * model1io.zip beside it (`bios`), as MAME finds it. */
#define DAYTONA_RECIPE(name, bios_zip) \
    static const rom_recipe_t name##_recipe = { \
        .set  = #name, \
        .size = { [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, \
                  [ROM_COPRO_DATA] = 0x800000, \
                  [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x1000000, \
                  [ROM_AUDIOCPU] = 0xc0000, [ROM_SAMPLES] = 0x800000, \
                  [ROM_TGP_TABLES] = 0x40000, [ROM_IOCPU] = 0x10000 }, \
        .parts = name##_parts, .count = sizeof name##_parts / sizeof name##_parts[0], \
        .bios = bios_zip, \
    }; \
    static inline int name##_load(romset_t *rs, const char *child, const char *parent) { \
        return rom_recipe_load(rs, child, parent, &name##_recipe); \
    }
DAYTONA_RECIPE(daytona, NULL)
DAYTONA_RECIPE(daytona93, "model1io.zip")
DAYTONA_RECIPE(daytonase, "model1io.zip")
DAYTONA_RECIPE(daytonas, "model1io.zip")
DAYTONA_RECIPE(daytonat, "model1io.zip")
DAYTONA_RECIPE(daytonata, "model1io.zip")
DAYTONA_RECIPE(daytonagtx, "model1io.zip")
#undef DAYTONA_RECIPE

/* The settings block (Pinboard #1035): the test menu's GAME ASSIGNMENTS, 128
 * bytes the boot (ROM 0x28A60) takes from the first copy that is valid -- the
 * I/O board's EEPROM, read into backup RAM at 0x1D00000, then backup RAM's own
 * at 0x1D00080 -- and else from the ROM's factory block at 0x39D60. A copy is
 * valid when its first 8 bytes are the factory block's ("SEGA" ...) and the
 * u16 at +8 is the CRC (ROM 0x292D8: table at 0x39DE0, seed 0) of +0x0A..+0x7F.
 * The byte at +0x0B is LINK ID: 0 SINGLE, 1 MASTER, 2 SLAVE; +0x7C counts the
 * boots.
 *
 * The factory value is MASTER: this is the twin cabinet's set, and a board alone
 * waits for its other half, prints CANCELLED and reboots (ROM 0x1B5F4), forever.
 * An operator sets SINGLE in the test menu; a blank board here gets the same
 * block that leaves: the factory one, SINGLE, with its CRC. A valid block in
 * the player's backup RAM is always left alone, MASTER included: the game
 * writes the factory settings with a good CRC both when it boots blank and when
 * the test menu sets MASTER, so the two cannot be told apart (Pinboard #1056).
 * Libretro runs this again after the .srm arrives, so a blank .srm is fixed. */
#define DAYTONA_SETTINGS       0x80u
#define DAYTONA_FACTORY        0x39D60u
#define DAYTONA_CRC_TABLE      0x39DE0u
#define DAYTONA_SETTINGS_LINK  0x0Bu
#define DAYTONA_SETTINGS_BOOTS 0x7Cu

/* Where a build keeps its factory block; the CRC table follows it at +0x80
 * (in every build here: found by the boot's `ldl 0x2xxxxx` of the block
 * through the 0x220000 mirror). The Special Edition and the Saturn-ads build
 * moved them; the hacks kept Revision A's. 0: leave backup RAM alone (the
 * 1993 build, whose factory block is SINGLE already and kept at 0x1D00000,
 * ROM 0x27CF0). */
typedef struct {
    uint32_t factory;
} daytona_build_t;

static inline uint16_t daytona_settings_crc(const uint8_t *rom, uint32_t table, const uint8_t *p, uint32_t n) {
    uint32_t c = 0;
    while (n--) {
        uint32_t i = ((c >> 8) ^ *p++) & 0xFFu;
        c = (c << 8) ^ ((uint32_t)rom[table + i * 2u] | (uint32_t)rom[table + i * 2u + 1u] << 8);
    }
    return (uint16_t)c;
}

static inline void daytona_settings_single(memory_bus_t *bus, const uint8_t *rom, size_t rom_size,
                                           uint32_t factory) {
    uint32_t table = factory + 0x80u;
    if (!factory || !bus->back || rom_size < table + 0x200u) return;
    uint8_t *b = bus->back + DAYTONA_SETTINGS;
    if (memcmp(b, rom + factory, 8) == 0
        && (uint16_t)(b[8] | b[9] << 8) == daytona_settings_crc(rom, table, b + 0x0A, 0x76)) return;
    memcpy(b, rom + factory, 0x80);
    b[DAYTONA_SETTINGS_LINK] = 0;
    uint16_t crc = daytona_settings_crc(rom, table, b + 0x0A, 0x76);
    b[8] = (uint8_t)crc;
    b[9] = (uint8_t)(crc >> 8);
}

/* The colour tables by cabinet (Pinboard #1056). The settings' +0x1A is CABINET
 * (0 DELUXE, 1 TWIN, 2 UPLIGHT; the factory block's is TWIN), and the boot
 * (ROM 0xA74, once: nothing else writes 0x01810000-0x0181BFFF) builds the three
 * channels' tables from it, the same 32 rows of 256 halfwords each: for colour
 * c (0..31), entry j < 64 is (c * j * step + base) >> 6, no base where c * j is
 * 0, and 0xFFFF where that passes 255; the 192 after it are (step * c + top) >> 1,
 * 0 for c = 0. Twin and Uplight take step 10, base 0x1920, top 0xC9; Deluxe,
 * whose projector is darker in the shadows, 12, 0x1160 and 0x8B. The Picture
 * menu shows either live (core/cabinet_picture.h); the board keeps its own. */
static const game_cabinet_t daytona_cabinets[] = {
    { "twin",   "Twin" },
    { "deluxe", "Deluxe" },
};

static inline void daytona_cabinet_xlat(int which, uint8_t *xlat) {
    const bool dx = which == 1;
    const uint32_t step = dx ? 12u : 10u, end = dx ? 0x300u : 0x280u;
    const uint32_t base = dx ? 0x1160u : 0x1920u, top = dx ? 0x8Bu : 0xC9u;
    uint32_t at = 0;
    for (uint32_t c = 0; c < 32; c++) {
        for (uint32_t j = 0; j < end; j += step) {
            uint32_t v = c * j;
            if (v) v += base;
            v >>= 6;
            if (v >= 256) v = 0xFFFFu;
            for (uint32_t ch = 0; ch < 3; ch++) {
                xlat[ch * 0x4000u + at]      = (uint8_t)v;
                xlat[ch * 0x4000u + at + 1u] = (uint8_t)(v >> 8);
            }
            at += 2;
        }
        const uint32_t flat = c ? (step * c + top) >> 1 : 0;
        for (uint32_t k = 0; k < 192; k++, at += 2)
            for (uint32_t ch = 0; ch < 3; ch++) {
                xlat[ch * 0x4000u + at]      = (uint8_t)flat;
                xlat[ch * 0x4000u + at + 1u] = 0;
            }
    }
}

/* The original Model 2 (model2o.h) with the Model 1 I/O board (model1io.h). */
static inline void daytona_install_build(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus,
                                         const daytona_build_t *k) {
    if (!rs->loaded) { LOG_ERROR("daytona_install: romset not loaded"); return; }

    model2o_install_bus(rs, cpu, bus);
    daytona_settings_single(bus, rs->maincpu, rs->maincpu_size, k->factory);
    model1io_install(bus, rs->iocpu, rs->iocpu_size);
    model2o_install_finish(rs, cpu, bus);
    io5649_reset();
    savestate_extra("daytona", "IO5649", &g_io5649, sizeof g_io5649);
    savestate_extra("daytona", "MODEL1IO", &g_model1io, sizeof g_model1io);
    savestate_extra("daytona", "M2COMM", &g_m2comm, sizeof g_m2comm);
    savestate_extra("daytona", "TGP", &g_tgp, sizeof g_tgp);
    savestate_extra("daytona", "I960FRAC", &g_i960_cyc_frac, sizeof g_i960_cyc_frac);
    savestate_extra("daytona", "I960BT", &g_i960_bt, sizeof g_i960_bt);
    savestate_extra("daytona", "M1SOUND", &g_sound.m1, sizeof g_sound.m1);
}

/* name##_backup is the profile's backup_fn: libretro runs it again once the
 * .srm has arrived (Pinboard #1056). */
#define DAYTONA_INSTALL(name, factory_at) \
    static inline void name##_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) { \
        static const daytona_build_t k = { factory_at }; \
        daytona_install_build(rs, cpu, bus, &k); \
    } \
    static inline void name##_backup(const romset_t *rs, memory_bus_t *bus) { \
        if (rs->maincpu) daytona_settings_single(bus, rs->maincpu, rs->maincpu_size, factory_at); \
    }
DAYTONA_INSTALL(daytona,    DAYTONA_FACTORY)
DAYTONA_INSTALL(daytona93,  0)
DAYTONA_INSTALL(daytonase,  0x3AB20u)
DAYTONA_INSTALL(daytonas,   0x3C570u)
DAYTONA_INSTALL(daytonat,   DAYTONA_FACTORY)
DAYTONA_INSTALL(daytonata,  DAYTONA_FACTORY)
DAYTONA_INSTALL(daytonagtx_base, DAYTONA_FACTORY)
#undef DAYTONA_INSTALL

/* GTX 2004's banked data (MAME model2o_gtx_state::gtx_r): a byte read of
 * 0x02C00000-0x02CFFFFF reads prot_data at bank * 0x100000 + offset, where
 * reading offset 0xFFFFC picks bank 2 (all 0xFF), 0xFF00C or 0xF0003 bank 0
 * and 0xFF000 bank 1, each before the byte is read. bank0.bin and bank1.bin
 * are the high half of each word (ROM_LOAD32_WORD at +2), the rest 0xFF. */
#define DAYTONA_GTX_SIZE 0x300000u
static uint8_t *g_daytona_gtx_prot;
static uint8_t  g_daytona_gtx_bank;

static inline int daytonagtx_load_all(romset_t *rs, const char *child, const char *parent) {
    if (daytonagtx_load(rs, child, parent) != 0) return -1;
    if (!g_daytona_gtx_prot && !(g_daytona_gtx_prot = (uint8_t *)malloc(DAYTONA_GTX_SIZE))) return -1;
    memset(g_daytona_gtx_prot, 0xFF, DAYTONA_GTX_SIZE);
    static const struct { const char *name; uint32_t crc, at; } banks[] = {
        { "bank0.bin", 0x21b603b4u, 0x000002u }, { "bank1.bin", 0xc1971f23u, 0x100002u },
    };
    for (size_t b = 0; b < 2; b++) {
        size_t n = 0;
        uint8_t *d = rom_recipe_extract(&daytonagtx_recipe, child, parent, banks[b].name, &n, banks[b].crc);
        if (!d) { LOG_ERROR("daytonagtx: ROM set load failed"); romset_free(rs); return -1; }
        for (size_t i = 0; i + 1 < n && banks[b].at + i * 2 + 1 < DAYTONA_GTX_SIZE; i += 2) {
            g_daytona_gtx_prot[banks[b].at + i * 2]     = d[i];
            g_daytona_gtx_prot[banks[b].at + i * 2 + 1] = d[i + 1];
        }
        free(d);
    }
    return 0;
}

static uint32_t daytona_gtx_read_cb(mem_region_t *r, uint32_t addr, int size) {
    uint32_t v = 0;
    for (int k = 0; k < size; k++) {
        uint32_t off = (addr - r->base + (uint32_t)k) & 0xFFFFFu;
        if (off == 0xFFFFCu) g_daytona_gtx_bank = 2;
        else if (off == 0xFF00Cu || off == 0xF0003u) g_daytona_gtx_bank = 0;
        else if (off == 0xFF000u) g_daytona_gtx_bank = 1;
        v |= (uint32_t)g_daytona_gtx_prot[g_daytona_gtx_bank * 0x100000u + off] << (8 * k);
    }
    return v;
}

static inline void daytonagtx_backup(const romset_t *rs, memory_bus_t *bus) { daytonagtx_base_backup(rs, bus); }

static inline void daytonagtx_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    daytonagtx_base_install(rs, cpu, bus);
    if (!rs->loaded || !g_daytona_gtx_prot) return;
    g_daytona_gtx_bank = 0;
    /* Ahead of MAIN_DATA, which spans it. */
    mem_region_t *r = mem_add_region_first(bus, "GTX_PROT", 0x02C00000u, 0x100000u, NULL, 1);
    if (r) {
        r->read_cb  = daytona_gtx_read_cb;
        r->write_cb = mem_ignore_write_cb;
        mem_regions_changed(bus);
    }
    savestate_extra("daytonagtx", "GTXBANK", &g_daytona_gtx_bank, sizeof g_daytona_gtx_bank);
}

/* Byte 0 is IN0 as the I/O board reads it (MAME INPUT_PORTS_START(daytona)):
 * COIN1 0x01, COIN2 0x02, TEST 0x04, SERVICE 0x08, START 0x10 and the
 * view buttons VR1-3 0x20-0x80. Byte 1 is io_5649.h's wheel, pedals and
 * lever (quirks.wheel), and VR4. VR3 and VR4 take P2's B3/B4: P1's
 * four buttons are the pedals and VR1-2. */
#define DAYTONA_INPUT { .bits = { \
    [GAME_INPUT_P1_COIN]  = 0x00000001, \
    [GAME_INPUT_TEST]     = 0x00000004, \
    [GAME_INPUT_SERVICE]  = 0x00000008, \
    [GAME_INPUT_P1_START] = 0x00000010, \
    [GAME_INPUT_P1_B3]    = 0x00000020, \
    [GAME_INPUT_P1_B4]    = 0x00000040, \
    [GAME_INPUT_P2_B3]    = 0x00000080, \
    [GAME_INPUT_P1_B1]    = 0x00000100, \
    [GAME_INPUT_P1_B2]    = 0x00000200, \
    [GAME_INPUT_P2_B4]    = 0x00000400, \
    [GAME_INPUT_P1_DOWN]  = 0x00001000, \
    [GAME_INPUT_P1_UP]    = 0x00002000, \
    [GAME_INPUT_P1_RIGHT] = 0x00004000, \
    [GAME_INPUT_P1_LEFT]  = 0x00008000, \
    /* A gear's own button (io_5649.h): bits no port reads. */ \
    [GAME_INPUT_GEAR_N]   = 0x00010000, \
    [GAME_INPUT_GEAR_1]   = 0x00020000, \
    [GAME_INPUT_GEAR_2]   = 0x00040000, \
    [GAME_INPUT_GEAR_3]   = 0x00080000, \
    [GAME_INPUT_GEAR_4]   = 0x00100000, \
} }

/* noclip games.js `daytona` (Rev A): 3190 entries of 20 bytes (the explorer
 * also reads oba/tpa/tha there; 16 bytes on STF), palette at 0x84F4EC. The
 * polygon decoder was confirmed on this set first. Every build but the 1993
 * one has Revision A's table (games.js DAYTONA_REV_A). */
#define DAYTONA_QUIRKS(table, count) { \
    .irq_vectors       = true, \
    .enable_68k_sound  = true, \
    .model1_sound      = true, \
    .board_vblank      = true, \
    .i960_board_timing = true, \
    .model1_io         = true, \
    .model1_io_vr4     = 0x0400, \
    .region            = { .kind = REGION_TODO, \
                           .why  = "country setting not looked for yet" }, \
    .wheel = { .left = 0x8000, .right = 0x4000, .accel = 0x0100, .brake = 0x0200, \
               .shift_up = 0x2000, .shift_down = 0x1000 }, \
    .model_table_offset = table, \
    .model_table_count  = count, \
    .model_layout       = MODEL_LAYOUT_DAYTONA, \
    .mesh_ptr_subtract  = 0x02000010, \
    .mesh_ptr_add       = 0x10, \
}

/* The car's meters (Pinboard #1361), found in RAM against the HUD on Rev A:
 *   0x501220  the player's car (0x505180 in a race; the next cars follow at
 *             0x300 apart), a pointer into work RAM
 *   car+0x18  speed, int32 km/h: the HUD's number (299 on the rolling start)
 *   car+0x14C gear, byte 0-3 = 1st-4th (0 too while the start drives itself;
 *             AT changes it as MT does)
 *   car+0x154 the engine, int16 rpm: 8500 at the limiter (the tacho reads to
 *             10); about 23 rpm per km/h in 4th, 30 in 3rd
 * Attract's demo race fills them too. Not checked on the clones. */
#define DAYTONA_CAR_PTR 0x00501220u

static void daytona_dash(memory_bus_t *bus, game_dash_t *out) {
    uint32_t car = mem_peek32(bus, DAYTONA_CAR_PTR);
    if (car < 0x00500000u || car > 0x0051FE00u) return;
    out->on      = true;
    out->speed   = (int)mem_peek32(bus, car + 0x18);
    out->gear    = (int)mem_peek8(bus, car + 0x14C) + 1;
    out->rpm     = (int)(int16_t)mem_peek16(bus, car + 0x154);
    out->rpm_lo  = 5000;
    out->rpm_max = 8500;
    if (out->speed < 0 || out->speed > 999) out->speed = -1;
    if (out->gear > 4) out->gear = -1;
}

static const game_profile_t daytona_profile = {
    .id           = "daytona",
    .display_name = "Daytona USA (Japan, Rev A)",
    .parent_zip_name = "model1io.zip",   /* the I/O board's program, as MAME finds it */
    .board        = BOARD_MODEL2,
    .load_fn      = daytona_load,
    .install_fn   = daytona_install,
    .backup_fn    = daytona_backup,
    .cabinets        = daytona_cabinets,
    .cabinet_count   = 2,
    .cabinet_xlat_fn = daytona_cabinet_xlat,
    .dash         = daytona_dash,
    .input        = DAYTONA_INPUT,
    .quirks       = DAYTONA_QUIRKS(0x83fbb4, 3190),
};

/* The clones (Pinboard #1286). Each is daytona.zip's clone in MAME, so a
 * split set finds the shared chips there and the I/O board's in model1io.zip;
 * a merged daytona.zip or a full clone zip has them itself. The game needs no
 * hooks, so a clone takes only its own chips, factory block and model table.
 * MAME marks the hacks MACHINE_NOT_WORKING. To The MAXX (daytonam) is
 * left out: its PIC check passes, but the program then jumps to 0xFFFFFFFB. */
#define DAYTONA_CLONE(name, title, load, table, count) \
    static const game_profile_t name##_profile = { \
        .id = #name, .display_name = title, .parent_zip_name = "daytona.zip", \
        .board = BOARD_MODEL2, .load_fn = load, .install_fn = name##_install, \
        .backup_fn = name##_backup, \
        .input = DAYTONA_INPUT, .quirks = DAYTONA_QUIRKS(table, count), \
    };
/* noclip games.js `daytona93`: its own table, 2822 entries. */
DAYTONA_CLONE(daytona93,  "Daytona USA (1993)",                        daytona93_load,  0x887928, 2822)
DAYTONA_CLONE(daytonase,  "Daytona USA Special Edition (Rev A)",        daytonase_load,  0x83fbb4, 3190)
DAYTONA_CLONE(daytonas,   "Daytona USA (with Saturn adverts)",         daytonas_load,   0x83fbb4, 3190)
DAYTONA_CLONE(daytonat,   "Daytona USA (Turbo hack, set 1)",           daytonat_load,   0x83fbb4, 3190)
DAYTONA_CLONE(daytonata,  "Daytona USA (Turbo hack, set 2)",           daytonata_load,  0x83fbb4, 3190)
DAYTONA_CLONE(daytonagtx, "Daytona USA (GTX 2004 Edition)",            daytonagtx_load_all, 0x83fbb4, 3190)
#undef DAYTONA_CLONE

#endif /* PROFILES_DAYTONA_H */
