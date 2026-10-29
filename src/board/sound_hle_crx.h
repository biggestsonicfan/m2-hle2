/*
 * sound_hle_crx.h — CRX_drv ("Ver3.2.8|96 10/28|CRX_drv|byHiroF"), the sound
 * driver of Dynamite Cop, The House of the Dead and Motor Raid, and its
 * Ver3.4.9 (97 8/26, Pilot Kids), in C in place of the 68000. sound_hle.h
 * includes it and runs it when the program ROM's code is one of these builds
 * (shle_drv_t.crx, k_crx_builds); the chip, the MIDI FIFO and the board's
 * clock are sound_hle.h's.
 *
 * The driver's RAM, absolute (the 68000's a5 is the SCSP, not a base):
 *   0x300  eight sequences x 16 (0 the music, 1-7 effects)
 *   0x380  eight engine records x 16
 *   0x400  32 voices x 0x20                 0x800  the event queue, 0x400 bytes
 *   0xC40  20 tracks x 0x20                 0xEC0  the globals
 *   0xF00  the LFO, 4 bytes a slot          0xF80  a pitch bend word a channel
 *   0x1000 32 stream records x 0x20         0x3000 the sample table
 *   [0x608038] the instruments, copied from [0x60802C]
 * Timer A (level 2) runs the sequencers, timer B (level 1) the release
 * countdowns and the LFO; the main loop streams, a record at a time, and takes
 * one queued event at each record it passes.
 *
 * Voice: +0 slot, +2 flags, +3 track, +4 channel, +5 tone, +6 stream window,
 * +7 exclusive group, +8 velocity, +9 order (bit 7: an effect's), +A transpose,
 * +B priority, +C release length, +E countdown, +10 note, +14 fine tune.
 * Flags: 0 counting down, 1 held by the pedal, 2 streamed, 3 effect, 4 drum,
 * 5 releasing, 6 keyed on, 7 LFO.
 * Track: +0 flags, +1 channel, +2 program, +3 volume, +4 transpose, +5 tune,
 * +6 pan, +7 bend range, +8 LFO word, +A level, +B velocity curve, +C sends,
 * +1E the entry its last note took.
 */
#ifndef SOUND_HLE_CRX_H
#define SOUND_HLE_CRX_H

#define CRX_SEQS     0x300u
#define CRX_ENGS     0x380u
#define CRX_VOICES   0x400u
#define CRX_VEND     0x800u
#define CRX_QUEUE    0x800u
#define CRX_TRACKS   0xC40u
#define CRX_TEND     (CRX_TRACKS + 20u * 0x20u)
#define CRX_RECS     0x1000u
#define CRX_P(o)     (0x608000u + (o))     /* the game's header */

/* the globals */
#define CRX_PANOFS   0xC04u   /* byte: the pan offset, kept over a restart */
#define CRX_SWAPSET  0xC18u
#define CRX_VOLIN    0xC19u
#define CRX_VOLRAW   0xC1Au
#define CRX_ECHO     0xC3Fu   /* MIDI in echoed to MIDI out */
#define CRX_RS       0xEC0u   /* MIDI in: status, first data byte, length, left */
#define CRX_QW       0xEC4u   /* queue write and read pointers (words), the count (byte) */
#define CRX_QR       0xEC6u
#define CRX_QN       0xECCu
#define CRX_VEL      0xECDu
#define CRX_TONE     0xECEu
#define CRX_ECF      0xECFu
#define CRX_SMP      0xED0u   /* long: the sample entry a note takes */
#define CRX_BENDW    0xED4u
#define CRX_SMPRAM   0xED6u   /* the entry's top byte: nonzero for a sample in sound RAM */
#define CRX_PAUSE    0xED7u
#define CRX_WINDOWS  0xED8u   /* long: the stream windows in use */
#define CRX_TEMPO    0xEDCu
#define CRX_PANLO    0xEDDu
#define CRX_NOTE     0xEDEu   /* word */
#define CRX_NMUS     0xEE0u   /* voices: music, effects; streamed: music, effects */
#define CRX_NEFF     0xEE1u
#define CRX_SMUS     0xEE2u
#define CRX_SEFF     0xEE3u
#define CRX_NEXTV    0xEE4u   /* long */
#define CRX_MVOL     0xEE9u
#define CRX_SWAPN    0xEEAu
#define CRX_FADEOUT  0xEEBu
#define CRX_FADECNT  0xEECu
#define CRX_FADELVL  0xEEDu
#define CRX_FADEEND  0xEEEu
#define CRX_FADEKEEP 0xEEFu
#define CRX_EFSDL    0xEF0u
#define CRX_DSPPROG  0xEF1u
#define CRX_FADEIN   0xEF2u
#define CRX_ENTRY    0xEF4u   /* long */
#define CRX_VELIN    0xEF8u
#define CRX_LED      0xEF9u
#define CRX_QMAX     0xEFAu
#define CRX_MONO     0xEFBu

/* A build: where its code keeps its tables (the same bytes in both), and what
 * 3.4.9 does otherwise (CRX_LATE):
 *   - its main loop sets $C3E bit 4 round a note's tracks, where 3.2.8 sets SR
 *     0x2200. Timer A finds $C3E set and counts the tick in $C1D, and its next
 *     run takes the music's sequence once more for each, all at SR 0x2700;
 *     timer B finds bit 4 (or its own bit 1) and skips its body (sub_6006A2);
 *   - A0 00 01/02/03 stop their sequences in MIDI in, ahead of the queue;
 *   - the fixes: the velocity curve's index doubled, drum group 0x7F (the same
 *     note again takes its voice), a steal with all 32 voices on takes any of
 *     them, CC 25 (the velocity curve), A0 02 clears the bends, and the sample
 *     set's copy (move.l blocks, one 256-byte block too many). */
typedef struct {
    uint16_t ver;
    uint32_t trytone, rate, freq, release, coef, init;
    uint32_t lat_less;      /* its main loop takes an interrupt this much sooner than 3.2.8's */
} crx_build_t;

static const crx_build_t k_crx_builds[] = {
    { 0 },
    { 328, 0x602E84u, 0x602F44u, 0x6030C4u, 0x600E28u, 0x603244u, 0x603344u },
    { 349, 0x60303Au, 0x6030FAu, 0x60327Au, 0x600ED2u, 0x6033FAu, 0x6034FAu, 4 },   /* pltkids 40-45 s:
                                                 timer A every 12,707.9 on average against 12,711.8 */
};
#define CRX_B         (&k_crx_builds[g_shle_drv->crx])
#define CRX_LATE      (CRX_B->ver >= 349)
#define CRX_T_TRYTONE (CRX_B->trytone)
#define CRX_T_RATE    (CRX_B->rate)
#define CRX_T_FREQ    (CRX_B->freq)
#define CRX_T_RELEASE (CRX_B->release)
#define CRX_T_DSPWORD 0x600320u
#define CRX_T_COEF    (CRX_B->coef)
#define CRX_C3E       0xC3Eu   /* the handlers' busy flags */
#define CRX_LATE_A    0xC1Du   /* 3.4.9: timer A ticks the music still owes */

/* Board time, clock periods. The handlers' lengths and the dispatch costs are medians from
 * hotd's board traces ($SND_TRACE at 50, 60 and 70 s, interrupts taken out); SCSP.md "CRX_drv"
 * has how they were measured. */
#define CRX_RELOAD_AT_A  95u       /* timer A's handler entered -> its reload written */
#define CRX_RELOAD_AT_B  70u
#define CRX_DUR_A        884u      /* the sequencers, before the events they queue */
#define CRX_DUR_EVENT    481u
#define CRX_A_UNMASK     717u      /* ...to its first event queued (sub_602AB4 leaves SR 0x2000) */
#define CRX_DUR_A_NESTED 228u      /* an A inside A's handler: the reload, then $C3E sends it back */
#define CRX_DUR_B        2590u     /* the countdowns and the LFO over 32 voices */
#define CRX_COST_EVENT   2900u
#define CRX_COST_KEYON   1500u
#define CRX_COST_STREAM  8300u     /* a streamed key-on copies 1 KB */
#define CRX_COST_FREE    2250u     /* a voice freed (sub_6005B0 renumbers all 32) */
#define CRX_COST_STEAL   4780u     /* sub_600F88 looks at all 32 for the one to free */
#define CRX_COST_NOTE    300u      /* a note-on's read, and its return */
#define CRX_COST_SCAN    45u       /* ...its look at a track */
#define CRX_COST_TRACK   1300u     /* a track's note, besides its key-ons */
#define CRX_LOOP_LAT     450u      /* an rte -> the idle loop's look at the queue (the trace's median
                                       * is 261; 450 holds hotd's drift best) */
#define CRX_LOOP_TRIP    7000u     /* the main loop once round the 32 records: a record's next chunk */
#define CRX_CHUNK_IRQ    25u       /* an interrupt that lands in a chunk's copy (0x601A84..0x601B0C, move.l
                                       * into sound RAM) is entered ~48 later than at the idle loop (hotd
                                       * 60-78 s: 110 vs 62); 70 for idle already holds part of it, and 25
                                       * holds the four games' drift best (20-40 all close) */
#define CRX_MASK_OFF     480u      /* a note-off read -> its track masked */
#define CRX_COST_BYTE    22u       /* a move.b loop */
#define CRX_COST_SWAP    11u       /* the swap's move.w loop, a byte */
#define CRX_COST_SWAP_L  6u        /* ...3.4.9's move.l blocks */
#define CRX_COST_DSP     410000u   /* sub_6021B2's fill of the 64 KB delay line, at SR 0x2200 (pltkids
                                       * 10.3 s: 410,745 from sub_60216C in to rts; a scene per song) */
#define CRX_DUR_A_PASS   300u      /* 3.4.9: the music's sequence once more for a tick it owed */
#define CRX_A_MUSIC      400u      /* 3.4.9: timer A's handler at SR 0x2700, the music's first pass */
#define CRX_DUR_B_SKIP   210u      /* 3.4.9: timer B in a note's window: the reload, $C3E, rte */
#define CRX_PASS_SAMPLES 4u        /* the main loop over 32 idle records */
/* EntryPoint: a delay loop, then the clear of sound RAM to 0x70000 (clr.l and
 * dbra, 7.5 clock periods a byte), then the init routines, whose fill of the
 * delay line and copies come to CRX_BOOT_FIXED plus the game's sizes. */
#define CRX_DELAY        557090ull
#define CRX_CLEAR_END    0x70000u
#define CRX_BOOT_FIXED   420000ull

static uint8_t g_crx_panofs;        /* $C04 over the clear, when the restart asked to keep it */
static bool    g_crx_warned_div;

static inline uint8_t crx_hb(uint32_t o) { return shle_mb(CRX_P(o)); }
static inline void crx_inc(uint32_t a, int d) { shle_wb(a, (uint8_t)(shle_rb(a) + d)); }
static inline bool crx_track_on(uint32_t t, uint8_t ch) { return shle_rb(t) && shle_rb(t + 1) == ch; }
static inline uint8_t crx_tidx(uint32_t t) { return (uint8_t)(((t - CRX_TRACKS) & 0xFFFFu) >> 5); }
static inline uint32_t crx_first_track(uint8_t ch) {
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20) if (crx_track_on(t, ch)) return t;
    return 0;
}
static inline uint32_t crx_vslot(uint32_t v) { return shle_rw(v); }

/* divu.w: a quotient past 16 bits leaves the dividend */
static uint16_t crx_divu(uint32_t n, uint16_t d) {
    if (!d) {                                           /* the board takes the zero-divide trap: a restart */
        if (!g_crx_warned_div) { LOG_WARN("sound HLE (CRX): release divide by zero"); g_crx_warned_div = true; }
        return (uint16_t)n;
    }
    uint32_t q = n / d;
    return q > 0xFFFFu ? (uint16_t)n : (uint16_t)q;
}
static inline uint16_t crx_divisor(uint16_t rate) { uint16_t x = rate >> 5; return (uint16_t)(x - (x >> 3)); }

/* a level and a velocity to TL: the product's high byte, inverted, halved */
static inline uint8_t crx_tl(uint8_t a, uint8_t b) {
    uint32_t x = (((uint32_t)a * b) & 0xFFFFu) >> 8;
    return x ? (uint8_t)((uint8_t)~x >> 1) : 0xFF;
}
static inline uint8_t crx_tl_pre(uint32_t p) { return p ? (uint8_t)((uint8_t)~(p >> 8) >> 1) : 0xFF; }
/* sub_6017F8: a controller's 0..127 to a pan */
static inline uint8_t crx_map(uint8_t d0) {
    uint8_t x = d0 >> 2;
    return x < 0x10 ? (uint8_t)(~x & 0x1F) : (uint8_t)(x & 0xF);
}

static void crx_fill(uint32_t dst, uint32_t n, uint32_t pattern) {
    if (g_ssyn.on) ssyn_ram_touch(dst, n);
    else           scsp_ram_touch(&g_sound.scsp, dst, n, 1);
    for (uint32_t k = 0; k < n; k++) shle_wb(dst + k, pattern >> (24 - 8 * (k & 3)));
}

/* ---- the queue, MIDI in ------------------------------------------------------- */

static void crx_enqueue(uint32_t l) {                  /* sub_602AB4 */
    g_shle.enq++;
    uint16_t w = shle_rw(CRX_QW);
    shle_wl(CRX_QUEUE + (w & 0x3FFu), l);
    shle_ww(CRX_QW, w + 4u);
    crx_inc(CRX_QN, 1);
}

static inline void crx_ctrl(uint8_t v) { sound_ctrl_w(&g_sound, v); }

/* Int3, a MIDI byte: false for 0xFF, which restarts the driver */
/* 3.4.9: A0 00 01/02/03 stop their sequences in MIDI in, ahead of the queue */
static void crx_midi_stop(uint8_t b) {
    if (b < 1 || b > 3) return;
    for (uint32_t s = b == 3 ? 1 : 0; s < (b == 2 ? 1u : 8u); s++) shle_wb(CRX_SEQS + s * 0x10u, 0);
}

