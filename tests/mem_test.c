/*
 * mem_test.c — standalone smoke test for the Phase 2 memory bus.
 *
 * Exercises region dispatch, the TILE-before-H_SYNC ordering invariant,
 * IO=0xFF idle init, and read/write round-trips across width 8/16/32.
 * Build: see the cl.exe invocation in the Phase 2 notes (not part of the
 * main CMake build).
 */
#define NDEBUG 1
#include <stdio.h>
#include <assert.h>

#include "../src/board/memory.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    static memory_bus_t bus;
    int ok = mem_init(&bus, NULL, 0);
    CHECK(ok, "mem_init succeeds");
    CHECK(bus.region_count > 25, "at least ~31 regions registered");

    /* IO region idles HIGH (0xFF), not 0x00. */
    CHECK(mem_read8(&bus, IO_BASE) == 0xFF, "IO region inits to 0xFF");
    CHECK(mem_read8(&bus, RAM_BASE) == 0x00, "RAM region inits to 0x00");

    /* Region-order invariant: H_SYNC (0x01040000) lies inside TILE's span and
     * must resolve to the TILE region because TILE is declared first. */
    mem_region_t *rt = mem_find_region(&bus, TILE_BASE);
    mem_region_t *rh = mem_find_region(&bus, H_SYNC_BASE);
    CHECK(rt && strcmp(rt->name, "TILE") == 0, "0x01000000 -> TILE");
    CHECK(rh && strcmp(rh->name, "TILE") == 0, "0x01040000 (H_SYNC) routes to TILE");

    /* Round-trips at each width in work RAM. */
    mem_write8(&bus, RAM_BASE + 0x10, 0xAB);
    CHECK(mem_read8(&bus, RAM_BASE + 0x10) == 0xAB, "write8/read8 round-trip");

    mem_write16(&bus, RAM_BASE + 0x20, 0x1234);
    CHECK(mem_read16(&bus, RAM_BASE + 0x20) == 0x1234, "write16/read16 round-trip");
    /* Little-endian byte order. */
    CHECK(mem_read8(&bus, RAM_BASE + 0x20) == 0x34, "write16 is little-endian (low byte first)");

    mem_write32(&bus, RAM_BASE + 0x30, 0xDEADBEEF);
    CHECK(mem_read32(&bus, RAM_BASE + 0x30) == 0xDEADBEEF, "write32/read32 round-trip");
    CHECK(mem_read8(&bus, RAM_BASE + 0x33) == 0xDE, "write32 is little-endian (high byte last)");

    /* Heap region round-trip (MAIN_DATA). */
    mem_write32(&bus, MAIN_DATA_BASE + 0x4000, 0xCAFEF00D);
    CHECK(mem_read32(&bus, MAIN_DATA_BASE + 0x4000) == 0xCAFEF00D, "MAIN_DATA round-trip");

    /* TEXRAM mirror aliases the same buffer. */
    mem_write32(&bus, TEXRAM0_BASE + 0x100, 0x11223344);
    CHECK(mem_read32(&bus, TEXRAM0_MIRROR_BASE + 0x100) == 0x11223344, "TEXRAM0 mirror aliases base");

    /* ROM is read-only: write is ignored (data is NULL here so it reads 0). */
    uint64_t w_before = bus.writes;
    mem_write32(&bus, ROM_BASE + 0x0, 0x55555555);
    CHECK(bus.writes == w_before + 1, "RO write counted but not applied");

    /* Unmapped access bumps the unmapped counters (gap above IAC). */
    uint64_t um_before = bus.unmapped_reads;
    (void)mem_read32(&bus, 0xFFFFFFF0);
    CHECK(bus.unmapped_reads == um_before + 1, "unmapped read tracked");

    mem_shutdown(&bus);

    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
