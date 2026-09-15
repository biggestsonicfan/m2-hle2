/*
 * sound.h — the Model 2 sound board: a 68000 running the game's own sound
 * driver, an SCSP (scsp.h), the 512 KB of sound RAM they share, and the sample
 * ROMs. The same on Model 2A, 2B and 2C (MAME model2.cpp, model2_snd).
 *
 * 68000 address map:
 *   0x000000-0x07FFFF  sound RAM, shared with the SCSP (vectors copied in at reset)
 *   0x100000-0x100FFF  SCSP registers
 *   0x400000-0x400001  sound control (bit 5 picks the sample banks, for sets > 8 MB)
 *   0x600000-0x67FFFF  sound program ROM
 *   0x800000-0x9FFFFF  sample ROM, first 2 MB
 *   0xA00000-0xDFFFFF  bank 4: sample ROM +2 MB (or +8 MB)
 *   0xE00000-0xFFFFFF  bank 5: sample ROM +6 MB (or +10 MB)
 * The SCSP sees only the sound RAM, so the driver copies what it plays out of
 * the sample ROMs — and streams long samples 8 KB at a time, timed off the
 * SCSP's play position. STF's driver keeps three quarters of its 602 samples
 * above 0xA00000.
 *
 * The i960 talks to the board through a UART (0x9C0000 data, 0x9C0004
 * control/status) whose bytes arrive in the SCSP's MIDI input buffer.
 *
 * Time: the board runs in samples. Each output sample the 68000 gets 256 clock
 * periods (11.2896 MHz / 44.1 kHz, counted per bus access in m68k_exec.h), then
 * the SCSP produces the sample. The 68000 takes the SCSP's interrupt lines
 * between instructions, and every register access lands on a chip that is at
 * exactly that point in time — the driver's timing loops and its slot-monitor
 * polling depend on it. The emu thread runs a slice's worth of samples per
 * slice and the host audio callback (core/audio_out.h) drains them.
 */
#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "memory.h"
#include "m68k.h"
#include "m68k_exec.h"
#include "scsp.h"

#define SOUND_RAM_SIZE            0x80000u
#define SOUND_RATE                44100u
#define SOUND_CYCLES_PER_SAMPLE   256
#ifndef SOUND_IPL_LEAD
#define SOUND_IPL_LEAD            10        /* see sound_run */
#endif
#define SOUND_OUT_FRAMES          16384u   /* host output ring, stereo frames (power of 2) */

typedef struct {
    m68k_state_t   m68k;
    scsp_t         scsp;
    uint8_t        ram[SOUND_RAM_SIZE];
    uint8_t        rom[M68K_ROM_SIZE];
    bool           rom_loaded;
    const uint8_t *samples;             /* the whole sample set (romset.samples, not owned) */
    uint32_t       samples_size;
    uint32_t       bank4, bank5;        /* offsets into samples for 0xA00000 / 0xE00000 */
    int32_t        budget;              /* 68000 clock periods owed to the current sample */
    int            mask_seen;           /* the interrupt mask the last instruction ran under */
    uint64_t       irqs[8];             /* interrupts taken, by level */

    /* host output: emu thread writes, audio thread reads */
    int16_t          out[SOUND_OUT_FRAMES * 2];
    volatile uint32_t out_w, out_r;
    uint64_t         out_dropped;

    /* i960 side */
    uint64_t write_count, read_count;
    bool     log_writes;
    uint8_t  midi_log[64];
    uint32_t midi_log_ip[64];
    uint32_t midi_log_n;
} sound_state_t;

static sound_state_t g_sound;

/* ---- capture, for grading against MAME ------------------------------------
 * The same files tools/mame/snd-capture.lua writes: records of four u32 words
 * [tag << 24 | offset, data, t, pc] — tag 1 a byte the i960 wrote to the sound
 * UART, 2 an SCSP register write (data = value | mask << 16, as a 16-bit bus
 * handler sees it), 3 an SCSP register read that differs from the last read of
 * that register (data = value | folded repeats << 16), 4 an interrupt taken
 * (offset = level) — and per frame a mark, sound RAM 0x1000-0x4FFF and the
 * SCSP register block as little-endian words. t is the 68000's clock-period
 * count, the unit MAME's capture uses. The slot monitor (0x408) is left out on
 * both sides: the driver polls it ~50,000 times a second. */