static bool crx_midi_byte(uint8_t b) {
    if (shle_rb(CRX_ECHO)) shle_sw8(0x407, b);
    if (b & 0x80) {
        if (b == 0xFF) return false;
        if (b >= 0xF0) return true;
        uint8_t n = (b >= 0xC0 && b < 0xE0) ? 1 : 2;
        shle_wb(CRX_RS, b);
        shle_wb(CRX_RS + 2, n);
        shle_wb(CRX_RS + 3, n);
        return true;
    }
    uint8_t d1 = shle_rb(CRX_RS + 3);
    if (!d1) d1 = shle_rb(CRX_RS + 2);
    if (!d1) return true;
    d1--;
    if (d1) shle_wb(CRX_RS + 1, b);
    else {
        uint32_t ev = shle_rb(CRX_RS + 2) != 1 ? (uint32_t)shle_rb(CRX_RS) << 24 | (uint32_t)shle_rb(CRX_RS + 1) << 16 | (uint32_t)b << 8
                                               : (uint32_t)shle_rb(CRX_RS) << 24 | (uint32_t)b << 16;
        uint16_t w = shle_rw(CRX_QW);
        shle_wl(CRX_QUEUE + (w & 0x3FFu), ev);
        if (CRX_LATE && (ev >> 16) == 0xA000u) crx_midi_stop(b);
        shle_ww(CRX_QW, w + 4u);
        crx_inc(CRX_QN, 1);
        uint8_t led = shle_rb(CRX_LED);
        led = (uint8_t)(led << 1 | led >> 7);
        shle_wb(CRX_LED, led);
        crx_ctrl((uint8_t)((led & 0xF) | crx_hb(0x68)));
    }
    shle_wb(CRX_RS + 3, d1);
    return true;
}

/* ---- voices --------------------------------------------------------------------- */

static void crx_order_close(uint8_t d2, bool fx) {
    for (uint32_t u = CRX_VOICES; u < CRX_VEND; u += 0x20) {
        uint8_t x = shle_rb(u + 9);
        if (((x & 0x80) != 0) == fx && x > d2) shle_wb(u + 9, x - 1u);
    }
}

static void crx_free(uint32_t v) {                     /* sub_6005B0 */
    uint32_t slot = crx_vslot(v);
    shle_sw8(slot + 0xB, 0x1F);
    shle_sw8(slot, 0x10);
    uint8_t f = shle_rb(v + 2);
    if (f & 4) {
        shle_ww(v + 0xC00, 0);                          /* its stream record stops */
        shle_wl(CRX_WINDOWS, shle_rl(CRX_WINDOWS) & ~(1u << (shle_rb(v + 6) & 31)));
    }
    shle_ww(v + 2, 0x00FF);
    shle_wl(v + 0xC, 0);
    shle_wb(v + 0xB, 0);
    shle_wb(v + 7, 0);
    uint8_t d2 = shle_rb(v + 9);
    if (!(d2 & 0x80)) {
        shle_wb(v + 9, 0);
        crx_inc(CRX_NMUS, -1);
        if (f & 4) crx_inc(CRX_SMUS, -1);
        crx_order_close(d2, false);
    } else {
        crx_inc(CRX_NEFF, -1);
        if (f & 4) crx_inc(CRX_SEFF, -1);
        shle_wb(v + 9, 0);
        crx_order_close(d2, true);
    }
}

/* the LFO step of timer B, for a voice with flags bit 7 */
static void crx_lfo_step(uint32_t v) {
    uint32_t slot = crx_vslot(v), i = 0xF00u + (slot >> 3);
    uint8_t c = (uint8_t)(shle_rb(i) - 1), d2;
    shle_wb(i, c);
    if (c) d2 = shle_rb(i + 1);
    else {
        d2 = shle_rb(i + 2);
        if (!d2) { shle_wb(v + 2, shle_rb(v + 2) & 0x7F); return; }
        shle_wb(i, d2);
        if (shle_rb(i + 3)) {
            shle_wb(i + 1, shle_rb(i + 3));
            shle_wb(i + 2, 0);
            d2 = shle_rb(i + 3);
        } else {
            d2 = (uint8_t)-shle_rb(i + 1);
            shle_wb(i + 1, d2);
        }
    }
    uint32_t a = 0x600u + (slot >> 4);
    shle_sw16(a, (uint16_t)(shle_sr16(a) + (int8_t)d2));
}

/* Int1, timer B: the countdowns, then the LFO; the voices it freed */
static uint32_t crx_timer_b_body(void) {
    uint32_t n = 0;
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20) {
        if (shle_rb(v + 2) & 1) {
            uint16_t c = (uint16_t)(shle_rw(v + 0xE) - 1);
            shle_ww(v + 0xE, c);
            if (!c) { crx_free(v); n++; }
        }
        if (shle_rb(v + 2) & 0x80) crx_lfo_step(v);
    }
    return n;
}

/* ---- allocation ------------------------------------------------------------------ */

/* sub_600958: the sample a note plays; false when there is none */
static bool crx_sample(uint16_t idx) {
    uint32_t a1;
    if (idx & 0x8000) a1 = 0x608080u + (idx & 0x3FFu) * 12u;
    else {
        if ((int16_t)idx > (int16_t)shle_rw(0x3002)) return false;
        a1 = 0x3004u + (uint32_t)(uint16_t)(idx - shle_rw(0x3000)) * 12u;
    }
    shle_wl(CRX_SMP, a1);
    shle_wb(CRX_SMPRAM, shle_mb(a1));
    return shle_rb(CRX_SMPRAM) || crx_hb(0x7D);
}

/* sub_600F88; `any` (3.4.9, all 32 voices on): a sample in ROM may take a voice that is not streaming */
static uint32_t crx_steal(bool any) {
    uint8_t d1 = 0x20, d6 = 0x20, ram = shle_rb(CRX_SMPRAM);
    uint32_t a1 = 0, a3 = 0;
    g_shle.cost += CRX_COST_STEAL;
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20) {
        uint8_t f = shle_rb(v + 2), o = shle_rb(v + 9);
        if (!f || (!any && !ram && !(f & 4))) continue;
        if (o <= d1) { d1 = o; a1 = v; }
        if ((f & 0x20) && o <= d6) { d6 = o; a3 = v; }
    }
    if (d6 != 0x20) a1 = a3;
    if (a1) { crx_free(a1); g_shle.cost += CRX_COST_FREE; }
    return a1;
}

static uint32_t crx_alloc(void) {                      /* sub_600F32 */
    if (!shle_rb(CRX_SMPRAM) && (int8_t)(shle_rb(CRX_SEFF) + shle_rb(CRX_SMUS)) >= (int8_t)crx_hb(0x7D))
        return crx_steal(false);
    if ((uint8_t)(shle_rb(CRX_NMUS) + shle_rb(CRX_NEFF)) == 0x20) return crx_steal(CRX_LATE);
    uint32_t a = shle_rl(CRX_NEXTV);
    for (int k = 0; k < 32; k++, a += 0x20) {
        if (a >= CRX_VEND) a = CRX_VOICES;
        if (!shle_rb(a + 2)) { shle_wl(CRX_NEXTV, a + 0x20); return a; }
    }
    return 0;
}

/* loc_6008A0: the effect voice of the lowest priority, the oldest of those */
static uint32_t crx_scan(uint32_t e, uint8_t d3) {
    uint8_t d0 = 0, d1 = crx_hb(0x7C) | 0x80, d2 = d3 ? crx_hb(0x7C) : crx_hb(0x7E);
    uint32_t a1 = 0;
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20) {
        uint8_t o = shle_rb(v + 9), pr = shle_rb(v + 0xB);
        if (!(o & 0x80) || (!d3 && !(shle_rb(v + 2) & 4))) continue;
        if ((int8_t)d0 < (int8_t)pr) { d0 = pr; d1 = o; a1 = v; }
        else if (d0 == pr && !((int8_t)d1 < (int8_t)o)) { d1 = o; a1 = v; }
        if (--d2 == 0) {
            if (d0 < shle_mb(e + 0xA) || !a1) return 0;
            crx_free(a1);
            g_shle.cost += CRX_COST_FREE;
            return a1;
        }
    }
    return 0;
}

static uint32_t crx_pool(uint32_t t, uint32_t e) {     /* loc_600856 */
    if (!(shle_rb(t) & 1)) return crx_alloc();
    uint8_t d3 = shle_rb(CRX_SMPRAM);
    if (!d3) {
        if ((int8_t)shle_rb(CRX_SEFF) >= (int8_t)crx_hb(0x7E)) return crx_scan(e, 0);
        if ((int8_t)(shle_rb(CRX_SEFF) + shle_rb(CRX_SMUS)) >= (int8_t)crx_hb(0x7D)) return crx_steal(false);
        d3 = 0x80;
    }
    if ((int8_t)crx_hb(0x7C) > (int8_t)shle_rb(CRX_NEFF)) return crx_alloc();
    return crx_scan(e, d3);
}

/* ---- pitch, pan --------------------------------------------------------------- */

/* sub_601136: the voice's frequency; returns the table index, *oct the octave carry */
static int16_t crx_freq(uint32_t v, uint16_t d5, uint8_t d2, uint16_t *oct) {
    uint8_t d4 = (uint8_t)(d5 >> 8);
    int32_t d3 = (int32_t)(int8_t)(uint8_t)(shle_rb(v + 0xA) + shle_rb(v + 0x14) + (d5 & 0xFF)) * 2;
    int32_t d0 = (int32_t)(int8_t)(uint8_t)((d2 & 0xF) + d4) * 32 + d3;
    uint16_t w = (uint16_t)d0, o = 0;
    if (d0 < 0) { w = (uint16_t)(w + 0x180); o = 0xF800; }
    else if ((uint32_t)d0 >= 0x180) { w = (uint16_t)(w - 0x180); o = 0x800; }
    uint16_t f = (uint16_t)(((d2 & 0xF0u) << 7) + o + shle_mw(CRX_T_FREQ + (uint32_t)(int32_t)(int16_t)w));
    shle_sw16(crx_vslot(v) + 0x10, f & 0x7BFF);
    if (oct) *oct = o;
    return (int16_t)w;
}

static inline uint8_t crx_pan_value(uint8_t d0) {
    int p = (d0 & 0xE0) >> 5;
    p += (int8_t)shle_rb(CRX_PANOFS);
    if (p < 0) p = 0;
    if (p > 7) p = 7;
    return (uint8_t)(p << 5 | (d0 & 0x1F));
}

static void crx_pan(uint32_t slot, uint8_t d0) {       /* sub_600EF6 */
    uint8_t p = crx_pan_value(d0);
    if (shle_rb(CRX_MONO)) { shle_wb(CRX_PANLO, p & 0xF); p &= 0xE0; }
    shle_sw8(slot + 0x16, p);
}

/* ---- key-on ---------------------------------------------------------------------- */

/* loc_600ED0: the key-on itself; a0 is the track or engine record */
static void crx_keyon_done(uint32_t a0, uint32_t v, uint32_t slot, uint32_t cost) {
    shle_sw8(slot + 0x12, shle_sr8(slot + 0x12) & 0x7F);
    shle_wb(v + 3, (uint8_t)(((a0 - CRX_TRACKS) & 0xFFFFu) >> 5));
    shle_wb(v + 2, shle_rb(v + 2) | 0x40);
    shle_sw8(slot, shle_sr8(slot) | 0x18);
    g_shle.keyons++;
    g_shle.cost += cost;
}

/* loc_600E68: a sample in sound RAM */
static void crx_keyon_ram(uint32_t v, uint32_t slot, uint32_t a2, uint32_t d7, uint16_t rate) {
    d7 &= 0xFFFFFu;
    uint32_t d5 = shle_ml(a2 + 4) - 1u;
    if ((int32_t)d5 < 0) {                              /* looped */
        shle_sw32(slot, d7 | 0x10200000u | ((shle_mb(a2 + 8) & 0x80) ? 0 : 0x100000u));
        shle_sw16(slot + 6, d5);
        shle_sw16(slot + 4, (shle_ml(a2 + 8) & 0x7FFFFFFFu) - d7);
        shle_wb(v + 2, shle_rb(v + 2) & ~0x10u);
    } else {
        shle_sw32(slot, d7 | 0x10100000u);
        shle_sw32(slot + 4, d5 & 0xFFFFu);
        shle_ww(v + 0xE, crx_divu(d5, crx_divisor(rate)));
        shle_wb(v + 2, shle_rb(v + 2) | 1);
    }
}

/* a streamed sample's loop: where its record goes back to */
static void crx_stream_loop(uint32_t v, uint32_t r, uint32_t a2, uint32_t d7, uint32_t d1) {
    uint32_t d0 = shle_ml(a2 + 8) & 0x7FFFFFFFu;
    shle_wl(r + 0x14, d0);
    d0 -= d7;
    uint32_t len = d1 - d0, d2 = d1 & 0x1FFFu;
    if ((int32_t)len > (int32_t)d2) shle_wl(r + 0x18, len - 0x200u);
    else {                                              /* the loop fits the window: the chip loops it */
        shle_wl(r + 0x14, d0 & 0x1FFFu);
        if ((int16_t)d1 > 0x1000 && (int16_t)d2 <= 0x1000) shle_wb(r + 0x14, 0x80);
        shle_ww(r + 0x1A, d2);
        shle_wb(r + 0x18, 0x80);
    }
    shle_wb(r, shle_rb(r) | 1);
    shle_wb(v + 2, shle_rb(v + 2) & ~0x10u);
}

/* loc_600B80: a sample streamed from ROM through a 8 KB window; false when no window is free */
static bool crx_keyon_stream(uint32_t v, uint32_t slot, uint32_t a2, uint32_t d7, uint16_t rate) {
    uint32_t used = shle_rl(CRX_WINDOWS);
    int j = 0;
    while (j < 32 && (used & (1u << (31 - j)))) j++;
    if (j == 32) return false;
    uint32_t win = 0x6E000u - (uint32_t)j * 0x2000u, r = CRX_RECS + (v & 0x3E0u);
    shle_wl(CRX_WINDOWS, used | 1u << (31 - j));
    shle_wb(v + 6, (uint8_t)(31 - j));
    shle_sw32(slot + 4, 0x1FFF);
    shle_wl(r + 4, d7);
    shle_wl(r + 8, win);
    shle_wl(r + 0x10, win);
    shle_wb(r, 0);
    shle_sw32(slot, win | 0x10200000u | ((shle_mb(a2 + 8) & 0x80) ? 0 : 0x100000u));
    uint32_t d1 = shle_ml(a2 + 4);
    bool loop = (d1 & 0x80000000u) != 0;
    d1 &= 0x7FFFFFFFu;
    if (loop) crx_stream_loop(v, r, a2, d7, d1);
    else {
        uint16_t c = crx_divu(d1 - 1u, crx_divisor(rate));
        shle_ww(v + 0xE, c ? c : 1);
        d1 -= 0x200u;
        shle_wb(v + 2, shle_rb(v + 2) | 1);
    }
    shle_wl(r + 0xC, d1 - 0x400u);
    shle_wb(r + 1, 0x20);
    shle_wb(v + 2, shle_rb(v + 2) | 4);
    shle_wb(r, shle_rb(r) | 0x80);
    shle_copy(win, d7, 0x400);
    shle_wl(r + 4, d7 + 0x400u);
    shle_wl(r + 8, win + 0x400u);
    return true;
}

