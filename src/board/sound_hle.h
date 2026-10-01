/*
 * sound_hle.h — Sonic the Fighters' sound driver in C, in place of the 68000
 * (Pinboard #173; SCSP.md, "The driver in C"). Optional (--sound-hle): the
 * 68000 running the driver stays the default, the oracle, and the only path
 * for any other program ROM or homebrew.
 *
 * What it is. A port of the driver in epr-19021.31 (Hiro's driver; IDA listing
 * in schamp_SoundDriver_disasm, Explanation.txt / RAMData.txt), routine by
 * routine, with the driver's own RAM layout: the command queue at 0x1100, the
 * globals at 0x2400, the voices at 0x2800, the sequencers at 0x3000, the
 * tracks at 0x4000, the sample table at 0x5000 and the instruments at 0x9000
 * live at the same addresses in g_sound.ram, so a sound capture's RAM dumps
 * (sound.h sndcap, tools/mame/snd_compare.py) read the same on both paths. The
 * asm label or address of each piece is in the comment above it.
 *
 * The chip is scsp.h, the same as with the 68000: the port writes its
 * registers and sound RAM through the paths the 68000's bus takes (scsp_write,
 * scsp_ram_touch), streaming included -- the driver plays every sample through
 * an 8 KB window of sound RAM per slot and refills it off the slot monitor,
 * and that is ported as it is (shle_stream_step), with the monitor read off
 * the chip. What the chip plays is then what it plays on the board: the same
 * bytes, the same registers, only at slightly different moments.
 *
 * What it leaves out, and why that is enough for STF: the FM instrument type
 * (instrument byte 0 = 0x80) and the A? 7x engine commands. Nothing in STF's
 * song or effect data uses them (a scan of all 608 sequences), and they are
 * logged if they come.
 *
 * Time. The board's clock still runs (g_sound.m68k.cpu.cycles, 256 periods a
 * sample), so the i960's UART is clocked exactly as with the 68000 and the
 * i960 sees the same line: it cannot tell the two apart (det_digest
 * --sound-hle). The driver's three timers run on that clock: B (music) and C
 * (effects) tick the sequencers, A the release countdowns. What the 68000's
 * timing adds -- the main loop taking one event at a time, a preload holding
 * everything behind it, handlers delaying each other -- is modelled where it
 * moves a note by more than a millisecond, from measurements of the board
 * (the constants below). The rest is a few milliseconds this way or that.
 */
#ifndef SOUND_HLE_H
#define SOUND_HLE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "scsp.h"

/* ---- where things are ---------------------------------------------------- */

#define SHLE_A6(o)        (0x1000u + (uint32_t)(o))   /* the driver's a6-relative RAM */
#define SHLE_QUEUE        SHLE_A6(0x100)              /* 256 queued MIDI events, 4 bytes each */
#define SHLE_VOICES       SHLE_A6(0x1800)             /* 32 x 16: one per SCSP slot */
#define SHLE_VOICES_END   SHLE_A6(0x1A00)
#define SHLE_SEQS         SHLE_A6(0x2000)             /* 8 x 16: 0 music, 1-7 effects */
#define SHLE_TRACKS       SHLE_A6(0x3000)             /* 20 x 16 */
#define SHLE_NTRACKS      20
#define SHLE_SMPTBL       0x5000u                     /* CurSmplTable: the sample table, copied */
#define SHLE_INSTBL       0x9000u                     /* CurInsTable: the instruments, copied */
#define SHLE_PRELOAD      0x50000u                    /* SC05: samples copied into sound RAM */

/* program ROM (68000 addresses) */
#define SHLE_P_SMPTBL     0x608000u
#define SHLE_P_INSDATA    0x608004u
#define SHLE_P_A7XTBL     0x608008u
#define SHLE_P_INSLIB     0x60800Cu
#define SHLE_P_FIXEDTRK   0x608010u
#define SHLE_P_EFSDL      0x608014u
#define SHLE_P_MUSIC      0x60801Cu
#define SHLE_P_PRELOAD    0x608020u
#define SHLE_P_VELOCITY   0x608028u
#define SHLE_P_PAUSE_ON   0x608074u
#define SHLE_P_PAUSE_OFF  0x608076u
#define SHLE_P_HOLDVAL    0x60807Bu
#define SHLE_P_SFXVOICES  0x60807Cu
#define SHLE_T_RELRATE    0x602106u
#define SHLE_T_PAN        0x602D9Eu
#define SHLE_T_EFPAN      0x603CFAu
#define SHLE_T_FREQPITCH  0x604B04u
#define SHLE_T_BEND       0x604FB4u
#define SHLE_T_OCTNOTE    0x605BE4u
#define SHLE_T_FREQVALS   0x605CA4u
#define SHLE_T_SPEED      0x605E24u
#define SHLE_T_NEGSMP     0x601100u

/* Board time, in 1/16 of a 68000 clock period. The driver reloads a timer in
 * its handler, so a period is (255 - reload) samples plus the time from the
 * timer firing to the reload: 255 clock periods when nothing is in the way
 * (the interrupt is taken 68 periods after the timer fires, and the reload is
 * written 187 periods into the handler). Timers B and C share interrupt level
 * 2, so one's handler waits for the other's; the MIDI byte handler (level 3)
 * and timer A's (level 1) stretch what they interrupt. Measured on the board
 * (snd_replay traces, $SND_TRACE): the handlers' lengths are below, and with
 * them timer B comes out 50.015 samples in attract and 50.054 under an
 * effects storm. The interrupt also waits for the instruction it lands in to
 * end, and that depends on what the main loop is doing: 13 clock periods on
 * average, but 37 inside a refill, whose movem.l take 76-88 each (a trace of
 * the board under the effects storm: 24% of its time in refills). So the
 * latency grows with the refill work, which the HLE knows: the chunks it
 * copies (SHLE_CHUNK_CYCLES). Without it the music ran 0.05% fast under load
 * and drifted tens of ms a minute. */
#define SHLE_T16          16u
#define SHLE_IRQ_LAT      70u          /* timer fires -> handler entered, main loop idle */
#define SHLE_RELOAD_AT    187u         /* handler entered -> reload written (B and C) */
#define SHLE_RELOAD_AT_A  170u
#define SHLE_DUR_B        580u         /* Int2_Timer, timer B, before any event it queues */
#define SHLE_DUR_C        900u         /* Int2_Timer, timer C */
#define SHLE_DUR_EVENT    650u         /* each event a sequencer queues */
#define SHLE_DUR_MIDI     535u         /* Int3_MidiIn, one byte */
#define SHLE_DUR_A        4000u        /* Interrupt1, timer A */
#define SHLE_CHUNK_CYCLES 2752u        /* a refill: 16 x (movem.l in, movem.l out with its wait states, adda) */
#define SHLE_REFILL_WAIT  24u          /* extra wait for an interrupt landing in one: 37 - 13 */
/* The driver clears its RAM, runs a 2.3 s delay loop and only then takes an
 * interrupt: the first one on the board is at this clock period after reset.
 * The i960's bytes wait in the SCSP's MIDI buffer until then. */
#define SHLE_BOOT_CYCLES  33193169ull
/* ...and it writes the SCSP (InitSCSP, which cuts every voice) this long
 * after reset: until then whatever was playing plays on. The same after a
 * restart (MIDI byte 0xFF), measured on the board from the byte. */
#define SHLE_INIT_CYCLES  4506800ull
/* What fills that 0.4 s: a delay loop (0x1500 x 58 clock periods), then a
 * clear of all 512 KB of sound RAM a long at a time, 8 clock periods a byte.
 * The voices' windows go to zero as it passes them, so what was playing falls
 * silent well before InitSCSP. */
#define SHLE_CLEAR_DELAY  311808ull
#define SHLE_CLEAR_BYTE   8u

/* The main loop's pass over the 32 slots, where each streaming slot gets its
 * refill step: 11129 clock periods median on the board (43 samples). */
#define SHLE_PASS_SAMPLES 43u

/* The main loop's time. It takes one queued event per pass over a slot, in
 * between the streaming, so an event waits for the one before it; a burst of
 * notes on the board lands over a few milliseconds, and a preloaded sample set
 * (SC05, a byte loop) holds everything behind it for a third of a second
 * (0.320 s for set 0's 134,116 bytes, interrupts included, which the
 * handlers' own lengths add). Clock periods, fitted to the board (snd_replay
 * traces, the bgm stimulus). */
#define SHLE_COST_EVENT   600u
#define SHLE_COST_KEYON   8000u
#define SHLE_COST_BYTE    23u

typedef struct {
    bool     on;             /* the HLE runs the sound board */
    bool     booted;         /* past the driver's boot delay */
    uint64_t restart_at;     /* a restart's InitSCSP is due (0 = none) */
    uint64_t clear_at;       /* ...and its RAM clear begins here */
    uint32_t cleared;        /* bytes of sound RAM the clear has reached */
    uint64_t boot_at;        /* clock period the driver takes its first interrupt */
    uint64_t due_a, due_b, due_c;   /* timer expiries, 1/16 clock periods */
    uint32_t pass_left;      /* samples to the main loop's next pass over the slots */
    bool     stall;          /* the event being dispatched runs a long copy */
    uint64_t stall_until;    /* the main loop is inside a long copy: no streaming till then */
    uint64_t busy_until;     /* clock period the main loop is free to take the next event */
    uint64_t l2_free;        /* clock period no level 2 or 3 handler is running any more */
    uint32_t enq;            /* events queued (the handlers' length) */
    uint32_t chunks;         /* refill chunks copied in the current pass */
    uint32_t refill_frac;    /* the main loop's share of time in refills, 16.16, averaged over passes */
    uint32_t cost;           /* clock periods the event being dispatched keeps it busy */
    uint64_t events, keyons, unknown, chunks_total;
    bool     warned_fm, warned_a7x;
} shle_t;

static shle_t g_shle;
/* A host's switch: take the HLE when the driver is one it knows. */
static int g_sound_hle_want;

/* ---- memory ------------------------------------------------------------------ */

static inline uint8_t shle_rb(uint32_t a) { return g_sound.ram[a & 0x7FFFFu]; }
static inline uint16_t shle_rw(uint32_t a) { return (uint16_t)(shle_rb(a) << 8 | shle_rb(a + 1)); }
static inline uint32_t shle_rl(uint32_t a) { return (uint32_t)shle_rw(a) << 16 | shle_rw(a + 2); }
static inline void shle_wb(uint32_t a, uint32_t v) { g_sound.ram[a & 0x7FFFFu] = (uint8_t)v; }
static inline void shle_ww(uint32_t a, uint32_t v) { shle_wb(a, v >> 8); shle_wb(a + 1, v); }
static inline void shle_wl(uint32_t a, uint32_t v) { shle_ww(a, v >> 16); shle_ww(a + 2, v); }

/* any 68000 address the driver reads: RAM, its program ROM, the sample ROM */
static inline uint8_t shle_mb(uint32_t a) { return sound_rom_byte(&g_sound, a & 0xFFFFFFu); }
static inline uint16_t shle_mw(uint32_t a) { return (uint16_t)(shle_mb(a) << 8 | shle_mb(a + 1)); }
static inline uint32_t shle_ml(uint32_t a) { return (uint32_t)shle_mw(a) << 16 | shle_mw(a + 2); }
/* a write the driver makes through a pointer: only RAM takes it */
static inline void shle_mwb(uint32_t a, uint32_t v) { if ((a & 0xFFFFFFu) < 0x80000u) shle_wb(a, v); }
static inline void shle_mww(uint32_t a, uint32_t v) { shle_mwb(a, v >> 8); shle_mwb(a + 1, v); }

/* the driver's globals, by their a6 offset */
#define G8(o)       shle_rb(SHLE_A6(o))
#define G16(o)      shle_rw(SHLE_A6(o))
#define G32(o)      shle_rl(SHLE_A6(o))
#define SG8(o, v)   shle_wb(SHLE_A6(o), (v))
#define SG16(o, v)  shle_ww(SHLE_A6(o), (v))
#define SG32(o, v)  shle_wl(SHLE_A6(o), (v))

/* ---- the chip ------------------------------------------------------------------ */

/* An SCSP access the way the 68000's bus makes it (sound_m68k_write): the
 * chip merges a byte into its word, syncs what it owes first, and a capture
 * sees it. */