typedef struct {
    FILE    *f, *ramf, *regsf;
    int      active, done;
    uint32_t want, n, nmarks;
    uint32_t (*marks)[4];
    uint32_t buf[4096 * 4];
    int      nb;
    uint16_t lastr[0x1000];
    uint8_t  seen[0x1000];
    uint32_t reps[0x1000];
} sndcap_t;
static sndcap_t g_sndcap;

static inline void sndcap_put(uint32_t tag, uint32_t off, uint32_t data, uint32_t pc) {
    sndcap_t *c = &g_sndcap;
    uint32_t *r = c->buf + c->nb * 4;
    r[0] = (tag << 24) | (off & 0xFFFFFFu);
    r[1] = data;
    r[2] = (uint32_t)g_sound.m68k.cpu.cycles;
    r[3] = pc;
    if (++c->nb == 4096) { fwrite(c->buf, 4, 4096 * 4, c->f); c->nb = 0; }
    c->n++;
}

/* A 16-bit bus handler's view of an sz-byte access at off (as MAME's tap sees it). */
static inline void sndcap_scsp(int write, uint32_t off, uint32_t val, int sz) {
    uint32_t pc = g_sound.m68k.cpu.pc;
    for (int k = 0; k < (sz == 4 ? 2 : 1); k++) {
        uint32_t o, v, mask;
        if (sz == 1) {
            o = off & ~1u;
            if (off & 1u) { v = val & 0xFFu; mask = 0x00FFu; }
            else          { v = (val & 0xFFu) << 8; mask = 0xFF00u; }
        } else {
            o = (off & ~1u) + (uint32_t)k * 2u;
            v = sz == 4 ? (k ? val & 0xFFFFu : val >> 16) : val & 0xFFFFu;
            mask = 0xFFFFu;
        }
        if (o >= 0x1000u || o == 0x408u) continue;
        if (write) {
            sndcap_put(2, o, v | (mask << 16), pc);
        } else if (!g_sndcap.seen[o] || g_sndcap.lastr[o] != (uint16_t)v) {
            uint32_t reps = g_sndcap.reps[o] > 0xFFFFu ? 0xFFFFu : g_sndcap.reps[o];
            sndcap_put(3, o, v | (reps << 16), pc);
            g_sndcap.seen[o] = 1; g_sndcap.lastr[o] = (uint16_t)v; g_sndcap.reps[o] = 0;
        } else {
            g_sndcap.reps[o]++;
        }
    }
}

static inline void sndcap_stop(void) {
    sndcap_t *c = &g_sndcap;
    if (!c->active) return;
    if (c->nb) fwrite(c->buf, 4, (size_t)c->nb * 4, c->f);
    fclose(c->f); fclose(c->ramf); fclose(c->regsf);
    c->f = c->ramf = c->regsf = NULL;
    c->nb = 0;
    c->active = 0;
    c->done = 1;
}

/* Call once per game frame, on the emu thread. */
static inline void sndcap_frame(uint32_t frame, uint32_t frame_counter) {
    sndcap_t *c = &g_sndcap;
    if (!c->active) return;
    uint32_t *mk = c->marks[c->nmarks++];
    mk[0] = frame; mk[1] = frame_counter; mk[2] = (uint32_t)g_sound.m68k.cpu.cycles; mk[3] = c->n;
    fwrite(g_sound.ram + 0x1000, 1, 0x4000, c->ramf);
    uint8_t regs[1072] = {0};
    for (uint32_t o = 0; o < 0x430u; o += 2) {
        if (o >= 0x404u && o < 0x40Au) continue;
        uint16_t w = scsp_peek16(&g_sound.scsp, o);
        regs[o] = (uint8_t)w; regs[o + 1] = (uint8_t)(w >> 8);
    }
    fwrite(regs, 1, sizeof regs, c->regsf);
    if (c->nmarks > c->want) sndcap_stop();
}

