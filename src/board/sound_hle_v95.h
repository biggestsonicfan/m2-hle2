/*
 * sound_hle_v95.h — Sega's 1994-95 Model 2 sound driver (Virtua Fighter 2's
 * build; Sega Rally's is a later one, two thirds the same code, still on the
 * 68000), in C in place of the 68000. It is older than STF's driver
 * (sound_hle.h) and CRX_drv (sound_hle_crx.h): no header at 0x608000, its
 * tables at fixed places in its code. sound_hle.h includes it and runs it when
 * the program ROM's code is one of these builds (shle_drv_t.v95, k_v95_builds);
 * the chip, the MIDI FIFO and the board's clock are sound_hle.h's.
 *
 * The driver's RAM (a6 = 0x1000; a5 the SCSP):
 *   0x1000 the event queue, 4 bytes an event, 0x1000 bytes
 *   0x2000 32 stream records x 0x20 (one a slot)
 *   0x2500 the globals                      0x2800 32 voices x 16
 *   0x3000 eight sequences x 0x20 (0 the music, 1-7 effects)
 *   0x4000 20 tracks x 16                   0x4200 a pitch bend word a channel
 *   0x4220 CC 1 a channel (bit 7: the pedal) 0x4240 CC 2 a channel
 *   0x4250 the slots' TL over a DSP load    0x5000 the sample table
 *   0x50000 the preloaded samples (to 0x6FFFF)
 * Timer A (level 2) runs the sequencers, timer B (level 1) the release
 * countdowns; the main loop streams, a record at a time, and takes one queued
 * event at each record that is not streaming.
 *
 * Voice: +0 slot, +2 flags, +3 note value, +4 channel, +5 effect order, +6
 * sample, +8 velocity, +9 music order (0xFF: an effect), +A split transpose,
 * +B countdown, +C stream window.
 * Flags: 0 counting down, 1 held by the pedal, 3 effect, 4 drum, 6 keyed on.
 * Track: +0 flags (7 on, 0 fixed level), +1 channel, +2 program (>= 0xF0
 * drums), +3 volume, +4 transpose, +5 tune, +6 pan, +7 bend range, +8 LFO
 * word, +A level, +C tone, +D tone once.
 * Record: +0 flags (7 streaming, 0 looped, 3 its last chunk in), +1 ring
 * position, +2 source, +6 destination, +A bytes left, +E window, +12 (top
 * byte its slot x 2) loop source, +16 (top byte the next slot's monitor
 * select) loop length, +1A pace, +1C its accumulator, +1E end.
 */
#ifndef SOUND_HLE_V95_H
#define SOUND_HLE_V95_H

#define V95_QUEUE    0x1000u
#define V95_RECS     0x2000u
#define V95_RS       0x2500u   /* MIDI in: status, first data byte, length, left */
#define V95_QW       0x2504u   /* queue write and read pointers (words), the count (byte) */
#define V95_QN       0x2506u
#define V95_QR       0x2508u
#define V95_TONE     0x250Du
#define V95_STATUS   0x250Eu
#define V95_NOTE     0x250Fu
#define V95_VEL      0x2510u
#define V95_PAN      0x2511u
#define V95_FREQ     0x2512u   /* word: a drum's octave and note */
#define V95_NVAL     0x2515u
#define V95_SMPW     0x2516u
#define V95_SPLIT    0x2518u
#define V95_NEXTV    0x251Au   /* long */
#define V95_NMUS     0x251Eu
#define V95_MVOL     0x251Fu
#define V95_BENDW    0x2524u
#define V95_LED      0x2526u
#define V95_NEFF     0x252Eu
#define V95_VOICES   0x2800u
#define V95_VEND     0x2A00u
#define V95_SEQS     0x3000u
#define V95_TRACKS   0x4000u
#define V95_BEND     0x4200u
#define V95_CC1      0x4220u
#define V95_CC2      0x4240u
#define V95_TLSAVE   0x4250u
#define V95_SMPTBL   0x5000u
#define V95_PRELOAD  0x50000u

/* A build: where its code keeps its tables */
typedef struct {
    uint32_t vel, drums, notes, splits, drumfreq, phase, pitch, bend, release, pan;
    uint32_t trk, trk2, dsp, efsdl, smptbl, preload, songs, vinit_w, vinit_l;
    uint64_t boot_fixed;    /* the init's delay loops and its fixed copies: reset -> the first interrupt,
                             * less SHLE_INIT_CYCLES and the copies the game's sizes */
    uint32_t engines;       /* Sega Rally's engine sounds (sound_hle_rally.h) */
    bool rally;             /* epr-17890: its own voices, records and engines */
    uint32_t reload_a;      /* timer A's handler entered -> its reload written, and the mean wait
                             * for the instruction it comes in (vf2's A: 12770 a tick) */
    uint32_t midi_msg;      /* Int3's extra for a byte that ends a message (queued, logged) */
    uint32_t refill_wait;   /* a timer's extra wait in a refill's movem.l (0: none, vf2's fit) */
} v95_build_t;

static const v95_build_t k_v95_builds[] = {
    { 0 },
    { 0x60C6A6u, 0x6032C6u, 0x60C3E6u, 0x6053AAu, 0x60C4A6u, 0x60C726u, 0x602676u, 0x602B26u, 0x600C48u, 0x60179Cu,
      0x603066u, 0x6030A6u, 0x60493Eu, 0x605368u, 0x605B26u, 0x608DF0u, 0x608E12u, 0x6006E6u, 0x600726u,
      25380238ull, 0, false, 155u, 0, 0 },
    { 0x603F24u, 0x60882Eu, 0x603C64u, 0, 0x603D24u, 0x603FA4u, 0x6057E4u, 0x605C94u, 0x600D5Cu, 0x601A5Eu,
      0x606236u, 0x6061D4u, 0x606384u, 0x608804u, 0x609C24u, 0x60B40Eu, 0x602F8Eu, 0x60078Cu, 0x6007CCu,
      139972ull, 0x606DAEu, true, 150u, 420u, 33u },
};
#define V95_B (&k_v95_builds[g_shle_drv->v95])
#define V95_SEQ_STRIDE (V95_B->rally ? 0x10u : 0x20u)

/* Sega Rally's own (sound_hle_rally.h) */
static bool rl_midi_byte(uint8_t b);
static uint32_t rl_timer_b_body(void);
static void rl_dispatch_event(uint8_t st, uint32_t p);
static void rl_rec_step(unsigned k);
static void rl_boot_tracks(void);
static void rl_boot_engines(void);
static void rl_samples(uint8_t d0, uint32_t top);

/* Board time, clock periods (medians off the 68000 under a song and effects,
 * vf2: tools/grade-sound-hle.py --v95) */
#define V95_RELOAD_AT_B  150u
#define V95_DUR_MIDI     453u
#define V95_DUR_A        1059u     /* the fade and the eight sequencers, nothing queued */
#define V95_DUR_EVENT    390u      /* ...and each event they queue */
#define V95_DUR_B        2300u     /* the countdowns over 32 voices */
#define V95_COST_FREE    700u      /* a voice freed: the 32 renumbered */
#define V95_COST_EVENT   2160u     /* an event over the 20 tracks; and on a track: */
#define V95_COST_NOTE    600u      /* a note-on's lookups */
#define V95_COST_KEYON   4400u
#define V95_COST_SCAN    2700u     /* a note-off's, a bend's, a controller's look over the 32 voices */
#define V95_COST_SONG    1100u
#define V95_EVENT_LEAD   3300u     /* an event's start -> its key-on written: the HLE writes at the start */
#define V95_COST_BYTE    23u       /* a move.b loop */
#define V95_LOOP_LAT     200u      /* an rte -> the main loop's look at the queue */
#define V95_PASS_SAMPLES 15u       /* the main loop over 32 idle records, 118 clock periods each */

