/*
 * dc_link.h -- the Dreamcast build held against MAME over its serial port
 * (Pinboard #461). Built with `make LINK=1`.
 *
 * The board plays attract's Sonic vs Bean replay fight, the one
 * tools/match-replay.mjs holds the desktop build to, and at every game frame
 * edge sends what the fighters are over the SCIF at 1.56 Mbaud. Flycast's
 * libretro core carries the port to a TCP socket when FLYCAST_SCIF names one
 * (dreamcast/tools/flycast-scif.patch), and tools/dc-lockstep.py, on the other
 * end, runs MAME's tools/mame/match-replay.lua beside it and names the first
 * frame and field where the two boards part.
 *
 * The whole of both work structures is 26 KB, ~170 ms of line a frame, so a
 * frame record (DC_LINK_REC) carries the fight state (+0..0x1F8, what
 * match-replay.mjs grades first) raw and a CRC-32 per 0x100 bytes of the rest
 * and of the coprocessor's bufferram the i960 reads back. After each record
 * the Dreamcast waits for the host's word on it:
 *
 *   'G'  go on;
 *   'F'  send the frame whole (both work structures, then the bufferram), then
 *        wait again: the host asks for it at the first frame that differs;
 *   'Q'  stop sending and run on.
 *
 * So the Dreamcast never runs past a frame the host has not seen, and stops
 * where it parted from MAME.
 *
 * Answered 'B' instead of 'G', the link holds the board from power-on (Pinboard
 * #478): no replay jump, the warning screen kept (MAME's boot, as det_digest
 * --nowarnskip), and at every game frame edge a 'B' record of a CRC-32 per
 * DC_LINK_BBLOCK of the ranges in dc_link_boot (work RAM, RAM, bufferram, tile
 * RAM, the palette), the ranges tools/mame/boot-lockstep.lua and det_digest
 * --raw dump. After it the host's word is 'G', 'Q', or 'D' + address + length
 * (u32 each), which sends that memory raw in a 'D' record and waits again: the
 * host asks for the blocks whose CRC differs. With nobody on the line (no FLYCAST_SCIF, a plain
 * emulator or a real console with no cable), the hello goes unanswered for
 * DC_LINK_WAIT_MS and the board runs on unlinked.
 *
 * The profile is the arcade `sfight` and the region Japan, as match-replay's:
 * the console profile's traps play the replay another way (CLAUDE.md, "STF has
 * two profiles"). Gems is off unless built with LINK_GEMS=1, so the first thing
 * held is the port's own i960 and COP. The sound trap stays: it is the port.
 */
#ifndef DC_LINK_H
#define DC_LINK_H

#include <dc/scif.h>

#ifndef DC_LINK_WAIT_MS
#define DC_LINK_WAIT_MS 3000u
#endif

#define DC_LINK_BAUD   1562500   /* SCBRR2 0: P0 / 32 */
#define DC_LINK_ROB0   0x510D00u
#define DC_LINK_ROB1   0x514100u
#define DC_LINK_ROB    0x3400u
#define DC_LINK_FIGHT  0x1F8u    /* the fight state, sent raw */
#define DC_LINK_BLOCK  0x100u

static const struct { uint32_t addr, len; } dc_link_extra[] = {
    { 0x90E800u, 0x800u },       /* match-replay.mjs's MR_EXTRA: bufferram the i960 reads */
    { 0x90F600u, 0x100u },
};

/* The boot mode's ranges, in the host's order (tools/dc-lockstep.py BOOT_RANGES). */
#define DC_LINK_BBLOCK 0x1000u
static const struct { uint32_t addr, len; } dc_link_boot[] = {
    { 0x500000u, 0x100000u },    /* work RAM */
    { 0x200000u, 0x40000u },     /* RAM */
    { 0x900000u, 0x20000u },     /* bufferram */
    { 0x1000000u, 0x10000u },    /* tile RAM */
    { 0x1800000u, 0x4000u },     /* palette */
};