/* loc_600B78: the sample at $ED0 keyed on in voice v at `rate` */
static void crx_keyon(uint32_t a0, uint32_t v, uint16_t rate) {
    uint32_t slot = crx_vslot(v), a2 = shle_rl(CRX_SMP), d7 = shle_ml(a2);
    if (d7 & 0x80000000u) { crx_keyon_ram(v, slot, a2, d7, rate); crx_keyon_done(a0, v, slot, CRX_COST_KEYON); }
    else if (crx_keyon_stream(v, slot, a2, d7, rate)) crx_keyon_done(a0, v, slot, CRX_COST_STREAM);
}

/* ---- note on: the voice's setup ------------------------------------------------------ */

/* the rate a release countdown runs at, from the tone and the octave carry */
static uint16_t crx_rate(uint8_t d2, int16_t idx, uint16_t oct) {
    uint16_t d6 = oct == 0x800 ? 1 : oct == 0xF800 ? 0xFFFF : 0;
    uint16_t d0 = (uint16_t)(((d2 & 0xF0u) >> 4) + d6);
    if (d0 && d0 < 8) return 0x7FFF;
    return (uint16_t)(shle_mw(CRX_T_RATE + (uint32_t)(int32_t)idx) >> ((uint8_t)(0x10 - (uint8_t)d0) & 0xF));
}

/* an LFO instrument's entry: the chip's LFO and the driver's own */
static void crx_setup_lfo(uint32_t v, uint32_t slot, uint32_t e) {
    shle_sw8(slot + 0xC, 2);
    shle_sw8(slot + 0xE, shle_mb(e + 0xA));
    uint32_t d1 = 0x600u + (slot >> 4);
    shle_sw16(d1, shle_mw(e + 0xC));
    shle_sw16(d1 + 0x40, shle_mw(e + 0xE));
    uint8_t d0 = shle_mb(e + 0xB);
    if (!d0) return;
    shle_wb(v + 2, shle_rb(v + 2) | 0x80);
    uint32_t f = 0xF00u + (slot >> 3);
    uint8_t d6 = shle_mb(e + 0x11);
    shle_wb(f, d0);
    shle_wb(f + 1, shle_mb(e + 0x10));
    shle_wb(f + 3, d6);
    shle_wb(f + 2, d6 == 0x80 ? 0 : d0);
}

/* loc_600A5E: what every note's voice gets, then the key-on */
static void crx_setup_common(uint32_t t, uint32_t v, uint8_t d2, uint8_t ch) {
    uint32_t slot = crx_vslot(v), e = shle_rl(CRX_ENTRY);
    shle_ww(v + 0x10, shle_rw(CRX_NOTE));
    shle_wb(v + 5, d2);
    shle_wb(v + 4, shle_rb(t + 1));
    shle_wb(v + 0x14, shle_rb(t + 5));
    shle_wb(v + 8, shle_rb(CRX_VEL));
    uint8_t tl = crx_tl(shle_rb(t + 0xA), shle_rb(CRX_VEL));
    if (shle_rb(CRX_MONO)) {
        unsigned x = tl + shle_rb(CRX_PANLO);
        tl = x > 0xFF ? 0xFF : (uint8_t)x;
        if (x <= 0xFF) { x = tl + 3u; tl = x > 0xFF ? 0xFF : (uint8_t)x; }
    }
    shle_sw8(slot + 0xD, tl);
    uint16_t oct;
    int16_t idx = crx_freq(v, shle_rw(0xF80u + ch * 2u), d2, &oct);
    uint16_t rate = crx_rate(d2, idx, oct);
    shle_sw16(slot + 0x12, shle_rw(t + 8));
    shle_sw32(slot + 8, shle_ml(e + 6));
    shle_ww(v + 0xC, shle_mw(CRX_T_RELEASE + (shle_mb(e + 9) & 0x1Fu) * 2u));
    if (shle_rb(t) & 0x10) crx_setup_lfo(v, slot, e);
    else shle_sw16(slot + 0xE, 0);
    crx_keyon(t, v, rate);
}

static void crx_setup(uint32_t t, uint32_t v, uint32_t e, uint8_t ch) {   /* sub_6009A2 */
    uint32_t slot = crx_vslot(v);
    uint8_t t0 = shle_rb(t), d2;
    bool ram = shle_rb(CRX_SMPRAM) != 0;
    shle_sw8(slot, 0x10);
    if (t0 & 2) {                                       /* a drum */
        shle_wb(v + 0xB, shle_mb(e + 0xA));
        shle_wb(v + 7, shle_mb(e + 0xB) & 0x7F);
        shle_wb(v + 0xA, shle_mb(e + 3));
        crx_pan(slot, (shle_mb(e + 5) & 0x80) ? shle_rb(t + 6) : shle_mb(e + 4));
        uint8_t d0 = shle_mb(e + 5);
        if (!(d0 & 0x7F)) d0 = shle_rb(t + 0xC);
        shle_sw8(slot + 0x15, d0 & 0x7F);
        d2 = shle_mb(e + 2);
        shle_wb(v + 2, shle_rb(v + 2) | 0x10);
        if (t0 & 1) {                                   /* an effect's */
            shle_wb(v + 9, shle_rb(CRX_NEFF) | 0x80);
            crx_inc(CRX_NEFF, 1);
            shle_wb(v + 2, shle_rb(v + 2) | 8);
            if (!ram) crx_inc(CRX_SEFF, 1);
            crx_setup_common(t, v, d2, ch);
            return;
        }
    } else {
        d2 = shle_rb(CRX_TONE);
        shle_wb(v + 0xB, 0);
        shle_wb(v + 0xA, shle_mb(e + 2));
        crx_pan(slot, shle_rb(t + 6));
        shle_sw8(slot + 0x15, shle_rb(t + 0xC));
    }
    shle_wb(v + 9, shle_rb(CRX_NMUS));
    crx_inc(CRX_NMUS, 1);
    if (!ram) crx_inc(CRX_SMUS, 1);
    crx_setup_common(t, v, d2, ch);
}

/* ---- note on, note off, bend ----------------------------------------------------------- */

static void crx_release_voice(uint32_t v) {
    shle_sw8(crx_vslot(v), 0x10);
    shle_wb(v + 2, shle_rb(v + 2) | 0x21);
    uint16_t d0 = shle_rw(v + 0xC);
    if (!d0) { shle_wl(v + 0xC, 1); return; }
    uint16_t c = shle_rw(v + 0xE);
    if (!c || d0 < c) shle_ww(v + 0xE, d0);
}

/* loc_60104A: the voices playing note d3 on the track's channel */
static void crx_note_off_voices(uint32_t t, uint8_t ch, uint16_t d3) {
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20) {
        uint8_t f = shle_rb(v + 2);
        if ((f & 0x72) != 0x40 || shle_rw(v + 0x10) != d3 || shle_rb(v + 4) != ch) continue;
        if (shle_rb(t) & 0x40) shle_wb(v + 2, f | 2);   /* the pedal holds it */
        else crx_release_voice(v);
    }
}

/* the drum's exclusive group: false when a note of the group must not sound */
static uint32_t crx_drum_voice(uint32_t t, uint32_t e, uint8_t ch) {
    uint8_t x = shle_mb(e + 0xB);
    if (x & 0x80) {
        for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20)
            if (shle_rb(v + 4) == ch && shle_rb(v + 7) == (x & 0x7F)) return 0;
    } else if (x == 0x7F && CRX_LATE) {                 /* the same note again: its voice */
        uint32_t from = CRX_VOICES + shle_rl(0xC10);
        for (uint32_t v = from; v < from + 0x400u; v += 0x20)
            if (shle_rb(v + 4) == ch && shle_rb(v + 7) == x && shle_rw(v + 0x10) == shle_rw(CRX_NOTE)) {
                crx_free(v);
                g_shle.cost += CRX_COST_FREE;
                return v;
            }
    } else if (x) {
        for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20)
            if (shle_rb(v + 4) == ch && shle_rb(v + 7) == x) { crx_free(v); g_shle.cost += CRX_COST_FREE; return v; }
    }
    return crx_pool(t, e);
}

static void crx_note_drum(uint32_t t, uint32_t a2, uint8_t note, uint8_t b3, uint8_t ch) {
    uint32_t e;
    if (shle_mb(a2 + 1)) {
        uint16_t d3 = (uint16_t)(note + (b3 << 7));
        if ((int16_t)d3 > (int16_t)shle_mw(a2 + 2)) return;
        e = a2 + 4u + d3 * 12u;
    } else {
        uint8_t lo = shle_mb(a2 + 2);
        if ((int8_t)note < (int8_t)lo || note >= shle_mb(a2 + 3)) return;
        e = a2 + 4u + (uint8_t)(note - lo) * 12u;
    }
    shle_wl(CRX_ENTRY, e);
    shle_ww(t + 0x1E, e);
    if ((shle_mw(e) & 0x8000) || !crx_sample(shle_mw(e))) return;
    uint32_t v = crx_drum_voice(t, e, ch);
    if (v) crx_setup(t, v, e, ch);
}

static void crx_note_split(uint32_t t, uint32_t a2, uint8_t note, uint8_t ch) {   /* loc_60090A */
    uint8_t d3 = (uint8_t)(note + shle_rb(t + 4));
    if (d3 & 0x80) return;
    uint32_t stride = 0xA;
    if (shle_mb(a2) & 0x80) { stride = 0x12; a2 += 2; }
    uint32_t e = a2;
    int k = 0;
    while (k < 128 && d3 > shle_mb(e)) { k++; e += stride; }
    if (k == 128) return;
    shle_wl(CRX_ENTRY, e);
    shle_ww(t + 0x1E, e);
    shle_wb(CRX_TONE, shle_mb(CRX_T_TRYTONE + (uint8_t)(d3 + shle_mb(e + 3))));
    if (!crx_sample(shle_mw(e + 4))) return;
    uint32_t v = crx_alloc();
    if (v) crx_setup(t, v, e, ch);
}

static void crx_note_track(uint32_t t, uint8_t note, uint8_t b3, uint8_t ch) {      /* loc_60076C */
    if (!shle_rb(CRX_VELIN)) { crx_note_off_voices(t, ch, (uint16_t)(note | b3 << 8)); return; }
    uint32_t vt = shle_ml(CRX_P(0x28));
    uint32_t a1 = vt + shle_mw(vt + 2u + shle_rb(t + 0xB) * (CRX_LATE ? 2u : 1u));
    shle_wb(CRX_VEL, shle_mb(a1 + shle_rb(CRX_VELIN)));
    uint32_t ins = shle_ml(CRX_P(0x38));
    uint16_t off = shle_mw(ins + 2u + shle_rb(t + 2) * 2u);
    if (off & 0x8000) return;
    uint32_t a2 = ins + off;
    if (shle_mb(a2) == 0xFF) crx_note_drum(t, a2, note, b3, ch);
    else crx_note_split(t, a2, note, ch);
}

/* The tracks run at SR 0x2200 one at a time (cost offsets; crx_dispatch makes them times);
 * `soft`: 3.4.9's $C3E bit 4 in its place */
static void crx_mask(uint32_t from, uint32_t until, bool soft) {
    if (g_shle.amask_n < 20u) {
        if (soft) g_shle.amask_soft |= 1u << g_shle.amask_n;
        else      g_shle.amask_soft &= ~(1u << g_shle.amask_n);
        g_shle.amask_win[g_shle.amask_n][0] = from;
        g_shle.amask_win[g_shle.amask_n++][1] = until;
    }
}

static void crx_note_on(uint32_t p, uint8_t ch) {      /* sub_60072E */
    uint8_t note = shle_rb(p), b3 = shle_rb(p + 2);
    shle_ww(CRX_NOTE, (uint16_t)(b3 << 8 | note));
    shle_wb(CRX_VELIN, shle_rb(p + 1));
    g_shle.cost = CRX_COST_NOTE;
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20) {
        g_shle.cost += CRX_COST_SCAN;
        if (!crx_track_on(t, ch)) continue;
        uint32_t from = g_shle.cost;
        g_shle.cost += CRX_COST_TRACK;
        crx_note_track(t, note, b3, ch);
        crx_mask(from, g_shle.cost, CRX_LATE);
    }
    g_shle.cost += CRX_COST_NOTE;
}

static void crx_note_off(uint32_t p, uint8_t ch) {     /* sub_600FEA */
    uint32_t t = crx_first_track(ch);
    if (!t) return;
    crx_note_off_voices(t, ch, (uint16_t)(shle_rb(p) | shle_rb(p + 2) << 8));
    crx_mask(CRX_MASK_OFF, g_shle.cost, CRX_LATE);
}

static void crx_bend(uint32_t p, uint8_t ch) {         /* sub_601092 */
    uint32_t t = crx_first_track(ch);
    if (!t) return;
    uint16_t d2 = (uint16_t)((shle_rb(p) | shle_rb(p + 1) << 7) >> 5);
    bool up = (d2 & 0x100) != 0;
    uint8_t m = (uint8_t)d2;
    if (!up) { m = (uint8_t)-m; if (!m) m = 0xFF; }
    uint8_t r = shle_rb(t + 7);
    if (!r) return;
    uint32_t x = ((uint32_t)r * m) >> 8;
    uint8_t hi = (uint8_t)((x & 0xF0) >> 4), lo = (uint8_t)(x & 0xF);
    if (!up) { hi = (uint8_t)-hi; lo = (uint8_t)-lo; }
    uint16_t w = (uint16_t)(hi << 8 | lo);
    uint8_t c = shle_rb(t + 1);
    shle_ww(CRX_BENDW, w);
    shle_ww(0xF80u + c * 2u, w);
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20)
        if (shle_rb(v + 4) == c && shle_rb(v + 2)) crx_freq(v, w, shle_rb(v + 5), NULL);
}

/* ---- controllers ----------------------------------------------------------------------- */

static void crx_track_lfo(uint32_t t) {                /* loc_6016B8 */
    uint8_t i = crx_tidx(t);
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20)
        if (shle_rb(v + 3) == i) shle_sw16(crx_vslot(v) + 0x12, shle_rw(t + 8) & 0x7FFF);
}

static uint8_t crx_level(uint32_t t, uint8_t m) {      /* a track's level from its volume */
    uint32_t x = (((uint32_t)shle_rb(t + 3) * m) & 0xFFFFu) >> 8;
    return (uint8_t)(x ? x + 1 : 0);
}

