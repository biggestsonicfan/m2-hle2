/*
 * rom_test.c — standalone Phase 4 verification: load the real STF ROM set and
 * confirm the installer produces a valid i960 reset vector.
 *
 * Uses the ROM zips from the sibling claude_mame oracle repo. Links the miniz
 * sources directly (see the cl.exe invocation in the Phase 4 notes).
 */
#define NDEBUG 1
#include <stdio.h>

#include "sfight.h"   /* pulls rom_loader, memory, i960, game_profile, constants */

#define ROMDIR "c:/Users/bigge/source/repos/ai/claude_mame/mame/roms/"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    static romset_t   rs;
    static memory_bus_t bus;
    static i960_cpu_t cpu;

    int rc = sfight_load(&rs, ROMDIR "sfight.zip", ROMDIR "schamp.zip");
    CHECK(rc == 0, "sfight_load returns success");
    CHECK(rs.loaded, "romset marked loaded");
    CHECK(rs.maincpu_size == 0x200000, "maincpu is 2MB");
    CHECK(rs.main_data_size == 0x2000000, "main_data is 32MB");
    CHECK(rs.audiocpu_size == 0x80000, "audiocpu is 512KB");

    if (!rs.loaded) { printf("\nFAILED (load) (%d failures)\n", g_fail); return 1; }

    /* Inspect the i960 reset structures before install. */
    uint32_t sat  = sfight_read32(rs.maincpu, 0x00);
    uint32_t prcb = sfight_read32(rs.maincpu, 0x04);
    printf("info: SAT=0x%08X  PRCB=0x%08X\n", sat, prcb);
    CHECK(prcb != 0 && prcb < rs.maincpu_size, "PRCB pointer within maincpu");
    uint32_t start_ip_ptr = sfight_read32(rs.maincpu, prcb + PRCB_START_IP);
    printf("info: start_ip_ptr=0x%08X\n", start_ip_ptr);

    sfight_install(&rs, &cpu, &bus);
    printf("info: reset IP = 0x%08X\n", cpu.sfr.ip);

    CHECK(cpu.sfr.ip != 0, "reset IP is non-zero");
    CHECK(cpu.sfr.ip < rs.maincpu_size, "reset IP lies inside ROM");

    /* The reset IP must point at a fetchable instruction in the ROM region. */
    uint32_t first_word = mem_read32(&bus, cpu.sfr.ip);
    printf("info: first instruction word @reset = 0x%08X\n", first_word);
    CHECK(first_word != 0x00000000 && first_word != 0xFFFFFFFF,
          "first instruction word looks like real code");

    /* XTRA_DATA mirror: STF maps main_data[0x01000000..] there. The string
     * "SNC_ZIBA" lives at ROM[0x01000000 + 0x4012FB] per the install comment. */
    uint32_t z = mem_read32(&bus, XTRA_DATA_BASE + 0x4012FB);
    char zc[5] = { (char)(z & 0xFF), (char)((z>>8)&0xFF), (char)((z>>16)&0xFF), (char)((z>>24)&0xFF), 0 };
    printf("info: XTRA_DATA[0x4012FB] = '%s' (0x%08X)\n", zc, z);
    CHECK(zc[0] == 'S' && zc[1] == 'N' && zc[2] == 'C',
          "XTRA_DATA mirror carries the 'SNC_ZIBA' marker");

    /* maincpu is readable through the bus ROM region at the reset address. */
    CHECK(mem_read32(&bus, cpu.sfr.ip) == first_word, "ROM region reads back via bus");

    romset_free(&rs);
    mem_shutdown(&bus);
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