static void shle_clear_step(scsp_t *s, uint64_t now);

static inline void v95_inc(uint32_t a, int d) { shle_wb(a, (uint8_t)(shle_rb(a) + d)); }
static inline uint8_t v95_ch(void) { return shle_rb(V95_STATUS) & 0xF; }
static inline uint8_t v95_tl(uint32_t p) { return (uint8_t)((uint8_t)~(p >> 8) >> 1); }
static inline uint8_t v95_level(uint8_t vol, uint8_t m) {
    uint8_t l = (uint8_t)((vol * m) >> 8);
    return l ? (uint8_t)(l + 1) : 0;
}
static inline uint8_t v95_x2(uint8_t v) { uint8_t d = (uint8_t)(v << 1); return d ? (uint8_t)(d + 1) : 0; }

/* ---- the queue, MIDI in ------------------------------------------------------- */

static void v95_enqueue(uint8_t a, uint8_t b, uint8_t c) {   /* a sequencer's three bytes */
    g_shle.enq++;
    uint16_t w = shle_rw(V95_QW);
    uint32_t p = V95_QUEUE + (w & 0xFFFu);
    shle_wb(p, a);
    shle_wb(p + 1, b);
    shle_wb(p + 2, c);
    shle_ww(V95_QW, (w + 4u) & 0xFFFu);
    v95_inc(V95_QN, 1);
}

static void v95_led(void) {                            /* 0x60049E */
    uint8_t d = shle_rb(V95_LED);
    d = (uint8_t)(d >> 1 | d << 7);
    if (d == 0x7F) d = 0xF7;
    shle_wb(V95_LED, d);
    sound_ctrl_w(&g_sound, (d & 0xFu) | 0x10u);
}

/* Int3 (0x600526), a MIDI byte: false for 0xFF, which restarts the driver */
static bool v95_midi_byte(uint8_t b) {
    if (V95_B->rally) return rl_midi_byte(b);
    if (b == 0xFF) return false;
    if (b >= 0xF0) return true;
    uint8_t d1;
    if (b & 0x80) {
        shle_wb(V95_RS, b);
        d1 = (b >= 0xC0 && b < 0xE0) ? 1 : 2;
        shle_wb(V95_RS + 2, d1);
    } else {
        d1 = shle_rb(V95_RS + 3);
        if (!d1) d1 = shle_rb(V95_RS + 2);
        d1--;
        if (d1) shle_wb(V95_RS + 1, b);
        else {
            uint16_t w = shle_rw(V95_QW);
            uint32_t p = V95_QUEUE + (w & 0xFFFu);
            shle_wb(p, shle_rb(V95_RS));
            if ((uint8_t)(shle_rb(V95_RS + 2) - 1)) { shle_wb(p + 1, shle_rb(V95_RS + 1)); p++; }
            shle_wb(p + 1, b);
            shle_ww(V95_QW, (w + 4u) & 0xFFFu);
            v95_inc(V95_QN, 1);
            v95_led();
        }
    }
    shle_wb(V95_RS + 3, d1);
    return true;
}

/* ---- voices -------------------------------------------------------------------- */

static inline void v95_cut(uint32_t slot) {
    shle_sw8(slot + 0xB, 0x1F);
    shle_sw8(slot, 0x10);
}

static void v95_renum_mus(uint8_t d2) {
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        uint8_t o = shle_rb(v + 9);
        if (o != 0xFF && o > d2) shle_wb(v + 9, o - 1u);
    }
}

static void v95_renum_eff(uint8_t d2) {
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16)
        if (shle_rb(v + 9) == 0xFF && shle_rb(v + 5) > d2) v95_inc(v + 5, -1);
}

/* a voice off: its record stopped, its order closed up */
static void v95_free(uint32_t v) {
    shle_wb(V95_RECS + shle_rw(v), 0);
    bool fx = shle_rb(v + 2) & 8;
    shle_wb(v + 2, 0);
    if (fx) {
        uint8_t d2 = shle_rb(v + 5);
        shle_wb(v + 9, 0);
        shle_wb(v + 5, 0);
        v95_inc(V95_NEFF, -1);
        v95_renum_eff(d2);
    } else {
        uint8_t d2 = shle_rb(v + 9);
        shle_wb(v + 9, 0);
        v95_inc(V95_NMUS, -1);
        v95_renum_mus(d2);
    }
    g_shle.cost += V95_COST_FREE;
}

/* Int1, timer B (0x6005FE): the release countdowns; the voices freed */
static uint32_t v95_timer_b_body(void) {
    if (V95_B->rally) return rl_timer_b_body();
    uint32_t n = 0;
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        if (!(shle_rb(v + 2) & 1)) continue;
        v95_inc(v + 0xB, -1);
        if (shle_rb(v + 0xB)) continue;
        shle_sw8(shle_rw(v) + 0xB, 0x1F);
        v95_free(v);
        n++;
    }
    return n;
}

/* the melodic note's frequency and its stream pace, bent by d5 (0x601062) */
static uint16_t v95_freq(uint32_t v, int16_t bend, uint8_t tune, uint32_t slot) {
    int32_t d5 = (int16_t)(bend + (int8_t)(uint8_t)(shle_rb(v + 0xA) + tune));
    uint8_t n = shle_rb(v + 3);
    uint32_t a3 = shle_ml(V95_B->pitch + (n & 0xFu) * 4u) + 2u * (uint32_t)d5;
    shle_sw16(slot + 0x10, (((uint32_t)(n & 0xF0u) << 7) + shle_mw(a3)) & 0x7BFFu);
    a3 = shle_ml(V95_B->phase + ((n & 0xF0u) >> 2)) + (((n & 0xFu) << 5) & 0xFFu) + 2u * (uint32_t)d5;
    return shle_mw(a3);
}

/* a drum's (0x601014) */
static uint16_t v95_drum_freq(uint32_t slot) {
    uint8_t hi = shle_rb(V95_FREQ), lo = shle_rb(V95_FREQ + 1) & 0xF;
    uint16_t f = (uint16_t)(((uint32_t)hi << 7) & 0x7800u);
    f |= shle_mw(V95_B->drumfreq + ((((hi & 0xFu) << 4) + lo) * 2u));
    shle_sw16(slot + 0x10, f);
    return shle_mw(shle_ml(V95_B->phase + ((hi & 0xF0u) >> 2)) + (((hi & 0xFu) << 5) & 0xFFu));
}

/* the first chunk of a streamed sample into the voice's window (0x600C68) */
static void v95_keyon_stream(uint32_t v, uint32_t slot, uint32_t e, uint16_t d2) {
    uint32_t src = shle_ml(e), len = shle_ml(e + 4), lp = shle_ml(e + 0xC);
    uint32_t r = V95_RECS + slot, win = shle_rl(v + 0xC);
    if (lp) {
        shle_wb(v + 2, shle_rb(v + 2) & ~0x10u);
        shle_wl(r + 0x16, (shle_rl(r + 0x16) & 0xFF000000u) | (lp & 0xFFFFFFu));
        shle_wl(r + 0x12, (shle_rl(r + 0x12) & 0xFF000000u) | (shle_ml(e + 8) & 0xFFFFFFu));
    }
    shle_sw32(slot, win | 0x10300000u);
    shle_wl(r + 0xA, len - 0x200u);
    shle_wb(r + 1, 0x10);
    shle_ww(r + 0x1A, d2);
    shle_ww(r + 0x1C, d2 + 3770u);
    shle_copy(win, src, 0x200);
    shle_wl(r + 2, src + 0x200u);
    shle_wl(r + 6, win + 0x200u);
    shle_sw8(slot, shle_sr8(slot) | 0x18);
    shle_wb(r, lp ? 0x81 : 0x80);
    g_shle.cost += SHLE_CHUNK_CYCLES;
}