static void crx_cc_01(uint8_t ch, uint8_t d0in) {
    shle_wb(CRX_VEL, (uint8_t)(d0in << 1) & 0xE0);
    uint32_t ins = shle_ml(CRX_P(0x38));
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20) {
        if (!crx_track_on(t, ch)) continue;
        uint8_t d0 = shle_rb(CRX_VEL);
        uint16_t off = shle_mw(ins + 2u + shle_rb(t + 2) * 2u);
        if (off & 0x8000) return;
        uint32_t a1 = ins + off;
        bool limit = true;
        if (shle_mb(a1) & 0x80) {
            if (shle_mw(a1) != 0x8000) limit = false;
            else a1 += 2;
        }
        if (limit) {
            uint8_t b = shle_mb(a1 + 1), cap = b & 0xE0;
            unsigned s = (d0 < cap ? d0 : cap) + ((b & 7u) << 5);
            d0 = s > 0xFF ? 0xE0 : (uint8_t)s;
        }
        uint8_t t9 = shle_rb(t + 9);
        if ((t9 & 0xE0) == d0) return;
        shle_wb(t + 9, (t9 & 0x1F) | d0);
        crx_track_lfo(t);
    }
}

static void crx_cc_02(uint8_t ch, uint8_t d0) {
    d0 >>= 4;
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20) {
        if (!crx_track_on(t, ch)) continue;
        uint8_t t9 = shle_rb(t + 9);
        if ((t9 & 7) == d0) return;
        shle_wb(t + 9, (t9 & 0xF8) | d0);
        crx_track_lfo(t);
    }
}

static void crx_cc_volume(uint8_t ch, uint8_t d0) {
    uint8_t vol = (uint8_t)(d0 << 1);
    if (vol) vol++;
    uint8_t ta = 0;
    bool any = false;
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20) {
        if (!crx_track_on(t, ch)) continue;
        shle_wb(t + 3, vol);
        ta = vol ? crx_level(t, (shle_rb(t) & 1) ? 0xFF : shle_rb(CRX_MVOL)) : 0;
        shle_wb(t + 0xA, ta);
        any = true;
    }
    if (!any) return;
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20)
        if (shle_rb(v + 4) == ch && shle_rb(v + 2))
            shle_sw8(crx_vslot(v) + 0xD, crx_tl_pre((uint32_t)shle_rb(v + 8) * ta));
}

static void crx_cc_pan(uint8_t ch, uint8_t d0) {
    if (shle_rb(CRX_MONO)) return;
    d0 = crx_map(d0);
    uint32_t last = 0;
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20) {
        if (!crx_track_on(t, ch)) continue;
        shle_wb(t + 6, (shle_rb(t + 6) & 0xE0) | d0);
        last = t;
    }
    if (!last) return;
    uint8_t i = crx_tidx(last);
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20)
        if (shle_rb(v + 3) == i) shle_sw8(crx_vslot(v) + 0x16, crx_pan_value(shle_rb(last + 6)));
}

static inline uint8_t crx_min(uint8_t v, uint8_t m) { return v < m ? v : m; }

/* 11-2B: a field of every track on the channel; true when the LFO word changed */
static bool crx_cc_field(uint32_t t, uint8_t cc, uint8_t d0) {
    switch (cc) {
    case 0x11: shle_wb(t + 4, d0 - 0x40u); return false;
    case 0x12: shle_wb(t + 5, d0 - 0x40u); return false;
    case 0x13: shle_wb(t + 7, (uint8_t)(crx_min(d0, 12) << 4)); return false;
    case 0x14: if (d0 >= 3) return false; shle_wb(t + 8, (shle_rb(t + 8) & 0xFC) | d0); return true;
    case 0x15: if (d0 >= 4) return false; shle_wb(t + 9, (shle_rb(t + 9) & 0xE7) | d0 << 3); return true;
    case 0x18: shle_wb(t + 8, (shle_rb(t + 8) & 0x83) | crx_min(d0, 0x1F) << 2); return true;
    case 0x19: shle_wb(t + 6, (shle_rb(t + 6) & 0x1F) | crx_min(d0, 7) << 5); return false;
    case 0x2A: shle_wb(t + 0xC, (shle_rb(t + 0xC) & 0x78) | crx_min(d0, 7)); return false;
    default:   shle_wb(t + 0xC, (shle_rb(t + 0xC) & 7) | ((d0 << 3) & 0x78)); return false;   /* 2B */
    }
}

/* 30-36: the entry the track's last note took, in the instruments in RAM */
static void crx_cc_entry(uint8_t ch, uint8_t cc, uint8_t d0) {
    uint8_t m = crx_min(d0, 0x1F);
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20) {
        if (!crx_track_on(t, ch)) continue;
        uint32_t a1 = shle_rw(t + 0x1E);
        if (!a1) return;
        switch (cc) {
        case 0x30: shle_wb(a1 + 7, (shle_rb(a1 + 7) & 0xE0) | m); break;
        case 0x31: shle_ww(a1 + 8, (shle_rw(a1 + 8) & 0x7C1F) | m << 5); break;
        case 0x32: shle_ww(a1 + 6, (shle_rw(a1 + 6) & 0xF83F) | m << 6); break;
        case 0x33: shle_wb(a1 + 6, (shle_rb(a1 + 6) & 7) | m << 3); break;
        case 0x34: shle_wb(a1 + 9, (shle_rb(a1 + 9) & 0xE0) | m); break;
        default:   shle_wb(a1 + 2, d0 < 0x40 ? d0 | 0xC0 : d0 & 0x3F); break;   /* 36 */
        }
    }
}

static void crx_cc_lfo_push(uint8_t ch, uint32_t a1) {
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20) {
        if (shle_rb(v + 4) != ch || !shle_rb(v + 2)) continue;
        uint32_t slot = crx_vslot(v), d1 = 0x600u + (slot >> 4);
        shle_sw8(slot + 0xE, shle_rb(a1 + 0xA));
        shle_sw16(d1, shle_rw(a1 + 0xC));
        shle_sw16(d1 + 0x40, shle_rw(a1 + 0xE));
    }
}

static inline void crx_bit7(uint32_t a, bool on) { shle_wb(a, on ? shle_rb(a) | 0x80 : shle_rb(a) & 0x7F); }

/* 41-46: the program's LFO words */
static void crx_cc_lfo_word(uint32_t a1, uint8_t cc, uint8_t d0) {
    uint32_t a;
    switch (cc) {
    case 0x41: case 0x43: a = a1 + (cc == 0x41 ? 0xD : 0xF); shle_wb(a, (shle_rb(a) & 0x80) | d0); break;
    case 0x42: case 0x44: a = a1 + (cc == 0x42 ? 0xC : 0xE); shle_ww(a, (shle_rw(a) & 0x7F) | d0 << 7); break;
    case 0x45: shle_wb(a1 + 0xA, (shle_rb(a1 + 0xA) & 8) | (uint8_t)(d0 << 4)); break;
    default:   shle_wb(a1 + 0xA, d0 ? shle_rb(a1 + 0xA) | 8 : shle_rb(a1 + 0xA) & ~8u); break;   /* 46 */
    }
}

/* 47-4D: the program's driver LFO bytes, not pushed to the voices */
static void crx_cc_lfo_byte(uint32_t a1, uint8_t cc, uint8_t d0) {
    static const uint8_t k_byte[] = { 0xB, 0xB, 0x10, 0x10, 0x11, 0x11 };
    if (cc == 0x4D) { if (d0 < crx_hb(0x69)) shle_wb(a1 + 5, d0); return; }
    uint32_t a = a1 + k_byte[cc - 0x47];
    if (cc & 1) crx_bit7(a, d0 != 0);
    else shle_wb(a, (shle_rb(a) & 0x80) | d0);
}

/* 41-4D: the LFO of the program the channel's first track plays (sub_601266) */
static void crx_cc_lfo(uint8_t ch, uint8_t cc, uint8_t d0) {
    uint32_t t = crx_first_track(ch);
    if (!t) return;
    uint32_t ins = shle_ml(CRX_P(0x38)), a1 = ins + shle_mw(ins + 2u + shle_rb(t + 2) * 2u);
    if (shle_mw(a1) != 0x8000) return;
    a1 += 2;
    if (cc >= 0x47) { crx_cc_lfo_byte(a1, cc, d0); return; }
    crx_cc_lfo_word(a1, cc, d0);
    crx_cc_lfo_push(shle_rb(t + 1), a1);
}

static void crx_cc_hold(uint8_t ch, uint8_t d0) {      /* 40 */
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20)
        if (crx_track_on(t, ch)) shle_wb(t, d0 == crx_hb(0x7B) ? shle_rb(t) & ~0x40u : shle_rb(t) | 0x40);
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20)
        if (shle_rb(v + 4) == ch && (shle_rb(v + 2) & 2)) crx_release_voice(v);
}

static void crx_cc_off(uint8_t ch) {                   /* 50 */
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20) {
        if (!crx_track_on(t, ch)) continue;
        shle_wb(t, shle_rb(t) & 0xBF);
        shle_ww(t + 8, shle_rw(t + 8) & 0x7F18);
    }
    shle_ww(0xF80u + ch * 2u, 0);
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20)
        if (shle_rb(v + 2) && shle_rb(v + 4) == ch) crx_free(v);
}

/* the controllers the driver knows, by kind */
enum { CRX_CC_NONE, CRX_CC_01, CRX_CC_02, CRX_CC_VOL, CRX_CC_PAN, CRX_CC_FIELD, CRX_CC_ENTRY, CRX_CC_HOLD, CRX_CC_LFO, CRX_CC_OFF,
       CRX_CC_VELC };

static uint8_t crx_cc_kind(uint8_t cc) {
    static const char k_kind[] =                        /* by controller number, 00-50 */
        "0120000300400000"
        "0555550055000000"
        "0000000000550000"
        "6666606000000000"
        "7888888888888800"
        "9";
    if (cc == 0x25 && CRX_LATE) return CRX_CC_VELC;
    return cc <= 0x50 ? (uint8_t)(k_kind[cc] - '0') : CRX_CC_NONE;
}

/* 25 (3.4.9, sub_6016B4): the tracks' velocity curve, one there is */
static void crx_cc_velc(uint8_t ch, uint8_t d0) {
    if ((int16_t)d0 > (int16_t)shle_mw(shle_ml(CRX_P(0x28)))) return;
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20)
        if (crx_track_on(t, ch)) shle_wb(t + 0xB, d0);
}

static void crx_cc(uint32_t p, uint8_t ch) {           /* sub_6011A8 */
    uint8_t cc = shle_rb(p), d0 = shle_rb(p + 1);
    switch (crx_cc_kind(cc)) {
    case CRX_CC_01:    crx_cc_01(ch, d0); break;
    case CRX_CC_02:    crx_cc_02(ch, d0); break;
    case CRX_CC_VOL:   crx_cc_volume(ch, d0); break;
    case CRX_CC_PAN:   crx_cc_pan(ch, d0); break;
    case CRX_CC_FIELD:
        for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20)
            if (crx_track_on(t, ch) && crx_cc_field(t, cc, d0)) crx_track_lfo(t);
        break;
    case CRX_CC_ENTRY: crx_cc_entry(ch, cc, d0); break;
    case CRX_CC_HOLD:  crx_cc_hold(ch, d0); break;
    case CRX_CC_LFO:   crx_cc_lfo(ch, cc, d0); break;
    case CRX_CC_OFF:   crx_cc_off(ch); break;
    case CRX_CC_VELC:  crx_cc_velc(ch, d0); break;
    default: break;
    }
}

static void crx_program(uint32_t p, uint8_t ch) {      /* sub_601952 */
    uint8_t d0 = shle_rb(p);
    uint32_t ins = shle_ml(CRX_P(0x38));
    if ((int16_t)d0 > (int16_t)shle_mw(ins)) return;
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20) {
        if (!crx_track_on(t, ch)) continue;
        shle_wb(t, shle_rb(t) & 0xC1);
        uint16_t off = shle_mw(ins + 2u + d0 * 2u);
        uint32_t a1 = ins;
        if (!(off & 0x8000)) {
            a1 += off;
            if (shle_mb(a1) == 0xFF) {                  /* a drum kit */
                shle_wb(t, shle_rb(t) | 2);
                shle_ww(t + 8, shle_rw(t + 8) & 0x7F18);
                shle_wb(t + 8, shle_rb(t + 8) | 0x80);
                shle_wb(t + 2, d0);
                continue;
            }
            if (shle_mb(a1) == 0x80) { shle_wb(t, shle_rb(t) | 0x10); a1 += 2; }
        }
        shle_ww(t + 8, shle_rw(t + 8) & 0x7F18);
        uint8_t b = shle_mb(a1 + 1);
        if (b & 8) shle_wb(t + 8, shle_rb(t + 8) | 0x80);
        shle_wb(t + 9, shle_rb(t + 9) | (uint8_t)(b << 5));
        shle_wb(t + 2, d0);
    }
}

/* ---- sound commands: volume, DSP, scenes ------------------------------------------------- */

/* loc_601FE6: the master volume to the tracks' levels and their voices */
static void crx_apply_volume(uint16_t d0) {
    shle_wb(CRX_MVOL, (uint8_t)d0);
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20) {
        if (!shle_rb(t) || (shle_rb(t) & 1)) continue;
        uint32_t x = (((uint32_t)shle_rb(t + 3) * d0) & 0xFFFFu) >> 8;
        uint8_t ta = (uint8_t)(x ? x + 1 : 0);
        shle_wb(t + 0xA, ta);
        for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20)
            if (shle_rb(v + 2) && shle_rb(v + 4) == shle_rb(t + 1))
                shle_sw8(crx_vslot(v) + 0xD, crx_tl(shle_rb(v + 8), ta));
    }
}

static uint16_t crx_scale_volume(uint8_t d0) {         /* sub_60205A */
    shle_wb(CRX_VOLRAW, d0);
    uint32_t x = ((uint32_t)d0 * (uint16_t)(shle_rb(CRX_VOLIN) << 2)) >> 8;
    uint16_t w = (uint16_t)x;
    if (w & 0xFF00) w = (uint16_t)((w & 0xFF00) | 0xFF);
    return w;
}

static void crx_cmd_volume(uint8_t ch, uint8_t d0) {   /* SoundCmd 01 */
    if (ch) { shle_wb(CRX_VOLIN, d0); d0 = shle_rb(CRX_VOLRAW); }
    else { d0 = (uint8_t)(d0 << 1); if (d0) d0++; }
    crx_apply_volume(crx_scale_volume(d0));
}

/* sub_6021B2: the DSP program cleared, the delay line filled */
static void crx_dsp_clear(void) {
    for (uint32_t k = 0; k < 18; k++) shle_sw8(k * 0x20u + 0x17, 0);
    for (uint32_t a = 0x700; a < 0xC00; a += 2) shle_sw16(a, 0);
    crx_fill(0x70000u, 0x10000u, 0x60006000u);
}

/* sub_602382: the EFSDL table of the DSP program in place; 0 when there is none */
static uint32_t crx_efsdl_table(void) {
    uint32_t a1 = shle_ml(CRX_P(0x18));
    uint8_t prog = shle_rb(CRX_DSPPROG);
    if ((a1 & 0x80000000u) || (prog & 0x80)) return 0;
    a1 += shle_mw(a1 + 2u + prog * 2u);
    return a1 + shle_mw(a1);
}