static inline void shle_sw(uint32_t off, uint32_t v, int sz) {
    if (sndcap_on()) sndcap_scsp(1, off, v, sz);
    scsp_write(&g_sound.scsp, off, v, sz);
    if (g_sound.scsp.dsp_moved) sound_map_pages(&g_sound);
}
static inline void shle_sw8(uint32_t off, uint32_t v)  { shle_sw(off, v & 0xFF, 1); }
static inline void shle_sw16(uint32_t off, uint32_t v) { shle_sw(off, v & 0xFFFF, 2); }
static inline void shle_sw32(uint32_t off, uint32_t v) { shle_sw(off, v, 4); }
static inline uint16_t shle_sr16(uint32_t off) { return scsp_peek16(&g_sound.scsp, off & ~1u); }
static inline uint8_t shle_sr8(uint32_t off) {
    uint16_t w = shle_sr16(off);
    return (uint8_t)((off & 1) ? w : w >> 8);
}

/* CA bit 0 of a slot, as the driver tests it after selecting the slot on the
 * monitor (btst #7,$409): which 4 KB half of its window the chip is in. */
static inline bool shle_ca_upper(unsigned i) {
    scsp_t *s = &g_sound.scsp;
    if (s->owed && s->done[i] < s->owed) { scsp_slot_run(s, i, s->owed); s->catches++; }
    return (s->slot[i].cur >> (SCSP_SHIFT + 12)) & 1;
}

/* n bytes from any 68000 address into sound RAM, which the chip may be
 * reading: what it owes out of there is made first */
static void shle_copy(uint32_t dst, uint32_t src, uint32_t n) {
    dst &= 0x7FFFFu;
    scsp_ram_touch(&g_sound.scsp, dst, n, 1);
    uint8_t *d = g_sound.ram + dst;
    src &= 0xFFFFFFu;
    if (src >= 0x800000u && g_sound.samples) {          /* the sample ROM, as one run where it is one */
        uint32_t o = src < 0xA00000u ? src - 0x800000u : src < 0xE00000u ? g_sound.bank4 + (src - 0xA00000u)
                   : g_sound.bank5 + (src - 0xE00000u);
        uint32_t end = src < 0xA00000u ? 0xA00000u : src < 0xE00000u ? 0xE00000u : 0x1000000u;
        if (src + n <= end && o + n <= g_sound.samples_size) { memcpy(d, g_sound.samples + o, n); return; }
    }
    for (uint32_t k = 0; k < n; k++) d[k] = shle_mb(src + k);
}

/* ---- the command queue --------------------------------------------------------- */

static inline void shle_enqueue(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    g_shle.enq++;
    uint32_t a = SHLE_QUEUE + (G16(0x1404) & 0x3FFu);
    shle_wb(a, b0); shle_wb(a + 1, b1); shle_wb(a + 2, b2); shle_wb(a + 3, b3);
    SG16(0x1404, (G16(0x1404) + 4) & 0x3FF);
    SG8(0x1406, G8(0x1406) + 1);
}
/* enqueue three bytes, leaving the fourth as it was (the sequencer's writes) */
static inline void shle_enqueue3(uint8_t b0, uint8_t b1, uint8_t b2) {
    uint32_t a = SHLE_QUEUE + (G16(0x1404) & 0x3FFu);
    shle_enqueue(b0, b1, b2, shle_rb(a + 3));
}

/* sub_601712: the LED byte the driver rotates per MIDI message */
static inline void shle_led(void) {
    if (G8(0x142F)) return;
    uint8_t d = G8(0x1426);
    d = (uint8_t)((d >> 1) | (d << 7));
    if (d == 0x7F) d = 0xF7;
    SG8(0x1426, d);
}

/* ---- voices: killing, allocating -------------------------------------------------- */

/* RR 0x1F and key off: how every path in the driver cuts a voice short */
static inline void shle_cut(uint32_t slot) {
    shle_sw8(slot + 0xB, 0x1F);
    shle_sw8(slot + 0x0, 0x10);
}
static inline void shle_stream_clear(uint32_t slot) { shle_wb(SHLE_A6(0x1000) + slot, 0); }

/* music voices behind a freed allocation order `d2` move up one */
static void shle_music_order_close(uint8_t d2, bool need_used) {
    for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
        uint8_t f = shle_rb(v + 2);
        if (f & 0x08) continue;
        if (need_used && !(f & 0x40)) continue;
        if (shle_rb(v + 9) > d2) shle_wb(v + 9, shle_rb(v + 9) - 1);
    }
}
static void shle_sfx_order_close(uint8_t d2) {
    for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16)
        if ((shle_rb(v + 2) & 0x08) && shle_rb(v + 5) > d2) shle_wb(v + 5, shle_rb(v + 5) - 1);
}

/* loc_6025FC / loc_60263E: a free voice round-robin, or the oldest music voice
 * (a released one first). 0 = none to be had. */
static uint32_t shle_alloc_music(void) {
    uint32_t a1 = G32(0x141A);
    for (int n = 0; n < 32; n++) {
        if (a1 == SHLE_VOICES_END) { a1 = SHLE_VOICES; SG32(0x141A, a1); }
        if (a1 < SHLE_VOICES || a1 > SHLE_VOICES_END) { a1 = SHLE_VOICES; SG32(0x141A, a1); }  /* not in the driver: a corrupt pointer */
        if (shle_rb(a1 + 2) == 0) { SG32(0x141A, a1 + 16); return a1; }
        a1 += 16;
    }
    uint8_t d6 = 0xFF; uint32_t pick = 0;
    for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
        uint8_t f = shle_rb(v + 2);
        if ((f & 0x08) || !(f & 0x20)) continue;
        if (shle_rb(v + 9) <= d6) { pick = v; d6 = shle_rb(v + 9); }
    }
    if (d6 == 0xFF) {
        for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
            if (shle_rb(v + 2) & 0x08) continue;
            if (shle_rb(v + 9) <= d6) { pick = v; d6 = shle_rb(v + 9); }
        }
        if (d6 == 0xFF) return 0;
    }
    uint32_t slot = shle_rw(pick);
    shle_cut(slot);
    shle_wb(pick + 2, 0);
    shle_stream_clear(slot);
    shle_music_order_close(d6, true);
    SG8(0x141E, G8(0x141E) - 1);
    return pick;
}

/* loc_601C7A: an effect voice. Under byte_60807C of them, any free voice;
 * otherwise the effect voice with the highest priority number (the oldest of
 * equals) if the new note's is not higher. */
static uint32_t shle_alloc_sfx(void) {
    uint8_t cap = shle_mb(SHLE_P_SFXVOICES);
    if ((int8_t)cap > (int8_t)G8(0x142E)) return shle_alloc_music();
    uint8_t d0 = 0xFF, d1 = G8(0x140C), d6 = cap;
    uint32_t a1 = 0;
    for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
        uint8_t f = shle_rb(v + 2);
        if (!(f & 0x40) || !(f & 0x08)) continue;
        uint8_t c = shle_rb(v + 0xC);
        if (d1 < c) { d0 = shle_rb(v + 5); d1 = c; a1 = v; }
        else if (d1 == c && d0 >= shle_rb(v + 5)) { d0 = shle_rb(v + 5); a1 = v; }
        if (--d6 == 0) {
            if (d0 & 0x80) return 0;                 /* nothing it may take: the note is dropped */
            /* loc_601D06 */
            uint32_t slot = shle_rw(a1);
            shle_cut(slot);
            if (!(shle_rb(a1 + 0xC) & 0xF0)) {       /* an engine voice's bit in its 0x1600 record */
                uint8_t d5 = shle_rb(a1 + 0xA);
                uint32_t a = SHLE_A6(0x1600) + (d5 & 0xF0);
                shle_wb(a, shle_rb(a) & ~(1u << (d5 & 7)));
            }
            shle_stream_clear(slot);
            shle_wb(a1 + 5, cap);
            shle_sfx_order_close(d0);
            SG8(0x142E, G8(0x142E) - 1);
            return a1;
        }
    }
    SG8(0x142E, 0);
    return shle_alloc_music();
}

/* ---- pitch --------------------------------------------------------------------- */

/* SendFrequency (a drum's fixed pitch): writes OCT/FNS, returns the streaming
 * speed. */
static uint16_t shle_send_frequency(uint32_t slot, uint8_t b6, uint8_t b5) {
    uint16_t d6 = b6, d5 = b5, d0;
    if ((d6 & 0xF) == 0 && (d5 & 0x80)) { d6 = (uint16_t)(d6 + 0xFC); d0 = (uint16_t)((d6 - 1) & 0xFF); }
    else d0 = d6;
    d6 = (uint16_t)(d6 << 7);
    uint16_t d2 = d6;
    d6 &= 0x7800;
    d2 = (uint16_t)(((d2 >> 3) & 0xF0) + d5) & 0xFF;
    d6 |= shle_mw(SHLE_T_FREQVALS + d2 * 2u);
    shle_sw16(slot + 0x10, d6);
    uint32_t t = shle_ml(SHLE_T_SPEED + ((d0 & 0xF0) >> 2));
    return shle_mw(t + (d0 & 0xF) * 32u);
}

/* SendPitchedFreq / sub_6029E0: a melodic voice's pitch from its note, the
 * channel's bend and the track's detune. */
static uint16_t shle_send_pitched(uint32_t t, uint32_t v, uint32_t slot, int16_t bend) {
    int16_t d3 = (int8_t)(uint8_t)(shle_rb(v + 0xA) + shle_rb(t + 5));
    int32_t d5 = (int16_t)(bend + d3);
    uint8_t on = shle_rb(v + 3);
    uint32_t tbl = shle_ml(SHLE_T_FREQPITCH + (on & 0xF) * 4u);
    uint16_t fns = shle_mw(tbl + (uint32_t)(d5 * 2));
    uint16_t d0 = (uint16_t)((((on & 0xF0) << 7) + fns) & 0x7BFF);
    shle_sw16(slot + 0x10, (shle_sr16(slot + 0x10) & 0x8000) | d0);
    uint32_t s = shle_ml(SHLE_T_SPEED + ((on & 0xF0) >> 2));
    return shle_mw(s + (on & 0xF) * 32u + (uint32_t)(d5 * 2));
}

/* ---- key-on: the sample (loc_602146) ------------------------------------------ */

/* the voice's first 0x200 bytes into its window, and the key-on */
static inline void shle_first_chunk(uint32_t slot, uint32_t ring, uint32_t src) {
    shle_copy(ring, src, 0x200);
    shle_sw8(slot, shle_sr8(slot) | 0x18);
}