/* ---- 68000 bus ------------------------------------------------------------- */

static inline uint8_t sound_rom_byte(const sound_state_t *ss, uint32_t a) {
    if (a < 0x080000u) return ss->ram[a];
    if (a >= M68K_ROM_BASE && a < M68K_ROM_BASE + M68K_ROM_SIZE) return ss->rom[a - M68K_ROM_BASE];
    if (!ss->samples) return 0;
    uint32_t o;
    if      (a >= 0x800000u && a < 0xA00000u) o = a - 0x800000u;
    else if (a >= 0xA00000u && a < 0xE00000u) o = ss->bank4 + (a - 0xA00000u);
    else if (a >= 0xE00000u)                  o = ss->bank5 + (a - 0xE00000u);
    else return 0;
    return o < ss->samples_size ? ss->samples[o] : 0;
}

static uint32_t sound_bus_read(sound_state_t *ss, uint32_t addr, int sz, int peek) {
    addr &= 0xFFFFFFu;
    if (addr >= M68K_SCSP_BASE && addr < M68K_SCSP_BASE + M68K_SCSP_SIZE) {
        uint32_t off = addr - M68K_SCSP_BASE;
        if (peek) {
            uint16_t w = scsp_peek16(&ss->scsp, off & ~1u);
            if (sz == 1) return (off & 1) ? (w & 0xFFu) : (uint32_t)(w >> 8);
            if (sz == 2) return w;
            return ((uint32_t)w << 16) | scsp_peek16(&ss->scsp, (off & ~1u) + 2);
        }
        uint32_t v = scsp_read(&ss->scsp, off, sz);
        if (g_sndcap.active) sndcap_scsp(0, off, v, sz);
        return v;
    }
    if (addr + (uint32_t)sz <= 0x080000u) {                /* the hot path */
        const uint8_t *p = ss->ram + addr;
        if (sz == 1) return p[0];
        if (sz == 2) return ((uint32_t)p[0] << 8) | p[1];
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
    uint32_t v = 0;
    for (int i = 0; i < sz; i++) v = (v << 8) | sound_rom_byte(ss, (addr + (uint32_t)i) & 0xFFFFFFu);
    return v;
}

static uint32_t sound_m68k_read(void *ctx, uint32_t addr, int sz) {
    return sound_bus_read((sound_state_t *)ctx, addr, sz, 0);
}

/* a debugger's read: no side effects on the SCSP */
static inline uint32_t sound_m68k_peek(sound_state_t *ss, uint32_t addr, int sz) {
    return sound_bus_read(ss, addr, sz, 1);
}

static inline void sound_ctrl_w(sound_state_t *ss, uint32_t val) {
    if (ss->samples_size > 0x800000u) {                    /* bigger sets bank their upper half */
        ss->bank4 = (val & 0x20) ? 0x200000u : 0x800000u;
        ss->bank5 = (val & 0x20) ? 0x600000u : 0xA00000u;
    }
}

static void sound_m68k_write(void *ctx, uint32_t addr, uint32_t val, int sz) {
    sound_state_t *ss = (sound_state_t *)ctx;
    addr &= 0xFFFFFFu;
    if (addr + (uint32_t)sz <= 0x080000u) {
        uint8_t *p = ss->ram + addr;
        if (sz == 1) { p[0] = (uint8_t)val; return; }
        if (sz == 2) { p[0] = (uint8_t)(val >> 8); p[1] = (uint8_t)val; return; }
        p[0] = (uint8_t)(val >> 24); p[1] = (uint8_t)(val >> 16); p[2] = (uint8_t)(val >> 8); p[3] = (uint8_t)val;
        return;
    }
    if (addr >= M68K_SCSP_BASE && addr < M68K_SCSP_BASE + M68K_SCSP_SIZE) {
        uint32_t off = addr - M68K_SCSP_BASE;
        if (g_sndcap.active) sndcap_scsp(1, off, val, sz);
        scsp_write(&ss->scsp, off, val, sz);
        return;
    }
    if (addr >= M68K_SNDCTL_BASE && addr < M68K_SNDCTL_BASE + M68K_SNDCTL_SIZE) {
        sound_ctrl_w(ss, val);
        return;
    }
    /* ROM and unmapped space ignore writes */
}

/* ---- i960 side: the sound UART ---------------------------------------------- */

static uint32_t sound_midi_read_cb(mem_region_t *r, uint32_t addr, int size) {
    (void)r; (void)size;
    g_sound.read_count++;
    /* i8251 status at +4: transmitter ready and empty — the byte went straight
     * into the SCSP's MIDI buffer (MAME: model2_serial_w). */
    return (addr - MIDI_BASE) == 4 ? 0x05u : 0u;
}

static void sound_midi_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    (void)r; (void)size;
    if (g_sndcap.active) sndcap_put(1, addr, (val & 0xFFu) | 0xFF0000u, 0);
    if ((addr - MIDI_BASE) != 0) return;                   /* +4 is the UART's control register */
    g_sound.write_count++;
    if (g_sound.midi_log_n < 64) {
        g_sound.midi_log[g_sound.midi_log_n]    = (uint8_t)val;
        g_sound.midi_log_ip[g_sound.midi_log_n] = g_mem_last_write_ip;
        g_sound.midi_log_n++;
    }
    if (g_sound.log_writes) LOG_INFO("SOUND write @ 0x%08X sz=%d val=0x%08X", addr, size, val);
    scsp_midi_in(&g_sound.scsp, (uint8_t)val);
}

