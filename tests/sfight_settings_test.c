/*
 * sfight_settings_test.c -- sfight_apply_menu_settings (profiles/sfight.h), the
 * PS3 port's ARCADE / OFFLINE VERSUS menus applied to STF's GAME ASSIGNMENTS
 * block, on a bare memory bus. No ROM.
 *
 *   sfight_settings_test
 *       Self-test: the block the PS3 would write for its defaults and for a
 *       non-default set, the versus variant leaving the arcade-only bytes alone,
 *       and the checksum against a value the ROM itself computed.
 *
 *   sfight_settings_test --crc <0x20-byte block, hex>
 *       make_crc of a block, to hold against crc_value_bk (0x1D03302) as the
 *       ROM itself computed it on a running board.
 *
 *   sfight_settings_test --apply <0x42-byte block, hex> <s0..s7, hex> <versus>
 *       Runs the function on a bus holding that block (as read over the MCP
 *       bridge from 0x59C340) and prints every write it made, one per line:
 *       "<addr> <hex bytes>". A driver writes them back over the bridge; that is
 *       how the end-to-end check on a running board uses the real function.
 *
 * A ctest (CMakeLists.txt, sfight_settings_test).
 */
#define NDEBUG 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sfight.h"

static memory_bus_t bus;
static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } \
                              else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

/* The factory block (init_game_assignments) as a cold USA boot leaves it at
 * 0x59C340 -- read off a running board (sfight_console, --region usa). */
static void factory_block(uint8_t b[SFIGHT_SETTINGS_LEN]) {
    memset(b, 0, SFIGHT_SETTINGS_LEN);
    b[0x00] = 2; b[0x01] = 2; b[0x02] = 1; b[0x03] = 1; b[0x04] = 1;
    for (int i = 0x05; i <= 0x10; i++) b[i] = 1;
    b[0x11] = 2; b[0x12] = 1; b[0x13] = 0; b[0x18] = 5;
    b[0x14] = 0xA0; b[0x16] = 0xC8;       /* max_energy_vs_array 160, un_stage_width 200 */
    b[0x19] = b[0x1A] = b[0x1B] = 0x16;
    b[0x1C] = b[0x1D] = b[0x1E] = 0x36; b[0x1F] = 0x1F;
}

static void load_block(const uint8_t b[SFIGHT_SETTINGS_LEN]) {
    mem_write32(&bus, SFIGHT_SETTINGS_PTR, 0x00599000u);
    for (uint32_t i = 0; i < SFIGHT_SETTINGS_LEN; i++) {
        mem_write8(&bus, SFIGHT_SETTINGS_WORK + i, b[i]);
        mem_write8(&bus, SFIGHT_SETTINGS_BACKUP + i, b[i]);
    }
}

static void read_block(uint32_t at, uint8_t b[SFIGHT_SETTINGS_LEN]) {
    for (uint32_t i = 0; i < SFIGHT_SETTINGS_LEN; i++) b[i] = (uint8_t)mem_read8(&bus, at + i);
}

static int hexbytes(const char *s, uint8_t *out, size_t n) {
    if (strlen(s) != n * 2) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned v; if (sscanf(s + i * 2, "%2x", &v) != 1) return 0; out[i] = (uint8_t)v;
    }
    return 1;
}

static void print_range(uint32_t at, uint32_t n) {
    printf("%08X ", at);
    for (uint32_t i = 0; i < n; i++) printf("%02X", (unsigned)mem_read8(&bus, at + i));
    printf("\n");
}