static void shle_key_on(uint32_t t, uint32_t v, uint32_t slot, uint32_t e, uint16_t speed) {
    uint32_t start = shle_ml(e);
    g_shle.keyons++;
    g_shle.cost += SHLE_COST_KEYON;
    uint32_t ring = (uint32_t)shle_rb(v + 0xD) << 12;
    uint32_t rec = SHLE_A6(0x1000) + slot, lr = SHLE_A6(0x1A00) + slot;
    if (start & 0x80000000u) {                           /* preloaded into sound RAM (SC05) */
        start &= 0x7FFFFFFFu;
        uint32_t loop = shle_ml(e + 0xC);
        if (loop) {
            shle_wb(v + 2, shle_rb(v + 2) & ~0x10u);
            if (shle_rb(t) & 0x10) {                     /* an FM track: 16-bit, looped to the loop's end */
                shle_sw32(slot, start | 0x10200000u);
                shle_sw16(slot + 4, shle_ml(e + 8));
                shle_sw16(slot + 6, loop - 1);
            } else {
                shle_sw32(slot, start | 0x10300000u);
                shle_sw16(slot + 4, shle_ml(e + 8));
                shle_sw16(slot + 6, shle_ml(e + 4) - 1);
            }
            shle_sw8(slot, shle_sr8(slot) | 0x18);
            shle_wb(rec, 0x40);
        } else {                                         /* loc_602488 */
            shle_sw32(slot, start | 0x10100000u);
            uint32_t len = shle_ml(e + 4) & 0xFFFF;
            shle_sw32(slot + 4, len);
            uint16_t div = (uint16_t)(speed >> 5);
            uint32_t q = div ? len / div : 0;
            uint16_t dur = q > 0xFFFF ? (uint16_t)len : (uint16_t)q;   /* divu overflow leaves the dividend */
            shle_ww(lr + 0xA, shle_rw(v + 0xE));
            shle_ww(v + 0xE, dur);
            shle_sw8(slot, shle_sr8(slot) | 0x18);
            shle_wb(v + 2, shle_rb(v + 2) | 0x05);
        }
        return;
    }
    uint32_t len = shle_ml(e + 4);
    if (!len) return;
    uint32_t loop = shle_ml(e + 0xC);
    if (len <= 0x200) {                                  /* loc_6024C6: one chunk, the chip loops it */
        if (loop) {
            shle_wb(v + 2, shle_rb(v + 2) & ~0x10u);
            shle_sw32(slot + 4, ((len - loop) & 0xFFFF) << 16 | (len & 0xFFFF));
            shle_sw32(slot, ring | 0x10300000u);
        } else {                                         /* loc_6025D4 */
            shle_sw32(slot + 4, len);
            shle_sw32(slot, ring | 0x10100000u);
            shle_ww(v + 0xE, shle_rw(v + 0xE) + 2);
            shle_wb(v + 2, shle_rb(v + 2) | 0x01);
        }
        shle_first_chunk(slot, ring, start);
        return;
    }
    if (loop) {                                          /* loc_60227E: streamed, looped */
        shle_wb(v + 2, shle_rb(v + 2) & ~0x10u);
        uint32_t lp = shle_ml(e + 8);
        shle_sw32(slot, ring | 0x10300000u);
        shle_wb(lr + 0x10, 0);
        shle_wl(lr + 4, len);
        shle_wl(lr, len - loop);
        if (len <= 0x1000) shle_wb(lr + 0x10, shle_rb(lr + 0x10) | 1);
        shle_wl(rec + 0xA, len - 0x200);
        shle_wl(rec + 0x16, (shle_rl(rec + 0x16) & 0xFF000000u) | (loop & 0xFFFFFFu));
        shle_wl(rec + 0x12, (shle_rl(rec + 0x12) & 0xFF000000u) | (lp & 0xFFFFFFu));
        shle_ww(rec + 0x1A, speed);
        shle_ww(rec + 0x1C, (uint16_t)(speed - 0x8000));
        shle_wb(rec + 1, 0x10);
        if (loop < 0x200) shle_wb(rec, shle_rb(rec) | 2);
        shle_wb(rec, shle_rb(rec) | 1);
        shle_wl(rec + 2, start + 0x200);
        shle_wl(rec + 6, ring + 0x200);
        shle_first_chunk(slot, ring, start);
        shle_wb(rec, shle_rb(rec) | 0x80);
        return;
    }
    /* streamed, once through */
    shle_sw32(slot, ring | 0x10300000u);
    shle_wl(rec + 0xA, len - 0x200);
    shle_wb(rec + 1, 0x10);
    shle_ww(rec + 0x1A, speed);
    shle_ww(rec + 0x1C, (uint16_t)(speed - 0x8000));
    shle_wl(rec + 2, start + 0x200);
    shle_wl(rec + 6, ring + 0x200);
    shle_first_chunk(slot, ring, start);
    shle_wb(rec, 0x80);
}

/* ---- the streaming (the main loop's step for one slot, loc_6041EA) ---------------------- */

/* the voice's release countdown, when its stream reaches its end: some extra
 * ticks, fewer the faster it plays */
static void shle_stream_done(unsigned i, uint32_t rec) {
    uint32_t slot = i * 0x20u, v = SHLE_VOICES + i * 16u;
    shle_sw8(slot + 1, shle_sr8(slot + 1) & 0x9F);      /* the loop off... */
    shle_sw16(slot + 6, shle_rw(rec + 0x1E));           /* ...and the end where the sample ends */
    shle_wb(rec, 0);
    uint8_t d2 = (uint8_t)(shle_rw(rec + 0x1A) >> 8);
    int8_t d3 = (int8_t)(uint8_t)(((shle_rb(v + 2) & 0x08) ? 0x60 : 0x41) - d2);
    if (d3 >= 0) shle_ww(v + 0xE, shle_rw(v + 0xE) + (uint8_t)d3);
    shle_wb(v + 2, shle_rb(v + 2) | 0x01);
}

/* loc_6044FC: a short loop handed to the chip (LSA / LEA out of the loop record) */
static void shle_stream_hw_loop(uint32_t slot, uint32_t rec, uint32_t lr) {
    shle_sw32(slot + 4, (uint32_t)shle_rw(lr + 2) << 16 | shle_rw(lr + 6));
    shle_wb(rec, shle_rb(rec) & 0x7F);
}

static void shle_stream_step(unsigned i) {
    uint32_t slot = i * 0x20u, rec = SHLE_A6(0x1000) + slot, lr = SHLE_A6(0x1A00) + slot;
    uint8_t f = shle_rb(rec);
    if (!(f & 0x80)) return;
    uint16_t d0 = (uint16_t)(shle_rw(rec + 0x1C) + shle_rw(rec + 0x1A));
    if (d0 & 0x8000) { shle_ww(rec + 0x1C, d0); return; }    /* not yet: the pacing */
    uint8_t d2 = shle_rb(rec + 1);
    uint32_t a2;
    if (d2 == 0) {                                      /* the lower half: once the chip has left it */
        if (!shle_ca_upper(i)) return;
        shle_wl(lr, shle_rl(lr) - 0x2000);
        shle_wl(lr + 4, shle_rl(lr + 4) - 0x2000);
        a2 = shle_rl(rec + 0xE);
    } else if (d2 == 0x80) {                            /* the upper half: likewise */
        if (shle_ca_upper(i)) return;
        a2 = shle_rl(rec + 6);
    } else a2 = shle_rl(rec + 6);
    uint32_t a1 = shle_rl(rec + 2), d5 = shle_rl(rec + 0xA);
    g_shle.chunks++;
    g_shle.chunks_total++;
    if (d5 > 0x200) {                                   /* a whole chunk */
        shle_wl(rec + 0xA, d5 - 0x200);
        shle_wb(rec + 1, d2 + 0x10);
        shle_ww(rec + 0x1C, d0 & 0x7FFF);
        shle_copy(a2, a1, 0x200);
        shle_wl(rec + 2, a1 + 0x200);
        shle_wl(rec + 6, a2 + 0x200);
        return;
    }
    if (f & 0x01) {                                     /* loc_604376: the end of a looped sample */
        uint32_t x = (0x200u - d5) & 0xFFFF;
        uint32_t d4 = (shle_rl(rec + 0x12) & 0xFFFFFFu) + x;
        int32_t d3 = (int32_t)((shle_rl(rec + 0x16) & 0xFFFFFFu) - x);
        if (d3 < 0) d3 = (int32_t)(shle_rl(rec + 0x16) & 0xFFFFFFu);
        shle_wb(rec + 1, d2 + 0x10);
        shle_ww(rec + 0x1C, d0 & 0x7FFF);
        shle_wl(rec + 2, d4);
        shle_wl(rec + 0xA, (uint32_t)d3);
        shle_copy(a2, a1, 0x200);                       /* the tail, and the bytes after it, which repeat the loop's start */
        shle_wl(rec + 6, a2 + 0x200);
        if (!(f & 0x02)) return;
        if ((int32_t)shle_rl(lr) < 0) {
            shle_wl(lr, shle_rl(lr + 4));
            shle_ww(lr + 6, shle_rw(lr + 6) + shle_rw(rec + 0x18));
            shle_wl(rec + 2, shle_rl(rec + 0x12));
            shle_wl(rec + 6, shle_rl(lr) + shle_rl(rec + 0xE));
            return;
        }
        if ((uint8_t)(shle_rb(rec + 1) - 0x10) & 0x80) { shle_stream_hw_loop(slot, rec, lr); return; }
        if (shle_rb(lr + 0x10) & 1) { shle_stream_hw_loop(slot, rec, lr); return; }
        shle_wb(rec, (shle_rb(rec) | 0x04) & ~0x01u);
        return;
    }
    if (f & 0x04) {                                     /* loc_604522: waiting to hand a loop to the chip */
        if (shle_ca_upper(i)) return;
        shle_stream_hw_loop(slot, rec, lr);
        return;
    }
    if (f & 0x08) {                                     /* loc_60453A: the end is in the lower half */
        if (shle_ca_upper(i)) return;
        shle_stream_done(i, rec);
        return;
    }
    /* loc_6045D8: the last chunk */
    shle_ww(rec + 0x1E, (uint16_t)((a2 - shle_rl(rec + 0xE)) + d5));
    shle_copy(a2, a1, 0x200);
    if (!(d2 & 0x80)) {
        shle_wb(rec, shle_rb(rec) | 0x08);
        shle_wb(rec + 1, d2 + 0x10);
        return;
    }
    shle_stream_done(i, rec);                           /* loc_6046C2: in the upper half, at once */
}

/* ---- note on (MidEvt_NoteOn) ------------------------------------------------------ */

static void shle_note_off_voices(uint32_t t, uint8_t d4, uint8_t d2);

static inline uint8_t shle_voice_tl(uint32_t v, uint32_t t) {
    uint16_t d6 = (uint16_t)((shle_rb(v + 8) * shle_rb(t + 0xA)) >> 8);
    return (uint8_t)((uint8_t)~d6 >> 1);
}

/* the LFO depth bits a voice takes from its track, scaled by the channel's
 * modulation (0x4220) and breath (0x4240) controllers */
static void shle_voice_lfo(uint32_t t, uint32_t slot, bool mask_mod) {
    shle_sw16(slot + 0x12, shle_rw(t + 8));
    shle_sw16(slot + 0x12, shle_sr16(slot + 0x12) & 0xFF18);
    uint8_t ch = shle_rb(t + 1);
    uint32_t d5 = shle_rb(t + 9) & 0xE0u, d6 = shle_rb(t + 9) & 7u;
    uint32_t m = G8(0x3220 + ch);
    if (mask_mod) m &= 7;
    shle_sw8(slot + 0x13, shle_sr8(slot + 0x13) | ((d5 * m / 7) & 0xE0));
    uint32_t b = G8(0x3240 + ch) & 7u;
    shle_sw8(slot + 0x13, shle_sr8(slot + 0x13) | ((d6 * b / 7) & 7));
}

/* loc_601EAE onwards: the voice takes the note */
static void shle_voice_setup(uint32_t t, uint32_t v) {
    shle_wb(v + 2, 0x40);
    uint32_t slot = shle_rw(v);
    shle_sw8(slot, 0x10);
    shle_ww(v + 6, G16(0x1416));
    shle_wb(v + 3, G8(0x1415));
    shle_wb(v + 8, G8(0x1410));
    shle_wb(v + 4, shle_rb(t + 1));
    shle_wb(v + 0xC, G8(0x140C));
    if (shle_rb(t) & 0x01) {
        shle_wb(v + 9, 0xFF);
        shle_wb(v + 5, G8(0x142E));
        SG8(0x142E, G8(0x142E) + 1);
        shle_wb(v + 2, shle_rb(v + 2) | 0x08);
    } else {
        shle_wb(v + 9, G8(0x141E));
        SG8(0x141E, G8(0x141E) + 1);
    }
    uint16_t speed;
    if (shle_rb(t) & 0x08) {                            /* a drum */
        shle_wb(v + 2, shle_rb(v + 2) | 0x10);
        shle_wb(v + 0xB, G8(0x140A));
        shle_sw8(slot + 0xD, shle_voice_tl(v, t));
        speed = shle_send_frequency(slot, G8(0x1412), G8(0x1413));
        shle_sw32(slot + 4, 0x1FFF);
        shle_sw8(slot + 0xC, 0);
        shle_sw16(slot + 0xE, 0);
        shle_voice_lfo(t, slot, false);
        shle_sw8(slot + 0x15, G8(0x140D));
        shle_sw8(slot + 0x16, G8(0x1411));
    } else {                                            /* loc_601FB8 */
        shle_wb(v + 0xA, G8(0x1418));
        shle_sw8(slot + 0xD, shle_voice_tl(v, t));
        shle_sw8(slot + 0xC, 0);
        shle_sw16(slot + 0xE, 0);
        shle_sw8(slot + 0x10, 0);
        if ((shle_rb(t) & 0x10) && !g_shle.warned_fm) {
            LOG_WARN("sound HLE: an FM instrument (type 0x80) keyed; its modulation is not modelled");
            g_shle.warned_fm = true;
        }
        speed = shle_send_pitched(t, v, slot, (int16_t)shle_rw(SHLE_A6(0x3200) + shle_rb(v + 4) * 2u));
        shle_sw32(slot + 4, 0x1FFF);
        shle_voice_lfo(t, slot, true);
        shle_sw8(slot + 0x15, shle_rb(t + 0xC));
        shle_sw8(slot + 0x16, G8(0x1411));
    }
    /* loc_6020B0 */
    uint32_t ch = G8(0x140E) & 0xFu;
    uint16_t id = shle_rw(v + 6);
    uint32_t e;
    shle_ww(SHLE_A6(0x3270) + ch * 2, id);
    if (!(id & 0x8000)) e = SHLE_SMPTBL + (id & 0x7FFu) * 16u;
    else                e = SHLE_T_NEGSMP + (id & 0x7FFu) * 16u;
    shle_sw32(slot + 8, G32(0x143A));
    shle_ww(v + 0xE, shle_mw(SHLE_T_RELRATE + (G8(0x143D) & 0x1Fu) * 2u));
    shle_key_on(t, v, slot, e, speed);
}