static struct {
    int      on;                 /* 1: a host answered the hello */
    int      boot;               /* 1: answered 'B', held from power-on */
    int      seen_jump;
    uint32_t frames, crc[256];
    memory_bus_t *bus;
} g_link;

static void dc_link_crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_link.crc[i] = c;
    }
}

static uint32_t dc_link_crc(uint32_t c, uint8_t b) {
    return g_link.crc[(c ^ b) & 0xFF] ^ (c >> 8);
}

static void dc_link_put(const void *p, size_t n) {
    const uint8_t *s = p;
    while (n--) scif_write(*s++);
}

static void dc_link_u32(uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    dc_link_put(b, 4);
}

/* The bus's bytes, in memory order (MAME's read_range). */
static void dc_link_raw(uint32_t a, uint32_t n) {
    for (uint32_t i = 0; i < n; i += 4) dc_link_u32(mem_read32(g_link.bus, a + i));
}

/* CRC-32 (zlib's) of each DC_LINK_BLOCK of [a, a+n), the last one short. */
static void dc_link_crcs(uint32_t a, uint32_t n) {
    for (uint32_t o = 0; o < n; o += DC_LINK_BLOCK) {
        uint32_t c = 0xFFFFFFFFu, end = o + DC_LINK_BLOCK < n ? o + DC_LINK_BLOCK : n;
        for (uint32_t i = o; i < end; i += 4) {
            uint32_t w = mem_read32(g_link.bus, a + i);
            c = dc_link_crc(c, (uint8_t)w);         c = dc_link_crc(c, (uint8_t)(w >> 8));
            c = dc_link_crc(c, (uint8_t)(w >> 16)); c = dc_link_crc(c, (uint8_t)(w >> 24));
        }
        dc_link_u32(~c);
    }
}

/* The host's word: blocks, as the Dreamcast must not run on without it. */
static int dc_link_getc(uint32_t wait_ms) {
    uint64_t t0 = timer_ms_gettime64();
    for (;;) {
        int c = scif_read();
        if (c >= 0) return c;
        if (wait_ms && timer_ms_gettime64() - t0 >= wait_ms) return -1;
    }
}

static void dc_link_header(char kind, uint32_t len) {
    dc_link_put("M2L", 3);
    dc_link_put(&kind, 1);
    dc_link_u32(mem_read32(g_link.bus, 0x500020u));               /* frame_counter */
    uint8_t b[4] = { mem_read8(g_link.bus, 0x500064u), mem_read8(g_link.bus, 0x500030u), 0, 0 };
    dc_link_put(b, 4);                                            /* stage_num, attract step */
    dc_link_u32(g_link.frames);
    dc_link_u32(len);
}

/* The host's word after a record: 'G' on, 'Q' stop, 'F' (replay) or 'D' (boot) answered and waited on. */
static void dc_link_wait(void (*full)(void)) {
    for (;;) {
        int c = dc_link_getc(0);
        if (c == 'G') return;
        if (c == 'Q') { g_link.on = 0; return; }
        if (c == 'F' && full) full();
        if (c == 'D') {
            uint8_t b[8];
            for (int i = 0; i < 8; i++) b[i] = (uint8_t)dc_link_getc(0);
            uint32_t a = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
            uint32_t n = (b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24) & ~3u;
            dc_link_header('D', n + 4);
            dc_link_u32(a);
            dc_link_raw(a, n);
        }
    }
}

