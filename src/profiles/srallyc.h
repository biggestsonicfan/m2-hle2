/*
 * profiles/srallyc.h — Sega Rally Championship (srallyc, Revision C).
 *
 * The first Model 2A profile: the coprocessor is a TGP running the program the
 * i960 uploads at boot (board/tgp.h, board/model2a.h), not the SHARC.
 * ROM names and CRCs are MAME's (src/mame/sega/model2.cpp, ROM_START(srallyc));
 * the files are found by CRC, so a set that names them differently loads too.
 *
 * Status: boots the i960 and the TGP. No sound (the 2A UART is not wired yet),
 * no wheel (the 315-5649 I/O board's analog ports), no hooks.
 */
#ifndef PROFILES_SRALLYC_H
#define PROFILES_SRALLYC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "rom_loader.h"
#include "game_profile.h"
#include "memory.h"
#include "model2a.h"
#include "i960.h"

#define SRALLYC_PAIR_32(dest, dest_sz, lo, crc_lo, hi, crc_hi, half_sz, offset) \
    do { \
        tmp1 = zip_extract_from_set(child, parent, lo, &fsize, crc_lo); \
        tmp2 = zip_extract_from_set(child, parent, hi, &fsize, crc_hi); \
        if (!tmp1 || !tmp2) { ok = 0; goto done; } \
        interleave_32_word(dest, dest_sz, tmp1, half_sz, tmp2, half_sz, offset); \
        free(tmp1); tmp1 = NULL; \
        free(tmp2); tmp2 = NULL; \
    } while (0)

#define SRALLYC_SAMPLE(dest, dest_sz, name, crc, offset) \
    do { \
        tmp1 = zip_extract_from_set(child, parent, name, &fsize, crc); \
        if (!tmp1) { ok = 0; goto done; } \
        load_16_word_swap(dest, dest_sz, tmp1, fsize, offset); \
        free(tmp1); tmp1 = NULL; \
    } while (0)

static inline uint8_t *srallyc_region(size_t size) { return (uint8_t *)calloc(1, size); }