/* the voice of this channel playing this exclusive group, if any, cut (loc_601C04 / loc_601D8E) */
static uint32_t shle_exclusive(uint32_t t, uint8_t grp, bool sfx) {
    uint8_t ch = shle_rb(t + 1);
    for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
        uint8_t f = shle_rb(v + 2);
        if (sfx ? !(f & 0x08) : !(f & 0x40)) continue;
        if (shle_rb(v + 4) != ch || shle_rb(v + 0xB) != grp) continue;
        shle_wb(v + 2, 0);
        uint32_t slot = shle_rw(v);
        if (sfx) {
            uint8_t d2 = shle_rb(v + 5);
            shle_wb(v + 5, 0); shle_wb(v + 9, 0);
            shle_cut(slot); shle_stream_clear(slot);
            SG8(0x142E, G8(0x142E) - 1);
            shle_sfx_order_close(d2);
        } else {
            uint8_t d2 = shle_rb(v + 9);
            shle_wb(v + 9, 0);
            shle_cut(slot); shle_stream_clear(slot);
            SG8(0x141E, G8(0x141E) - 1);
            shle_music_order_close(d2, false);
        }
        return v;
    }
    return 0;
}

static void shle_note_on(uint32_t t, uint32_t p) {
    uint8_t note = shle_rb(p);
    uint8_t d2 = note;
    uint8_t d4 = (uint8_t)(note + shle_rb(t + 4));
    if (d4 & 0x80) return;
    uint8_t vel = shle_rb(p + 1);
    if (!vel) { shle_note_off_voices(t, d4, d2); return; }
    uint32_t vt = shle_ml(SHLE_P_VELOCITY);
    vt += shle_mw(vt + 2);
    SG8(0x1410, shle_mb(vt + vel));
    uint32_t d3 = shle_rb(t + 2) & 0x7Fu;
    if ((int16_t)d3 > (int16_t)shle_rw(SHLE_INSTBL)) return;
    uint32_t a2 = SHLE_INSTBL + shle_rw(SHLE_INSTBL + 2 + d3 * 2);
    uint8_t ch = shle_rb(t + 1);
    uint32_t v = 0;
    if (shle_rb(t) & 0x08) {                            /* a drum kit: one entry per key */
        a2 += 2;
        uint8_t lo = shle_rb(a2), hi = shle_rb(a2 + 1);
        if (d2 < lo) return;
        if ((int8_t)d2 > (int8_t)hi) return;
        a2 += 2 + (uint32_t)(uint8_t)(d2 - lo) * 12u;
        SG16(0x1416, shle_rw(a2));
        if (shle_rw(a2) & 0x8000) return;
        if (shle_rb(a2 + 5) & 0x80) SG8(0x1411, shle_rb(t + 6));
        else                        SG8(0x1411, shle_rb(a2 + 4));
        SG16(0x1412, shle_rw(a2 + 2));
        SG8(0x140C, shle_rb(a2 + 6));
        SG8(0x140A, shle_rb(a2 + 7));
        SG32(0x143A, shle_rl(a2 + 8));
        shle_wl(SHLE_A6(0x3290) + ch * 4u, a2);
        uint8_t d1 = shle_rb(t + 0xD);
        if (d1) shle_wb(t + 0xD, 0);
        else {
            d1 = shle_rb(t + 0xC);
            if (!(d1 & 7)) d1 = shle_rb(a2 + 5) & 0x7F;
        }
        SG8(0x140D, d1);
        SG8(0x1415, shle_mb(SHLE_T_OCTNOTE + (d4 & 0x7F)));
        uint8_t grp = shle_rb(a2 + 7);
        if (shle_rb(t) & 0x01) {
            if (grp) v = shle_exclusive(t, grp, true);
            if (!v) v = shle_alloc_sfx();
        } else {
            if (grp) v = shle_exclusive(t, grp, false);
            if (!v) v = shle_alloc_music();
        }
    } else {                                            /* loc_601E04: key splits of 10 bytes */
        shle_wb(t, shle_rb(t) & ~0x10u);
        if (shle_rb(a2) == 0x80) {
            shle_wb(t, shle_rb(t) | 0x10);
            if (!g_shle.warned_fm) { LOG_WARN("sound HLE: FM instrument %u (type 0x80) not modelled", d3); g_shle.warned_fm = true; }
        } else {
            int n = 0;
            while (d4 > shle_rb(a2)) { a2 += 10; if (++n > 0x7F) return; }
        }
        shle_wl(SHLE_A6(0x3290) + ch * 4u, a2);
        SG16(0x1416, shle_rw(a2 + 4));
        SG8(0x1418, shle_rb(a2 + 2));
        SG32(0x143A, shle_rl(a2 + 6));
        d4 = (uint8_t)((d4 + shle_rb(a2 + 3)) & 0x7F);
        SG8(0x1415, shle_mb(SHLE_T_OCTNOTE + d4));
        SG8(0x1411, shle_rb(t + 6));
        SG8(0x140C, shle_rb(t + 0xF));
        if (shle_rb(t) & 0x01) v = shle_alloc_sfx();
        else { SG8(0x140C, 0); v = shle_alloc_music(); }
    }
    if (v) shle_voice_setup(t, v);
}

/* ---- note off (MidEvt_NoteOff) --------------------------------------------------- */

/* sub_6028E8: release a voice, or hold it for the sustain pedal */
static void shle_release(uint32_t v, uint8_t ch) {
    uint8_t f = shle_rb(v + 2);
    if (G8(0x3220 + ch) & 0x80) { shle_wb(v + 2, f | 0x02); return; }
    uint32_t slot = shle_rw(v);
    shle_sw8(slot, 0x10);
    f |= 0x21;
    shle_wb(v + 2, f);
    if (f & 0x04) {
        uint16_t d0 = shle_rw(SHLE_A6(0x1A00) + slot + 0xA);
        if (d0 < shle_rw(v + 0xE)) shle_ww(v + 0xE, d0);
    }
}

static void shle_note_off_voices(uint32_t t, uint8_t d4, uint8_t d2) {
    uint32_t d3 = shle_rb(t + 2) & 0x7Fu;
    uint32_t a2 = shle_ml(SHLE_P_INSDATA);
    if ((int16_t)d3 > (int16_t)shle_mw(a2)) return;
    a2 += shle_mw(a2 + 2 + d3 * 2);
    if (shle_rb(t) & 0x08) {
        a2 += 2;
        uint8_t lo = shle_mb(a2), hi = shle_mb(a2 + 1);
        if (d2 < lo) return;
        if ((int8_t)d2 > (int8_t)hi) return;
        a2 += 2 + (uint32_t)(uint8_t)(d2 - lo) * 12u;
        SG16(0x1416, shle_mw(a2));
        if (shle_mw(a2) & 0x8000) return;
        SG8(0x1415, shle_mb(SHLE_T_OCTNOTE + (d4 & 0x7F)));
    } else {
        int n = 0;
        while (d4 > shle_mb(a2)) { a2 += 10; if (++n > 0x7F) return; }
        SG16(0x1416, shle_mw(a2 + 4));
        d4 = (uint8_t)((d4 + shle_mb(a2 + 3)) & 0x7F);
        SG8(0x1415, shle_mb(SHLE_T_OCTNOTE + d4));
    }
    uint8_t ch = G8(0x140E) & 0xF;
    for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
        if (shle_rb(v + 3) != G8(0x1415) || shle_rb(v + 4) != ch || shle_rw(v + 6) != G16(0x1416)) continue;
        uint8_t f = shle_rb(v + 2);
        if (f & 0x12) continue;
        f &= 0x7F;
        shle_wb(v + 2, f);
        if (!f) continue;
        shle_release(v, ch);
    }
}

static void shle_note_off(uint32_t t, uint32_t p) {
    uint8_t note = shle_rb(p);
    uint8_t d4 = (uint8_t)(note + shle_rb(t + 4));
    if (d4 & 0x80) return;
    shle_note_off_voices(t, d4, note);
}

/* ---- pitch bend (MidEvt_PBend) ----------------------------------------------------- */

static void shle_pitch_bend(uint32_t t, uint32_t p) {
    uint32_t d2 = ((uint32_t)shle_rb(p + 1) << 7 | shle_rb(p)) >> 5;
    uint32_t d1 = d2;
    if (!(d2 & 0x100)) {
        d2 = (uint8_t)(-(int)(d2 & 0xFF));
        if (!d2) d2 = 0xFF;
    }
    uint8_t range = shle_rb(t + 7);
    if (!range) return;
    uint32_t tbl = shle_ml(SHLE_T_BEND + (uint8_t)(range << 2));
    int16_t d0 = shle_mb(tbl + (d2 & 0xFF));
    if (!(d1 & 0x100)) d0 = (int16_t)-d0;
    SG16(0x1424, (uint16_t)d0);
    uint8_t ch = G8(0x140E) & 0xF;
    shle_ww(SHLE_A6(0x3200) + ch * 2u, (uint16_t)d0);
    bool found = false;
    for (uint32_t t2 = SHLE_TRACKS; t2 < SHLE_TRACKS + SHLE_NTRACKS * 16; t2 += 16)
        if (shle_rb(t2 + 1) == ch && !(shle_rb(t2) & 0x08)) { found = true; break; }
    if (!found) return;
    for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
        if (shle_rb(v + 4) != ch || !shle_rb(v + 2)) continue;
        uint16_t speed = shle_send_pitched(t, v, shle_rw(v), d0);
        shle_ww(SHLE_A6(0x1000) + shle_rw(v) + 0x1A, speed);
    }
}

/* ---- controllers (MidEvt_CtrlChg) ---------------------------------------------------- */

/* sub_603406: every voice on the event's channel released at once */
static void shle_channel_off(void) {
    uint8_t ch = G8(0x140E) & 0xF;
    SG8(0x3220 + ch, 0); SG8(0x3240 + ch, 0); shle_ww(SHLE_A6(0x3200) + ch * 2u, 0);
    for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
        if (!(shle_rb(v + 2) & 0x40) || shle_rb(v + 4) != ch) continue;
        shle_cut(shle_rw(v));
        shle_wb(v + 2, shle_rb(v + 2) | 0x01);
    }
}

/* the voice volume: TL from velocity and the track's final volume */
static void shle_retl_voices(uint8_t ch, uint8_t tvol, bool need_used) {
    for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
        if (need_used && !(shle_rb(v + 2) & 0x40)) continue;
        if (shle_rb(v + 4) != ch) continue;
        uint32_t d6 = (uint32_t)shle_rb(v + 8) * tvol;
        shle_sw8(shle_rw(v) + 0xD, d6 ? (uint8_t)((uint8_t)~(d6 >> 8) >> 1) : 0xFF);
    }
}

static inline uint8_t shle_min(uint8_t v, uint8_t lim, uint8_t to) { return v < lim ? v : to; }

/* the instrument entry the channel last keyed, in RAM (sub_603038 / sub_6032C6) */
static inline uint32_t shle_ins_cache(uint32_t t, bool *ok) {
    uint32_t a2 = shle_rl(SHLE_A6(0x3290) + (uint8_t)(shle_rb(t + 1) << 2));
    *ok = a2 >= 0x9000u;
    return a2;
}

