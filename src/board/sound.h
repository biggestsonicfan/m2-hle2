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
#define SOUND_CODE_LOG            512u     /* i960 commands kept (power of 2) */
#define SOUND_AHEAD_STEP          16       /* samples per catch-up step (see sound_make_midi_room) */
#define SOUND_AHEAD_MAX           735      /* at most a slice of samples run ahead of it (44100 / 60) */

typedef struct {
    m68k_state_t   m68k;
    scsp_t         scsp;
    uint8_t        ram[SOUND_RAM_SIZE];
    uint8_t        rom[M68K_ROM_SIZE];
    bool           rom_loaded;
    bool           detached;            /* sound_detach: the host has taken the board off (heat) */
    const uint8_t *samples;             /* the whole sample set (romset.samples, not owned) */
    uint32_t       samples_size;
    uint32_t       bank4, bank5;        /* offsets into samples for 0xA00000 / 0xE00000 */
    int32_t        budget;              /* 68000 clock periods owed to the current sample */
    int            mask_seen;           /* the interrupt mask the last instruction ran under */
    uint32_t       slice_frac;          /* sound_run_slice's remainder: part of the board, reset with it */
    int32_t        ahead;               /* samples run inside the slice, owed back by sound_run_slice */
    uint64_t       irqs[8];             /* interrupts taken, by level */

    /* host output: emu thread writes, audio thread reads */
    int16_t          out[SOUND_OUT_FRAMES * 2];
    volatile uint32_t out_w, out_r;
    uint64_t         out_dropped;
    /* Every sample the board has ever produced, whether the ring took it or
     * not. The one clock a consumer outside the ring can trust, and monotonic
     * across a board reset — sound_reset() leaves it, and the ring, alone. */
    uint64_t         out_total;
    uint64_t         midi_drains;      /* catch-up steps run to make room in the MIDI ring */
    uint64_t         midi_holds;       /* times a byte had to wait for the next slice */

    /* i960 side */
    uint64_t write_count, read_count;
    bool     log_writes;
    uint8_t  midi_log[64];
    uint32_t midi_log_ip[64];
    uint32_t midi_log_n;

    /* Every command the i960 has sent, framed the way the driver frames them:
     * the codes are MIDI messages (0xAE1004 = status 0xAE, data 0x10 0x04), so
     * a byte with bit 7 set opens one and two data bytes close it. A command
     * cut short by another status byte is logged with bit 31 set. Stamped with
     * out_total, the board's sample clock. Across a sound_reset. */
    struct { uint32_t code; uint64_t sample; } code_log[SOUND_CODE_LOG];
    uint32_t code_n;                   /* commands ever logged; the ring holds the last SOUND_CODE_LOG */
    uint32_t code_acc;
    int      code_have;                /* bytes of the open command, 0 = none open */
    uint32_t queue_hi;                 /* deepest the game's own command queue has been (32 = full: it drops) */
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
 * both sides: the driver polls it ~50,000 times a second.
 *
 * t is 32 bits, and at 11.2896 MHz that wraps after 380.4 seconds. A capture
 * longer than that silently folds back on itself -- every consumer here and in
 * tools/ reads t as monotonic -- so a session to be graded whole has to stay
 * under it, which is the shorter of the two limits on how long a run these tools
 * can hold against MAME. Widening it means widening the record, on both sides at
 * once. */
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

/* ---- the streaming watchdog ------------------------------------------------
 *
 * The driver does not hand the SCSP a sample and let it play: it gives every
 * voice a window in sound RAM, loops the slot inside it, and keeps refilling the
 * 4 KB chunk the chip is NOT playing out of the sample ROMs. Which chunk that is
 * it learns from the slot monitor CA field, 4096 samples per step -- STF drives
 * it by writing the slot number to MSLC (0x408), waiting out ten ror.l, and
 * testing CA bit 0 with btst.b #7,0x409(a5) (sound ROM 0x604224, 0x60452C,
 * 0x604544). In STF all 32 slots stream this way, each out of its own 8 KB
 * window from 0x010000 up: two 4 KB chunks, double-buffered.
 *
 * That refill is a hard real-time race, and losing it is audible. A copy into a
 * chunk that only STARTS after the chip has already entered it leaves everything
 * from the chunk boundary up to the play position holding the bytes of the
 * previous lap, which are played before the new ones -- a fragment of stale
 * audio, and for a voice that stays late, one every lap until the game restarts
 * the track.
 *
 * READ late_chunk_up, NOT late. A pass that begins at offset 0 with the chip
 * already somewhere in chunk 0 is almost always not a late refill at all but the
 * driver giving the slot a DIFFERENT sample: the window address is fixed per
 * slot, so switching samples means copying the new one over the window from
 * offset 0, and the old sample is still releasing while that happens. There is
 * no register change to spot it by -- SA, LSA and LEA all stay as they were.
 * What separates the two is which chunk the pass was filling. A driver losing
 * the race loses it in both directions, so chunk 1 would be hit too; a reload
 * only ever starts at 0. Measured over 330 s of fights, all 587 flagged passes
 * were chunk 0 at offset 0, with the lateness spread evenly across the 4096
 * samples instead of clustered just past the boundary -- so on this board the
 * race is not being lost, and late_chunk_up is the number to watch for a
 * regression.
 *
 * Nothing in the capture harness can see this. tools/mame/snd-capture.lua leaves
 * the monitor register out of both sides on purpose (the driver polls it around
 * 50,000 times a second, which would slow MAME to a crawl), so the one register
 * the streaming is paced by is the one register never compared. So it is
 * measured here instead, against the play position the chip actually has.
 *
 * A page can belong to several slots at once -- the driver plays one loaded
 * sample from more than one voice -- so the map is a slot MASK per page, not a
 * slot. Getting that wrong attributes a write to whichever slot was stored last,
 * and with 32 voices sharing buffers that is most of them.
 *
 * Off by default and armed over the bridge (snd_watch): it costs a page map
 * rebuild per output sample and a lookup per sound-RAM write. */
#define SND_WATCH_PAGES  (SOUND_RAM_SIZE >> 12)
#define SND_WATCH_EVENTS 128

typedef struct {
    int      on;
    uint32_t page_mask[SND_WATCH_PAGES];   /* 4 KB page -> bit per slot playing through it */
    uint8_t  active[32];
    uint64_t on_sample[32];                /* out_total when this sample started in the slot */
    int32_t  last_chunk[32];               /* chunk the last write to this slot hit, -1 none */
    uint64_t refills[32], late[32];        /* refill passes seen, and those that started late */
    uint32_t worst[32];                    /* largest lateness, in samples into the chunk */
    uint64_t total_writes, total_refills, total_late;
    uint64_t late_chunk0, late_chunk_up;   /* see the note above: only the second is a defect */
    uint32_t nev;
    struct {
        uint64_t sample;                   /* board sample the refill started on */
        uint32_t addr, pc, play, off, lateness;
        uint8_t  slot, chunk;
    } ev[SND_WATCH_EVENTS];
} snd_watch_t;
static snd_watch_t g_snd_watch;

/* The byte of its window a slot is playing, addressed the way scsp_slot_sample
 * addresses the sample data: a PCM8B slot steps one byte per sample, a 16-bit
 * one two. */
static inline uint32_t snd_watch_play_byte(const scsp_slot_t *sl) {
    return SCSP_PCM8B(sl) ? (sl->cur >> SCSP_SHIFT)
                          : ((sl->cur >> (SCSP_SHIFT - 1)) & ~1u);
}

/* Rebuild the page map and notice slots that have just started. Once per output
 * sample, which is 4096 times finer than a chunk. */
static inline void snd_watch_sample(void) {
    snd_watch_t *w = &g_snd_watch;
    scsp_t *sc = &g_sound.scsp;
    memset(w->page_mask, 0, sizeof w->page_mask);
    for (int i = 0; i < 32; i++) {
        scsp_slot_t *sl = &sc->slot[i];
        if (sl->active && !w->active[i]) {
            w->on_sample[i]  = g_sound.out_total;
            w->last_chunk[i] = -1;
        }
        w->active[i] = sl->active;
        if (!sl->active) continue;
        uint32_t sa = SCSP_SA(sl), end = sa + SCSP_LEA(sl);
        if (sa >= SOUND_RAM_SIZE) continue;
        if (end >= SOUND_RAM_SIZE) end = SOUND_RAM_SIZE - 1;
        for (uint32_t pg = sa >> 12; pg <= (end >> 12); pg++) w->page_mask[pg] |= 1u << i;
    }
}

static inline void snd_watch_write(uint32_t addr, int sz) {
    snd_watch_t *w = &g_snd_watch;
    scsp_t *sc = &g_sound.scsp;
    for (int k = 0; k < sz; k++) {
        uint32_t a = addr + (uint32_t)k;
        if (a >= SOUND_RAM_SIZE) break;
        for (uint32_t m = w->page_mask[a >> 12]; m; m &= m - 1) {
            int i = 0;
            for (uint32_t b = m & (~m + 1u); b > 1u; b >>= 1) i++;
            scsp_slot_t *sl = &sc->slot[i];
            uint32_t sa = SCSP_SA(sl);
            if (a < sa) continue;
            uint32_t off = a - sa;
            if (off > SCSP_LEA(sl)) continue;
            w->total_writes++;
            int32_t chunk = (int32_t)(off >> 12);
            if (chunk == w->last_chunk[i]) continue;   /* still inside the same pass */
            w->last_chunk[i] = chunk;
            /* The fill right after a key-on legitimately covers the whole window,
             * playback included, so a voice is only held to the rule once it has
             * been running for a chunk. */
            if (g_sound.out_total - w->on_sample[i] < 4096) continue;
            w->refills[i]++;
            w->total_refills++;
            uint32_t play = snd_watch_play_byte(sl);
            if ((int32_t)(play >> 12) != chunk) continue;   /* the chip is elsewhere: in time */
            uint32_t lateness = play & 0xFFFu;
            w->late[i]++;
            w->total_late++;
            if (chunk) w->late_chunk_up++; else w->late_chunk0++;
            if (lateness > w->worst[i]) w->worst[i] = lateness;
            if (w->nev < SND_WATCH_EVENTS) {
                uint32_t e = w->nev++;
                w->ev[e].sample   = g_sound.out_total;
                w->ev[e].addr     = a;
                w->ev[e].pc       = g_sound.m68k.cpu.pc;
                w->ev[e].play     = play;
                w->ev[e].off      = off;
                w->ev[e].lateness = lateness;
                w->ev[e].slot     = (uint8_t)i;
                w->ev[e].chunk    = (uint8_t)chunk;
            }
        }
    }
}

static inline void snd_watch_arm(int on) {
    memset(&g_snd_watch, 0, sizeof g_snd_watch);
    for (int i = 0; i < 32; i++) g_snd_watch.last_chunk[i] = -1;
    g_snd_watch.on = on;
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
        if (g_snd_watch.on) snd_watch_write(addr, sz);
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

static inline void sound_code_put(uint32_t code) {
    uint32_t i = g_sound.code_n++ & (SOUND_CODE_LOG - 1);
    g_sound.code_log[i].code   = code;
    g_sound.code_log[i].sample = g_sound.out_total;
}

static inline void sound_code_byte(uint8_t b) {
    if (b & 0x80u) {
        if (g_sound.code_have) sound_code_put(g_sound.code_acc | 0x80000000u);
        g_sound.code_acc  = b;
        g_sound.code_have = 1;
        return;
    }
    if (!g_sound.code_have) { sound_code_put(0x80000000u | b); return; }   /* a stray data byte */
    g_sound.code_acc = (g_sound.code_acc << 8) | b;
    if (++g_sound.code_have == 3) { sound_code_put(g_sound.code_acc); g_sound.code_have = 0; }
}

static uint32_t sound_midi_read_cb(mem_region_t *r, uint32_t addr, int size) {
    (void)r; (void)size;
    g_sound.read_count++;
    /* i8251 status at +4: transmitter ready and empty — the byte went straight
     * into the SCSP's MIDI buffer (MAME: model2_serial_w). */
    return (addr - MIDI_BASE) == 4 ? 0x05u : 0u;
}

static void sound_run(uint32_t n);                      /* below */
static inline bool sound_make_midi_room(uint32_t need);  /* below; the write callback needs it */

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
    sound_code_byte((uint8_t)val);
    /* The run loop only hands the i960 a TxRDY interrupt when the ring has room
     * (emu_sound_ready), so a byte the game sends always fits. This is for any
     * other writer: make room the same accounted way, and if the driver truly
     * will not take it, scsp_midi_in drops the byte and counts it. */
    sound_make_midi_room(1);
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
    g_sound.slice_frac = 0;
    g_sound.ahead = 0;
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
    g_sound.detached = false;
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

/* Take the board off again, mid-run: the UART goes back to the plain region
 * mem_init made it (the i960's bytes land in bus->midi and mean nothing, as
 * with the sound board never attached -- the game writes the UART's control
 * register at boot and never reads its status), and sound_run steps nothing
 * until the next sound_attach. The driver's state goes with it, so coming
 * back is sound_reset + sound_attach, a fresh boot of the 68000. A host does
 * this when the device has to run cooler (main_libretro.c, the heat guard),
 * and never inside a netplay session: the other board runs the sound board,
 * so this one has to. The output ring is the reader's; a host that stops
 * reading moves out_r itself. */
static inline void sound_detach(memory_bus_t *bus) {
    g_sound.detached = true;
    for (int i = 0; i < bus->region_count; i++) {
        mem_region_t *r = &bus->regions[i];
        if (r->base == MIDI_BASE) {
            r->read_cb  = NULL;
            r->write_cb = NULL;
            break;
        }
    }
    LOG_INFO("sound: board detached");
}

/* ---- the sample clock ------------------------------------------------------------- */

/* ---- the producer tap ----------------------------------------------------
 *
 * sound_out_push below is the ONLY producer of board audio, and the ring it
 * feeds has a single reader (core/audio_out.h). A host with no audio device
 * drains nothing, so a second reader of that ring would only ever see samples
 * the first one had already counted into out_dropped: anything wanting its own
 * copy of the board's output — the A/V stream, a capture — has to take it
 * HERE, ahead of the ring. The tap is handed the sample's absolute index, so
 * what it keeps carries the same clock as g_sound.out_total.
 *
 * Called on the emu thread, once per 44.1 kHz sample, with the emu mutex held.
 * Keep it to a ring write. */
static void (*g_sound_tap)(int16_t l, int16_t r, uint64_t index, void *ud);
static void  *g_sound_tap_ud;

static inline void sound_set_tap(void (*fn)(int16_t, int16_t, uint64_t, void *), void *ud) {
    g_sound_tap_ud = ud;
    g_sound_tap    = fn;      /* last: ud has to be in place before the first call */
}

static inline void sound_out_push(int16_t l, int16_t r) {
    uint64_t index = g_sound.out_total++;
    if (g_sound_tap) g_sound_tap(l, r, index, g_sound_tap_ud);
    uint32_t w = g_sound.out_w;
    if (((w + 1) & (SOUND_OUT_FRAMES - 1)) == g_sound.out_r) { g_sound.out_dropped++; return; }
    g_sound.out[w * 2] = l;
    g_sound.out[w * 2 + 1] = r;
    g_sound.out_w = (w + 1) & (SOUND_OUT_FRAMES - 1);
}

/* Run the board for n output samples. */
static void sound_run(uint32_t n) {
    if (!g_sound.rom_loaded || g_sound.detached) return;
    m68k_state_t *m = &g_sound.m68k;
    for (uint32_t i = 0; i < n; i++) {
        if (g_snd_watch.on) snd_watch_sample();
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

/* ---- the UART's backpressure ----------------------------------------------------
 *
 * The board runs a slice of the i960 and then a slice of sound, so the 68000
 * cannot take a MIDI byte while the i960 is sending it. On the board it would: the
 * UART sends a byte in a third of a millisecond (MAME's capture: 0.33 ms apart),
 * the driver takes it straight out of the SCSP's buffer, and the i960 queues
 * nothing it cannot send. Here a burst meets a buffer nobody is emptying.
 *
 * So when the buffer has no room for what is about to be sent, the sound board
 * runs on now, SOUND_AHEAD_STEP samples at a time, until the driver has taken
 * enough. Those samples are the slice's own, run early: `ahead` counts them and
 * sound_run_slice owes them back, so the board's clock -- and the host's audio --
 * never gain a sample. At most SOUND_AHEAD_MAX may be run early; past that the
 * byte waits for the next slice, as it would wait on the UART.
 *
 * The old drain valve ran the 68000 without owing the samples back, and gave up
 * after 32 passes and dropped the byte. Returns whether `need` bytes now fit. */
static inline bool sound_make_midi_room(uint32_t need) {
    while (scsp_midi_room(&g_sound.scsp) < need) {
        if (!g_sound.rom_loaded || g_sound.detached ||
            g_sound.ahead + SOUND_AHEAD_STEP > SOUND_AHEAD_MAX) return false;
        g_sound.midi_drains++;
        sound_run(SOUND_AHEAD_STEP);
        g_sound.ahead += SOUND_AHEAD_STEP;
    }
    return true;
}

/* One emu slice (1/60 s) of sound, less what was run early inside it. The
 * remainder carries from slice to slice and is reset with the board: two
 * boards cold-booted together have to put the same samples in every slice. */
static inline void sound_run_slice(uint32_t slices_per_sec) {
    g_sound.slice_frac += SOUND_RATE;
    uint32_t n = g_sound.slice_frac / slices_per_sec;
    g_sound.slice_frac -= n * slices_per_sec;
    uint32_t early = (uint32_t)g_sound.ahead < n ? (uint32_t)g_sound.ahead : n;
    g_sound.ahead -= (int32_t)early;
    sound_run(n - early);
}

#endif /* SOUND_H */