/* Boot mode: every game frame edge from the first. */
static void dc_link_boot_frame(void) {
    uint32_t n = 0;
    for (size_t i = 0; i < sizeof dc_link_boot / sizeof dc_link_boot[0]; i++)
        n += (dc_link_boot[i].len + DC_LINK_BBLOCK - 1) / DC_LINK_BBLOCK;
    dc_link_header('B', 4 * n + 4);
    dc_link_u32(mem_read8(g_link.bus, 0x50002Au));                /* mode */
    for (size_t i = 0; i < sizeof dc_link_boot / sizeof dc_link_boot[0]; i++)
        for (uint32_t o = 0; o < dc_link_boot[i].len; o += DC_LINK_BBLOCK) {
            uint32_t c = 0xFFFFFFFFu;
            for (uint32_t k = 0; k < DC_LINK_BBLOCK; k += 4) {
                uint32_t w = mem_read32(g_link.bus, dc_link_boot[i].addr + o + k);
                c = dc_link_crc(c, (uint8_t)w);         c = dc_link_crc(c, (uint8_t)(w >> 8));
                c = dc_link_crc(c, (uint8_t)(w >> 16)); c = dc_link_crc(c, (uint8_t)(w >> 24));
            }
            dc_link_u32(~c);
        }
    g_link.frames++;
    dc_link_wait(NULL);
}

static void dc_link_full(void) {
    const uint32_t robs[2] = { DC_LINK_ROB0, DC_LINK_ROB1 };
    uint32_t len = 2 * DC_LINK_ROB;
    for (size_t i = 0; i < sizeof dc_link_extra / sizeof dc_link_extra[0]; i++) len += dc_link_extra[i].len;
    dc_link_header('F', len);
    for (int r = 0; r < 2; r++) dc_link_raw(robs[r], DC_LINK_ROB);
    for (size_t i = 0; i < sizeof dc_link_extra / sizeof dc_link_extra[0]; i++)
        dc_link_raw(dc_link_extra[i].addr, dc_link_extra[i].len);
}

static void dc_link_frame(memory_bus_t *bus) {
    if (!g_link.on) return;
    g_link.bus = bus;
    if (g_link.boot) { dc_link_boot_frame(); return; }
    if (g_match_replay != 2) return;
    if (!g_link.seen_jump) { g_link.seen_jump = 1; return; }    /* MAME records from the edge after the jump */
    const uint32_t robs[2] = { DC_LINK_ROB0, DC_LINK_ROB1 };
    uint32_t nb = (DC_LINK_ROB - DC_LINK_FIGHT + DC_LINK_BLOCK - 1) / DC_LINK_BLOCK, ne = 0;
    for (size_t i = 0; i < sizeof dc_link_extra / sizeof dc_link_extra[0]; i++)
        ne += (dc_link_extra[i].len + DC_LINK_BLOCK - 1) / DC_LINK_BLOCK;
    dc_link_header('R', 2 * DC_LINK_FIGHT + 4 * (2 * nb + ne));
    for (int r = 0; r < 2; r++) dc_link_raw(robs[r], DC_LINK_FIGHT);
    for (int r = 0; r < 2; r++) dc_link_crcs(robs[r] + DC_LINK_FIGHT, DC_LINK_ROB - DC_LINK_FIGHT);
    for (size_t i = 0; i < sizeof dc_link_extra / sizeof dc_link_extra[0]; i++)
        dc_link_crcs(dc_link_extra[i].addr, dc_link_extra[i].len);
    g_link.frames++;
    dc_link_wait(dc_link_full);
}

/* Before the board's install: the profile and region match-replay plays, and
 * the hello. The answer decides whether the frames go out. */
static void dc_link_init(memory_bus_t *bus, const char *build) {
    g_link.bus = bus;
    dc_link_crc_init();
    g_region = GAME_REGION_JAPAN;
    g_match_replay = 1;
    g_game_frame_edge_cb = dc_link_frame;
    scif_set_parameters(DC_LINK_BAUD, 1);
    scif_init();
    uint32_t n = (uint32_t)strlen(build);
    dc_link_put("M2LH", 4);
    dc_link_u32(0); dc_link_u32(0); dc_link_u32(0);
    dc_link_u32(n);
    dc_link_put(build, n);
    int c = dc_link_getc(DC_LINK_WAIT_MS);
    g_link.on = c == 'G' || c == 'B';
    if (c == 'B') {             /* from power-on, MAME's boot */
        g_link.boot = 1;
        g_match_replay = 0;
        g_warning_skip = 0;
    }
}

#endif