static void shle_ctrl(uint32_t t, uint32_t p) {
    uint8_t cc = shle_rb(p), d0 = shle_rb(p + 1);
    if (cc >= 0x51) return;
    uint8_t ch = shle_rb(t + 1) & 0xF;
    bool ok;
    uint32_t a2;
    switch (cc) {
    case 0x01: case 0x02: {                             /* modulation, breath: the LFO depth */
        uint8_t m = (d0 >> 4) & 7;
        uint32_t tbl = SHLE_A6(cc == 1 ? 0x3220 : 0x3240) + ch;
        uint8_t cur = shle_rb(tbl);
        if ((cur & 0x7F) == m) return;
        shle_wb(tbl, (cur & 0x80) | m);
        for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
            if (shle_rb(v + 4) != ch || !shle_rb(v + 2)) continue;
            uint32_t slot = shle_rw(v);
            if (cc == 1) shle_sw8(slot + 0x13, (shle_sr8(slot + 0x13) & 0x1F) | (((shle_rb(t + 9) & 0xE0u) * m / 7) & 0xE0));
            else         shle_sw8(slot + 0x13, (shle_sr8(slot + 0x13) & 0xF8) | (((shle_rb(t + 9) & 7u) * m / 7) & 7));
        }
        return;
    }
    case 0x07: {                                        /* CtrlB0_SetVol */
        uint8_t vol = (uint8_t)(d0 << 1);
        if (vol) vol++;
        shle_wb(t + 3, vol);
        uint32_t d2 = (shle_rb(t) & 1) ? 0xFF : G8(0x141F);
        uint8_t fin = (uint8_t)((vol * d2) >> 8);
        if (fin) fin++;
        shle_wb(t + 0xA, fin);
        shle_retl_voices(G8(0x140E) & 0xF, fin, true);
        return;
    }
    case 0x0A: {                                        /* CtrlB0_SetPan */
        uint8_t pan = (shle_rb(t + 6) & 0xE0) | shle_mb(SHLE_T_PAN + (d0 & 0x7F));
        shle_wb(t + 6, pan);
        uint8_t c = G8(0x140E) & 0xF;
        for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16)
            if (shle_rb(v + 4) == c && shle_rb(v + 2)) shle_sw8(shle_rw(v) + 0x16, pan);
        return;
    }
    case 0x11: shle_wb(t + 4, (uint8_t)((d0 & 0x7F) - 0x40)); return;
    case 0x12: {
        uint8_t d1 = shle_mb(SHLE_T_PAN + (d0 & 0x7F));
        shle_wb(t + 5, (d1 & 0x10) ? (uint8_t)-(int)(d1 & 0xF) : d1);
        return;
    }
    case 0x13: shle_wb(t + 7, shle_min(d0, 0xC, 0xC)); return;
    case 0x14: if (d0 < 3) shle_ww(t + 8, (shle_rw(t + 8) & 0xFCFF) | (uint16_t)((d0 & 3) << 8)); return;
    case 0x15: if (d0 < 4) shle_ww(t + 8, (shle_rw(t + 8) & 0xFFE7) | (uint16_t)((d0 & 3) << 3)); return;
    case 0x16: shle_wb(t + 9, (shle_rb(t + 9) & 0x1F) | ((shle_min(d0, 7, 7) << 5) & 0xE0)); return;
    case 0x17: shle_wb(t + 9, (shle_rb(t + 9) & 0xF8) | (shle_min(d0, 7, 7) & 7)); return;
    case 0x18: shle_ww(t + 8, (shle_rw(t + 8) & 0x83FF) | (uint16_t)((shle_min(d0, 0x1F, 0x1F) << 10) & 0x7C00)); return;
    case 0x19: shle_wb(t + 6, (shle_rb(t + 6) & 0x1F) | ((shle_min(d0, 8, 7) << 5) & 0xE0)); return;
    case 0x29: shle_wb(t + 0xD, (uint8_t)((d0 & 0xF) << 3) | ((d0 & 0x70) >> 4)); return;
    case 0x2A: shle_wb(t + 0xC, (shle_rb(t + 0xC) & 0x78) | (shle_min(d0, 8, 7) & 7)); return;
    case 0x2B: if (d0 < 0x10) shle_wb(t + 0xC, (shle_rb(t + 0xC) & 7) | ((d0 << 3) & 0x78)); return;
    /* 0x30-0x34 edit the envelope of the instrument entry the channel last keyed */
    case 0x30: a2 = shle_ins_cache(t, &ok); if (ok) shle_mwb(a2 + 7, (shle_mb(a2 + 7) & 0xE0) | (shle_min(d0, 0x1F, 0x1F) & 0x1F)); return;
    case 0x31: a2 = shle_ins_cache(t, &ok); if (ok) shle_mww(a2 + 8, (shle_mw(a2 + 8) & 0xFC1F) | ((shle_min(d0, 0x1F, 0x1F) << 5) & 0x3E0)); return;
    case 0x32: a2 = shle_ins_cache(t, &ok); if (ok) shle_mww(a2 + 6, (shle_mw(a2 + 6) & 0xF81F) | ((shle_min(d0, 0x1F, 0x1F) << 6) & 0x7C0)); return;
    case 0x33: a2 = shle_ins_cache(t, &ok); if (ok) shle_mwb(a2 + 6, (shle_mb(a2 + 6) & 7) | ((shle_min(d0, 0x1F, 0x1F) << 3) & 0xF8)); return;
    case 0x34: if (!d0) return; a2 = shle_ins_cache(t, &ok); if (ok) shle_mwb(a2 + 9, (shle_mb(a2 + 9) & 0xE0) | (shle_min(d0, 0x1F, 0x1F) & 0x1F)); return;
    case 0x35: {                                        /* the loop of the channel's last sample, on or off */
        uint32_t e = (uint32_t)(SHLE_SMPTBL + (int16_t)(uint16_t)(shle_rw(SHLE_A6(0x3270) + (uint8_t)(shle_rb(t + 1) << 1)) << 4));
        if (d0) shle_wl(e + 0xC, shle_rl(e + 4) + shle_rl(e) - shle_rl(e + 8));
        else    shle_wl(e + 0xC, 0);
        return;
    }
    case 0x40: {                                        /* sustain pedal: 0x7F lets go (sic) */
        uint32_t tbl = SHLE_A6(0x3220) + ch;
        if (d0 != shle_mb(SHLE_P_HOLDVAL)) { shle_wb(tbl, shle_rb(tbl) | 0x80); return; }
        for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
            if (shle_rb(v + 4) != ch || !(shle_rb(v + 2) & 0x02)) continue;
            uint32_t slot = shle_rw(v);
            shle_sw8(slot, 0x10);
            uint8_t f = (uint8_t)((shle_rb(v + 2) & ~0x02) | 0x01);
            shle_wb(v + 2, f);
            if (f & 0x04) {
                uint16_t r = shle_rw(SHLE_A6(0x1A00) + slot + 0xA);
                if (r < shle_rw(v + 0xE)) shle_ww(v + 0xE, r);
            }
        }
        shle_wb(tbl, shle_rb(tbl) & 0x7F);
        return;
    }
    case 0x50: if (d0 == 0x7C) shle_channel_off(); return;
    default: break;
    }
    if (cc >= 0x41 && cc <= 0x4C) {
        /* edits of a negative-id (ROM table) sample's instrument entry; only
         * when the channel's last sample was one (sub_6032AE: bmi) */
        if (!(shle_rw(SHLE_A6(0x3270) + (uint8_t)(shle_rb(t + 1) << 1)) & 0x8000)) return;
        a2 = shle_ins_cache(t, &ok);
        if (!ok) return;
        uint8_t hi = d0 == 0x7F ? 0x80 : (uint8_t)(d0 & 0x80);
        switch (cc) {
        case 0x41: shle_mwb(a2 + 0xD, (shle_mb(a2 + 0xD) & 0x80) | (d0 & 0x7F)); break;
        case 0x42: shle_mww(a2 + 0xC, (shle_mw(a2 + 0xC) & 0x7F) | (uint16_t)((d0 & 0x7F) << 7)); break;
        case 0x43: shle_mwb(a2 + 0xF, (shle_mb(a2 + 0xF) & 0x80) | (d0 & 0x7F)); break;
        case 0x44: shle_mww(a2 + 0xE, (shle_mw(a2 + 0xE) & 0x7F) | (uint16_t)((d0 & 0x7F) << 7)); break;
        case 0x45: shle_mwb(a2 + 0xA, (shle_mb(a2 + 0xA) & 8) | (uint8_t)((shle_min(d0, 0xF, 0xF) & 0x1F) << 4)); break;
        case 0x46: shle_mwb(a2 + 0xA, (shle_mb(a2 + 0xA) & 0xF0) | ((d0 == 0x7F ? d0 : 0) & 8)); break;
        case 0x47: shle_mwb(a2 + 0xB, (shle_mb(a2 + 0xB) & 0x7F) | hi); break;
        case 0x48: shle_mwb(a2 + 0xB, (shle_mb(a2 + 0xB) & 0x80) | (d0 & 0x7F)); break;
        case 0x49: shle_mwb(a2 + 0x10, (shle_mb(a2 + 0x10) & 0x7F) | hi); break;
        case 0x4A: shle_mwb(a2 + 0x10, (shle_mb(a2 + 0x10) & 0x80) | (d0 & 0x7F)); break;
        case 0x4B: shle_mwb(a2 + 0x11, (shle_mb(a2 + 0x11) & 0x7F) | hi); break;
        case 0x4C: shle_mwb(a2 + 0x11, (shle_mb(a2 + 0x11) & 0x80) | (d0 & 0x7F)); break;
        }
    }
}

/* ---- program change (MidEvt_InsChg) ------------------------------------------------ */

static void shle_program(uint32_t t, uint32_t p) {
    uint8_t d0 = shle_rb(p);
    uint32_t a2 = shle_ml(SHLE_P_INSDATA);
    if ((int16_t)d0 > (int16_t)shle_mw(a2)) return;
    a2 += shle_mw(a2 + 2 + d0 * 2u);
    if (shle_mb(a2) != 0xFF)            shle_wb(t, 0x80);
    else if (shle_mb(a2 + 1) & 0x80)    shle_wb(t, 0x89);
    else                                shle_wb(t, 0x8A);
    shle_wb(t + 2, d0);
    shle_channel_off();
}

/* ---- the sound commands (DoSoundCmd) ------------------------------------------------- */

/* sub_6015DC / SC05_LoadIns: the sample table, and a set of samples copied
 * into sound RAM at 0x50000 */
static void shle_load_samples(uint8_t set, bool pick) {
    uint32_t a0 = shle_ml(SHLE_P_SMPTBL);
    uint32_t n = (uint32_t)shle_mw(a0) + 1u;
    for (uint32_t k = 0; k < n; k++) shle_wb(SHLE_SMPTBL + k, shle_mb(a0 + 2 + k));
    g_shle.cost += n * SHLE_COST_BYTE;
    uint32_t a1 = shle_ml(SHLE_P_PRELOAD);
    if (pick) {
        set &= 0x7F;
        if (set == 0x7F) return;
        if ((int16_t)set > (int16_t)shle_mw(a1)) return;
    }
    a1 += shle_mw(a1 + 2 + set * 2u);
    int16_t cnt = (int16_t)shle_mw(a1);
    a1 += 2;
    if (cnt < 0) return;
    uint32_t a3 = SHLE_PRELOAD;
    a0 += 2;
    g_shle.stall = true;                                /* the byte loop keeps the main loop from streaming */
    for (int k = 0; k <= cnt; k++) {
        uint32_t d1 = (uint32_t)(uint16_t)(shle_mw(a1) << 4);
        a1 += 2;
        uint32_t d2 = shle_ml(a0 + d1), d3 = shle_ml(a0 + d1 + 8);
        uint32_t len = ((shle_ml(a0 + d1 + 4) - 1) & 0xFFFF) + 1;
        if (shle_ml(a0 + d1 + 12)) d3 -= d2;
        shle_wl(SHLE_SMPTBL + d1 + 8, d3);
        shle_wl(SHLE_SMPTBL + d1, a3 | 0x80000000u);
        shle_copy(a3, d2, len);
        a3 += len;
        g_shle.cost += len * SHLE_COST_BYTE;
        if (a3 > 0x6FFFFu) { LOG_WARN("sound HLE: preloaded samples ran past 0x6FFFF (the driver halts here)"); return; }
    }
}

/* A DSP program (COEF, MADRS, MPRO: 0x500 bytes at 0x700), entry `d0` of
 * the table at 0x608018, 0x7F for none (SoundCmd08). The voices are muted
 * while it goes in. */
static void shle_dsp_program(uint8_t d0) {
    for (uint32_t k = 0; k < 32; k++) {
        shle_wb(SHLE_A6(0x3250) + k, shle_sr8(k * 0x20u + 0xD));
        shle_sw8(k * 0x20u + 0xD, 0xFF);
    }
    uint32_t a1 = shle_ml(0x608018u);
    d0 &= 0x7F;
    if (d0 == 0x7F) {
        for (uint32_t k = 0; k < 0x500; k++) shle_sw8(0x700 + k, 0);
    } else if ((int16_t)d0 <= (int16_t)shle_mw(a1)) {
        a1 += shle_mw(a1 + 2 + d0 * 2u);
        for (uint32_t k = 0; k < 0x500; k += 2) shle_sw16(0x700 + k, shle_mw(a1 + k));
    }
    for (uint32_t k = 0; k < 32; k++) shle_sw8(k * 0x20u + 0xD, shle_rb(SHLE_A6(0x3250) + k));
    g_shle.cost += 0x500u * SHLE_COST_BYTE;
    g_shle.stall = true;
}