/* ---- lifecycle ------------------------------------------------------------------ */

/* MAME model2.cpp reset_model2_scsp: the 68000's reset vectors come from the
 * first 16 bytes of the program ROM, copied into sound RAM. */
static inline void sound_boot_68k(void) {
    memcpy(g_sound.ram, g_sound.rom, 16);
    m68k_reset(&g_sound.m68k);
    g_sound.m68k.read_cb  = sound_m68k_read;
    g_sound.m68k.write_cb = sound_m68k_write;
    g_sound.m68k.mem_ctx  = &g_sound;
    m68k_startup(&g_sound.m68k);
}

static inline void sound_reset(void) {
    bool log_writes = g_sound.log_writes;
    const uint8_t *samples = g_sound.samples;
    uint32_t samples_size = g_sound.samples_size;
    bool rom_loaded = g_sound.rom_loaded;
    uint8_t rom_vectors[16];
    memcpy(rom_vectors, g_sound.rom, 16);

    memset(g_sound.ram, 0, sizeof g_sound.ram);
    memset(&g_sound.m68k, 0, sizeof g_sound.m68k);
    scsp_reset(&g_sound.scsp, g_sound.ram, SOUND_RAM_SIZE, &g_sound.m68k.cpu.cycles);
    g_sound.budget = 0;
    g_sound.mask_seen = 7;
    memset(g_sound.irqs, 0, sizeof g_sound.irqs);
    g_sound.write_count = g_sound.read_count = 0;
    g_sound.midi_log_n = 0;
    g_sound.log_writes = log_writes;
    g_sound.samples = samples;
    g_sound.samples_size = samples_size;
    g_sound.bank4 = 0x200000u;
    g_sound.bank5 = 0x600000u;
    g_sound.m68k.read_cb  = sound_m68k_read;
    g_sound.m68k.write_cb = sound_m68k_write;
    g_sound.m68k.mem_ctx  = &g_sound;
    if (rom_loaded) sound_boot_68k();
}

/* The sound program ROM, big-endian as it comes out of the zip. */
static inline void sound_load_rom(const uint8_t *bytes, uint32_t size) {
    if (size > M68K_ROM_SIZE) {
        LOG_WARN("sound: ROM size %u > %u — truncating", size, M68K_ROM_SIZE);
        size = M68K_ROM_SIZE;
    }
    memcpy(g_sound.rom, bytes, size);
    if (size < M68K_ROM_SIZE) memset(g_sound.rom + size, 0xFF, M68K_ROM_SIZE - size);
    g_sound.rom_loaded = true;
    sound_boot_68k();
    LOG_INFO("sound: 68000 ROM loaded (%u bytes), SSP=0x%08X PC=0x%08X", size, g_sound.m68k.cpu.ssp, g_sound.m68k.cpu.pc);
}