static void v95_keyon_sample(uint32_t v, uint32_t slot, uint16_t d2) {
    /* an entry past 0x7FF/20 is below the table, over the bus (0xFFxxxx: sample ROM) */
    uint32_t e = (V95_SMPTBL + (uint32_t)(int32_t)(int16_t)(uint16_t)((shle_rw(v + 6) & 0x7FFu) * 20u)) & 0xFFFFFFu;
    shle_sw32(slot + 8, shle_ml(e + 0x10));
    shle_wb(v + 0xB, shle_mb(V95_B->release + (shle_mb(e + 0x13) & 0x1Fu)));
    uint32_t d5 = shle_ml(e);
    if (!(d5 & 0x80000000u)) { v95_keyon_stream(v, slot, e, d2); return; }
    shle_sw32(slot, (d5 & 0xFFFFFu) | 0x10300000u);    /* preloaded: the chip plays it from sound RAM */
    shle_sw16(slot + 4, shle_ml(e + 8));
    shle_sw16(slot + 6, (uint16_t)shle_ml(e + 4) - 1u);
    shle_sw8(slot, shle_sr8(slot) | 0x18);
    shle_wb(V95_RECS + slot, 0x40);
}

/* 0x600A8C */
static void v95_keyon(uint32_t t, uint32_t v) {
    g_shle.keyons++;
    g_shle.cost += V95_COST_KEYON;
    uint32_t slot = shle_rw(v);
    uint8_t ch = v95_ch();
    shle_wb(v + 2, 0x40);
    shle_sw8(slot, 0x10);
    shle_ww(v + 6, shle_rw(V95_SMPW));
    bool drum = shle_rb(t + 2) >= 0xF0;
    shle_wb(v + 3, shle_rb(V95_NVAL));
    shle_wb(v + 8, shle_rb(V95_VEL));
    shle_wb(v + 4, ch);
    if (drum) {
        shle_wb(v + 2, 0x50);
        if (ch != 15) {
            shle_wb(v + 9, 0xFF);
            shle_wb(v + 5, shle_rb(V95_NEFF));
            v95_inc(V95_NEFF, 1);
            shle_wb(v + 2, 0x58);
        } else {
            shle_wb(v + 9, shle_rb(V95_NMUS));
            v95_inc(V95_NMUS, 1);
        }
    } else {
        shle_wb(v + 9, shle_rb(V95_NMUS));
        v95_inc(V95_NMUS, 1);
        shle_wb(v + 0xA, shle_rb(V95_SPLIT));
    }
    shle_sw8(slot + 0xD, v95_tl((uint32_t)shle_rb(v + 8) * shle_rb(t + 0xA)));
    uint16_t d2 = drum ? v95_drum_freq(slot) : v95_freq(v, (int16_t)shle_rw(V95_BEND + ch * 2u), shle_rb(t + 5), slot);
    shle_sw32(slot + 4, 0x1FFF);
    shle_sw8(slot + 0xC, 0);
    shle_sw16(slot + 0xE, 0);
    uint16_t lfo = shle_rw(t + 8) | (uint16_t)((shle_rb(V95_CC1 + ch) << 5) & 0xE0u);
    if (!drum) lfo |= shle_rb(V95_CC2 + ch) & 7u;
    shle_sw16(slot + 0x12, lfo);
    shle_sw8(slot + 0x15, drum ? shle_rb(V95_TONE) : shle_rb(t + 0xC));
    shle_sw8(slot + 0x16, 0x60u | (shle_rb(V95_PAN) & 0x1Fu));
    v95_keyon_sample(v, slot, d2);
}

/* 0x600F12: the next free voice, else the oldest music voice taken */
static void v95_alloc(uint32_t t) {
    uint32_t a1 = shle_rl(V95_NEXTV);
    for (int k = 0; k < 32; k++, a1 += 16) {
        if (a1 == V95_VEND) { a1 = V95_VOICES; shle_wl(V95_NEXTV, a1); }
        if (!shle_rb(a1 + 2)) {
            shle_wl(V95_NEXTV, a1 + 16u);
            v95_keyon(t, a1);
            return;
        }
    }
    uint32_t v = V95_VOICES;
    while (v < V95_VEND && shle_rb(v + 9)) v += 16;
    if (v == V95_VEND) return;
    uint32_t slot = shle_rw(v);
    v95_cut(slot);
    shle_wb(v + 2, 0);
    shle_wb(V95_RECS + slot, 0);
    for (uint32_t u = V95_VOICES; u < V95_VEND; u += 16)
        if (shle_rb(u + 9) != 0xFF) v95_inc(u + 9, -1);
    v95_inc(V95_NMUS, -1);
    v95_keyon(t, v);
}

/* a drum on channels 0-14: five effects at most, the oldest taken (0x60091E) */
static void v95_drum_effect(uint32_t t) {
    if (shle_rb(V95_NEFF) < 5) { v95_alloc(t); return; }
    uint32_t v = V95_VOICES;
    while (v < V95_VEND && !(shle_rb(v + 9) == 0xFF && shle_rb(v + 5) == 0)) v += 16;
    if (v == V95_VEND) { shle_wb(V95_NEFF, 0); v95_alloc(t); return; }
    uint32_t slot = shle_rw(v);
    v95_cut(slot);
    shle_wb(V95_RECS + slot, 0);
    shle_wb(v + 5, 5);
    for (uint32_t u = V95_VOICES; u < V95_VEND; u += 16)
        if (shle_rb(u + 9) == 0xFF) v95_inc(u + 5, -1);
    v95_inc(V95_NEFF, -1);
    v95_keyon(t, v);
}

/* channel 15's notes 0xB6 and 0xB8 stop its 0xBA (0x6009A2) */
static void v95_drum_choke(void) {
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        if (!shle_rb(v + 2) || shle_rb(v + 3) != 0xBA || shle_rb(v + 4) != 15) continue;
        shle_wb(v + 2, 0);
        uint8_t d2 = shle_rb(v + 9);
        shle_wb(v + 9, 0);
        uint32_t slot = shle_rw(v);
        shle_sw8(slot, 0x10);
        shle_wb(V95_RECS + slot, 0);
        v95_inc(V95_NMUS, -1);
        v95_renum_mus(d2);
    }
}

static uint32_t v95_drum_entry(uint8_t prog, uint8_t note) {
    return shle_ml(V95_B->drums + ((uint8_t)(prog - 0xF0) & 0x7Fu) * 4u) + (note & 0x7Fu) * 6u;
}

/* the split a note falls in: its entry */
static uint32_t v95_split(uint8_t prog, uint8_t note) {
    uint32_t a2 = shle_ml(V95_B->splits + (prog & 0x7Fu) * 4u);
    for (int guard = 0; guard < 128 && note > shle_mb(a2); guard++) a2 += 6;
    return a2;
}

static void v95_note_off(uint32_t t, uint8_t note);