static inline int srallyc_load(romset_t *rs, const char *child, const char *parent) {
    romset_free(rs);
    size_t   fsize = 0;
    uint8_t *tmp1 = NULL, *tmp2 = NULL;
    int      ok = 1;

    rs->maincpu_size = 0x200000;
    rs->maincpu = srallyc_region(rs->maincpu_size);
    SRALLYC_PAIR_32(rs->maincpu, rs->maincpu_size,
        "epr-17888c.12", 0x3d6808aa, "epr-17889c.13", 0xf43c7802, 0x80000, 0x000000);

    rs->main_data_size = 0x2400000;
    rs->main_data = srallyc_region(rs->main_data_size);
    SRALLYC_PAIR_32(rs->main_data, rs->main_data_size,
        "mpr-17746.10", 0x8fe311f4, "mpr-17747.11", 0x543593fd, 0x200000, 0x000000);
    SRALLYC_PAIR_32(rs->main_data, rs->main_data_size,
        "mpr-17744.8",  0x71fed098, "mpr-17745.9",  0x8ecca705, 0x200000, 0x400000);
    SRALLYC_PAIR_32(rs->main_data, rs->main_data_size,
        "mpr-17884.6",  0x4cfc95e1, "mpr-17885.7",  0xa08d2467, 0x200000, 0x800000);

    /* Course collision and height, read by the TGP through its bank window. */
    rs->copro_data_size = 0x800000;
    rs->copro_data = srallyc_region(rs->copro_data_size);
    SRALLYC_PAIR_32(rs->copro_data, rs->copro_data_size,
        "mpr-17754.28", 0x81a84f67, "mpr-17755.29", 0x2a6e7da4, 0x200000, 0x000000);

    rs->polygons_size = 0x1000000;
    rs->polygons = srallyc_region(rs->polygons_size);
    SRALLYC_PAIR_32(rs->polygons, rs->polygons_size,
        "mpr-17748.16", 0x3148a2b2, "mpr-17750.20", 0x232aec29, 0x200000, 0x000000);
    SRALLYC_PAIR_32(rs->polygons, rs->polygons_size,
        "mpr-17749.17", 0x0838d184, "mpr-17751.21", 0xed87ac62, 0x200000, 0x400000);

    rs->textures_size = 0x1000000;
    rs->textures = srallyc_region(rs->textures_size);
    SRALLYC_PAIR_32(rs->textures, rs->textures_size,
        "mpr-17753.25", 0x6db0eb36, "mpr-17752.24", 0xd6aa86ce, 0x200000, 0x000000);

    rs->audiocpu_size = 0x80000;
    rs->audiocpu = srallyc_region(rs->audiocpu_size);
    tmp1 = zip_extract_from_set(child, parent, "epr-17890a.30", &fsize, 0x5bac3fa1);
    if (!tmp1) { ok = 0; goto done; }
    load_16_word_swap(rs->audiocpu, rs->audiocpu_size, tmp1, fsize, 0);
    free(tmp1); tmp1 = NULL;

    rs->samples_size = 0x800000;
    rs->samples = srallyc_region(rs->samples_size);
    SRALLYC_SAMPLE(rs->samples, rs->samples_size, "mpr-17756.31", 0x7725f111, 0x000000);
    SRALLYC_SAMPLE(rs->samples, rs->samples_size, "mpr-17757.32", 0x1616e649, 0x200000);
    SRALLYC_SAMPLE(rs->samples, rs->samples_size, "mpr-17886.36", 0x54a72923, 0x400000);
    SRALLYC_SAMPLE(rs->samples, rs->samples_size, "mpr-17887.37", 0x38c31fdd, 0x600000);

    /* MODEL2_CPU_BOARD: the TGP's sine / atan / 1/x / 1/sqrt(x) tables. */
    rs->tgp_tables_size = 0x40000;
    rs->tgp_tables = srallyc_region(rs->tgp_tables_size);
    SRALLYC_PAIR_32(rs->tgp_tables, rs->tgp_tables_size,
        "opr-14742a.45", 0x90c6b117, "opr-14743a.46", 0xae7f446b, 0x20000, 0x000000);

    rs->loaded = true;
    LOG_INFO("srallyc: ROM set loaded");

done:
    free(tmp1);
    free(tmp2);
    if (!ok) { LOG_ERROR("srallyc: ROM set load failed"); romset_free(rs); return -1; }
    return 0;
}

#undef SRALLYC_PAIR_32
#undef SRALLYC_SAMPLE

static inline uint32_t srallyc_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

static inline void srallyc_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("srallyc_install: romset not loaded"); return; }

    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2a_install(bus, rs->tgp_tables, rs->copro_data, rs->copro_data_size);

    /* main_data at 0x02000000; "extra" data at 0x06000000 is main_data from
     * 0x1000000, which Sega Rally leaves empty (MAME model2_base_mem). */
    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
    }
    if (rs->main_data && bus->xtra_data && rs->main_data_size > 0x1000000u) {
        size_t n = rs->main_data_size - 0x1000000u;
        memcpy(bus->xtra_data, rs->main_data + 0x1000000u, n < XTRA_DATA_SIZE ? n : XTRA_DATA_SIZE);
    }

    uint32_t prcb = srallyc_read32(rs->maincpu, 0x04);
    if (prcb + 0x2C < rs->maincpu_size) {
        uint32_t start_ip_ptr = srallyc_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = srallyc_read32(rs->maincpu, start_ip_ptr);
    }
    LOG_INFO("srallyc_install: PRCB=0x%08X initial IP=0x%08X", prcb, cpu->sfr.ip);
}

static const game_profile_t srallyc_profile = {
    .id              = "srallyc",
    .display_name    = "Sega Rally Championship (Rev C)",
    .parent_zip_name = NULL,
    .board           = BOARD_MODEL2A_CRX,
    .load_fn         = srallyc_load,
    .install_fn      = srallyc_install,
    .hook_count      = 0,
    .quirks = {
        .enable_68k_sound = false,
        .board_vblank     = true,
        .geo_displaylist  = true,
    },
};

#endif /* PROFILES_SRALLYC_H */