/* sub_602338: the sends of the slots the EFSDL set lists */
static void crx_efsdl_sends(void) {
    uint32_t a2 = crx_efsdl_table();
    if (!a2) return;
    uint32_t a1 = a2 + (shle_mb(a2) + 2u) * 4u + shle_rb(CRX_EFSDL) * (uint32_t)shle_mb(a2 + 5) + shle_mb(a2 + 6);
    uint32_t n = shle_mw(a1) + 1u;
    a1 += 2;
    for (uint32_t k = 0; k < n; k++, a1 += 2)
        shle_sw8((uint32_t)shle_mb(a1) * 0x20u + 0x17, shle_rb(CRX_MONO) ? 0xE0 : shle_mb(a1 + 1));
}

/* the words an EFSDL entry writes to the DSP: n at 0x700+, m at 0x780+ with an offset */
static uint32_t crx_efsdl_words(uint32_t a1, uint32_t a2, uint32_t a3, unsigned n, unsigned m) {
    uint32_t d = 0x700u + shle_mb(a2 + 0xA);
    for (unsigned k = 0; k < n; k++, a1 += 2) shle_sw16(d + 2 * k, shle_mw(a1));
    uint16_t add = (shle_mb(a2 + 8) & 0x20) ? shle_mw(a3 + 2) : 0;
    d = 0x780u + shle_mb(a2 + 0xB);
    for (unsigned k = 0; k < m; k++, a1 += 2) shle_sw16(d + 2 * k, (uint16_t)(shle_mw(a1) + add));
    return a1;
}

static void crx_efsdl(uint8_t d0) {                    /* sub_6021F2 */
    uint32_t a3 = crx_efsdl_table();
    if (!a3) return;
    uint32_t a2 = a3;
    if ((int8_t)d0 > (int8_t)shle_mb(a2 + 4)) return;
    shle_wb(CRX_EFSDL, d0);
    uint32_t a1 = a2 + (shle_mb(a2) + 2u) * 4u + (uint32_t)shle_mb(a2 + 5) * d0;
    uint32_t n = (uint16_t)(shle_mb(a2) - 1u) + 1u;
    for (uint32_t k = 0; k < n; k++, a2 += 4) {
        switch (shle_mb(a2 + 8) & 0x1F) {
        case 0x01: a1 = crx_efsdl_words(a1, a2, a3, 1, 0); break;
        case 0x02: a1 = crx_efsdl_words(a1, a2, a3, 10, 7); break;
        case 0x04: a1 = crx_efsdl_words(a1, a2, a3, 3, 2); break;
        case 0x0D: a1 = crx_efsdl_words(a1, a2, a3, 2, 0); break;
        case 0x0E: case 0x10: a1 = crx_efsdl_words(a1, a2, a3, 5, 0); break;
        case 0x16: a1 = crx_efsdl_words(a1, a2, a3, 38, 1); break;
        default:                                        /* the board restarts the driver */
            LOG_WARN("sound HLE (CRX): EFSDL entry type 0x%02X", shle_mb(a2 + 8) & 0x1F);
            return;
        }
    }
    crx_efsdl_sends();
}

/* sub_600272: a DSP program, packed, to the chip from 0x700 */
static void crx_dsp_decode(uint32_t a2) {
    uint32_t a0 = 0x700;
    a2 += 2;
    for (int guard = 0; guard < 0x2000; guard++) {
        uint8_t b = shle_mb(a2++), d0 = b & 0xF;
        if (!(b & 0x80)) { shle_sw16(a0, (uint16_t)(b << 8 | shle_mb(a2++))); a0 += 2; continue; }
        switch ((b >> 4) & 7) {
        case 0: { unsigned n = d0 == 0xF ? shle_mb(a2++) : d0;
                  for (unsigned k = 0; k <= n; k++, a0 += 2) shle_sw16(a0, 0);
                  break; }
        case 1: shle_sw16(a0, d0); a0 += 2; break;
        case 2: shle_sw16(a0, 0x1000u | d0 << 8); a0 += 2; break;
        case 3: shle_sw16(a0, d0 | 0x2000u); a0 += 2; break;
        case 4: { static const uint16_t k_w[4] = { 1, 2, 0x11, 0x12 };
                  shle_sw16(a0, 0x2000); shle_sw16(a0 + 2, k_w[b & 3]); a0 += 4;
                  if (!(b & 8)) { shle_sw16(a0, (uint16_t)(shle_mb(a2++) << 8)); a0 += 2; }
                  break; }
        case 5: shle_sw16(a0, shle_mw(CRX_T_DSPWORD + d0 * 2u)); a0 += 2; break;
        case 6: shle_sw16(a0, (uint16_t)(shle_mb(a2) << 8 | shle_mb(a2 + 1))); a2 += 2; a0 += 2; break;
        default: crx_efsdl(0); return;
        }
    }
}

/* an event's DSP clear: the main loop busy, levels 1-2 held off */
static void crx_dsp_cost(void) {
    uint32_t from = g_shle.cost;
    g_shle.cost += CRX_COST_DSP;
    g_shle.stall = true;
    crx_mask(from, g_shle.cost, false);
}

static void crx_dsp_load(uint8_t d0) {                 /* loc_602184 */
    uint32_t a2 = shle_ml(CRX_P(0x18));
    if ((a2 & 0x80000000u) || (int16_t)d0 > (int16_t)shle_mw(a2)) return;
    uint16_t off = shle_mw(a2 + 2u + d0 * 2u);
    crx_dsp_cost();
    crx_dsp_clear();
    shle_wb(CRX_DSPPROG, d0);
    crx_dsp_decode(a2 + off);
}

static void crx_dsp(uint8_t d0) {                      /* SoundCmd 08 */
    if (d0 == 0x7F) { shle_wb(CRX_DSPPROG, 0xFF); crx_dsp_cost(); crx_dsp_clear(); return; }
    if (d0 == shle_rb(CRX_DSPPROG)) return;
    crx_dsp_load(d0);
}

static void crx_coef(uint8_t ch, uint8_t d0) {         /* SoundCmd 09 */
    uint32_t a1 = crx_efsdl_table();
    if (!a1) return;
    uint32_t n = (uint16_t)(shle_mb(a1) - 3u) + 1u;
    for (uint32_t k = 0; k < n; k++, a1 += 4) {
        if (shle_mb(a1 + 0x10) == 1 && shle_mb(a1 + 0x11) == (ch & 0xF)) {
            shle_sw16(0x700u + shle_mb(a1 + 0x12), shle_mw(CRX_T_COEF + (uint8_t)(d0 << 1)));
            return;
        }
    }
}

/* SoundCmd 05: a set of samples copied into sound RAM, the last set's put back */
static void crx_swap_restore(void) {
    uint32_t n = shle_rb(CRX_SWAPN);
    if (!n) return;
    shle_wb(CRX_SWAPN, 0);
    uint32_t a2 = 0x3004;
    for (uint32_t k = 0; k < n; k++, a2 += 12) {
        while (!(shle_rb(a2) & 0x80) && a2 < 0x70000u) a2 += 12;
        uint32_t d1 = shle_rl(0x200u + 4u * k), d3 = shle_rl(a2);
        shle_wl(a2, d1);
        shle_wl(a2 + 8, shle_rl(a2 + 8) + d1 - (d3 & 0x7FFFFFFFu));
    }
}

/* sub_60209C runs at SR 0x2200: timer A waits out the copy (hotdp at 81.6 s: 165 ms) */
static void crx_swap(uint8_t d0) {
    if (d0 == shle_rb(CRX_SWAPSET)) return;
    uint32_t from = g_shle.cost;
    crx_swap_restore();
    shle_wb(CRX_SWAPSET, d0);
    uint32_t a1 = shle_ml(CRX_P(0x20));
    if (d0 == 0x7F || (int16_t)d0 > (int16_t)shle_mw(a1)) return;
    a1 += shle_mw(a1 + 2u + d0 * 2u);
    uint32_t n = (shle_mw(a1) & 0x3Fu) + 1u, dst = shle_ml(CRX_P(0x34));
    a1 += 2;
    for (uint32_t k = 0; k < n; k++, a1 += 2) {
        uint32_t e = 0x3004u + (uint32_t)shle_mw(a1) * 12u, src = shle_rl(e);
        shle_wl(0x200u + 4u * k, src);
        shle_wl(e, dst | 0x80000000u);
        crx_inc(CRX_SWAPN, 1);
        shle_wl(e + 8, shle_rl(e + 8) - src + dst);
        uint32_t len = shle_rl(e + 4) & 0x7FFFFFFFu, words = (uint16_t)(len >> 1) + 1u;
        uint32_t n = words * 2u, per = CRX_COST_SWAP;
        if (CRX_LATE) {                                 /* (len >> 8) + 1 blocks of 256, then the words */
            n = ((uint16_t)(len >> 8) + 1u) * 256u + (((len & 0xFFu) >> 1) + 1u) * 2u;
            per = CRX_COST_SWAP_L;
        }
        shle_copy(dst, src, n);
        g_shle.cost += n * per;
        g_shle.stall = true;
        dst += len + (len & 1);
    }
    crx_mask(from, g_shle.cost, false);
}

/* sub_6023A6: a scene: volume, DSP program, EFSDL, sample set and tracks */
static void crx_scene(uint8_t d0) {
    uint32_t a0 = shle_ml(CRX_P(0x14));
    if ((int16_t)d0 > (int16_t)shle_mw(a0)) return;
    a0 += shle_mw(a0 + 4u + d0 * 2u);
    uint8_t vol = shle_mb(a0++);
    if (!(vol & 0x80)) shle_wb(CRX_MVOL, (uint8_t)crx_scale_volume((uint8_t)(vol << 1) + 1u));
    uint8_t dsp = shle_mb(a0++);
    shle_wb(CRX_ENTRY, shle_mb(a0++));
    if (dsp & 0x80) crx_dsp(0x7F);
    else { crx_dsp(dsp); crx_efsdl(shle_rb(CRX_ENTRY)); }
    uint8_t sw = shle_mb(a0++);
    if (!(sw & 0x80)) crx_swap(sw);
    a0++;
    uint32_t n = shle_mb(a0++);
    if (!n) return;
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20) if (!(shle_rb(t) & 1)) shle_wb(t, 0);
    uint32_t t = CRX_TRACKS;
    for (uint32_t k = 0; k < n; k++, t += 0x20) {
        for (uint32_t i = 0; i < 16; i++) shle_wb(t + i, shle_mb(a0++));
        for (uint32_t i = 16; i < 32; i++) shle_wb(t + i, 0);
    }
    t = CRX_TRACKS;
    for (uint32_t k = 0; k < n; k++, t += 0x20)
        shle_wb(t + 0xA, crx_level(t, (shle_rb(t) & 1) ? 0xFF : shle_rb(CRX_MVOL)));
}

/* ---- sound commands: stops, pause, sequences -------------------------------------------- */

static void crx_eng_stop(void) {                       /* sub_602694 */
    if (shle_ml(CRX_P(0x3C)) & 0x80000000u) return;
    for (uint32_t r = CRX_ENGS; r < CRX_ENGS + 0x80u; r += 0x10) shle_wb(r, 0);
    shle_wb(0xEFC, 0);
}

static void crx_voice_unbend(uint32_t v) {             /* sub_60266E */
    uint8_t c = shle_rb(v + 4);
    if (c & 0x80) return;
    shle_ww(0xF80u + c * 2u, 0);
    uint32_t t = CRX_TRACKS + shle_rb(v + 3) * 0x20u;
    shle_ww(t + 8, shle_rw(t + 8) & 0x7F18);
}

static void crx_clear(uint32_t a, uint32_t n) { for (uint32_t k = 0; k < n; k++) shle_wb(a + k, 0); }

static void crx_tracks_unhold(void) {
    for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20) shle_wb(t, shle_rb(t) & 0xBF);
}

static void crx_stop_all(bool from_seq) {              /* A0 01 */
    if (!from_seq) { crx_eng_stop(); crx_clear(CRX_SEQS, 0x80); }
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20) if (shle_rb(v + 2)) crx_free(v);
    shle_wl(CRX_NMUS, 0);
    crx_clear(0xF80, 0x20);
    crx_tracks_unhold();
    crx_dsp_load(shle_rb(CRX_DSPPROG));
}

static void crx_stop_music(bool from_seq) {            /* A0 02 */
    if (!from_seq) crx_clear(CRX_SEQS, 0x10);
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20)
        if (shle_rb(v + 2) && !(shle_rb(v + 9) & 0x80)) { crx_voice_unbend(v); crx_free(v); }
    if (CRX_LATE) crx_clear(0xF80, 0x20);
    crx_tracks_unhold();
    crx_apply_volume(shle_rb(CRX_MVOL));
}

static void crx_stop_effects(bool from_seq) {          /* A0 03 */
    if (!from_seq) crx_eng_stop();
    crx_clear(CRX_SEQS + 0x10, 0x70);
    for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20)
        if (shle_rb(v + 2) & 8) { crx_voice_unbend(v); crx_free(v); }
}

static void crx_pause(void) {                          /* A0 04 */
    uint16_t word;
    if (!shle_rb(CRX_PAUSE)) {
        shle_wb(CRX_FADEKEEP, shle_rb(CRX_FADEOUT));
        shle_wb(CRX_FADEOUT, 0);
        for (uint32_t s = 0; s < 0x400; s += 0x20) shle_sw8(s, shle_sr8(s) | 1);
        uint8_t s0 = shle_rb(CRX_SEQS);
        shle_wb(CRX_PAUSE, (s0 & 0x80) ? 0x80 : 0x10);
        shle_wb(CRX_SEQS, s0 & 0x7F);
        word = shle_mw(CRX_P(0x74));
    } else {
        if (shle_rb(CRX_PAUSE) & 0x80) shle_wb(CRX_SEQS, shle_rb(CRX_SEQS) | 0x80);
        shle_wb(CRX_PAUSE, 0);
        shle_wb(CRX_FADEOUT, shle_rb(CRX_FADEKEEP));
        for (uint32_t s = 0; s < 0x400; s += 0x20) shle_sw8(s, shle_sr8(s) & 0x1E);
        word = shle_mw(CRX_P(0x76));
    }
    crx_enqueue((uint32_t)word << 8 | 0xA0000000u);
}