/* 0x600858 */
static void v95_note_on(uint32_t t, uint32_t p) {
    g_shle.cost += V95_COST_NOTE;
    uint8_t note = shle_rb(p), vel = shle_rb(p + 1);
    shle_wb(V95_NOTE, (uint8_t)(note + shle_rb(t + 4)));
    if (!vel) { v95_note_off(t, note); return; }
    shle_wb(V95_VEL, shle_mb(V95_B->vel + vel));
    uint8_t prog = shle_rb(t + 2);
    if (prog < 0xF0) {
        uint32_t a2 = v95_split(prog, shle_rb(V95_NOTE));
        shle_ww(V95_SMPW, shle_mw(a2 + 4));
        shle_wb(V95_SPLIT, shle_mb(a2 + 2));
        shle_wb(V95_NVAL, shle_mb(V95_B->notes + ((shle_rb(V95_NOTE) + shle_mb(a2 + 3)) & 0x7Fu)));
        shle_wb(V95_PAN, shle_rb(t + 6));
        v95_alloc(t);
        return;
    }
    uint32_t a2 = v95_drum_entry(prog, note);
    if (shle_mw(a2) == 0xFF) return;
    uint8_t pan = shle_mb(a2 + 4);
    shle_wb(V95_PAN, pan == 0x80 ? shle_rb(t + 6) : pan);
    shle_ww(V95_FREQ, shle_mw(a2 + 2));
    shle_ww(V95_SMPW, shle_mw(a2));
    uint8_t tone = shle_rb(t + 0xD);
    if (tone) shle_wb(t + 0xD, 0);
    else {
        tone = shle_rb(t + 0xC);
        if (!(tone & 7)) tone = shle_mb(a2 + 5);
    }
    shle_wb(V95_TONE, tone);
    shle_wb(V95_NVAL, shle_mb(V95_B->notes + (shle_rb(V95_NOTE) & 0x7Fu)));
    if (v95_ch() != 15) { v95_drum_effect(t); return; }
    uint8_t nv = shle_rb(V95_NVAL);
    if (nv == 0xB6 || nv == 0xB8) v95_drum_choke();
    v95_alloc(t);
}

/* 0x6011D0 */
static void v95_release(uint32_t v, uint8_t ch) {
    if (shle_rb(V95_CC1 + ch) & 0x80) { shle_wb(v + 2, shle_rb(v + 2) | 2); return; }
    uint32_t slot = shle_rw(v);
    if (shle_rb(v + 0xB)) {
        shle_sw8(slot, 0x10);
        shle_wb(v + 2, shle_rb(v + 2) | 1);
        return;
    }
    v95_cut(slot);
    v95_free(v);
}

/* 0x6010E2, NOTE set: note is the event's byte */
static void v95_note_off(uint32_t t, uint8_t note) {
    uint8_t prog = shle_rb(t + 2);
    if (prog >= 0xF0) {
        shle_ww(V95_SMPW, shle_mw(v95_drum_entry(prog, note)));
        shle_wb(V95_NVAL, shle_mb(V95_B->notes + (shle_rb(V95_NOTE) & 0x7Fu)));
    } else {
        uint32_t a2 = v95_split(prog, shle_rb(V95_NOTE));
        shle_ww(V95_SMPW, shle_mw(a2 + 4));
        shle_wb(V95_NVAL, shle_mb(V95_B->notes + ((shle_rb(V95_NOTE) + shle_mb(a2 + 3)) & 0x7Fu)));
    }
    uint8_t ch = v95_ch(), nv = shle_rb(V95_NVAL);
    uint16_t smp = shle_rw(V95_SMPW);
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        uint8_t f = shle_rb(v + 2);
        if (shle_rb(v + 3) != nv || shle_rb(v + 4) != ch || shle_rw(v + 6) != smp || (f & 0x12)) continue;
        shle_wb(v + 2, f & 0x7F);
        if (f & 0x7F) v95_release(v, ch);
    }
}

static void v95_note_off_ev(uint32_t t, uint32_t p) {  /* 0x6010D0 */
    g_shle.cost += V95_COST_SCAN;
    uint8_t note = shle_rb(p);
    shle_wb(V95_NOTE, (uint8_t)((note & 0x7F) + shle_rb(t + 4)));
    v95_note_off(t, note);
}

/* 0x6012B4 */
static void v95_bend(uint32_t t, uint32_t p) {
    g_shle.cost += V95_COST_SCAN - 300u;
    uint16_t v = (uint16_t)(((shle_rb(p + 1) & 0x7Fu) << 7 | (shle_rb(p) & 0x7Fu)) >> 5);
    uint8_t d2 = (uint8_t)v;
    if (!(v & 0x100)) { d2 = (uint8_t)-d2; if (!d2) d2 = 0xFF; }
    int16_t d0 = shle_mb(shle_ml(V95_B->bend + (shle_rb(t + 7) & 0xFu) * 4u) + d2);
    if (!(v & 0x100)) d0 = (int16_t)-d0;
    uint8_t ch = v95_ch();
    shle_ww(V95_BENDW, (uint16_t)d0);
    shle_ww(V95_BEND + ch * 2u, (uint16_t)d0);
    if (ch >= 9) return;
    for (uint32_t u = V95_VOICES; u < V95_VEND; u += 16) {
        if (shle_rb(u + 4) != ch || !(shle_rb(u + 2) & 0x7F)) continue;
        uint32_t slot = shle_rw(u);
        shle_ww(V95_RECS + slot + 0x1A, v95_freq(u, d0, shle_rb(t + 5), slot));
    }
}

/* ---- controllers (0x6013CA's jump) ----------------------------------------------- */

static void v95_cc_lfo(uint8_t d0, bool pitch) {      /* CC 1, CC 2 */
    uint8_t ch = v95_ch(), d6 = d0, n = (d0 >> 4) & 7;
    uint32_t a = (pitch ? V95_CC1 : V95_CC2) + ch;
    uint8_t old = shle_rb(a);
    if ((old & 0x7F) == n) return;
    shle_wb(a, (old & 0x80) | n);
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        if (shle_rb(v + 4) != ch || !(shle_rb(v + 2) & 0x7F)) continue;
        uint32_t s = shle_rw(v) + 0x13;
        if (pitch) { d6 = (uint8_t)(d6 << 1) & 0xE0; shle_sw8(s, (shle_sr8(s) & 0x1Fu) | d6); }
        else       { d6 = (d6 >> 4) & 7;            shle_sw8(s, (shle_sr8(s) & 0xF8u) | d6); }
    }
}

/* a voice's TL from its velocity and its track's level */
static void v95_voice_tl(uint32_t v, uint8_t lvl) {
    uint32_t p = (uint32_t)shle_rb(v + 8) * lvl;
    shle_sw8(shle_rw(v) + 0xD, p ? v95_tl(p) : 0xFF);
}

static void v95_cc_volume(uint32_t t, uint8_t d0) {   /* CC 7 */
    d0 = v95_x2(d0);
    shle_wb(t + 3, d0);
    shle_wb(t + 0xA, v95_level(d0, (shle_rb(t) & 1) ? 0xFF : shle_rb(V95_MVOL)));
    uint8_t ch = v95_ch();
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16)
        if ((shle_rb(v + 2) & 0x40) && shle_rb(v + 4) == ch) v95_voice_tl(v, shle_rb(t + 0xA));
}

static void v95_cc_pan(uint32_t t, uint8_t d0) {      /* CC 10 */
    uint8_t pan = shle_mb(V95_B->pan + (d0 & 0x7Fu)), d2 = v95_ch();
    shle_wb(t + 6, pan);
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        if (shle_rb(v + 4) != d2) continue;
        d2 = shle_rb(v + 2) & 0x7F;                     /* (the driver's own d2: the channel is lost) */
        if (!d2) continue;
        uint32_t s = shle_rw(v) + 0x16;
        shle_sw8(s, (shle_sr8(s) & 0xE0u) | pan);
    }
}

static void v95_track_levels(uint8_t m, bool fixed_rule) {
    for (uint32_t t = V95_TRACKS; t < V95_TRACKS + 10u * 16u; t += 16)
        shle_wb(t + 0xA, v95_level(shle_rb(t + 3), fixed_rule && (shle_rb(t) & 1) ? 0xFF : m));
}

static void v95_cc_master(uint8_t d0) {               /* CC 16 */
    d0 = v95_x2(d0);
    shle_wb(V95_MVOL, d0);
    for (uint32_t t = V95_TRACKS; t < V95_TRACKS + 10u * 16u; t += 16) {
        shle_wb(t + 0xA, v95_level(shle_rb(t + 3), d0));
        for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16)
            if (shle_rb(t + 1) == shle_rb(v + 4) && (shle_rb(v + 2) & 0x40)) v95_voice_tl(v, shle_rb(t + 0xA));
    }
}