/* SoundCmd06: the effect return send (slot +0x17) of the first 18 slots */
static void shle_efsdl(uint8_t d0) {
    uint32_t a1 = shle_ml(SHLE_P_EFSDL);
    d0 &= 0x7F;
    if ((int16_t)d0 > (int16_t)shle_mw(a1)) return;
    a1 += shle_mw(a1 + 2 + d0 * 2u);
    for (uint32_t k = 0; k < 18; k++) shle_sw8(k * 0x20u + 0x17, shle_mb(a1 + k));
}

/* the track table's final volumes (sub_601434's tail, SC07's) */
static void shle_track_volumes(bool need_active) {
    for (uint32_t t = SHLE_TRACKS; t < SHLE_TRACKS + SHLE_NTRACKS * 16; t += 16) {
        uint8_t f = shle_rb(t);
        if (need_active ? !(f & 0x80) : !f) continue;
        uint32_t d3 = (f & 1) ? 0xFF : G8(0x141F);
        uint8_t v = (uint8_t)((shle_rb(t + 3) * d3) >> 8);
        if (v) v++;
        shle_wb(t + 0xA, v);
    }
}

/* the track records of an instrument library entry, and the three fixed
 * effect tracks behind them (sub_601434 / SC07_InitTrkIns) */
static void shle_load_tracks(uint32_t a2, bool fixed_too) {
    shle_mb(a2);                                        /* a byte the driver skips */
    uint8_t d6 = shle_mb(a2 + 1);
    a2 += 2;
    uint32_t fixed = shle_ml(SHLE_P_FIXEDTRK);
    uint16_t d7 = (uint16_t)(0x13 - shle_mb(fixed)) & 0xFF;
    uint32_t a3 = SHLE_TRACKS;
    if (d6 != 0xFF) {
        for (int k = 0; k <= d6; k++) {
            for (int b = 0; b < 16; b++) shle_wb(a3 + b, shle_mb(a2 + b));
            a2 += 16; a3 += 16;
            d7 = (uint16_t)((d7 & 0xFF00) | ((d7 - 1) & 0xFF));
        }
    }
    for (int k = 0; k <= (int)d7; k++) { shle_wb(a3, 0); shle_wb(a3 + 1, 0xFF); a3 += 16; }
    if (!fixed_too) return;
    uint8_t nf = shle_mb(fixed);
    for (uint32_t b = 0; b < nf * 16u; b++) shle_wb(a3 + b, shle_mb(fixed + 1 + b));
}

static void shle_master_volume(uint8_t d0);

/* SC07_InitTrkIns: an instrument library entry */
static void shle_init_tracks(uint8_t d0) {
    d0 &= 0x7F;
    uint32_t a2 = shle_ml(SHLE_P_INSLIB);
    if ((int16_t)d0 > (int16_t)shle_mw(a2)) return;
    a2 += shle_mw(a2 + 2 + d0 * 2u);
    uint8_t b = shle_mb(a2++);
    if (!(b & 0x80)) {
        if (shle_rb(SHLE_SEQS + 0xC)) SG8(0x1421, b);
        else shle_enqueue(0xA0, 0x01, b, 0);
    }
    b = shle_mb(a2++); if (!(b & 0x80)) shle_dsp_program(b);
    b = shle_mb(a2++); if (!(b & 0x80)) shle_efsdl(b);
    b = shle_mb(a2++); if (!(b & 0x80)) shle_load_samples(b, true);
    shle_load_tracks(a2, false);
    shle_track_volumes(true);
}

static void shle_master_volume(uint8_t d0) {             /* SoundCmd01 */
    uint32_t s = SHLE_SEQS;
    if (!shle_rb(s + 0xC)) shle_wb(s + 0xE, d0);
    d0 = (uint8_t)(d0 << 1);
    if (d0) d0++;
    SG8(0x141F, d0);
    for (uint32_t t = SHLE_TRACKS; t < SHLE_TRACKS + SHLE_NTRACKS * 16; t += 16) {
        uint8_t f = shle_rb(t);
        if (!(f & 0x80) || (f & 1)) continue;
        uint8_t v = (uint8_t)((shle_rb(t + 3) * d0) >> 8);
        if (v) v++;
        shle_wb(t + 0xA, v);
        shle_retl_voices(shle_rb(t + 1), v, true);
    }
}

/* voices of one kind cut and their channels' controllers cleared */
static void shle_cut_voices(bool sfx) {
    for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
        uint8_t f = shle_rb(v + 2);
        if (!(f & 0x40) || ((f & 0x08) != 0) != sfx) continue;
        uint32_t slot = shle_rw(v);
        shle_cut(slot);
        shle_stream_clear(slot);
        uint8_t ch = shle_rb(v + 4);
        shle_wb(v + 2, 0); shle_wb(v + 4, 0); shle_wb(v + 9, 0);
        if (sfx) shle_wb(v + 5, 0);
        SG8(0x3220 + ch, 0); SG8(0x3240 + ch, 0); shle_ww(SHLE_A6(0x3200) + ch * 2u, 0);
    }
}

static void shle_cmd00(uint8_t d0) {
    if ((int8_t)d0 > 0xF) return;
    switch (d0) {
    case 0x01:                                          /* everything stops */
        for (uint32_t a = SHLE_SEQS; a < SHLE_SEQS + 0x80; a++) shle_wb(a, 0);
        for (uint32_t a = SHLE_A6(0x1600); a < SHLE_A6(0x1700); a++) shle_wb(a, 0);
        for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
            if (!shle_rb(v + 2)) continue;
            uint32_t slot = shle_rw(v);
            shle_cut(slot); shle_stream_clear(slot);
            shle_wb(v + 2, 0); shle_wb(v + 4, 0); shle_wb(v + 9, 0); shle_wb(v + 5, 0);
        }
        SG8(0x141E, 0); SG8(0x142E, 0);
        for (int c = 0; c < 16; c++) { SG8(0x3220 + c, 0); SG8(0x3240 + c, 0); SG16(0x3200 + c * 2, 0); }
        return;
    case 0x02: {                                        /* the music stops (a song's own A0 00 02 only cuts voices) */
        uint32_t s = SHLE_SEQS;
        if (!(shle_rb(s) & 0x40)) { shle_wb(s, 0); shle_wb(s + 0xC, 0); }
        shle_wb(s, shle_rb(s) & ~0x40u);
        shle_cut_voices(false);
        SG8(0x141E, 0);
        return;
    }
    case 0x03:                                          /* the effects stop */
        for (uint32_t a = SHLE_A6(0x1600); a < SHLE_A6(0x1700); a++) shle_wb(a, 0);
        for (uint32_t a = SHLE_SEQS + 0x10; a < SHLE_SEQS + 0x80; a++) shle_wb(a, 0);
        shle_cut_voices(true);
        SG8(0x142E, 0);
        return;
    case 0x04: {                                        /* pause: every slot's source to silence, and back */
        if (!G8(0x1430)) {
            for (uint32_t k = 0; k < 32; k++) shle_sw8(k * 0x20u, shle_sr8(k * 0x20u) | 1);
            SG8(0x1430, 0x80);
            uint8_t f = shle_rb(SHLE_SEQS);
            shle_wb(SHLE_SEQS, f & 0x7F);
            if (!(f & 0x80)) SG8(0x1430, 0x10);
            for (uint32_t s = SHLE_SEQS + 0x10; s < SHLE_SEQS + 0x80; s += 16)
                if (shle_rb(s) & 0x80) { shle_wb(s, 0); SG8(0x1431, G8(0x1431) | 1); }
            uint16_t w = shle_mw(SHLE_P_PAUSE_ON);
            shle_enqueue(0xA0, (uint8_t)(w >> 8), (uint8_t)w, 0);
        } else {
            if (G8(0x1430) & 0x80) shle_wb(SHLE_SEQS, shle_rb(SHLE_SEQS) | 0x80);
            SG8(0x1430, 0);
            for (uint32_t k = 0; k < 32; k++) shle_sw8(k * 0x20u, shle_sr8(k * 0x20u) & 0x1E);
            uint16_t w = shle_mw(SHLE_P_PAUSE_OFF);
            shle_enqueue(0xA0, (uint8_t)(w >> 8), (uint8_t)w, 0);
        }
        return;
    }
    case 0x09: if (G8(0x1440) != 0xF8) SG8(0x1440, G8(0x1440) + 1); return;   /* tempo */
    case 0x0A: if (G8(0x1440) != 0) SG8(0x1440, G8(0x1440) - 1); return;
    case 0x0B: SG8(0x1440, 0xF8); return;
    case 0x0C: SG8(0x1440, 0xCE); return;
    case 0x0E: SG8(0x142F, G8(0x142F) ? 0 : 0x80); return;
    default: return;
    }
}

/* LoadSong: the music sequencer onto a song's pattern list */
static void shle_load_song(uint32_t a2) {
    uint32_t s = SHLE_SEQS;
    if (!(shle_rb(s) & 0x80) && shle_rb(s + 0xC)) {     /* a fade-in was asked for */
        uint8_t d1 = shle_rb(s + 0xC) & 0x7F;
        for (int b = 0; b < 16; b++) shle_wb(s + b, 0);
        shle_wb(s + 0xC, d1); shle_wb(s + 0xD, d1);
        shle_wb(s, shle_rb(s) | 0x10);
        SG8(0x141F, 0);
        for (uint32_t t = SHLE_TRACKS; t < SHLE_TRACKS + SHLE_NTRACKS * 16; t += 16)
            if ((shle_rb(t) & 0x80) && !(shle_rb(t) & 1)) shle_wb(t + 0xA, 0);
    } else {
        uint8_t d1 = shle_rb(s + 0xE);
        for (int b = 0; b < 16; b++) shle_wb(s + b, 0);
        shle_wb(s + 0xE, d1);
    }
    a2 += 3;
    shle_wl(s + 4, shle_ml(a2));
    shle_wl(s + 8, a2 + 4);
    shle_ww(s + 2, 1);
    shle_wb(s, shle_rb(s) | 0x88);
}

/* loc_6035A0: an effect onto sequencer 1-7 (one with its id, a free one, or 7) */
static void shle_start_sfx(uint8_t d0, uint32_t a2) {
    uint32_t a4 = SHLE_SEQS;
    uint8_t id = d0 & 0xF;
    bool found = false;
    if (d0 & 0x10) {
        for (int k = 0; k < 7; k++) {
            a4 += 16;
            if (shle_rb(a4 + 0xF) == id) { found = true; break; }
        }
        if (!found) a4 = SHLE_SEQS;
    }
    if (!found) {
        for (int k = 0; k < 7; k++) {
            a4 += 16;
            if (!(shle_rb(a4) & 0x80)) break;
        }
    }
    for (int b = 0; b < 16; b++) shle_wb(a4 + b, 0);
    shle_wl(a4 + 4, a2);
    shle_ww(a4 + 2, 1);
    shle_wb(a4, shle_rb(a4) | 0x80);
    shle_wb(a4 + 0xF, id);
}