static void crx_cmd00_misc(uint8_t d0) {
    if ((d0 | 1) == 0x07) { shle_wb(CRX_MONO, d0 == 7); crx_efsdl_sends(); }
    else if ((d0 | 1) == 0x09) {                        /* every music track a semitone up or down */
        for (uint32_t t = CRX_TRACKS; t < CRX_TEND; t += 0x20)
            if (!(shle_rb(t) & 1)) crx_inc(t + 4, d0 == 8 ? 1 : -1);
    }
    else if ((d0 | 1) == 0x0D) shle_wb(CRX_TEMPO, d0 == 0xC ? 0xFF : 0);
    else if (d0 == 0x10) {
        for (uint32_t v = CRX_VOICES; v < CRX_VEND; v += 0x20) if (shle_rb(v + 2) & 4) crx_free(v);
    }
    else if (d0 == 0x13 || d0 == 0x14) shle_wb(CRX_ECHO, d0 == 0x13);
}

static void crx_cmd00(uint8_t d0, bool from_seq) {     /* sub_6024D8 */
    switch (d0) {
    case 0x01: crx_stop_all(from_seq); break;
    case 0x05: shle_ww(0xEFE, 0x8000); crx_stop_all(from_seq); break;
    case 0x02: crx_stop_music(from_seq); break;
    case 0x03: crx_stop_effects(from_seq); break;
    case 0x04: crx_pause(); break;
    default:   crx_cmd00_misc(d0); break;
    }
}

static void crx_fade_start(uint8_t d0, bool in) {      /* SoundCmd 03 / 04 */
    if (shle_rb(CRX_FADEOUT) || shle_rb(CRX_FADEIN)) return;
    uint8_t mv = shle_rb(CRX_MVOL);
    shle_wb(CRX_FADECNT, d0);
    if (in) {
        shle_wb(CRX_FADEIN, d0);
        shle_wb(CRX_FADEEND, mv >> 1);
        shle_wb(CRX_FADELVL, 0);
        shle_wb(0xEF3, 0);
    } else {
        shle_wb(CRX_FADEOUT, d0);
        shle_wb(CRX_FADEEND, mv);
        shle_wb(CRX_FADELVL, mv >> 1);
    }
}

static void crx_seq_music(uint32_t a2) {
    uint32_t s = CRX_SEQS;
    crx_clear(s, 16);
    a2 += 2;
    shle_wb(s, shle_rb(s) | (shle_mb(a2++) & 0x60));
    shle_wb(CRX_FADEOUT, 0);
    shle_wl(s + 4, shle_ml(a2));
    a2 += 4;
    shle_wl(s + 8, a2);
    shle_ww(s + 2, 1);
    shle_wb(s, shle_rb(s) | 0x88);
}

static void crx_seq_effect(uint8_t d3, uint32_t a2) {
    uint8_t d0 = d3;
    uint32_t s = 0;
    d3 &= 0xF;
    if (d0 & 0x10)
        for (uint32_t x = CRX_SEQS + 0x10; x < CRX_SEQS + 0x80; x += 0x10) if (shle_rb(x + 0xF) == d3) { s = x; break; }
    if (!s)
        for (uint32_t x = CRX_SEQS + 0x10; x < CRX_SEQS + 0x80; x += 0x10) if (!(shle_rb(x) & 0x80)) { s = x; break; }
    if (!s) return;
    crx_clear(s, 16);
    shle_wb(s + 0xF, d3);
    shle_wl(s + 4, a2);
    shle_ww(s + 2, 1);
    shle_wb(s, shle_rb(s) | (d0 & 0x60) | 0x80);
}

static void crx_seq_start(uint8_t cmd, uint8_t b) {
    uint32_t base = shle_ml(CRX_P(0x1C)), a4 = base;
    uint16_t i = (uint16_t)(cmd - 0x10);
    if ((int16_t)i > (int16_t)shle_mw(a4)) return;
    a4 += shle_mw(a4 + 2u + i * 2u);
    if ((int16_t)b > (int16_t)shle_mw(a4)) return;
    uint32_t a2 = base + shle_mw(a4 + 2u + b * 2u);
    uint8_t d3 = shle_mb(a2++);
    if (d3 & 0x80) crx_seq_effect(d3, a2);
    else crx_seq_music(a2);
}

/* ---- the engine (A?6x-A?7x) ------------------------------------------------------------ */

static inline uint32_t crx_eng_vel_table(void) {
    uint32_t vt = shle_ml(CRX_P(0x28));
    return vt + (uint32_t)(int32_t)(int16_t)shle_mw(vt + 2);
}

static void crx_eng_release(uint32_t r, unsigned d6) {  /* sub_602D22 */
    uint32_t s = (shle_rb(r + 8 + d6) & 0x1Fu) << 5, v = CRX_VOICES + s;
    shle_sw8(s, 0x10);
    shle_wb(v + 2, shle_rb(v + 2) | 1);
    shle_wb(r, shle_rb(r) & ~(1u << d6));
    shle_ww(v + 0xE, shle_rw(v + 0xC));
}

/* sub_602CA4: a layer's level and pitch at position pos */
static void crx_eng_update(uint32_t r, uint32_t L, uint32_t v, uint8_t pos) {
    uint8_t d0 = shle_mb(L + 0x13);
    for (uint32_t a3 = L + 6; a3 < L + 6 + 16; a3 += 4) {
        if ((int8_t)pos < (int8_t)shle_mb(a3)) {
            uint32_t x = (((uint32_t)(uint8_t)(pos - shle_mb(a3 - 4)) * shle_mw(a3 + 2)) & 0xFFFFu) >> 8;
            d0 = (uint8_t)(x + shle_mb(a3 - 3));
            break;
        }
    }
    uint32_t slot = crx_vslot(v);
    shle_wb(v + 8, d0);
    uint8_t x = shle_mb(crx_eng_vel_table() + (d0 >> 1));
    uint8_t d1 = (uint8_t)((((uint32_t)shle_rb(r + 5) * x) >> 8) + 1);
    shle_sw8(slot + 0xD, (uint8_t)((uint8_t)~d1 >> 1));
    uint16_t w = (uint16_t)(((uint32_t)((int32_t)pos * (int16_t)shle_mw(L + 0x1C)) >> 8) + shle_mw(L + 0x16));
    shle_ww(CRX_NOTE, w);
    shle_sw16(slot + 0x10, (w & 0x3FFu) | (((uint32_t)(w << 1) + 0x4000u) & 0x7800u));
}

/* a layer's new voice; false when the command ends here */
static bool crx_eng_voice(uint32_t r, uint32_t L, unsigned d6, uint8_t pos) {
    uint16_t d3 = shle_mw(L) & 0xFFF;
    if ((int16_t)d3 > (int16_t)shle_rw(0x3002)) return false;
    uint32_t a3 = 0x3004u + (uint32_t)(uint16_t)(d3 - shle_rw(0x3000)) * 12u;
    shle_wl(CRX_SMP, a3);
    shle_wb(CRX_SMPRAM, shle_rb(a3));
    if ((int8_t)crx_hb(0x7C) <= (int8_t)shle_rb(CRX_NEFF)) return true;
    uint32_t v = crx_alloc();
    if (!v) return false;
    uint32_t slot = crx_vslot(v);
    shle_wb(v + 2, 8);
    shle_sw8(slot, 0x10);
    shle_wb(r + 8 + d6, (uint8_t)(slot >> 5));
    shle_wb(r, shle_rb(r) | 1u << d6);
    shle_wb(v + 5, (uint8_t)d6);
    shle_wb(v + 6, shle_rb(CRX_ECF));
    shle_wb(v + 4, 0xFF);
    shle_wb(v + 9, shle_rb(CRX_NEFF) | 0x80);
    crx_inc(CRX_NEFF, 1);
    if (!shle_rb(CRX_SMPRAM)) crx_inc(CRX_SEFF, 1);
    crx_eng_update(r, L, v, pos);
    uint16_t rate = shle_rw(CRX_NOTE);
    shle_sw8(slot + 0xC, 0);
    shle_sw16(slot + 0xE, 0);
    shle_sw16(slot + 0x12, 0);
    shle_sw8(slot + 0x15, shle_rb(r + 3));
    shle_sw8(slot + 0x16, shle_mb(L + 4));
    shle_wb(r + 2, shle_mb(L + 4));
    uint8_t e = shle_mb(L + 5);
    shle_sw16(slot + 8, ((e & 0xF0u) >> 3) + 1u);
    shle_sw16(slot + 0xA, (e & 0xFu) * 2u + 1u);
    shle_ww(v + 0xC, shle_mw(CRX_T_RELEASE + ((e & 0xFu) * 2u + 1u) * 2u));
    if (!(shle_mb(shle_rl(CRX_SMP) + 4) & 0x80)) shle_wb(r, shle_rb(r) & ~(1u << d6));
    crx_keyon(r, v, rate);
    return true;
}

static void crx_eng_position(uint32_t r, uint8_t pos) {   /* sub 0 */
    uint32_t tbl = shle_ml(CRX_P(0x3C));
    uint8_t prog = shle_rb(r + 1);
    if ((int16_t)prog > (int16_t)shle_mw(tbl)) return;
    uint32_t a2 = shle_ml(tbl + 2u + prog * 4u);
    unsigned d7 = shle_mw(a2) & 0xFFFF;
    if (!pos) {
        for (unsigned d6 = 0; d6 <= d7 && d6 < 8; d6++) if (shle_rb(r) & 1u << d6) crx_eng_release(r, d6);
        return;
    }
    uint32_t L = a2 + 4;
    for (unsigned d6 = 0; d6 <= d7 && d6 < 8; d6++, L += 0x1E) {
        uint8_t lo = shle_mb(L + 2);
        if (lo & 0x80) continue;
        bool active = (shle_rb(r) & 1u << d6) != 0;
        if (pos < lo || (int8_t)pos > (int8_t)shle_mb(L + 3)) { if (active) crx_eng_release(r, d6); continue; }
        if (active) { crx_eng_update(r, L, CRX_VOICES + (shle_rb(r + 8 + d6) & 0x1Fu) * 0x20u, pos); continue; }
        if (!crx_eng_voice(r, L, d6, pos)) return;
    }
}

/* the active layers' slots */
static void crx_eng_slots(uint32_t r, uint32_t reg, uint8_t val) {
    for (unsigned d6 = 0; d6 < 8; d6++)
        if (shle_rb(r) & 1u << d6) shle_sw8(((shle_rb(r + 8 + d6) & 0x1Fu) << 5) + reg, val);
}

static void crx_eng_volume(uint32_t r, uint8_t b) {   /* sub 5 */
    uint8_t r5 = (uint8_t)((b << 1) + 1);
    shle_wb(r + 5, r5);
    uint32_t tbl = crx_eng_vel_table();
    for (unsigned d6 = 0; d6 < 8; d6++) {
        if (!(shle_rb(r) & 1u << d6)) continue;
        uint32_t s = (shle_rb(r + 8 + d6) & 0x1Fu) << 5;
        uint8_t x = shle_mb(tbl + (shle_rb(CRX_VOICES + s + 8) >> 1));
        uint8_t d1 = (uint8_t)((((uint32_t)x * r5) >> 8) + 1);
        shle_sw8(s + 0xD, (uint8_t)((uint8_t)~d1 >> 1));
    }
}

static void crx_eng_select(uint32_t r, uint8_t d0) {  /* sub 4 */
    uint32_t tbl = shle_ml(CRX_P(0x3C));
    if (shle_rb(r) || (int16_t)d0 > (int16_t)shle_mw(tbl)) return;
    shle_wb(r + 1, d0);
    shle_wb(r + 2, 0);
    uint32_t a2 = shle_ml(tbl + 2u + d0 * 4u) + 2u;
    shle_wb(r + 3, shle_mb(a2));
    shle_wb(r + 4, shle_mb(a2 + 1));
    shle_wb(r + 5, 0xFF);
}

static void crx_engine(uint8_t cmd, uint8_t ch, uint8_t b) {   /* sub_602AD8 */
    if (shle_ml(CRX_P(0x3C)) & 0x80000000u) return;
    uint32_t r = CRX_ENGS + (ch & 0xFu) * 16u;
    shle_wb(CRX_ECF, (uint8_t)((ch & 0xF) << 4));
    switch (cmd & 7) {
    case 0: crx_eng_position(r, b); break;
    case 1: shle_wb(r + 2, (shle_rb(r + 2) & 0xE0) | crx_map(b)); crx_eng_slots(r, 0x16, shle_rb(r + 2)); break;
    case 2: shle_wb(r + 3, (shle_rb(r + 3) & 0x78) | crx_min(b, 7)); crx_eng_slots(r, 0x15, shle_rb(r + 3)); break;
    case 3: shle_wb(r + 3, (shle_rb(r + 3) & 7) | ((b & 0xF) << 3)); crx_eng_slots(r, 0x15, shle_rb(r + 3)); break;
    case 4: crx_eng_select(r, b); break;
    case 5: crx_eng_volume(r, b); break;
    default: break;
    }
}

static void crx_cmd_send_pan(uint8_t ch, uint8_t b) {   /* SoundCmd 0A */
    if (shle_rb(CRX_MONO)) return;
    uint32_t s = (ch & 0xFu) << 5;
    shle_sw8(s + 0x17, shle_sr8(s + 0x17) & 0xE0);
    shle_sw8(s + 0x17, shle_sr8(s + 0x17) | crx_map(b));
}

static void crx_cmd_panofs(uint8_t ch, uint8_t b) {   /* SoundCmd 0D */
    (void)ch;
    int8_t x = (int8_t)b;
    x = x < 0x39 ? 0x39 : x > 0x47 ? 0x47 : x;
    shle_wb(CRX_PANOFS, (uint8_t)(x - 0x40));
}

static void crx_cmd_fade_out(uint8_t ch, uint8_t b) { (void)ch; crx_fade_start(b, false); }
static void crx_cmd_fade_in(uint8_t ch, uint8_t b)  { (void)ch; crx_fade_start(b, true); }
static void crx_cmd_swap(uint8_t ch, uint8_t b)     { (void)ch; crx_swap(b); }
static void crx_cmd_scene(uint8_t ch, uint8_t b)    { (void)ch; crx_scene(b); }
static void crx_cmd_dsp(uint8_t ch, uint8_t b)      { (void)ch; crx_dsp(b); }
static void crx_cmd_efsdl(uint8_t ch, uint8_t b)    { (void)ch; crx_efsdl(b); }

/* sub_601E60: an A? event: p at its first data byte */
static void crx_sound_cmd(uint32_t p, uint8_t ch) {
    static void (*const k_cmd[16])(uint8_t, uint8_t) = {
        NULL, crx_cmd_volume, NULL, crx_cmd_fade_out, crx_cmd_fade_in, crx_cmd_swap, NULL, crx_cmd_scene,
        crx_cmd_dsp, crx_coef, crx_cmd_send_pan, crx_cmd_efsdl, NULL, crx_cmd_panofs, NULL, NULL,
    };
    uint8_t cmd = shle_rb(p), b = shle_rb(p + 1);
    if (cmd >= 0x60) crx_engine(cmd, ch, b);
    else if (cmd >= 0x10) crx_seq_start(cmd, b);
    else if (cmd == 0) crx_cmd00(b, shle_rb(p + 2) == 0x80);
    else if (k_cmd[cmd]) k_cmd[cmd](ch, b);
}