static void v95_cc_trackset(uint8_t d0) {             /* CC 32 */
    shle_copy(V95_TRACKS, shle_ml(V95_B->trk + (d0 & 0xFu) * 4u), 160);
    v95_track_levels(shle_rb(V95_MVOL), true);
}

static void v95_cc_tone(uint8_t cc, uint8_t d0) {     /* CC 41, 42, 43 over 21 tracks */
    uint8_t ch = v95_ch();
    if (cc == 41) d0 = (uint8_t)((d0 & 0xF) << 3 | (d0 & 0x70) >> 4);
    for (uint32_t t = V95_TRACKS; t < V95_TRACKS + 21u * 16u; t += 16) {
        if (shle_rb(t + 1) != ch) continue;
        if (cc == 41) { shle_wb(t + 0xD, d0); continue; }
        if (cc == 42) { d0 = ((d0 & 0x7F) >> 4) & 7;      shle_wb(t + 0xC, (shle_rb(t + 0xC) & 0x78) | d0); }
        else          { d0 = (uint8_t)((d0 & 0xF) << 3) & 0x78; shle_wb(t + 0xC, (shle_rb(t + 0xC) & 7) | d0); }
    }
}

static void v95_cc_dsp(uint8_t d0) {                  /* CC 44: a DSP program, the slots silent over it */
    for (uint32_t k = 0; k < 32; k++) {
        shle_wb(V95_TLSAVE + k, shle_sr8(k * 0x20u + 0xD));
        shle_sw8(k * 0x20u + 0xD, 0xFF);
    }
    d0 &= 0x7F;
    if (d0 == 0x7F) {
        for (uint32_t k = 0; k < 0x500; k++) shle_sw8(0x700 + k, 0);
    } else if ((int16_t)d0 <= (int16_t)shle_mw(V95_B->dsp)) {
        uint32_t a = shle_ml(V95_B->dsp + 2 + d0 * 4u);
        for (uint32_t k = 0; k < 0x500; k++) shle_sw8(0x700 + k, shle_mb(a + k));
    }
    for (uint32_t k = 0; k < 32; k++) shle_sw8(k * 0x20u + 0xD, shle_rb(V95_TLSAVE + k));
    g_shle.cost += 0x500u * V95_COST_BYTE * 2u;
}

static void v95_efsdl(uint8_t d0) {                   /* CC 45 (set 0 at boot) */
    d0 &= 0x7F;
    if ((int16_t)d0 > (int16_t)shle_mw(V95_B->efsdl)) return;
    uint32_t a = shle_ml(V95_B->efsdl + 2 + d0 * 4u);
    for (uint32_t k = 0; k < 16; k++) shle_sw8(k * 0x20u + 0x17, shle_mb(a + k));
}

static void v95_cc_send(uint8_t cc, uint8_t d0) {     /* CC 46, 47: a DSP input's level, its pan */
    uint32_t s = ((uint32_t)v95_ch() << 5) + 0x17;
    if (cc == 46) shle_sw8(s, (shle_sr8(s) & 0x1Fu) | (((d0 & 0x7Fu) << 1) & 0xE0u));
    else          shle_sw8(s, (shle_sr8(s) & 0xE0u) | shle_mb(V95_B->pan + (d0 & 0x7Fu)));
}

/* the sample table, then preloaded set d0 (0x600422, CC 58) */
static void v95_samples(uint8_t d0) {
    uint32_t n = shle_mw(V95_B->smptbl) + 1u;
    shle_copy(V95_SMPTBL, V95_B->smptbl + 2, n);
    g_shle.cost += n * V95_COST_BYTE;
    if (d0 == 0x7F || (int16_t)d0 > (int16_t)shle_mw(V95_B->preload)) return;
    uint32_t a1 = shle_ml(V95_B->preload + 2 + d0 * 4u);
    uint32_t cnt = shle_mw(a1) + 1u, a3 = V95_PRELOAD;
    for (uint32_t i = 0; i < cnt; i++) {
        int32_t d1 = (int16_t)(uint16_t)(shle_mw(a1 + 2 + i * 2u) * 20u);
        uint32_t src = V95_B->smptbl + 2 + (uint32_t)d1;
        uint32_t ptr = shle_ml(src), lp = shle_ml(src + 8), len = shle_ml(src + 4);
        uint32_t e = V95_SMPTBL + (uint32_t)d1;
        shle_wl(e + 8, lp - ptr);
        shle_wl(e, a3 | 0x80000000u);
        uint32_t nb = ((len + 511u) & 0xFFFFu) + 1u;
        shle_copy(a3, ptr, nb);
        a3 += nb;
        g_shle.cost += nb * V95_COST_BYTE;
        if (a3 > 0x6FFFFu) { LOG_WARN("sound HLE: preloaded samples ran past 0x6FFFF (the driver halts here)"); return; }
    }
}

static void v95_cc_samples(uint8_t d0) {             /* CC 58, at SR 0x2200 */
    v95_samples(d0 & 0x7F);
    g_shle.stall = true;
}

/* a voice cut by a controller: the pedal's (CC 64) and "all off"'s count rule */
static void v95_cut_counted(uint32_t v) {
    uint32_t slot = shle_rw(v);
    v95_cut(slot);
    shle_wb(V95_RECS + slot, 0);
    uint8_t f = shle_rb(v + 2), ch = shle_rb(v + 4);
    shle_wb(v + 2, 0);
    shle_wb(v + 4, 0);
    if (ch == 15 || !(f & 0x10)) {
        v95_inc(V95_NMUS, -1);
        v95_renum_mus(shle_rb(v + 9));
    }
}

static void v95_cut_plain(uint32_t v) {
    uint32_t slot = shle_rw(v);
    v95_cut(slot);
    shle_wb(V95_RECS + slot, 0);
    shle_wb(v + 2, 0);
    shle_wb(v + 4, 0);
}

static void v95_cc_pedal(uint8_t d0) {                /* CC 64 */
    uint8_t ch = v95_ch();
    if (d0) { shle_wb(V95_CC1 + ch, shle_rb(V95_CC1 + ch) | 0x80); return; }
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16)
        if (shle_rb(v + 4) == ch && (shle_rb(v + 2) & 2)) v95_cut_counted(v);
    shle_wb(V95_CC1 + ch, shle_rb(V95_CC1 + ch) & 0x7F);
}

static void v95_clear_channels(unsigned from, unsigned to) {
    for (unsigned c = from; c < to; c++) {
        shle_wb(V95_CC1 + c, 0);
        shle_wb(V95_CC2 + c, 0);
        shle_ww(V95_BEND + c * 2u, 0);
    }
}

static void v95_cc_stop(uint8_t d0) {                 /* CC 80: 127 all, 125 the effects, 126 the music */
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        if (!(shle_rb(v + 2) & 0x40)) continue;
        uint8_t ch = shle_rb(v + 4);
        if (d0 == 127) v95_cut_counted(v);
        else if (d0 == 125 && ch >= 9 && ch != 15) v95_cut_plain(v);
        else if (d0 == 126 && (ch < 9 || ch == 15)) v95_cut_plain(v);
    }
    if (d0 == 127) v95_clear_channels(0, 16);
    else if (d0 == 125) { v95_clear_channels(9, 15); shle_wb(V95_NEFF, 0); }
    else if (d0 == 126) { shle_wb(V95_NMUS, 0); v95_clear_channels(0, 9); }
}

static void v95_all_off(void) {                       /* CC 123 (0x601DE0) */
    g_shle.cost += V95_COST_SCAN - 700u;
    uint8_t ch = v95_ch();
    v95_clear_channels(ch, ch + 1u);
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        if (!(shle_rb(v + 2) & 0x40) || shle_rb(v + 4) != ch) continue;
        v95_cut_plain(v);
        v95_inc(V95_NMUS, -1);
    }
}