static void shle_sound_cmd(uint32_t p) {
    uint8_t d0 = shle_rb(p) & 0x7F;
    uint8_t d2 = d0;
    p++;
    if (d0 >= 0x70) {
        if (!g_shle.warned_a7x) { LOG_WARN("sound HLE: engine command A? %02X not modelled", d0); g_shle.warned_a7x = true; }
        g_shle.unknown++;
        return;
    }
    if (d0 >= 0x10) {
        uint32_t a4 = shle_ml(SHLE_P_MUSIC), a2 = a4;
        uint16_t g = (uint16_t)(d0 - 0x10);
        if ((int16_t)g > (int16_t)shle_mw(a4)) return;
        a4 += shle_mw(a4 + 2 + g * 2u);
        uint8_t e = shle_rb(p) & 0x7F;
        p++;
        if ((int16_t)e > (int16_t)shle_mw(a4)) return;
        a2 += shle_mw(a4 + 2 + e * 2u);
        uint8_t type = shle_mb(a2++);
        if (type & 0x80) shle_start_sfx(type, a2);
        else if (!type)  shle_load_song(a2);
        else { g_shle.unknown++; LOG_WARN("sound HLE: group %u entry %u has type %02X", g, e, type); }
        return;
    }
    uint8_t arg = shle_rb(p);
    uint32_t s = SHLE_SEQS;
    switch (d2) {
    case 0x00: shle_cmd00(arg); return;
    case 0x01: shle_master_volume(arg); return;
    case 0x03:                                          /* fade the music out */
        if (shle_rb(s) & 0x80) { SG8(0x1420, G8(0x141F)); SG8(0x1421, shle_rb(s + 0xE)); shle_wb(s + 0xC, arg); shle_wb(s + 0xD, arg); }
        return;
    case 0x04:                                          /* fade the next song in */
        if (!(shle_rb(s) & 0x80)) {
            SG8(0x1420, G8(0x141F)); SG8(0x1421, shle_rb(s + 0xE));
            shle_wb(s + 0xC, arg | 0x80); shle_wb(s + 0xD, arg | 0x80);
        }
        return;
    case 0x05: shle_load_samples(arg, true); return;
    case 0x06: shle_efsdl(arg); return;
    case 0x07: shle_init_tracks(arg); return;
    case 0x08: shle_dsp_program(arg); return;
    case 0x09: {
        uint32_t o = (uint32_t)(G8(0x140E) & 0xF) << 5;
        shle_sw8(o + 0x17, (shle_sr8(o + 0x17) & 0x1F) | ((shle_min(arg, 7, 7) << 5) & 0xE0));
        return;
    }
    case 0x0A:
        if (arg < 0x20) {
            uint32_t o = (uint32_t)(G8(0x140E) & 0xF) << 5;
            shle_sw8(o + 0x17, (shle_sr8(o + 0x17) & 0xE0) | shle_mb(SHLE_T_EFPAN + (arg & 0x1F)));
        }
        return;
    default: return;
    }
}

/* ---- the sequencers (sub_603DA2) and the fade (sub_604096) ---------------------------------- */

static void shle_seq(uint32_t s) {
    if (!(shle_rb(s) & 0x80)) return;
    uint16_t w = (uint16_t)(shle_rw(s + 2) - 1);
    shle_ww(s + 2, w);
    if (w) return;
    uint32_t a3 = shle_rl(s + 4), a0 = shle_rl(s + 8);
    for (int guard = 0; guard < 4096; guard++) {
        if (a3 & 0x80000000u) goto list;
        {
            shle_wb(s, shle_rb(s) & ~1u);
            uint8_t st = shle_mb(a3++);
            if (!(st & 0x80)) { st = shle_rb(s + 1); a3--; }
            shle_wb(s + 1, st);
            uint8_t b1, b2;
            switch (st >> 4) {
            case 0x8:
                b1 = shle_mb(a3++);
                if (b1 & 0x80) shle_wb(s, shle_rb(s) | 1);
                shle_enqueue3(st, b1 & 0x7F, 0x7F);
                break;
            case 0xA:
                b1 = shle_mb(a3++);
                if (b1 == 0) {
                    if ((shle_mb(a3) & 0x7F) == 2) shle_wb(s, shle_rb(s) | 0x40);
                } else if (b1 == 1 && shle_rb(s + 0xC)) {
                    b2 = shle_mb(a3++);
                    if (shle_rb(s) & 0x10) SG8(0x1421, b2 & 0x7F);
                    if (b2 & 0x80) shle_wb(s, shle_rb(s) | 1);
                    goto after;
                }
                b2 = shle_mb(a3++);
                if (b2 & 0x80) shle_wb(s, shle_rb(s) | 1);
                shle_enqueue3(st, b1, b2 & 0x7F);
                break;
            case 0xC: case 0xD:
                b1 = shle_mb(a3++);
                if (b1 & 0x80) shle_wb(s, shle_rb(s) | 1);
                shle_enqueue3(st, b1 & 0x7F, 0);
                break;
            case 0xF:
                if (st == 0xF7) { a3 += 1u + shle_mb(a3); goto after; }
                if (st == 0xF0) { while (shle_mb(a3++) != 0xF7) {} goto after; }
                if (st == 0xFF) {
                    if (shle_mb(a3++) != 0x2F) { a3 += 1u + shle_mb(a3); goto after; }
                }
                goto seg_end;
            default:                                    /* 9x, Bx, Ex (and 0x-7x, which never come) */
                b1 = shle_mb(a3++);
                b2 = shle_mb(a3++);
                if (b2 & 0x80) shle_wb(s, shle_rb(s) | 1);
                shle_enqueue3(st, b1, b2 & 0x7F);
                break;
            }
        after:
            if (shle_rb(s) & 1) continue;
            {
                uint16_t d = shle_mb(a3++);
                if (!d) continue;
                if (d & 0x80) d = (uint16_t)((d & 0x7F) << 7 | shle_mb(a3++));
                shle_ww(s + 2, d);
                shle_wl(s + 4, a3);
                return;
            }
        }
    seg_end:
        if (!(shle_rb(s) & 0x08)) { shle_wb(s, 0); return; }
        a0 = shle_rl(s + 8);
        a3 = shle_ml(a0); a0 += 4;
    list:
        if ((a3 & 0xFF000000u) == 0x80000000u) {        /* a command in the pattern list */
            uint32_t d0 = a3 << 8;
            shle_enqueue((uint8_t)(d0 >> 24), (uint8_t)(d0 >> 16), (uint8_t)(d0 >> 8), (uint8_t)d0);
            a0 = shle_rl(s + 8);
            if (d0 == 0xA0000200u) shle_wb(s, shle_rb(s) | 0x40);
            a3 = shle_ml(a0); a0 += 4;
            shle_wl(s + 8, a0);
            continue;
        }
        if (a3 == 0xFFFFFFFFu) { shle_wb(s, 0); return; }
        if (a3 == 0xFFFFFFF1u) { a0 = shle_ml(a0); a3 = shle_ml(a0); a0 += 4; shle_wl(s + 8, a0); continue; }
        if (a3 == 0xFFFFFFF2u) { for (uint32_t a = SHLE_SEQS; a < SHLE_SEQS + 0x80; a++) shle_wb(a, 0); return; }
        shle_wl(s + 8, a0);
        if (a3 & 0x80000000u) break;                    /* the driver would spin here */
    }
    LOG_WARN("sound HLE: sequencer at 0x%04X ran away (0x%08X); stopped", s, a3);
    shle_wb(s, 0);
}

static void shle_fade(void) {
    uint32_t s = SHLE_SEQS;
    if (!(shle_rb(s) & 0x80) || !shle_rb(s + 0xC)) return;
    shle_wb(s + 0xD, shle_rb(s + 0xD) - 1);
    if (shle_rb(s + 0xD)) return;
    if (shle_rb(s) & 0x10) {                             /* in */
        shle_wb(s + 0xE, shle_rb(s + 0xE) + 2);
        if (shle_rb(s + 0xE) < G8(0x1421)) {
            shle_enqueue3(0xA0, 0x01, shle_rb(s + 0xE));
            shle_wb(s + 0xD, shle_rb(s + 0xC));
        } else {
            shle_wb(s, shle_rb(s) & ~0x10u);
            shle_wb(s + 0xC, 0);
            SG8(0x141F, G8(0x1420));
            shle_enqueue3(0xA0, 0x01, G8(0x1421));
        }
        return;
    }
    uint8_t e = (uint8_t)(shle_rb(s + 0xE) - 2);         /* out */
    shle_wb(s + 0xE, e);
    if (!(e & 0x80)) {
        shle_enqueue3(0xA0, 0x01, e);
        shle_wb(s + 0xD, shle_rb(s + 0xC));
        return;
    }
    shle_wb(s, shle_rb(s) & 0x7F);
    shle_wb(s + 0xC, 0);
    shle_enqueue(0xA0, 0x00, 0x02, 0x00);
    SG8(0x141F, G8(0x1420));
    shle_enqueue3(0xA0, 0x01, G8(0x1421));
}

/* ---- the interrupts ------------------------------------------------------------ */

/* Int3_MidiIn: one byte off the SCSP's MIDI buffer, framed into the queue */
static bool shle_midi_byte(uint8_t d0) {
    if (d0 == 0xFF) return false;                       /* the driver restarts */
    if (d0 >= 0xF0) return true;
    uint8_t d1;
    if (d0 & 0x80) {
        SG8(0x1400, d0);
        d1 = (d0 >= 0xC0 && d0 < 0xE0) ? 1 : 2;
        SG8(0x1402, d1);
    } else {
        d1 = G8(0x1403);
        if (!d1) d1 = G8(0x1402);
        d1--;
        if (d1) SG8(0x1401, d0);
        else if (G8(0x1406) != 0xFF) {
            if ((uint8_t)(G8(0x1402) - 1)) shle_enqueue3(G8(0x1400), G8(0x1401), d0);
            else {
                uint32_t a = SHLE_QUEUE + (G16(0x1404) & 0x3FFu);
                shle_enqueue(G8(0x1400), d0, shle_rb(a + 2), shle_rb(a + 3));
            }
            shle_led();
        }
    }
    SG8(0x1403, d1);
    return true;
}

/* Interrupt1 (timer A): release countdowns; a voice whose count runs out is freed */
static void shle_timer_a(void) {
    for (uint32_t v = SHLE_VOICES; v < SHLE_VOICES_END; v += 16) {
        uint8_t f = shle_rb(v + 2);
        if (!(f & 0x01)) continue;
        uint16_t e = (uint16_t)(shle_rw(v + 0xE) - 1);
        shle_ww(v + 0xE, e);
        if (e) continue;
        uint32_t slot = shle_rw(v);
        if (!(f & 0x08)) {
            shle_sw8(slot + 0xB, 0x1F);
            shle_stream_clear(slot);
            shle_wb(v + 2, 0);
            uint8_t d2 = shle_rb(v + 9);
            shle_wb(v + 9, 0);
            SG8(0x141E, G8(0x141E) - 1);
            shle_music_order_close(d2, true);
        } else {
            shle_cut(slot);
            shle_stream_clear(slot);
            shle_wb(v + 2, 0);
            uint8_t d2 = shle_rb(v + 5);
            shle_wb(v + 9, 0); shle_wb(v + 5, 0);
            SG8(0x142E, G8(0x142E) - 1);
            shle_sfx_order_close(d2);
        }
    }
}

/* The driver's main loop, less the streaming: every queued event to the
 * tracks listening on its channel (loc_601A20). */
static void shle_dispatch(uint64_t now) {
    int guard = 0;
    while (G8(0x1406) && now >= g_shle.busy_until && guard++ < 512) {
        SG8(0x1406, G8(0x1406) - 1);
        uint32_t p = SHLE_QUEUE + (G16(0x1408) & 0x3FFu);
        uint8_t st = shle_rb(p);
        SG8(0x140E, st);
        g_shle.events++;
        g_shle.cost = SHLE_COST_EVENT;
        g_shle.stall = false;
        if ((st & 0xF0) == 0xA0) shle_sound_cmd(p + 1);
        else {
            uint8_t ch = st & 0xF;
            for (uint32_t t = SHLE_TRACKS; t < SHLE_TRACKS + SHLE_NTRACKS * 16; t += 16) {
                if (shle_rb(t + 1) != ch || !(shle_rb(t) & 0x80)) continue;
                switch (st >> 4) {
                case 0x8: shle_note_off(t, p + 1); break;
                case 0x9: shle_note_on(t, p + 1); break;
                case 0xB: shle_ctrl(t, p + 1); break;
                case 0xC: shle_program(t, p + 1); break;
                case 0xE: shle_pitch_bend(t, p + 1); break;
                default: break;
                }
            }
        }
        SG16(0x1408, (G16(0x1408) + 4) & 0x3FF);
        g_shle.busy_until = (g_shle.busy_until > now ? g_shle.busy_until : now) + g_shle.cost;
        if (g_shle.stall) g_shle.stall_until = g_shle.busy_until;
    }
}

/* ---- boot (EntryPoint) --------------------------------------------------------------- */