/* ---- the sequencers (timer A) -------------------------------------------------------------- */

/* the next data byte of an event, bit 7 to the sequence's "no wait" bit */
static uint8_t crx_seq_data(uint32_t s, uint32_t *a3) {
    uint8_t b = shle_mb((*a3)++);
    if (b & 0x80) shle_wb(s, shle_rb(s) | 1);
    return b & 0x7F;
}

static void crx_seq_send(uint32_t s, uint8_t b) { if (shle_rb(s) & 0x20) shle_sw8(0x407, b); }

/* F0/F7 and FF: false at the sequence's end */
static bool crx_seq_system(uint32_t s, uint8_t st, uint32_t *a3) {
    if (st == 0xF0 || st == 0xF7) {
        crx_seq_send(s, st);
        uint32_t n = 0;
        for (int k = 0; k < 5; k++) {
            uint8_t b = shle_mb((*a3)++);
            if (!(b & 0x80)) { n |= b; break; }
            n = (n | (b & 0x7Fu)) << 7;
        }
        n = (uint16_t)(n - 1u) + 1u;
        for (uint32_t k = 0; k < n; k++) crx_seq_send(s, shle_mb((*a3)++));
        return true;
    }
    if (st != 0xFF) return false;
    if (shle_mb((*a3)++) == 0x2F) return false;
    uint8_t len = shle_mb((*a3)++);
    *a3 += len;
    return true;
}

/* one event to the queue (loc_60289E): false at the sequence's end */
static bool crx_seq_event(uint32_t s, uint32_t *a3) {
    shle_wb(s, shle_rb(s) & 0xFE);
    uint8_t st = shle_mb((*a3)++);
    if (!(st & 0x80)) { st = shle_rb(s + 1); (*a3)--; }
    shle_wb(s + 1, st);
    uint32_t ev = (uint32_t)st << 24, a, b;
    switch ((st >> 4) & 7) {
    case 0: ev |= (uint32_t)crx_seq_data(s, a3) << 16 | 0x7F00u; break;
    case 1: case 3: a = shle_mb((*a3)++); ev |= a << 16 | (uint32_t)crx_seq_data(s, a3) << 8; break;
    case 2:
        a = shle_mb((*a3)++);
        if (a == 1 && (shle_rb(CRX_FADEOUT) || shle_rb(CRX_FADEIN))) ev |= 0x80;
        else ev |= a << 16 | (uint32_t)crx_seq_data(s, a3) << 8 | 0x80;
        break;
    case 4: ev |= (uint32_t)crx_seq_data(s, a3) << 16; break;
    case 5:
        ev = (uint32_t)(st & 0x9F) << 24;
        a = shle_mb((*a3)++); b = shle_mb((*a3)++);
        ev |= a << 16 | b << 8 | crx_seq_data(s, a3);
        break;
    case 6: ev |= (uint32_t)crx_seq_data(s, a3) << 8; break;
    default: return crx_seq_system(s, st, a3);
    }
    crx_enqueue(ev);
    return true;
}

/* loc_602A1C: the wait to the next event; false when there is none */
static bool crx_seq_wait(uint32_t s, uint32_t *a3) {
    if (shle_rb(s) & 1) return false;
    uint16_t d = shle_mb((*a3)++);
    if (!d) return false;
    if (d & 0x80) d = (uint16_t)((d & 0x7F) << 7 | shle_mb((*a3)++));
    shle_ww(s + 2, d);
    shle_wl(s + 4, *a3);
    return true;
}

/* loc_602A52: a list entry a3 (a0 past it): true to play on from a3, false when the tick ends */
static bool crx_seq_list(uint32_t s, uint32_t *a3, uint32_t *a0) {
    for (int guard = 0; guard < 64; guard++) {
        uint32_t x = *a3;
        if ((x & 0xFF000000u) == 0x80000000u) {
            crx_enqueue(x << 8 | 0x80);
            *a0 = shle_rl(s + 8);
        } else if (x == 0xFFFFFFFFu) { shle_wb(s, 0); return false; }
        else if (x == 0xFFFFFFF1u) *a0 = shle_ml(*a0);
        else if (x == 0xFFFFFFF2u) { crx_clear(CRX_SEQS, 0x80); return false; }
        else { shle_wl(s + 8, *a0); return true; }
        *a3 = shle_ml(*a0);                             /* loc_602A9C */
        *a0 += 4;
        shle_wl(s + 8, *a0);
        if (!(*a3 & 0x80000000u)) return true;
    }
    return false;
}

static void crx_tick(uint32_t s) {                      /* sub_60289A */
    uint32_t a3 = shle_rl(s + 4), a0 = shle_rl(s + 8);
    bool list = (a3 & 0x80000000u) != 0;
    for (int guard = 0; guard < 4096; guard++) {
        if (list && !crx_seq_list(s, &a3, &a0)) return;
        list = false;
        if (crx_seq_event(s, &a3)) {
            if (crx_seq_wait(s, &a3)) return;
            continue;
        }
        if (!(shle_rb(s) & 8)) { shle_wb(s, 0); return; }   /* the end, no list to go on with */
        a0 = shle_rl(s + 8);
        a3 = shle_ml(a0);
        a0 += 4;
        list = true;
    }
}

static void crx_seq_due(uint32_t s) {
    if (shle_rb(CRX_TEMPO)) { crx_tick(s); return; }
    uint16_t w = (uint16_t)(shle_rw(s + 2) - 1);
    shle_ww(s + 2, w);
    if ((int16_t)w <= 0) crx_tick(s);
}

static void crx_fade_out(void) {                        /* sub_602802 */
    crx_inc(CRX_FADECNT, -1);
    if (shle_rb(CRX_FADECNT)) return;
    shle_wb(CRX_FADECNT, shle_rb(CRX_FADEOUT));
    uint8_t d0 = (uint8_t)(shle_rb(CRX_FADELVL) - 4);
    if (!(d0 & 0x80)) { shle_wb(CRX_FADELVL, d0); crx_enqueue(0xA0010000u | (uint32_t)d0 << 8); return; }
    crx_enqueue(0xA0000200u);
    shle_wb(CRX_MVOL, shle_rb(CRX_FADEEND));
    shle_wb(CRX_SEQS, 0);
    shle_wb(CRX_FADEOUT, 0);
}

static void crx_fade_in(void) {                         /* sub_602842 */
    crx_inc(CRX_FADECNT, -1);
    if (shle_rb(CRX_FADECNT)) return;
    shle_wb(CRX_FADECNT, shle_rb(CRX_FADEIN));
    uint8_t d0 = (uint8_t)(shle_rb(CRX_FADELVL) + 4);
    if (d0 >= shle_rb(CRX_FADEEND)) {
        shle_wb(CRX_FADEIN, 0);
        crx_enqueue(0xA0010000u | (uint32_t)shle_rb(CRX_FADEEND) << 8);
        return;
    }
    shle_wb(CRX_FADELVL, d0);
    crx_enqueue(0xA0010000u | (uint32_t)d0 << 8);
}

/* Int2, sub_602766 (3.4.9: sub_6028FC). Returns the music's passes, 0 for a queue too full;
 * *music the events those queued. */
static unsigned crx_timer_a_body(uint32_t *music) {
    uint8_t n = shle_rb(CRX_QN);
    *music = 0;
    if (CRX_LATE ? n >= 0xFC : (int8_t)n <= -4) return 0;
    unsigned passes = 0;
    for (;;) {
        passes++;
        if (shle_rb(CRX_SEQS) & 0x80) {
            if (shle_rb(CRX_FADEOUT)) crx_fade_out();
            if (shle_rb(CRX_FADEIN)) crx_fade_in();
            crx_seq_due(CRX_SEQS);
        }
        if (!CRX_LATE || !shle_rb(CRX_LATE_A) || passes >= 256) break;
        crx_inc(CRX_LATE_A, -1);
    }
    *music = g_shle.enq;
    for (uint32_t s = CRX_SEQS + 0x10; s < CRX_SEQS + 0x80; s += 0x10)
        if (shle_rb(s) & 0x80) crx_seq_due(s);
    return passes;
}

/* ---- the main loop: dispatch, streaming ------------------------------------------------------ */

static void crx_event(uint8_t st, uint32_t p) {        /* sub_6006D4's jump */
    uint8_t ch = st & 0xF;
    switch ((st >> 4) & 7) {
    case 0: crx_note_off(p, ch); break;
    case 1: crx_note_on(p, ch); break;
    case 2: crx_sound_cmd(p, ch); break;
    case 3: crx_cc(p, ch); break;
    case 4: crx_program(p, ch); break;
    case 6: crx_bend(p, ch); break;
    default: break;
    }
}

/* A refill goes on to the next record, not the queue: an event waits for the chunk under way,
 * and the chunks left wait for the event. A record copies one chunk a trip round the loop. */
#define CRX_CHUNK_EVERY  (SHLE_CHUNK_CYCLES + CRX_LOOP_TRIP)
static bool crx_refilling(uint64_t t) {
    return t >= g_shle.refill_at && t < g_shle.refill_at + (uint64_t)g_shle.refill_n * CRX_CHUNK_EVERY;
}
static uint64_t crx_refill_wait(uint64_t now) {
    uint64_t r = (now - g_shle.refill_at) % CRX_CHUNK_EVERY;
    return crx_refilling(now) && r && r < SHLE_CHUNK_CYCLES ? now + SHLE_CHUNK_CYCLES - r : now;
}

static void crx_dispatch(uint64_t now) {
    int guard = 0;
    if (shle_rb(CRX_QN) && now >= g_shle.busy_until && crx_refill_wait(now) > now) {
        g_shle.busy_until = crx_refill_wait(now);
        return;
    }
    while (shle_rb(CRX_QN) && now >= g_shle.busy_until && guard++ < 512) {
        uint8_t n = shle_rb(CRX_QN);
        if (n >= shle_rb(CRX_QMAX)) shle_wb(CRX_QMAX, n);
        shle_wb(CRX_QN, n - 1u);
        uint16_t r = shle_rw(CRX_QR);
        uint32_t p = CRX_QUEUE + (r & 0x3FFu);
        shle_ww(CRX_QR, r + 4u);
        g_shle.events++;
        g_shle.cost = CRX_COST_EVENT;
        g_shle.stall = false;
        g_shle.amask_n = 0;
        crx_event(shle_rb(p), p + 1);
        uint64_t from = g_shle.busy_until > now ? g_shle.busy_until : now;
        g_shle.busy_until = from + g_shle.cost;
        if (crx_refilling(from)) g_shle.refill_at += g_shle.cost;
        for (uint32_t i = 0; i < g_shle.amask_n; i++) {
            g_shle.amask_win[i][0] += from;
            g_shle.amask_win[i][1] += from;
        }
        if (g_shle.stall) g_shle.stall_until = g_shle.busy_until;
    }
}

/* loc_601B5E: the chip loops what is in the window, the record stops */
static void crx_rec_hw_loop(uint32_t r, unsigned k) {
    shle_sw32(k * 0x20u + 4, (uint32_t)shle_rw(r + 0x16) << 16 | shle_rw(r + 0x1A));
    shle_ww(r, 0);
}

static void crx_rec_copy(uint32_t r, uint32_t a2, uint32_t a1, uint8_t d2) {
    shle_wb(r + 1, d2 + 0x10u);
    shle_copy(a2, a1, 0x200);
    shle_wl(r + 4, a1 + 0x200u);
    shle_wl(r + 8, a2 + 0x200u);
    g_shle.chunks++;
}

/* the sample past its end, no loop: the rest of the chunk silence */
static void crx_rec_tail(uint32_t r, uint32_t a2, uint32_t d5, uint8_t d2) {
    shle_wb(r + 1, d2 + 0x10u);
    uint32_t n = d5 & 0x1FCu;
    if (n) {
        shle_wl(r + 0xC, 0x80000001u);
        shle_copy(a2, shle_rl(r + 4), n);
    }
    crx_fill(a2 + n, 0x200u - n, 0);
    shle_wl(r + 8, a2 + 0x200u);
    g_shle.chunks++;
}

/* one 0x200-byte step of stream record k (loc_601A02): false when it waits */
static bool crx_rec_step(uint32_t r, unsigned k) {
    uint8_t r0 = shle_rb(r), d2 = shle_rb(r + 1);
    if (!(r0 & 0x80)) return false;
    uint32_t a2;
    if (d2 == 0) { if (!shle_ca_upper(k)) return false; a2 = shle_rl(r + 0x10); }
    else if (d2 == 0x80) {
        if (shle_ca_upper(k)) return false;
        if (r0 & 4) { crx_rec_hw_loop(r, k); return false; }
        a2 = shle_rl(r + 8);
    } else a2 = shle_rl(r + 8);
    uint32_t d5 = shle_rl(r + 0xC);
    if (!(d5 & 0x80000000u)) {
        shle_wl(r + 0xC, d5 - 0x200u);
        crx_rec_copy(r, a2, shle_rl(r + 4), d2);
    } else if (r0 & 1) {
        uint32_t r18 = shle_rl(r + 0x18);
        if (!(r18 & 0x80000000u)) {
            shle_wl(r + 0xC, r18 + d5);
            crx_rec_copy(r, a2, shle_rl(r + 0x14) - d5, d2);
        } else if (shle_rb(r + 0x14) & 0x80) {
            shle_wb(r, r0 | 4);
            shle_wb(r + 1, 0x80);
        } else { crx_rec_hw_loop(r, k); return false; }
    } else crx_rec_tail(r, a2, d5, d2);
    return true;
}

static void crx_stream_step(unsigned k) {
    uint32_t r = CRX_RECS + k * 0x20u;
    for (int guard = 0; guard < 32 && crx_rec_step(r, k); guard++) {}
}

/* ---- boot (EntryPoint, 0x600120) -------------------------------------------------------------- */