/* a controller's own clock periods, measured (CC 16: ten tracks' voices) */
static uint32_t v95_cc_cost(uint8_t cc) {
    switch (cc) {
    case 1: case 2: case 10: return 1750;
    case 7:  return 2300;
    case 16: return 27000;
    case 32: return 12000;
    case 41: case 42: case 43: return 1150;
    case 44: return 1000;
    case 64: return 1000;
    case 80: return 5800;
    default: return 80;
    }
}

static void v95_cc_high(uint8_t cc, uint8_t d0) {    /* CC 41 and up */
    switch (cc) {
    case 41: case 42: case 43: v95_cc_tone(cc, d0); break;
    case 44:  v95_cc_dsp(d0); break;
    case 45:  v95_efsdl(d0); break;
    case 46: case 47: v95_cc_send(cc, d0); break;
    case 58:  v95_cc_samples(d0); break;
    case 64:  v95_cc_pedal(d0); break;
    case 80:  v95_cc_stop(d0); break;
    case 123: v95_all_off(); break;
    default:  break;
    }
}

static void v95_cc(uint32_t t, uint32_t p) {
    uint8_t cc = shle_rb(p) & 0x7F, d0 = shle_rb(p + 1);
    g_shle.cost += v95_cc_cost(cc);
    switch (cc) {
    case 1:   v95_cc_lfo(d0, true); break;
    case 2:   v95_cc_lfo(d0, false); break;
    case 7:   v95_cc_volume(t, d0); break;
    case 10:  v95_cc_pan(t, d0); break;
    case 16:  v95_cc_master(d0); break;
    case 26:  shle_wb(V95_SEQS + 0xC, d0); shle_wb(V95_SEQS + 0xD, d0); break;   /* the music's fade */
    case 32:  v95_cc_trackset(d0); break;
    default:  v95_cc_high(cc, d0); break;
    }
}

static void v95_program(uint32_t t, uint32_t p) {     /* 0x601E5A */
    uint8_t v = shle_rb(p);
    shle_wb(t + 2, v >= 0x70 ? v | 0x80 : v);
    v95_all_off();
}

/* 0x601E74: A? group song: the music's sequence, or an effect's */
static void v95_song(uint32_t p) {
    g_shle.cost += V95_COST_SONG;
    uint32_t g = shle_ml(V95_B->songs + (shle_rb(p) & 0x7Fu) * 4u);
    uint8_t n = shle_rb(p + 1);
    if ((int16_t)n > (int16_t)shle_mw(g)) return;
    uint32_t q = shle_ml(g + 2 + n * 4u);
    uint32_t s = V95_SEQS;
    if (shle_mb(q) & 0x80) {
        for (s = V95_SEQS + 0x20; s < V95_SEQS + 0xE0 && (shle_rb(s) & 0x80); s += 0x20) {}
        for (uint32_t k = 0; k < 32; k++) shle_wb(s + k, 0);
        shle_wl(s + 4, q + 1);
        shle_ww(s + 2, 1);
        shle_wb(s, 0x80);
        return;
    }
    for (uint32_t k = 0; k < 32; k++) shle_wb(s + k, 0);
    shle_wl(s + 4, shle_ml(q + 4));
    shle_wl(s + 8, q + 8);
    shle_ww(s + 2, 1);
    shle_wb(s, 0x88);
}

static void v95_event(uint32_t t, uint32_t p) {       /* 0x60081A */
    switch (shle_rb(V95_STATUS) & 0xF0) {
    case 0x90: v95_note_on(t, p); break;
    case 0x80: v95_note_off_ev(t, p); break;
    case 0xE0: v95_bend(t, p); break;
    case 0xB0: v95_cc(t, p); break;
    case 0xC0: v95_program(t, p); break;
    case 0xA0: v95_song(p); break;
    default: break;
    }
}

#include "sound_hle_rally.h"

/* ---- the sequencers (timer A) ------------------------------------------------------ */

/* a status 0xFx: sysex and meta events skipped; false at the end of the track (FF 2F) */
static bool v95_seq_meta(uint8_t d2, uint32_t *a3) {
    if (d2 == 0xF7) { *a3 += shle_mb(*a3) + 1u; return true; }
    if (d2 == 0xF0) { for (int g = 0; g < 0x10000 && shle_mb((*a3)++) != 0xF7; g++) {} return true; }
    if (d2 == 0xFF && shle_mb((*a3)++) != 0x2F) { *a3 += shle_mb(*a3) + 1u; return true; }
    return false;
}

/* one event of sequence s at *a3: false at its end */
static bool v95_seq_event(uint32_t s, uint32_t *a3) {
    uint8_t d2 = shle_mb((*a3)++);
    if (!(d2 & 0x80)) { d2 = shle_rb(s + 1); (*a3)--; }
    shle_wb(s + 1, d2);
    uint8_t hi = d2 >> 4;
    if (hi == 0xF) return v95_seq_meta(d2, a3);
    uint8_t b1 = shle_mb((*a3)++), b2;
    if (hi == 0x8 || hi == 0xC || hi == 0xD) {          /* one data byte: its top bit ends the step */
        b2 = hi == 0x8 ? 0x7F : 0;
        if (b1 & 0x80) shle_wb(s, shle_rb(s) | 1);
        v95_enqueue(d2, b1 & 0x7F, b2);
        return true;
    }
    b2 = shle_mb((*a3)++);
    if (hi == 0xB && b1 == 16) {                        /* master volume under a fade: the fade's */
        if (shle_rb(s + 12)) b2 = shle_rb(s + 14);
        else shle_wb(s + 14, b2 & 0x7F);
    }
    if (b2 & 0x80) shle_wb(s, shle_rb(s) | 1);
    v95_enqueue(d2, b1, b2 & 0x7F);
    return true;
}

/* the end of a sequence's data: the next in its list, or its stop. false: it stops */
static bool v95_seq_end(uint32_t s, uint32_t *a3) {
    if (!(shle_rb(s) & 8)) { shle_wb(s, 0); return false; }
    uint32_t a4 = shle_rl(s + 8), l = shle_ml(a4);
    if (l == 0xFFFFFFFFu) { shle_wb(s, 0); return false; }
    if (l == 0xFFFFFFF1u) {                             /* back to a mark */
        uint32_t a0 = shle_ml(a4 + 4);
        *a3 = shle_ml(a0);
        shle_wl(s + 8, a0 + 4);
        return true;
    }
    if (l == 0xFFFFFFF2u) {                             /* every sequence off */
        for (uint32_t k = 0; k < 8u * V95_SEQ_STRIDE; k++) shle_wb(V95_SEQS + k, 0);
        return false;
    }
    *a3 = l;
    shle_wl(s + 8, a4 + 4);
    return true;
}

static void v95_seq_step(uint32_t s) {                 /* 0x601F7C */
    if (!(shle_rb(s) & 0x80)) return;
    uint16_t w = (uint16_t)(shle_rw(s + 2) - 1u);
    shle_ww(s + 2, w);
    if (w) return;
    uint32_t a3 = shle_rl(s + 4);
    for (int guard = 0; guard < 4096; guard++) {
        shle_wb(s, shle_rb(s) & ~1u);
        if (!v95_seq_event(s, &a3)) { if (v95_seq_end(s, &a3)) continue; return; }
        if (shle_rb(s) & 1) continue;
        uint16_t d = shle_mb(a3++);
        if (!d) continue;
        if (d & 0x80) d = (uint16_t)((d & 0x7F) << 7 | shle_mb(a3++));
        shle_ww(s + 2, d);
        shle_wl(s + 4, a3);
        return;
    }
    LOG_WARN("sound HLE: sequencer at 0x%04X ran away (0x%08X); stopped", s, a3);
    shle_wb(s, 0);
}