static void shle_boot(void) {
    scsp_t *s = &g_sound.scsp;
    memset(g_sound.ram, 0, SOUND_RAM_SIZE);
    g_shle.booted = false;
    g_shle.restart_at = 0;
    g_shle.busy_until = g_shle.stall_until = 0;
    g_shle.l2_free = 0;
    g_shle.pass_left = SHLE_PASS_SAMPLES;
    g_shle.boot_at = g_sound.m68k.cpu.cycles + SHLE_BOOT_CYCLES;
    shle_sw8(0x400, 0x03);                              /* 18-bit DAC */
    shle_sw8(0x401, 0x00);                              /* InitSCSP: master volume 0 while it sets up */
    uint32_t dsp = shle_ml(0x608018u);                  /* DSP program 0 */
    dsp += shle_mw(dsp + 2);
    for (uint32_t k = 0; k < 0x500; k += 2) shle_sw16(0x700 + k, shle_mw(dsp + k));
    shle_sw16(0x402, 0x138);                            /* the delay line: 0x70000, 64 KB */
    for (int k = 0; k < 4; k++)                         /* four reads of MIBUF: what waits in the FIFO is lost */
        if (s->mi_r != s->mi_w) { s->mi_r = (uint8_t)((s->mi_r + 1) & 31); s->mi_taken++; }
    for (uint32_t k = 0; k < 32; k++) {
        shle_sw8(k * 0x20u + 0xB, 0xFF);
        shle_sw8(k * 0x20u + 0xD, 0xFF);
        shle_sw8(k * 0x20u + 0x0, 0x10);
    }
    SG8(0x1426, 0xEF);                                  /* sub_60141E */
    /* sub_601434: instrument library entry 0, its commands queued */
    uint32_t a1 = shle_ml(SHLE_P_INSLIB);
    a1 += shle_mw(a1 + 2);
    static const uint8_t cmd[4] = { 0x01, 0x08, 0x06, 0x05 };
    for (int k = 0; k < 4; k++) {
        uint8_t b = shle_mb(a1++);
        if (!(b & 0x80)) shle_enqueue(0xA0, cmd[k], b, 0);
    }
    shle_load_tracks(a1, true);
    SG8(0x141F, 0xFF);
    shle_track_volumes(false);
    SG32(0x141A, SHLE_VOICES);
    for (uint32_t k = 0; k < 32; k++) {                /* sub_601568, sub_60158A */
        uint32_t v = SHLE_VOICES + k * 16u, r = SHLE_A6(0x1000) + k * 0x20u;
        shle_ww(v, k * 0x20u);
        shle_wb(v + 0xD, 0x10 + 2 * k);
        shle_wl(r + 0xE, 0x10000u + k * 0x2000u);
        shle_wb(r + 0x12, 2 * k);
        shle_wb(r + 0x16, 8 + 8 * k);
    }
    shle_efsdl(0);                                      /* sub_6015BC */
    shle_load_samples(0, false);                        /* sub_6015DC */
    uint32_t ins = shle_ml(SHLE_P_INSDATA);             /* CopyInsTable */
    uint32_t nins = (shle_ml(SHLE_P_MUSIC) - ins) & 0x7FFF;
    for (uint32_t k = 0; k < nins; k++) shle_wb(SHLE_INSTBL + k, shle_mb(ins + k));
    for (uint32_t k = 0; k < 0x1000; k++) shle_wb(0x100 + k, shle_mb(0x600100u + k));   /* sub_601680 */
    uint32_t a0 = shle_ml(SHLE_P_A7XTBL);               /* sub_601696 */
    for (uint32_t k = 0; k < 16; k++) {
        uint8_t d2 = shle_mb(a0 - 0x10 + k);
        uint32_t a3 = a0 + shle_mw(a0 + 2 + d2 * 2u);
        uint32_t r = SHLE_A6(0x1600) + k * 16u;
        shle_wb(r + 1, d2);
        shle_wb(r + 3, shle_mb(a3 + 2));
        shle_wb(r + 4, shle_mb(a3 + 3));
    }
    SG8(0x1440, 0xCE);
    SG8(0x1441, 0xCE);
}

/* Is this a driver the HLE knows? STF's program ROM, byte for byte where it
 * matters: the tables' pointer block and the three routines it would stand in
 * for. */
static bool shle_driver_known(void) {
    if (!g_sound.rom_loaded) return false;
    static const uint8_t sig[] = { 0x00, 0x60, 0xA3, 0xFC, 0x00, 0x60, 0xC9, 0xE4, 0x00, 0x60, 0xA1, 0x94 };
    if (memcmp(g_sound.rom + 0x8000, sig, sizeof sig) != 0) return false;
    uint32_t h = 2166136261u;                            /* FNV-1a over the code */
    for (uint32_t a = 0x1000; a < 0x5000; a++) h = (h ^ g_sound.rom[a]) * 16777619u;
    return h == 0x94FDE5EFu;
}

static uint32_t shle_code_hash(void) {
    uint32_t h = 2166136261u;
    for (uint32_t a = 0x1000; a < 0x5000; a++) h = (h ^ g_sound.rom[a]) * 16777619u;
    return h;
}

/* ---- the run ------------------------------------------------------------------------ */

static inline uint64_t shle_period(uint8_t reload, unsigned prescale) {
    return (((uint64_t)(255u - reload)) << prescale) << 8;
}

/* A handler that ran `dur` clock periods from `at`: the main loop was
 * suspended for it. */
static inline void shle_suspend(uint64_t at, uint32_t dur) {
    if (g_shle.busy_until > at) g_shle.busy_until += dur;
}

/* When the timer due at d (1/16 periods) gets its handler: after the
 * interrupt latency, and not while a level 2 or 3 handler runs. */
static inline uint64_t shle_entry(uint64_t d) {
    uint64_t e = d / SHLE_T16 + SHLE_IRQ_LAT + ((SHLE_REFILL_WAIT * (uint64_t)g_shle.refill_frac) >> 16);
    return e > g_shle.l2_free ? e : g_shle.l2_free;
}

/* The board at clock period `now`: MIDI bytes, then any timer handler whose
 * turn has come, then the main loop's dispatch. */
static void shle_events(uint64_t now) {
    if (!g_shle.booted) {
        if (g_shle.restart_at && now >= g_shle.restart_at) {
            uint64_t boot_at = g_shle.boot_at;
            shle_boot();
            g_shle.boot_at = boot_at;
        }
        if (now < g_shle.boot_at) return;
        g_shle.booted = true;
        shle_sw8(0x401, 0x0F);                          /* master volume 15, the last thing before the interrupts */
        /* all three expired long ago; the board takes them A, B, C, a few
         * hundred clock periods apart, and that is their phase from then on */
        g_shle.due_a = (now - SHLE_IRQ_LAT) * SHLE_T16;
        g_shle.due_b = (now + 225u - SHLE_IRQ_LAT) * SHLE_T16;
        g_shle.due_c = (now + 828u - SHLE_IRQ_LAT) * SHLE_T16;
        g_shle.l2_free = now;
    }
    scsp_t *s = &g_sound.scsp;
    while (s->mi_r != s->mi_w) {
        uint8_t b = s->mi[s->mi_r];
        s->mi_r = (uint8_t)((s->mi_r + 1) & 31);
        s->mi_taken++;
        g_shle.l2_free = (g_shle.l2_free > now ? g_shle.l2_free : now) + SHLE_DUR_MIDI;
        shle_suspend(now, SHLE_DUR_MIDI);
        if (!shle_midi_byte(b)) {                       /* 0xFF: the driver starts over */
            g_shle.booted = false;
            g_shle.restart_at = now + SHLE_INIT_CYCLES;
            g_shle.clear_at = now + SHLE_CLEAR_DELAY;
            g_shle.cleared = 0;
            g_shle.boot_at = now + SHLE_BOOT_CYCLES;
            return;
        }
    }
    for (int guard = 0; guard < 16; guard++) {
        uint64_t eb = shle_entry(g_shle.due_b), ec = shle_entry(g_shle.due_c), ea = shle_entry(g_shle.due_a);
        uint64_t e = eb;
        int which = 1;                                  /* B's bit is tested first */
        if (ec < e) { e = ec; which = 2; }
        if (ea < e) { e = ea; which = 0; }
        if (e > now) break;
        g_shle.enq = 0;
        if (which == 1) {                               /* Int2_Timer, timer B: the music */
            g_shle.due_b = (e + SHLE_RELOAD_AT + shle_period(G8(0x1440), 0)) * SHLE_T16;
            if (G8(0x1406) != 0xFF) { shle_fade(); shle_seq(SHLE_SEQS); }
            uint32_t dur = SHLE_DUR_B + g_shle.enq * SHLE_DUR_EVENT;
            g_shle.l2_free = e + dur;
            shle_suspend(e, dur);
        } else if (which == 2) {                        /* timer C: the effects */
            g_shle.due_c = (e + SHLE_RELOAD_AT + shle_period(G8(0x1441), 0)) * SHLE_T16;
            if (G8(0x1406) != 0xFF)
                for (uint32_t k = 1; k < 8; k++) shle_seq(SHLE_SEQS + k * 16u);
            uint32_t dur = SHLE_DUR_C + g_shle.enq * SHLE_DUR_EVENT;
            g_shle.l2_free = e + dur;
            shle_suspend(e, dur);
        } else {                                        /* Interrupt1, timer A: releases */
            g_shle.due_a = (e + SHLE_RELOAD_AT_A + shle_period(0xC0, 3)) * SHLE_T16;
            shle_timer_a();
            shle_suspend(e, SHLE_DUR_A);
        }
        shle_dispatch(now);
    }
    shle_dispatch(now);
}

static inline uint64_t shle_next_due(void) {
    if (!g_shle.booted) return g_shle.restart_at && g_shle.restart_at < g_shle.boot_at ? g_shle.restart_at : g_shle.boot_at;
    uint64_t d = shle_entry(g_shle.due_b), x = shle_entry(g_shle.due_c), y = shle_entry(g_shle.due_a);
    if (x < d) d = x;
    if (y < d) d = y;
    if (G8(0x1406) && g_shle.busy_until < d) d = g_shle.busy_until;
    return d;
}

/* sound_run for the HLE: n samples of board time. The chip makes them as it
 * does under the 68000 (scsp_tick, scsp_sync). */
static void shle_run(uint32_t n) {
    m68k_state_t *m = &g_sound.m68k;
    scsp_t *s = &g_sound.scsp;
    uint64_t next = shle_next_due();
    for (uint32_t i = 0; i < n; i++) {
        m->cpu.cycles += SOUND_CYCLES_PER_SAMPLE;
        uint64_t now = m->cpu.cycles;
        if (g_sound.uart.count) sound_uart_service(&g_sound, now);
        if (now >= next || (g_shle.booted && s->mi_r != s->mi_w)) {
            shle_events(now);
            next = shle_next_due();
        }
        if (--g_shle.pass_left == 0) {                  /* the main loop's pass over the slots */
            g_shle.pass_left = SHLE_PASS_SAMPLES;
            if (g_shle.restart_at && now > g_shle.clear_at && g_shle.cleared < SOUND_RAM_SIZE) {   /* EntryPoint's clear */
                uint64_t to = (now - g_shle.clear_at) / SHLE_CLEAR_BYTE;
                if (to > SOUND_RAM_SIZE) to = SOUND_RAM_SIZE;
                if (to > g_shle.cleared) {
                    scsp_ram_touch(s, g_shle.cleared, (uint32_t)to - g_shle.cleared, 1);
                    memset(g_sound.ram + g_shle.cleared, 0, (uint32_t)to - g_shle.cleared);
                    g_shle.cleared = (uint32_t)to;
                }
            }
            if (g_shle.booted && now >= g_shle.stall_until) {
                g_shle.chunks = 0;
                for (unsigned k = 0; k < 32; k++) shle_stream_step(k);
                /* this pass's share of the main loop in refills, into a running average */
                uint64_t f = ((uint64_t)g_shle.chunks * SHLE_CHUNK_CYCLES << 16) / (SHLE_PASS_SAMPLES * SOUND_CYCLES_PER_SAMPLE);
                if (f > 0x10000u) f = 0x10000u;
                g_shle.refill_frac = (uint32_t)((g_shle.refill_frac * 15u + f) / 16u);
            }
        }
        scsp_tick(s);
    }
    scsp_sync(s);
}

/* Take the HLE for this board, or leave it: at a reset (sound_reset). */
static inline void shle_reset(void) {
    g_shle.on = g_sound_hle_want && shle_driver_known();
    g_shle.events = g_shle.keyons = g_shle.unknown = 0;
    g_shle.warned_fm = g_shle.warned_a7x = false;
    /* The driver's init comes 0.4 s after reset (SHLE_INIT_CYCLES), as on the
     * board -- by then the host has also handed over the sample ROMs, which
     * the boot's preload copies from. */
    g_shle.booted = false;
    g_shle.restart_at = g_sound.m68k.cpu.cycles + SHLE_INIT_CYCLES;
    g_shle.clear_at = g_sound.m68k.cpu.cycles + SHLE_CLEAR_DELAY;
    g_shle.cleared = 0;
    g_shle.boot_at = g_sound.m68k.cpu.cycles + SHLE_BOOT_CYCLES;
}

#undef G8
#undef G16
#undef G32
#undef SG8
#undef SG16
#undef SG32

#endif /* SOUND_HLE_H */