int main(int argc, char **argv) {
    if (!mem_init(&bus, NULL, 0)) { printf("mem_init failed\n"); return 1; }

    if (argc == 3 && strcmp(argv[1], "--crc") == 0) {
        uint8_t blk[0x20];
        if (!hexbytes(argv[2], blk, sizeof blk)) { fprintf(stderr, "bad hex\n"); return 2; }
        printf("%04X\n", sfight_make_crc(blk, sizeof blk));
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "--apply") == 0) {
        uint8_t blk[SFIGHT_SETTINGS_LEN], s[8];
        if (!hexbytes(argv[2], blk, sizeof blk) || !hexbytes(argv[3], s, 8)) {
            fprintf(stderr, "bad hex\n"); return 2;
        }
        load_block(blk);
        sfight_apply_menu_settings(&bus, s, atoi(argv[4]));
        print_range(SFIGHT_SETTINGS_BACKUP, SFIGHT_SETTINGS_LEN);
        print_range(SFIGHT_SETTINGS_WORK, SFIGHT_SETTINGS_LEN);
        print_range(SFIGHT_SETTINGS_CRC, 2);
        print_range(SFIGHT_BARRIER_DEFAULT, 4);
        print_range(SFIGHT_ROUND_TIME, 1);
        return 0;
    }

    uint8_t f[SFIGHT_SETTINGS_LEN], w[SFIGHT_SETTINGS_LEN], k[SFIGHT_SETTINGS_LEN];
    factory_block(f);

    /* CRC-16/XMODEM check value, and the ROM's own: after a cold USA boot the
     * board holds 0x1D03302 = the CRC of the factory block's first 0x20 bytes
     * (0x4842, read off a running board over the MCP bridge). */
    CHECK(sfight_make_crc((const uint8_t *)"123456789", 9) == 0x31C3, "make_crc is CRC-16/XMODEM");
    CHECK(sfight_make_crc(f, 0x20) == 0x4842, "make_crc of the factory block = the ROM's crc_value_bk 0x4842");

    /* Arcade defaults: the factory block but for AUTOMATIC (b4) turned on. */
    load_block(f);
    sfight_apply_menu_settings(&bus, (const uint8_t *)&SFIGHT_MENU_DEFAULT_ARCADE, 0);
    read_block(SFIGHT_SETTINGS_WORK, w);
    read_block(SFIGHT_SETTINGS_BACKUP, k);
    CHECK(memcmp(w, k, sizeof w) == 0, "backup and work copies identical");
    CHECK(w[0x00] == 2 && w[0x01] == 2 && w[0x02] == 1 && w[0x03] == 1 && w[0x04] == 1,
          "arcade defaults: rounds 2/2, Normal, energy 1/1");
    CHECK(w[0x11] == 2 && w[0x13] == 0x10 && w[0x18] == 5, "arcade defaults: time idx 2, flags 0x10, 5 barriers");
    CHECK(w[0x12] == 1 && w[0x1F] == 0x1F && w[0x05] == 1, "country, colours and the rest left alone");
    CHECK(mem_read32(&bus, SFIGHT_BARRIER_DEFAULT) == 5 && mem_read8(&bus, SFIGHT_ROUND_TIME) == 30,
          "barrier_default_num 5, time 30");
    CHECK(mem_read16(&bus, SFIGHT_SETTINGS_CRC) == sfight_make_crc(w, 0x20), "crc_value_bk matches block");

    /* Non-default arcade: Hardest, 5 rounds, 10 s, +3, 1 barrier, type D. */
    const uint8_t hard[8] = { 3, 1, 0, 0, 3, 4, 0, 3 };
    load_block(f);
    sfight_apply_menu_settings(&bus, hard, 0);
    read_block(SFIGHT_SETTINGS_WORK, w);
    CHECK(w[0x00] == 5 && w[0x01] == 5 && w[0x02] == 3 && w[0x03] == 3 && w[0x04] == 3,
          "arcade: rounds 5/5, Hardest, energy 3/3");
    CHECK(w[0x11] == 0, "arcade: time index 0 (10 s)");
    CHECK(w[0x13] == (0x80 | 0x40 | 0x10 | 0x08), "arcade: flags REAL damage, hyper off, auto, barrier reset (0x%02X)", w[0x13]);
    CHECK(w[0x18] == 1 && mem_read32(&bus, SFIGHT_BARRIER_DEFAULT) == 1, "arcade: 1 barrier");
    CHECK(mem_read8(&bus, SFIGHT_ROUND_TIME) == 10, "arcade: time 10");

    /* Versus leaves 1P rounds, rank and 1P energy as they were. */
    const uint8_t vs[8] = { 3, 1, 0, 3, 2, 0, 9, 1 };
    load_block(f);
    sfight_apply_menu_settings(&bus, vs, 1);
    read_block(SFIGHT_SETTINGS_WORK, w);
    CHECK(w[0x00] == 2 && w[0x02] == 1 && w[0x03] == 1, "versus: 1P rounds, rank, 1P energy untouched");
    CHECK(w[0x01] == 4 && w[0x04] == 0 && w[0x11] == 9 && w[0x18] == 10, "versus: VS rounds 4, energy 0, 99 s, 10 barriers");
    CHECK(w[0x13] == (0x40 | 0x10), "versus: type B flags 0x50");
    CHECK(mem_read8(&bus, SFIGHT_ROUND_TIME) == 99, "versus: time 99");

    /* Out of range falls back to index 0, as on the PS3. */
    const uint8_t junk[8] = { 9, 0, 0, 7, 7, 7, 77, 7 };
    load_block(f);
    sfight_apply_menu_settings(&bus, junk, 0);
    read_block(SFIGHT_SETTINGS_WORK, w);
    CHECK(w[0x00] == 2 && w[0x02] == 1 && w[0x04] == 0 && w[0x11] == 0 && w[0x18] == 1 && w[0x13] == 0x10,
          "out-of-range indices fall back");

    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