static void v95_fade(void) {                           /* 0x6021BA */
    uint32_t s = V95_SEQS;
    if (!shle_rb(s + 12)) return;
    v95_inc(s + 13, -1);
    if (shle_rb(s + 13)) return;
    v95_inc(s + 14, -4);
    if (!(shle_rb(s + 14) & 0x80)) {
        v95_enqueue(0xBE, 0x10, shle_rb(s + 14));
        shle_wb(s + 13, shle_rb(s + 12));
        return;
    }
    shle_wb(s, shle_rb(s) & 0x7F);
    shle_wb(s + 12, 0);
    v95_enqueue(0xBE, 0x50, 0x7E);                      /* CC 80 126: the music off */
    shle_wb(V95_QUEUE + ((shle_rw(V95_QW) - 4u) & 0xFFFu) + 3, 0);
}

static void v95_timer_a_body(void) {                   /* 0x601F16 */
    if (shle_rb(V95_QN) >= 0xFF) return;
    v95_fade();
    for (uint32_t s = V95_SEQS; s < V95_SEQS + 8u * V95_SEQ_STRIDE; s += V95_SEQ_STRIDE) v95_seq_step(s);
}

/* ---- the main loop ------------------------------------------------------------------ */

static void v95_dispatch(uint64_t now) {               /* 0x6007A6, one at a record */
    int guard = 0;
    while (shle_rb(V95_QN) && now >= g_shle.busy_until && guard++ < 512) {
        v95_inc(V95_QN, -1);
        uint16_t r = shle_rw(V95_QR);
        uint32_t p = V95_QUEUE + (r & 0xFFFu);
        uint8_t st = shle_rb(p);
        shle_wb(V95_STATUS, st);
        g_shle.events++;
        g_shle.cost = V95_COST_EVENT;
        g_shle.stall = false;
        if (V95_B->rally) rl_dispatch_event(st, p + 1);
        else for (uint32_t t = V95_TRACKS; t < V95_TRACKS + 20u * 16u; t += 16)
            if ((shle_rb(t) & 0x80) && shle_rb(t + 1) == (st & 0xF)) v95_event(t, p + 1);
        shle_ww(V95_QR, (r + 4u) & 0xFFFu);
        uint64_t from = g_shle.busy_until > now ? g_shle.busy_until : now;
        g_shle.busy_until = from + g_shle.cost;
        if (g_shle.stall) {                             /* SR 0x2200: no timer till it is done */
            g_shle.stall_until = g_shle.busy_until;
            if (g_shle.l2_free < g_shle.busy_until) g_shle.l2_free = g_shle.busy_until;
        }
    }
}

/* the chip has played past the window's last chunk: no loop, the countdown on (0x60260C) */
static void v95_rec_finish(uint32_t r) {
    uint32_t slot = (uint32_t)shle_rb(r + 0x12) * 16u;
    shle_sw8(slot + 1, shle_sr8(slot + 1) & 0x9Fu);
    shle_sw16(slot + 6, shle_rw(r + 0x1E));
    shle_wb(r, 0);
    uint32_t v = V95_VOICES + slot / 2u;
    shle_wb(v + 0xB, shle_rb(v + 0xB) + ((shle_rb(v + 2) & 8) ? 80u : 3u));
    shle_wb(v + 2, shle_rb(v + 2) | 1);
}

/* record k's visit (0x602242) */
static void v95_rec_step(unsigned k) {
    uint32_t r = V95_RECS + k * 0x20u;
    uint8_t r0 = shle_rb(r);
    if (!(r0 & 0x80)) return;
    uint16_t d0 = (uint16_t)(shle_rw(r + 0x1C) + shle_rw(r + 0x1A));
    if (d0 & 0x8000) { shle_ww(r + 0x1C, d0); return; }
    uint8_t d2 = shle_rb(r + 1);
    uint32_t a2;
    if (d2 == 0) { if (!shle_ca_upper(k)) return; a2 = shle_rl(r + 0xE); }
    else if (d2 == 0x80) { if (shle_ca_upper(k)) return; a2 = shle_rl(r + 6); }
    else a2 = shle_rl(r + 6);
    uint32_t a1 = shle_rl(r + 2), d5 = shle_rl(r + 0xA);
    if (d5 > 0x200u) {
        shle_wl(r + 0xA, d5 - 0x200u);
        shle_wb(r + 1, d2 + 0x10u);
        shle_ww(r + 0x1C, d0 & 0x7FFFu);
        shle_chunk_copy(a2, a1);
        shle_wl(r + 2, a1 + 0x200u);
        shle_wl(r + 6, a2 + 0x200u);
    } else if (r0 & 1) {                                /* past the end: on from the loop */
        uint32_t w = (0x200u - d5) & 0xFFFFu;
        shle_wl(r + 2, (shle_rl(r + 0x12) & 0xFFFFFFu) + w);
        shle_wl(r + 0xA, (shle_rl(r + 0x16) & 0xFFFFFFu) - w);
        shle_wb(r + 1, d2 + 0x10u);
        shle_ww(r + 0x1C, d0 & 0x7FFFu);
        shle_chunk_copy(a2, a1);
        shle_wl(r + 6, a2 + 0x200u);
    } else if (r0 & 8) {
        if (!shle_ca_upper(k)) v95_rec_finish(r);
    } else {                                            /* the last chunk: where it ends */
        shle_ww(r + 0x1E, (uint16_t)(a2 - shle_rl(r + 0xE) + d5));
        shle_chunk_copy(a2, a1);
        if (!(d2 & 0x80)) {
            shle_wb(r, r0 | 8);
            shle_wb(r + 1, d2 + 0x10u);
        } else v95_rec_finish(r);
    }
}

/* ---- boot (0x600100) ---------------------------------------------------------------- */

static void v95_boot_tracks(void) {                    /* 0x600336 */
    shle_copy(V95_TRACKS, shle_ml(V95_B->trk), 160);
    shle_copy(V95_TRACKS + 160u, shle_ml(V95_B->trk2), 160);
    shle_wb(V95_MVOL, 0xFF);
    for (uint32_t t = V95_TRACKS; t < V95_TRACKS + 20u * 16u; t += 16)
        shle_wb(t + 0xA, v95_level(shle_rb(t + 3), (shle_rb(t) & 1) ? 0xFF : 0xFF));
    shle_wl(V95_NEXTV, V95_VOICES);
}

static void v95_boot_scsp(void) {                      /* 0x60028E (the timers' registers are the HLE's) */
    scsp_t *s = &g_sound.scsp;
    shle_sw8(0x401, 0);
    uint32_t a = shle_ml(V95_B->dsp + 2);
    for (uint32_t k = 0; k < 0x500; k++) shle_sw8(0x700 + k, V95_B->rally ? 0 : shle_mb(a + k));   /* Rally's starts empty */
    shle_sw16(0x402, 312);
    for (uint32_t k = 0; k < 32; k++) {
        shle_sw8(k * 0x20u + 0xB, 0xFF);
        shle_sw8(k * 0x20u + 0xD, 0xFF);
        shle_sw8(k * 0x20u, 0x10);
    }
    for (int k = 0; k < 4; k++)                         /* four reads of MIBUF */
        if (s->mi_r != s->mi_w) { s->mi_r = (uint8_t)((s->mi_r + 1) & 31); s->mi_taken++; }
}

