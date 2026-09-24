/*
 * i960_fuzz.c -- the i960 and its bus running random code, for holding two
 * builds of i960_exec.h / memory.h to the same bits.
 *
 * det_digest holds the game's own code. This runs code the game never would,
 * aimed at the bus's edges: every load and store form (byte, short, word,
 * long, triple, quad, and lda) in every addressing mode, with the base and
 * index registers pointing at the last bytes before a 64 KB page ends, before
 * a region ends, into MMIO (the COP window, the tile and palette RAM, texture
 * RAM, the timers) and into the ROM -- so an access that straddles a page, a
 * region or a callback, or that stores to read-only memory, is taken often.
 * A third of each scenario's code is real instruction words from the game's
 * ROM and a third random words.
 *
 * The board is set up as det_digest sets it up (the sfight profile, one zip,
 * the sound board) and runs 300 real frames first, so the COP and RAM are in a
 * state the game made. Each scenario then writes 64 KB of code into work RAM,
 * randomises the registers, and steps from there. Every step's registers, IP,
 * AC and cycle count, and every writable region at the end of a scenario, go
 * into one file: build it from both trees and cmp.
 *
 *   i960_fuzz <merged sfight zip> <out-file> [scenarios=300] [steps=20000]
 *
 * Every random draw is sequenced, so two compilers draw the same program.
 * Not a ctest.
 */
#define NDEBUG 1
#include "net/netplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "memory.h"
#include "i960.h"
#include "i960_exec.h"
#include "breakpoint.h"
#include "watchpoint.h"
#include "rom_loader.h"
#include "emu_thread.h"
#include "sound.h"
#include "input.h"
#include "registry.h"

static memory_bus_t     bus;
static i960_cpu_t       cpu;
static emu_thread_ctx_t emu;
static romset_t         romset;

static uint32_t rs = 1;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static uint32_t rnd32(void) { uint32_t hi = rnd() << 16; return hi ^ rnd(); }

static FILE *out;
static uint32_t buf[1 << 16];
static int nb;
static void put(uint32_t v) { buf[nb++] = v; if (nb == 1 << 16) { fwrite(buf, 4, (size_t)nb, out); nb = 0; } }
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

/* An address worth aiming at: just before a page or region edge, in MMIO. */
static uint32_t edge(void) {
    static const uint32_t base[] = {
        RAM_BASE + 0x10000, RAM_BASE + 0x80000, RAM_BASE + RAM_SIZE,         /* work RAM pages, its end */
        RAM2_BASE + 0x10000, RAM2_BASE + RAM2_SIZE,                          /* RAM2 (ends mid-page) */
        BUFF_RAM_BASE + 0x10000, BUFF_RAM_BASE + BUFF_RAM_SIZE,              /* buffer RAM */
        COPROGRAM_BASE + 0x100, GEO_BASE + 0x100, GEO_CMD_BASE + 0x10,       /* COP / GEO windows */
        TILE_BASE + 0x10000, TILE_BASE + 0x40000, 0x01010000,                /* tile RAM, its mirror */
        0x01800000 + 0x4000, 0x01810000 + 0x10000,                           /* palette, colorxlat */
        0x11000000 + 0x10000, 0x11200000 + 0x100000,                         /* texture RAM */
        ROM_BASE + 0x10000, ROM_BASE + ROM_SIZE,                             /* ROM (stores: read-only) */
        TIMERS_BASE + 0x10, IRQ_REQUEST_BASE + 4, MIDI_BASE + 4,             /* MMIO */
    };
    uint32_t b = base[rnd() % (sizeof base / sizeof base[0])];
    uint32_t back = rnd() % 24;
    return b - back;
}

/* One memory-format instruction: op, then MEMA (reg+offset) or MEMB in any
 * of its modes, the displacement word after it where the mode has one. */