/* The sample ROMs, concatenated (not copied: the caller keeps them alive). */
static inline void sound_load_samples(const uint8_t *bytes, uint32_t size) {
    g_sound.samples      = bytes;
    g_sound.samples_size = size;
    g_sound.bank4 = 0x200000u;
    g_sound.bank5 = 0x600000u;
    LOG_INFO("sound: sample ROM mapped (%u bytes)", size);
}

/* Hook the UART callbacks. Call after mem_init(). */
static inline void sound_attach(memory_bus_t *bus) {
    for (int i = 0; i < bus->region_count; i++) {
        mem_region_t *r = &bus->regions[i];
        if (r->base == MIDI_BASE) {
            r->read_cb  = sound_midi_read_cb;
            r->write_cb = sound_midi_write_cb;
            LOG_INFO("sound: UART callbacks attached");
            return;
        }
    }
    LOG_WARN("sound: MIDI region not found — sound inactive");
}

/* ---- the sample clock ------------------------------------------------------------- */

static inline void sound_out_push(int16_t l, int16_t r) {
    uint32_t w = g_sound.out_w;
    if (((w + 1) & (SOUND_OUT_FRAMES - 1)) == g_sound.out_r) { g_sound.out_dropped++; return; }
    g_sound.out[w * 2] = l;
    g_sound.out[w * 2 + 1] = r;
    g_sound.out_w = (w + 1) & (SOUND_OUT_FRAMES - 1);
}

/* Run the board for n output samples. */
static void sound_run(uint32_t n) {
    if (!g_sound.rom_loaded) return;
    m68k_state_t *m = &g_sound.m68k;
    for (uint32_t i = 0; i < n; i++) {
        g_sound.budget += SOUND_CYCLES_PER_SAMPLE;
        while (g_sound.budget > 0) {
            if (m->cpu.halted) { g_sound.budget = 0; break; }
            uint64_t c0 = m->cpu.cycles;
            /* The 68000 compares its interrupt lines with the mask while an
             * instruction runs, SOUND_IPL_LEAD periods before it ends: a timer
             * that expires later than that, or a mask lowered by the
             * instruction itself (RTE, MOVE to SR), is only acted on after the
             * next one. Both are measurable in the driver's timer periods
             * (MAME: 49.884 samples for timer B, 505.30 for timer A). */
            scsp_timers(&g_sound.scsp, c0 >= SOUND_IPL_LEAD ? c0 - SOUND_IPL_LEAD : 0);
            int lvl = scsp_irq_level(&g_sound.scsp);
            int ipl = m68k_ipl(&m->cpu);
            if (lvl > ipl && lvl > g_sound.mask_seen && m68k_interrupt(m, lvl)) {
                g_sound.irqs[lvl]++;
                if (g_sndcap.active) sndcap_put(4, (uint32_t)lvl, 0, m->cpu.pc);
            } else if (m->cpu.stopped) {
                g_sound.mask_seen = ipl;
                m->cpu.cycles += (uint64_t)g_sound.budget;   /* time passes while it waits */
                g_sound.budget = 0;
                break;
            } else {
                g_sound.mask_seen = ipl;
                m68k_step(m);
                g_sound.budget -= (int32_t)(m->cpu.cycles - c0);
                continue;
            }
            g_sound.mask_seen = m68k_ipl(&m->cpu);
            g_sound.budget -= (int32_t)(m->cpu.cycles - c0);
        }
        int16_t l, r;
        scsp_sample(&g_sound.scsp, &l, &r);
        sound_out_push(l, r);
    }
}

/* One emu slice (1/60 s) of sound. */
static inline void sound_run_slice(uint32_t slices_per_sec) {
    static uint32_t frac;
    frac += SOUND_RATE;
    uint32_t n = frac / slices_per_sec;
    frac -= n * slices_per_sec;
    sound_run(n);
}

#endif /* SOUND_H */