static void v95_boot(uint64_t at) {
    g_shle.booted = false;
    g_shle.restart_at = 0;
    g_shle.busy_until = g_shle.stall_until = 0;
    g_shle.l2_free = 0;
    g_shle.a_ack = 0;
    g_shle.pass_left = V95_PASS_SAMPLES;
    g_shle.pass_rem = 0;
    g_shle.cost = 0;
    sound_ctrl_w(&g_sound, 0x1E);
    shle_sw8(0x400, 0x03);                              /* 18-bit DAC */
    shle_copy(0, 0x600000u, 0x100);                     /* the vectors */
    sound_ctrl_w(&g_sound, 0x1C);
    v95_boot_scsp();
    sound_ctrl_w(&g_sound, 0x18);
    memset(g_sound.ram + 0x1000, 0, 0x3F00);            /* 0x600320 */
    shle_wb(V95_LED, 0xEF);
    if (V95_B->rally) rl_boot_tracks(); else v95_boot_tracks();
    for (uint32_t k = 0; k < 32; k++) {                 /* 0x6003A8: the voices' slots and windows */
        uint32_t v = V95_VOICES + k * 16u;
        shle_ww(v, shle_mw(V95_B->vinit_w + k * 2u));
        if (V95_B->rally) shle_wb(v + 0xD, shle_mb(V95_B->vinit_l + k));   /* a byte: the window's page */
        else shle_wl(v + 0xC, shle_ml(V95_B->vinit_l + k * 4u));
    }
    for (uint32_t k = 0; k < 32; k++) {                 /* 0x6003CC: the records */
        uint32_t r = V95_RECS + k * 0x20u;
        shle_wl(r + 0xE, 0x10000u + k * 0x2000u);
        shle_wb(r + 0x12, k * 2u);
        shle_wb(r + 0x16, k * 8u + 8u);
    }
    v95_efsdl(0);
    if (V95_B->rally) { rl_samples(0, 0x7FFFFu); rl_boot_engines(); }
    else v95_samples(0);
    sound_ctrl_w(&g_sound, 0x10);
    g_shle.boot_at = at + V95_B->boot_fixed + g_shle.cost;
}

/* the driver from its entry point at clock period `t0` (reset, the MIDI byte
 * 0xFF): master volume 0 now, the RAM clear after the delay, the init after it */
static void v95_restart(uint64_t t0) {
    g_shle.booted = false;
    shle_sw8(0x401, 0);
    sound_ctrl_w(&g_sound, 0x1F);
    g_shle.clear_at = t0 + SHLE_CLEAR_DELAY;
    g_shle.cleared = 0;
    g_shle.restart_at = t0 + SHLE_INIT_CYCLES;
    g_shle.boot_at = g_shle.restart_at + V95_B->boot_fixed;   /* until v95_boot knows the copies */
}

/* the main loop's pass over the 32 stream records; its copies make the next one later */
static void v95_pass(scsp_t *s, uint64_t now) {
    g_shle.pass_left = V95_PASS_SAMPLES;
    shle_clear_step(s, now);
    if (!g_shle.booted || now < g_shle.stall_until) return;
    g_shle.chunks = 0;
    for (unsigned k = 0; k < 32; k++) { if (V95_B->rally) rl_rec_step(k); else v95_rec_step(k); }
    uint32_t rc = g_shle.chunks * SHLE_CHUNK_CYCLES;
    g_shle.pass_rem += rc;
    g_shle.pass_left += g_shle.pass_rem / SOUND_CYCLES_PER_SAMPLE;
    g_shle.pass_rem %= SOUND_CYCLES_PER_SAMPLE;
    /* the refills' share of the main loop's time, for a timer's wait in a
     * movem.l (shle_entry): a pass weighs by its length, and the share is
     * scaled to this loop's wait (refill_wait, not SHLE_REFILL_WAIT) */
    if (!V95_B->refill_wait) return;
    uint32_t len = V95_PASS_SAMPLES * SOUND_CYCLES_PER_SAMPLE + rc;
    int64_t f = (int64_t)(((uint64_t)rc << 16) * V95_B->refill_wait / SHLE_REFILL_WAIT / len);
    int64_t fr = g_shle.refill_frac;
    g_shle.refill_frac = (uint32_t)(fr + (f - fr) * len / (16 * V95_PASS_SAMPLES * SOUND_CYCLES_PER_SAMPLE));
}

/* ---- the board's time ------------------------------------------------------------------------- */

/* a handler at e holds up the main loop */
static void v95_suspend(uint64_t e, uint32_t dur) {
    if (g_shle.busy_until < e) g_shle.busy_until = e + V95_LOOP_LAT;
    g_shle.busy_until += dur;
}

static bool v95_events_boot(uint64_t now) {
    if (g_shle.restart_at && now >= g_shle.restart_at) {
        shle_clear_step(&g_sound.scsp, g_shle.restart_at);
        v95_boot(g_shle.restart_at);
    }
    if (now < g_shle.boot_at) return false;
    g_shle.booted = true;
    shle_sw8(0x401, 0x0F);
    sound_ctrl_w(&g_sound, 0x1F);
    g_shle.due_a = (now - SHLE_IRQ_LAT) * SHLE_T16;
    g_shle.due_b = (now + 225u - SHLE_IRQ_LAT) * SHLE_T16;
    g_shle.l2_free = now;
    return true;
}

static bool v95_events_midi(uint64_t now) {
    scsp_t *s = &g_sound.scsp;
    while (s->mi_r != s->mi_w) {
        uint8_t b = s->mi[s->mi_r];
        s->mi_r = (uint8_t)((s->mi_r + 1) & 31);
        s->mi_taken++;
        uint8_t qn = shle_rb(V95_QN);
        if (!v95_midi_byte(b)) { v95_restart(now); return false; }
        uint32_t dur = V95_DUR_MIDI + (shle_rb(V95_QN) != qn ? V95_B->midi_msg : 0u);
        g_shle.l2_free = (g_shle.l2_free > now ? g_shle.l2_free : now) + dur;
        v95_suspend(now, dur);
        bool idle = !qn;
        if (idle && shle_rb(V95_QN)) g_shle.busy_until += V95_EVENT_LEAD;
    }
    return true;
}

static bool v95_events_timer(uint64_t now) {
    uint64_t ea = shle_entry(g_shle.due_a), eb = shle_entry(g_shle.due_b);
    bool a = ea <= eb;                                  /* A is level 2, B level 1 */
    uint64_t e = a ? ea : eb;
    if (e > now) return false;
    g_shle.enq = 0;
    if (a) {
        g_shle.due_a = (e + V95_B->reload_a + shle_period(206, 0)) * SHLE_T16;
        bool idle = !shle_rb(V95_QN);
        v95_timer_a_body();
        uint32_t dur = V95_DUR_A + g_shle.enq * V95_DUR_EVENT;
        v95_suspend(e, dur);
        if (idle && g_shle.enq) g_shle.busy_until += V95_EVENT_LEAD;
        if (g_shle.l2_free < e + dur) g_shle.l2_free = e + dur;
    } else {
        g_shle.due_b = (e + V95_RELOAD_AT_B + shle_period(0xC0, 3)) * SHLE_T16;
        g_shle.cost = 0;
        v95_timer_b_body();
        v95_suspend(e, V95_DUR_B + g_shle.cost);
    }
    v95_dispatch(now);
    return true;
}

static void v95_events(uint64_t now) {
    if (!g_shle.booted && !v95_events_boot(now)) return;
    /* a byte came some time in this sample: a timer due before the middle of
     * it was in its handler first, not held up behind the byte's */
    if (g_sound.scsp.mi_r != g_sound.scsp.mi_w)
        for (int guard = 0; guard < 16 && v95_events_timer(now - SOUND_CYCLES_PER_SAMPLE / 2); guard++) {}
    if (!v95_events_midi(now)) return;
    for (int guard = 0; guard < 16 && v95_events_timer(now); guard++) {}
    v95_dispatch(now);
}

static inline uint64_t v95_next_due(void) {
    if (!g_shle.booted) return g_shle.restart_at && g_shle.restart_at < g_shle.boot_at ? g_shle.restart_at : g_shle.boot_at;
    uint64_t d = shle_entry(g_shle.due_a), x = shle_entry(g_shle.due_b);
    if (x < d) d = x;
    if (shle_rb(V95_QN) && g_shle.busy_until < d) d = g_shle.busy_until;
    return d;
}

#endif /* SOUND_HLE_V95_H */