/* sub_600366: the engine records and the default tracks, then scene 0 */
static void crx_boot_tracks(void) {
    crx_clear(CRX_ENGS, 0x80);
    uint32_t prog = shle_ml(CRX_P(0x3C));
    if (!(prog & 0x80000000u)) {
        uint32_t a1 = shle_ml(CRX_P(0x24)), r = CRX_ENGS;
        for (int k = 0; k < 8; k++, r += 0x10) {
            uint8_t id = shle_mb(a1++);
            if (id & 0x80) continue;
            uint32_t a4 = shle_ml(prog + 2u + id * 4u);
            shle_wb(r + 1, id);
            shle_wb(r + 3, shle_mb(a4 + 2));
            shle_wb(r + 4, shle_mb(a4 + 3));
            shle_wb(r + 5, 0xFF);
        }
    }
    uint32_t a1 = shle_ml(CRX_P(0x14));
    a1 += shle_mw(a1 + 2);
    uint32_t n = shle_mw(a1);
    a1 += 2;
    if (n) {
        uint32_t t = CRX_TRACKS + ((0x280u - n * 0x20u) & 0xFFFFu);
        for (uint32_t k = 0; k < n; k++, t += 0x20) {
            for (uint32_t i = 0; i < 16; i++) shle_wb(t + i, shle_mb(a1++));
            for (uint32_t i = 16; i < 32; i++) shle_wb(t + i, 0);
        }
    }
    shle_wl(CRX_NEXTV, CRX_VOICES);
    shle_wb(CRX_MVOL, 0xFF);
    crx_scene(0);
}

static void crx_boot_voices(void) {                    /* sub_600218 (the timers' registers are the HLE's), 600404, 60043C */
    for (uint32_t k = 0; k < 32; k++) {
        uint32_t s = k * 0x20u;
        shle_sw8(s + 0x15, 0x78);
        shle_sw32(s + 0xC, 0);
        shle_sw32(s + 0x12, 0);
        shle_sw8(s + 0xB, 0xFF);
        shle_sw8(s + 0xD, 0xFF);
        shle_sw8(s, 0x10);
    }
    for (uint32_t k = 0; k < 32; k++) shle_ww(CRX_VOICES + k * 0x20u, k * 0x20u);
    uint8_t n = crx_hb(0x7D);
    shle_wl(CRX_WINDOWS, n >= 32 ? 0 : 0xFFFFFFFFu >> n);
    shle_wb(CRX_LED, 0x77);
    shle_wb(CRX_SWAPSET, 0x7F);
    shle_wb(CRX_DSPPROG, 0xFF);
    for (uint32_t k = 0; k < 32; k++) shle_wb(CRX_RECS + k * 0x20u + 3, (uint8_t)(8 + 8 * k));
}

/* the init routines, at clock period `at` after the RAM clear */
static void crx_boot(uint64_t at) {
    g_shle.booted = false;
    g_shle.restart_at = 0;
    g_shle.busy_until = g_shle.stall_until = 0;
    g_shle.l2_free = 0;
    g_shle.a_ack = g_shle.a_busy_until = 0;
    g_shle.amask_n = 0;
    g_shle.amask_soft = 0;
    g_shle.b_busy_until = 0;
    g_shle.refill_at = 0;
    g_shle.refill_n = 0;
    g_shle.pass_left = CRX_PASS_SAMPLES;
    g_shle.pass_rem = 0;
    g_shle.cost = 0;
    shle_wb(CRX_PANOFS, g_crx_panofs);
    shle_wb(CRX_VOLIN, 0x40);
    shle_copy(0, 0x600000u, 0x100);                     /* sub_600352: the vectors */
    crx_dsp_clear();                                    /* sub_6021B2 */
    crx_boot_voices();
    uint32_t smp = shle_ml(CRX_P(0));                   /* sub_600470: the sample table */
    if (!(smp & 0x80000000u)) {
        uint32_t n = ((uint16_t)shle_ml(smp) + 1u) * 16u;
        shle_copy(0x3000, smp, n);
        g_shle.cost += n * CRX_COST_BYTE / 4u;
    }
    crx_boot_tracks();
    shle_copy(0x2000, CRX_B->init, 0x1000);             /* sub_600456 */
    uint32_t nins = (uint16_t)shle_ml(CRX_P(0x30)) + 1u;   /* sub_600492: the instruments */
    shle_copy(shle_ml(CRX_P(0x38)), shle_ml(CRX_P(0x2C)), nins);
    g_shle.cost += nins * CRX_COST_BYTE;
    shle_ww(0xC2A, 0x4F4B);                             /* 'OK' */
    g_shle.boot_at = at + CRX_BOOT_FIXED + g_shle.cost;
}

/* the driver from its entry point at clock period `t0` (reset, the MIDI byte
 * 0xFF): master volume 0 now, the RAM clear after the delay, the init after it */
static void crx_restart(uint64_t t0) {
    g_shle.booted = false;
    g_crx_panofs = (shle_rl(0xC08) == 0x4572726Fu && shle_rl(0xC0C) == 0x72536574u) ? shle_rb(CRX_PANOFS) : 0;
    shle_sw8(0x401, 0);
    crx_ctrl(0);
    g_shle.clear_at = t0 + CRX_DELAY;
    g_shle.cleared = 0;
    g_shle.restart_at = g_shle.clear_at + (uint64_t)CRX_CLEAR_END * 15u / 2u;
    g_shle.boot_at = g_shle.restart_at + CRX_BOOT_FIXED;   /* until crx_boot knows the copies */
}

/* EntryPoint's clear of sound RAM to 0x70000, as far as it has got by `now`
 * (the DAC and the timers' prescale written just before it) */
static void crx_clear_step(scsp_t *s, uint64_t now) {
    if (!g_shle.restart_at || now <= g_shle.clear_at || g_shle.cleared >= CRX_CLEAR_END) return;
    uint64_t to = (now - g_shle.clear_at) * 2u / 15u;
    if (to > CRX_CLEAR_END) to = CRX_CLEAR_END;
    if (to <= g_shle.cleared) return;
    if (!g_shle.cleared) { shle_sw16(0x400, 0x300); shle_sw16(0x402, 0x138); }
    if (g_ssyn.on) ssyn_ram_touch(g_shle.cleared, (uint32_t)to - g_shle.cleared);
    else           scsp_ram_touch(s, g_shle.cleared, (uint32_t)to - g_shle.cleared, 1);
    memset(g_sound.ram + g_shle.cleared, 0, (uint32_t)to - g_shle.cleared);
    g_shle.cleared = (uint32_t)to;
}

/* the main loop's pass over the 32 stream records; its copies make the next one later */
static void crx_pass(scsp_t *s, uint64_t now) {
    g_shle.pass_left = CRX_PASS_SAMPLES;
    crx_clear_step(s, now);
    if (!g_shle.booted || now < g_shle.stall_until) return;
    g_shle.chunks = 0;
    for (unsigned k = 0; k < 32; k++) crx_stream_step(k);
    g_shle.chunks_total += g_shle.chunks;
    if (g_shle.chunks) {
        g_shle.refill_at = now;
        g_shle.refill_n = g_shle.chunks;
    }
    g_shle.pass_rem += g_shle.chunks * SHLE_CHUNK_CYCLES;
    g_shle.pass_left += g_shle.pass_rem / SOUND_CYCLES_PER_SAMPLE;
    g_shle.pass_rem %= SOUND_CYCLES_PER_SAMPLE;
}

/* ---- the board's time ------------------------------------------------------------------------- */

static inline bool crx_in_window(uint32_t i, uint64_t e) {
    return e >= g_shle.amask_win[i][0] && e < g_shle.amask_win[i][1];
}

/* a timer's handler entered at e, its reload `reload_at` into it; 3.4.9's windows hold no one off */
static inline uint64_t crx_entry(uint64_t due) {
    uint64_t e = shle_entry(due - (uint64_t)CRX_B->lat_less * SHLE_T16);
    if (crx_refill_wait(e) > e) e += CRX_CHUNK_IRQ;
    for (uint32_t i = 0; i < g_shle.amask_n; i++)
        if (!(g_shle.amask_soft >> i & 1) && crx_in_window(i, e)) return g_shle.amask_win[i][1];
    return e;
}

/* 3.4.9: the main loop is inside a note's tracks at e, $C3E bit 4 set */
static bool crx_soft_at(uint64_t e) {
    for (uint32_t i = 0; i < g_shle.amask_n; i++)
        if ((g_shle.amask_soft >> i & 1) && crx_in_window(i, e)) return true;
    return false;
}

/* a handler at e holds up the main loop and the windows still ahead of it */
static void crx_suspend(uint64_t e, uint32_t dur) {
    if (g_shle.busy_until < e) g_shle.busy_until = e + CRX_LOOP_LAT;  /* an idle loop looks after the rte */
    g_shle.busy_until += dur;
    if (crx_refilling(e)) g_shle.refill_at += dur;
    for (uint32_t i = 0; i < g_shle.amask_n; i++)
        if (g_shle.amask_win[i][0] > e) {
            g_shle.amask_win[i][0] += dur;
            g_shle.amask_win[i][1] += dur;
        } else if (g_shle.amask_win[i][1] > e) {
            g_shle.amask_win[i][1] += dur;          /* MIDI (level 3) went in: the mask lasts longer */
        }
    if (e < g_shle.a_busy_until) g_shle.a_busy_until += dur;   /* ...and so does an A it went into */
    if (e < g_shle.b_busy_until) g_shle.b_busy_until += dur;   /* ...or 3.4.9's B */
}

/* Timer A's handler at e, dur long, holds off levels 1-2 till its first event goes in the
 * queue: sub_602AB4 leaves SR 0x2000. The rest runs open to B and to the next A, which finds
 * $C3E set and only reloads (hotd: one tick of 25 events every 1.8 s, the music falls behind). */
static void crx_a_level(uint64_t e, uint32_t dur) {
    uint64_t end = e + (g_shle.enq ? CRX_A_UNMASK : dur);
    if (g_shle.l2_free < end) g_shle.l2_free = end;
    if (g_shle.enq) g_shle.a_busy_until = e + dur;
}

static bool crx_events_boot(uint64_t now) {
    if (g_shle.restart_at && now >= g_shle.restart_at) {
        crx_clear_step(&g_sound.scsp, g_shle.restart_at);
        crx_boot(g_shle.restart_at);
    }
    if (now < g_shle.boot_at) return false;
    scsp_t *s = &g_sound.scsp;
    g_shle.booted = true;
    g_shle.amask_n = 0;                                 /* the boot scene's swap: cost offsets, not times */
    shle_sw8(0x401, 0x0F);
    crx_ctrl(crx_hb(0x68));
    for (int k = 0; k < 4; k++)                         /* four reads of MIBUF */
        if (s->mi_r != s->mi_w) { s->mi_r = (uint8_t)((s->mi_r + 1) & 31); s->mi_taken++; }
    g_shle.due_a = (now - SHLE_IRQ_LAT) * SHLE_T16;
    g_shle.due_b = (now + 225u - SHLE_IRQ_LAT) * SHLE_T16;
    g_shle.l2_free = now;
    return true;
}

static bool crx_events_midi(uint64_t now) {
    scsp_t *s = &g_sound.scsp;
    while (s->mi_r != s->mi_w) {
        uint8_t b = s->mi[s->mi_r];
        s->mi_r = (uint8_t)((s->mi_r + 1) & 31);
        s->mi_taken++;
        g_shle.l2_free = (g_shle.l2_free > now ? g_shle.l2_free : now) + SHLE_DUR_MIDI;
        crx_suspend(now, SHLE_DUR_MIDI);
        if (!crx_midi_byte(b)) {                        /* 0xFF: ErrorSet, then the entry point */
            shle_wl(0xC08, 0x4572726Fu);
            shle_wl(0xC0C, 0x72536574u);
            crx_restart(now);
            return false;
        }
    }
    return true;
}

/* 3.4.9's timer A (sub_6028FC) at e: $C3E set (a note's tracks, B, another A) only counts the
 * tick in $C1D; else the music runs at SR 0x2700 once and once more for each tick it owed, then
 * the effects open to the other levels. */
static bool crx_timer_a_late(uint64_t e, uint64_t now) {
    uint32_t dur, music = 0;
    unsigned passes = 0;
    bool held = crx_soft_at(e) || e < g_shle.a_busy_until || e < g_shle.b_busy_until;
    if (held) {
        crx_inc(CRX_LATE_A, 1);
        dur = CRX_DUR_A_NESTED;
    } else {
        passes = crx_timer_a_body(&music);
        dur = CRX_DUR_A + (passes > 1 ? passes - 1 : 0) * CRX_DUR_A_PASS + g_shle.enq * CRX_DUR_EVENT;
    }
    crx_suspend(e, dur);
    uint64_t end = e + (passes ? CRX_A_MUSIC + (passes - 1) * CRX_DUR_A_PASS + music * CRX_DUR_EVENT : dur);
    if (g_shle.l2_free < end) g_shle.l2_free = end;
    if (!held) g_shle.a_busy_until = e + dur;
    crx_dispatch(now);
    return true;
}

static bool crx_events_timer(uint64_t now) {
    uint64_t ea = crx_entry(g_shle.due_a), eb = crx_entry(g_shle.due_b);
    bool a = ea <= eb;                                  /* A is level 2, B level 1 (SCILV) */
    uint64_t e = a ? ea : eb;
    if (e > now) return false;
    g_shle.enq = 0;
    uint32_t dur;
    if (a) {
        uint16_t w = crx_hb(0x6A) << 8 | crx_hb(0x6B);
        g_shle.due_a = (e + CRX_RELOAD_AT_A + shle_period((uint8_t)w, (w >> 8) & 7)) * SHLE_T16;
        if (CRX_LATE) return crx_timer_a_late(e, now);
        bool nested = e < g_shle.a_busy_until;          /* the sequencers miss this tick */
        uint32_t music;
        if (!nested) crx_timer_a_body(&music);
        dur = nested ? CRX_DUR_A_NESTED : CRX_DUR_A + g_shle.enq * CRX_DUR_EVENT;
    } else {
        g_shle.due_b = (e + CRX_RELOAD_AT_B + shle_period(0xC6, 3)) * SHLE_T16;
        if (CRX_LATE && crx_soft_at(e)) dur = CRX_DUR_B_SKIP;      /* 3.4.9: $C3E & 0x12, the body skipped */
        else dur = CRX_DUR_B + crx_timer_b_body() * CRX_COST_FREE;
        if (CRX_LATE) g_shle.b_busy_until = e + dur;
    }
    crx_suspend(e, dur);
    if (a) crx_a_level(e, dur);
    crx_dispatch(now);
    return true;
}

static void crx_events(uint64_t now) {
    if (!g_shle.booted && !crx_events_boot(now)) return;
    if (!crx_events_midi(now)) return;
    for (int guard = 0; guard < 16 && crx_events_timer(now); guard++) {}
    crx_dispatch(now);
}

static inline uint64_t crx_next_due(void) {
    if (!g_shle.booted) return g_shle.restart_at && g_shle.restart_at < g_shle.boot_at ? g_shle.restart_at : g_shle.boot_at;
    uint64_t d = crx_entry(g_shle.due_a), x = crx_entry(g_shle.due_b);
    if (x < d) d = x;
    if (shle_rb(CRX_QN) && g_shle.busy_until < d) d = g_shle.busy_until;
    return d;
}

#endif /* SOUND_HLE_CRX_H */