static int mem_insn(uint32_t *w) {
    static const uint8_t ops[] = { 0x80, 0x82, 0x88, 0x8A, 0x8C, 0x90, 0x92, 0x98, 0x9A,
                                   0xA0, 0xA2, 0xB0, 0xB2, 0xC0, 0xC2, 0xC8, 0xCA };
    uint32_t op = ops[rnd() % sizeof ops];
    uint32_t sd = (rnd() % 8) * 4;               /* an aligned register group for ldl/ldt/ldq */
    uint32_t abase = rnd() % 32;
    if (rnd() & 1) {                             /* MEMA: abase + offset */
        w[0] = op << 24 | sd << 19 | abase << 14 | 1u << 13 | (rnd() & 0x3F);
        return 1;
    }
    static const uint8_t modes[] = { 0x4, 0x5, 0x7, 0xC, 0xD, 0xE, 0xF };
    uint32_t mode = modes[rnd() % sizeof modes];
    uint32_t index = rnd() % 32, scale = rnd() % 5;
    w[0] = op << 24 | sd << 19 | abase << 14 | mode << 10 | scale << 7 | index;
    if (mode == 0x5 || mode >= 0xC) {
        w[1] = (mode == 0xC || mode == 0xE) ? edge() : (rnd() & 0x3F);
        return 2;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: i960_fuzz <merged sfight zip> <out-file> [scenarios] [steps]\n"); return 2; }
    int scenarios = argc > 3 ? atoi(argv[3]) : 300;
    int steps = argc > 4 ? atoi(argv[4]) : 20000;
    out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }

    log_set_path("off");     /* random code warns on nearly every step; the file would be the bottleneck */
    mem_init(&bus, NULL, 0);
    i960_reset(&cpu);
    bp_init();
    wp_init();
    for (size_t i = 0; i < g_profile_count; i++)
        if (!strcmp(g_profiles[i]->id, "sfight")) g_active_profile = g_profiles[i];
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *zip = (uint8_t *)malloc((size_t)len);
    if (!zip || fread(zip, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    fclose(f);
    rl_mem_zip_set(zip, (size_t)len, true);
    if (g_active_profile->load_fn(&romset, NULL, NULL) != 0) { fprintf(stderr, "ROM load failed\n"); return 2; }
    rl_mem_zip_clear();
    g_active_profile->install_fn(&romset, &cpu, &bus);
    irqt_reset();
    if (g_active_profile->quirks.enable_68k_sound) {
        sound_reset();
        sound_attach(&bus);
        if (romset.audiocpu && romset.audiocpu_size > 0) sound_load_rom(romset.audiocpu, (uint32_t)romset.audiocpu_size);
        if (romset.samples && romset.samples_size > 0)   sound_load_samples(romset.samples, (uint32_t)romset.samples_size);
    }
    input_reset();
    input_attach(&bus);
    emu_board_reset_state();
    emu_ctx_init(&emu, &cpu, &bus);
    emu_run(&emu);
    while (g_emu_frames < 300) { emu_slice_body(&emu); emu_slice_finish(&emu); }

    const uint32_t code = RAM_BASE + 0xE0000;    /* 64 KB of work RAM the game leaves alone here */
    uint64_t total = 0, halts = 0;
    for (int sc = 0; sc < scenarios; sc++) {
        rs = 0x9E3779B1u * (uint32_t)(sc + 1);
        for (uint32_t a = 0; a < 0x10000; ) {
            uint32_t w[2], n = 1, k = rnd() % 3;
            if (k == 0) n = (uint32_t)mem_insn(w);
            else if (k == 1) w[0] = mem_read32(&bus, (rnd() % (ROM_SIZE / 4)) * 4);   /* a real instruction word */
            else w[0] = rnd32();
            for (uint32_t i = 0; i < n && a < 0x10000; i++, a += 4) mem_write32(&bus, code + a, w[i]);
        }
        for (int i = 0; i < 16; i++) {
            uint32_t v = rnd() & 1 ? edge() : rnd32();
            ((uint32_t *)&cpu.globals)[i] = v;
        }
        for (int i = 3; i < 16; i++) {
            uint32_t v = rnd() & 1 ? edge() : rnd32();
            cpu.locals.r[i] = v;
        }
        uint32_t sp = RAM_BASE + 0xC0000 + (rnd() & 0xFFC0);
        cpu.locals.r[1] = sp;                    /* sp and fp inside work RAM */
        ((uint32_t *)&cpu.globals)[15] = sp - 0x40;
        cpu.sfr.ip = code + (rnd() % 0x4000) * 4;
        cpu.sfr.ac = rnd() & 0x3F;
        cpu.halted = 0;
        for (int n = 0; n < steps && !cpu.halted; n++) {
            i960_step_hot(&cpu, &bus);
            if ((n & 15) == 15) {
                uint64_t h = fnv(0xcbf29ce484222325ull, &cpu.globals, sizeof cpu.globals);
                h = fnv(h, &cpu.locals, sizeof cpu.locals);
                h = fnv(h, cpu.fp_regs, sizeof cpu.fp_regs);
                put((uint32_t)h); put((uint32_t)(h >> 32));
                put(cpu.sfr.ip); put(cpu.sfr.ac); put((uint32_t)cpu.cycles);
            }
            total++;
        }
        halts += cpu.halted != 0;
        for (int i = 0; i < bus.region_count; i++) {
            const mem_region_t *r = &bus.regions[i];
            if (!r->data || r->readonly || r->size > 0x400000) continue;
            uint64_t h = fnv(0xcbf29ce484222325ull, r->data, r->size);
            put((uint32_t)h); put((uint32_t)(h >> 32));
        }
        put(bus.unmapped_reads & 0xFFFFFFFFu); put(bus.unmapped_writes & 0xFFFFFFFFu); put(bus.ro_writes & 0xFFFFFFFFu);
    }
    fwrite(buf, 4, (size_t)nb, out);
    fclose(out);
    fprintf(stderr, "%llu i960 steps over %d scenarios, %llu halted; bus %llu reads, %llu writes, %llu unmapped, %llu to read-only\n",
            (unsigned long long)total, scenarios, (unsigned long long)halts, (unsigned long long)bus.reads,
            (unsigned long long)bus.writes, (unsigned long long)(bus.unmapped_reads + bus.unmapped_writes),
            (unsigned long long)bus.ro_writes);
    return 0;
}
