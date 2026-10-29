/*
 * sound_hle_rally.h — Sega Rally's build of the 1994-95 driver (epr-17890),
 * the parts that are not Virtua Fighter 2's. sound_hle_v95.h includes it and
 * runs these where the build says so (v95_build_t.rally); the queue, the
 * sequencers, the DSP and send controllers, "all stop" and the board's time
 * stay sound_hle_v95.h's.
 *
 * What it adds (a6 = 0x1000):
 *   0x3200 a log of the MIDI events in, 4 bytes each; 0x3300 its index
 *   0x250A a drum's group, 0x250C its priority; 0x2520 the engine at work,
 *   0x2521 the engines' cut (A?72); 0x252F bit 7: the LED a voice meter
 *   0x2600 16 engines x 32: +0..14 a layer keyed, +10.. its slot,
 *          +F the engine's sound, +1F its last value
 *   0x2A00 a loop record a slot (x 32): +0 left to the loop, +4 the length,
 *          +A a preloaded sample's countdown, +10 bit 0: a short loop
 * Sequencers are 16 bytes (eight from 0x3000); voices count down a word at
 * +E (timer B, 960); +C is a drum's priority, +D its window's 4 KB page;
 * effects are tracks with bit 0 and drums (bit 3), up to 16 of them.
 * Voice flags: 0 counting down, 1 held, 2 preloaded one-shot, 3 effect,
 * 4 drum, 5 released, 6 keyed on, 7 an engine's layer.
 * Record flags: 0 looped, 1 a loop under a chunk, 2 its loop's registers
 * due, 3 its last chunk in, 7 streaming.
 */
#ifndef SOUND_HLE_RALLY_H
#define SOUND_HLE_RALLY_H

#define RL_LOG      0x3200u
#define RL_LOGI     0x3300u
#define RL_GROUP    0x250Au
#define RL_PRIO     0x250Cu
#define RL_ENGBUSY  0x2520u
#define RL_ENGCUT   0x2521u
#define RL_METER    0x252Fu
#define RL_ENG      0x2600u
#define RL_LOOPS    0x2A00u

#define RL_COST_LAYER 900u          /* an engine layer's update */

/* ---- MIDI in, the LED ---------------------------------------------------------- */

static void rl_led(void) {                             /* 0x6004F0 */
    if (!shle_rb(RL_METER)) v95_led();
}

static void rl_meter(void) {                           /* 0x6004B6: the voices in use */
    if (!(shle_rb(RL_METER) & 0x80)) return;
    uint8_t d0 = (uint8_t)(shle_rb(V95_NMUS) + shle_rb(V95_NEFF));
    sound_ctrl_w(&g_sound, d0 < 4 ? 0x1Fu : ((uint8_t)~((d0 >> 1) - 1u) & 15u) | 0x10u);
}

/* Int3 (0x600582): a full queue drops the event; each one logged */
static bool rl_midi_byte(uint8_t b) {
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
        else if (shle_rb(V95_QN) != 0xFF) {
            uint16_t w = shle_rw(V95_QW);
            uint32_t p = V95_QUEUE + (w & 0xFFFu), l = RL_LOG + shle_rb(RL_LOGI);
            shle_wb(p, shle_rb(V95_RS));
            shle_wb(l, shle_rb(V95_RS));
            if ((uint8_t)(shle_rb(V95_RS + 2) - 1)) {
                shle_wb(p + 1, shle_rb(V95_RS + 1));
                shle_wb(l + 1, shle_rb(V95_RS + 1));
                p++; l++;
            }
            shle_wb(p + 1, b);
            shle_wb(l + 1, b);
            shle_ww(V95_QW, (w + 4u) & 0xFFFu);
            v95_inc(V95_QN, 1);
            v95_inc(RL_LOGI, 4);
            rl_led();
        }
    }
    shle_wb(V95_RS + 3, d1);
    return true;
}

/* ---- voices -------------------------------------------------------------------- */

static void rl_renum_mus(uint8_t d2, bool keyed) {     /* the music voices after d2 move up */
    for (uint32_t u = V95_VOICES; u < V95_VEND; u += 16) {
        uint8_t f = shle_rb(u + 2);
        if ((keyed && !(f & 0x40)) || (f & 8)) continue;
        if (shle_rb(u + 9) > d2) v95_inc(u + 9, -1);
    }
}

static void rl_renum_eff(uint8_t d2) {
    for (uint32_t u = V95_VOICES; u < V95_VEND; u += 16)
        if ((shle_rb(u + 2) & 8) && shle_rb(u + 5) > d2) v95_inc(u + 5, -1);
}

/* Int1, timer B (0x60068E): the release countdowns, words */
static uint32_t rl_timer_b_body(void) {
    uint32_t n = 0;
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        if ((shle_rb(v + 2) & 0x41) != 0x41) continue;
        uint16_t c = (uint16_t)(shle_rw(v + 0xE) - 1u);
        shle_ww(v + 0xE, c);
        if (c) continue;
        uint32_t slot = shle_rw(v);
        shle_sw8(slot + 0xB, 0x1F);
        bool fx = shle_rb(v + 2) & 8;
        if (fx) shle_sw8(slot, 0x10);
        shle_wb(V95_RECS + slot, 0);
        shle_wb(v + 2, 0);
        if (fx) {
            uint8_t d2 = shle_rb(v + 5);
            shle_wb(v + 9, 0);
            shle_wb(v + 5, 0);
            v95_inc(V95_NEFF, -1);
            rl_renum_eff(d2);
        } else {
            uint8_t d2 = shle_rb(v + 9);
            shle_wb(v + 9, 0);
            v95_inc(V95_NMUS, -1);
            rl_renum_mus(d2, true);
        }
        g_shle.cost += V95_COST_FREE;
        n++;
    }
    rl_meter();
    return n;
}

/* a drum's frequency, and its pace (0x601332) */
static uint16_t rl_drum_freq(uint32_t slot) {
    uint16_t d6 = shle_rb(V95_FREQ);
    uint8_t lo = shle_rb(V95_FREQ + 1), d0;
    if (!(d6 & 0xF) && (lo & 0x80)) { d6 = (uint16_t)(d6 + 252u); d0 = (uint8_t)((uint8_t)d6 - 1u); }
    else d0 = (uint8_t)d6;
    d6 = (uint16_t)(d6 << 7);
    uint32_t i = ((((uint32_t)d6 >> 3) & 0xF0u) + lo) & 0xFFu;
    shle_sw16(slot + 0x10, (d6 & 0x7800u) | shle_mw(V95_B->drumfreq + i * 2u));
    return shle_mw(shle_ml(V95_B->phase + ((d0 & 0xF0u) >> 2)) + ((d0 & 0xFu) << 5));
}

/* a melodic voice's (0x601398; the bend's 0x6015E0 shifts the note's row as a byte) */
static uint16_t rl_freq(uint32_t v, int16_t bend, uint8_t tune, uint32_t slot, bool byte_row) {
    int32_t d5 = (int16_t)(bend + (int8_t)(uint8_t)(shle_rb(v + 0xA) + tune));
    uint8_t n = shle_rb(v + 3);
    uint32_t a3 = shle_ml(V95_B->pitch + (n & 0xFu) * 4u) + 2u * (uint32_t)d5;
    shle_sw16(slot + 0x10, (((uint32_t)(n & 0xF0u) << 7) + shle_mw(a3)) & 0x7BFFu);
    uint32_t row = (n & 0xFu) << 5;
    if (byte_row) row &= 0xFFu;
    return shle_mw(shle_ml(V95_B->phase + ((n & 0xF0u) >> 2)) + row + 2u * (uint32_t)d5);
}

/* the sample's start (0x600D30); win: the voice's window in sound RAM */
static void rl_copy_first(uint32_t slot, uint32_t win, uint32_t src) {
    shle_copy(win, src, 0x200);
    g_shle.chunks++;
    shle_sw8(slot, shle_sr8(slot) | 0x18);
    g_shle.cost += SHLE_CHUNK_CYCLES;
}

static void rl_keyon_stream(uint32_t v, uint32_t slot, uint32_t e, uint16_t d2, uint32_t src, uint32_t len) {
    uint32_t r = V95_RECS + slot, win = (uint32_t)shle_rb(v + 0xD) << 12, lp = shle_rl(e + 0xC);
    if (lp) {
        uint32_t T = RL_LOOPS + slot;
        shle_wb(v + 2, shle_rb(v + 2) & ~0x10u);
        shle_wb(T + 0x10, 0);
        shle_wl(T + 4, len);
        shle_wl(T, len - lp);
        if (len <= 0x1000u) shle_wb(T + 0x10, 1);
        shle_wl(r + 0x16, (shle_rl(r + 0x16) & 0xFF000000u) | (lp & 0xFFFFFFu));
        shle_wl(r + 0x12, (shle_rl(r + 0x12) & 0xFF000000u) | (shle_rl(e + 8) & 0xFFFFFFu));
    }
    shle_sw32(slot, win | 0x10300000u);
    shle_wl(r + 0xA, len - 0x200u);
    shle_wb(r + 1, 0x10);
    shle_ww(r + 0x1A, d2);
    shle_ww(r + 0x1C, d2 + 0x8000u);
    uint8_t f = 0x80;
    if (lp) f = (uint8_t)(shle_rb(r) | 0x81u | (lp < 0x200u ? 2u : 0u));
    shle_wl(r + 2, src + 0x200u);
    shle_wl(r + 6, win + 0x200u);
    rl_copy_first(slot, win, src);
    shle_wb(r, f);
}

static void rl_keyon_sample(uint32_t v, uint32_t slot, uint16_t d2) {
    uint32_t e = V95_SMPTBL + (shle_rw(v + 6) & 0x7FFu) * 20u;
    shle_sw32(slot + 8, shle_rl(e + 0x10));
    shle_ww(v + 0xE, shle_mw(V95_B->release + (shle_rb(e + 0x13) & 0x1Fu) * 2u));
    uint32_t d5 = shle_rl(e), len = shle_rl(e + 4), lp = shle_rl(e + 0xC);
    if (d5 & 0x80000000u) {                             /* preloaded: the chip plays it from sound RAM */
        d5 &= 0x7FFFFFFFu;
        if (lp) {
            shle_wb(v + 2, shle_rb(v + 2) & ~0x10u);
            shle_sw32(slot, d5 | 0x10300000u);
            shle_sw16(slot + 4, shle_rl(e + 8));
            shle_sw16(slot + 6, len - 1u);
            shle_sw8(slot, shle_sr8(slot) | 0x18);
            shle_wb(V95_RECS + slot, 0x40);
            return;
        }
        shle_sw32(slot, d5 | 0x10100000u);
        shle_sw32(slot + 4, len & 0xFFFFu);
        uint16_t div = d2 >> 5;                         /* the 68000 traps on 0: no sound here pace that slow */
        shle_ww(RL_LOOPS + slot + 0xA, shle_rw(v + 0xE));
        shle_ww(v + 0xE, div ? (uint16_t)((len & 0xFFFFu) / div) : (uint16_t)len);
        shle_sw8(slot, shle_sr8(slot) | 0x18);
        shle_wb(v + 2, shle_rb(v + 2) | 5);
        return;
    }
    if (!len) return;
    if (len > 0x200u) { rl_keyon_stream(v, slot, e, d2, d5, len); return; }
    uint32_t win = (uint32_t)shle_rb(v + 0xD) << 12;    /* short: one chunk, no record */
    if (lp) {
        shle_wb(v + 2, shle_rb(v + 2) & ~0x10u);
        shle_sw32(slot + 4, ((uint32_t)(uint16_t)(len - lp) << 16) | (len & 0xFFFFu));
        shle_sw32(slot, win | 0x10300000u);
    } else {
        shle_sw32(slot + 4, len);
        shle_sw32(slot, win | 0x10100000u);
        shle_ww(v + 0xE, shle_rw(v + 0xE) + 2u);
        shle_wb(v + 2, shle_rb(v + 2) | 1);
    }
    rl_copy_first(slot, win, d5);
}

/* 0x600BBE: a note's key-on into voice v */
static void rl_keyon(uint32_t t, uint32_t v) {
    g_shle.keyons++;
    g_shle.cost += V95_COST_KEYON;
    uint32_t slot = shle_rw(v);
    shle_wb(v + 2, 0x40);
    shle_sw8(slot, 0x10);
    shle_ww(v + 6, shle_rw(V95_SMPW));
    bool drum = shle_rb(t) & 8;
    uint8_t ch = shle_rb(t + 1);
    shle_wb(v + 3, shle_rb(V95_NVAL));
    shle_wb(v + 8, shle_rb(V95_VEL));
    shle_wb(v + 4, ch);
    if (drum) {
        shle_wb(v + 2, 0x50);
        shle_wb(v + 0xC, shle_rb(RL_PRIO));
        shle_wb(v + 0xB, shle_rb(RL_GROUP));
        if (shle_rb(t) & 1) {
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
    uint16_t d2 = drum ? rl_drum_freq(slot) : rl_freq(v, (int16_t)shle_rw(V95_BEND + ch * 2u), shle_rb(t + 5), slot, false);
    shle_sw32(slot + 4, 0x1FFF);
    shle_sw8(slot + 0xC, 0);
    shle_sw16(slot + 0xE, 0);
    uint16_t lfo = shle_rw(t + 8) | (uint16_t)((shle_rb(V95_CC1 + ch) << 5) & 0xE0u);
    if (!drum) lfo |= shle_rb(V95_CC2 + ch) & 7u;
    shle_sw16(slot + 0x12, lfo);
    shle_sw8(slot + 0x15, drum ? shle_rb(V95_TONE) : shle_rb(t + 0xC));
    shle_sw8(slot + 0x16, 0x60u | (shle_rb(V95_PAN) & 0x1Fu));
    rl_keyon_sample(v, slot, d2);
}

/* a voice taken: cut, its record stopped */
static void rl_take(uint32_t v) {
    uint32_t slot = shle_rw(v);
    v95_cut(slot);
    shle_wb(V95_RECS + slot, 0);
}

/* 0x601228: the next free voice, else the music voice latest in the order
 * (released ones first) taken; 0 when there is none */
static uint32_t rl_alloc(void) {
    uint32_t a1 = shle_rl(V95_NEXTV);
    for (int k = 0; k < 32; k++, a1 += 16) {
        if (a1 == V95_VEND) { a1 = V95_VOICES; shle_wl(V95_NEXTV, a1); }
        if (!shle_rb(a1 + 2)) { shle_wl(V95_NEXTV, a1 + 16u); return a1; }
    }
    uint32_t pick = 0;
    uint8_t d6 = 0xFF;
    for (int pass = 0; pass < 2 && d6 == 0xFF; pass++) {
        d6 = 0xFF;
        for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
            uint8_t f = shle_rb(v + 2);
            if ((f & 8) || (!pass && !(f & 0x20)) || shle_rb(v + 9) > d6) continue;
            pick = v;
            d6 = shle_rb(v + 9);
        }
    }
    if (d6 == 0xFF) return 0;
    rl_take(pick);
    shle_wb(pick + 2, 0);
    rl_renum_mus(d6, false);
    v95_inc(V95_NMUS, -1);
    return pick;
}

/* a voice of the same group on the channel stops for the new one (0x60097A, 0x600AF6) */
static uint32_t rl_group_voice(uint8_t ch, uint8_t group, bool fx) {
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        uint8_t f = shle_rb(v + 2);
        if (shle_rb(v + 4) != ch || shle_rb(v + 0xB) != group || !(f & (fx ? 8 : 0x40))) continue;
        shle_wb(v + 2, 0);
        if (fx) {
            uint8_t d2 = shle_rb(v + 5);
            shle_wb(v + 5, 0);
            shle_wb(v + 9, 0);
            rl_take(v);
            v95_inc(V95_NEFF, -1);
            rl_renum_eff(d2);
        } else {
            uint8_t d2 = shle_rb(v + 9);
            shle_wb(v + 9, 0);
            rl_take(v);
            v95_inc(V95_NMUS, -1);
            rl_renum_mus(d2, false);
        }
        return v;
    }
    return 0;
}

/* an effect drum past 16: the keyed effect of the highest priority value
 * (the oldest of equals) taken, if it is not below the note's (0x600A02) */
static uint32_t rl_effect_steal(void) {
    uint8_t d0 = 0xFF, d1 = shle_rb(RL_PRIO), n = 16;
    uint32_t pick = 0;
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        if ((shle_rb(v + 2) & 0x48) != 0x48) continue;
        uint8_t pr = shle_rb(v + 0xC), o = shle_rb(v + 5);
        if (d1 < pr) { d0 = o; d1 = pr; pick = v; }
        else if (d1 == pr && o <= d0) { d0 = o; pick = v; }
        if (--n) continue;
        if (d0 & 0x80) return 0;
        if (shle_rb(pick + 2) & 0x80)                   /* an engine's layer: its keyed mark */
            shle_wb(RL_ENG + (shle_rb(pick + 0xA) & 15u), 0);
        rl_take(pick);
        shle_wb(pick + 5, 16);
        for (uint32_t u = V95_VOICES; u < V95_VEND; u += 16)
            if ((shle_rb(u + 9) & 0x80) && shle_rb(u + 5) > d0) v95_inc(u + 5, -1);
        v95_inc(V95_NEFF, -1);
        return pick;
    }
    shle_wb(V95_NEFF, 0);
    return rl_alloc();
}

static void rl_note_off(uint32_t t, uint8_t raw, uint8_t d4);

/* 0x6008C4 */
static void rl_note_on(uint32_t t, uint32_t p) {
    g_shle.cost += V95_COST_NOTE;
    uint8_t raw = shle_rb(p), vel = shle_rb(p + 1);
    uint8_t d4 = (uint8_t)(raw + shle_rb(t + 4));
    if (d4 & 0x80) return;
    if (!vel) { rl_note_off(t, raw, d4); return; }
    shle_wb(V95_VEL, shle_mb(V95_B->vel + vel));
    uint8_t prog = shle_rb(t + 2) & 0x7F;
    if ((int16_t)prog > (int16_t)shle_mw(V95_B->drums)) return;
    uint32_t a2 = shle_ml(V95_B->drums + 2 + prog * 4u), v;
    if (!(shle_rb(t) & 8)) {                            /* melodic: the split the note is in */
        int k = 0;
        for (; k < 128 && d4 > shle_mb(a2); k++) a2 += 6;
        if (k == 128) return;
        shle_ww(V95_SMPW, shle_mw(a2 + 4));
        shle_wb(V95_SPLIT, shle_mb(a2 + 2));
        shle_wb(V95_NVAL, shle_mb(V95_B->notes + ((uint8_t)(d4 + shle_mb(a2 + 3)) & 0x7Fu)));
        shle_wb(V95_PAN, shle_rb(t + 6));
        if ((v = rl_alloc())) rl_keyon(t, v);
        return;
    }
    uint32_t e = a2 + 2 + raw * 8u;
    uint16_t smp = shle_mw(e);
    if (smp & 0x8000) return;
    shle_ww(V95_SMPW, smp);
    uint8_t pan = shle_mb(e + 4);
    shle_wb(V95_PAN, (pan & 0x80) ? shle_rb(t + 6) : pan);
    shle_ww(V95_FREQ, shle_mw(e + 2));
    shle_wb(RL_PRIO, shle_mb(e + 6));
    uint8_t group = shle_mb(e + 7);
    shle_wb(RL_GROUP, group);
    uint8_t tone = shle_rb(t + 0xD);
    if (tone) shle_wb(t + 0xD, 0);
    else {
        tone = shle_rb(t + 0xC);
        if (!(tone & 7)) tone = shle_mb(e + 5);
    }
    shle_wb(V95_TONE, tone);
    shle_wb(V95_NVAL, shle_mb(V95_B->notes + (d4 & 0x7Fu)));
    bool fx = shle_rb(t) & 1;
    v = group ? rl_group_voice(shle_rb(t + 1), group, fx) : 0;
    if (!v) v = fx && shle_rb(V95_NEFF) >= 16 ? rl_effect_steal() : rl_alloc();
    if (v) rl_keyon(t, v);
}

/* 0x6014EC */
static void rl_release(uint32_t v, uint8_t ch) {
    if (shle_rb(V95_CC1 + ch) & 0x80) { shle_wb(v + 2, shle_rb(v + 2) | 2); return; }
    uint32_t slot = shle_rw(v);
    shle_sw8(slot, 0x10);
    uint8_t f = shle_rb(v + 2) | 0x21;
    shle_wb(v + 2, f);
    if (!(f & 4)) return;
    uint16_t c = shle_rw(RL_LOOPS + slot + 0xA);
    if (c < shle_rw(v + 0xE)) shle_ww(v + 0xE, c);
}

/* 0x60141A: raw the event's note, d4 it transposed */
static void rl_note_off(uint32_t t, uint8_t raw, uint8_t d4) {
    uint8_t prog = shle_rb(t + 2) & 0x7F;
    if ((int16_t)prog > (int16_t)shle_mw(V95_B->drums)) return;
    uint32_t a2 = shle_ml(V95_B->drums + 2 + prog * 4u);
    if (shle_rb(t) & 8) {
        uint16_t smp = shle_mw(a2 + 2 + raw * 8u);
        shle_ww(V95_SMPW, smp);
        if (smp & 0x8000) return;
        shle_wb(V95_NVAL, shle_mb(V95_B->notes + (d4 & 0x7Fu)));
    } else {
        int k = 0;
        for (; k < 128 && d4 > shle_mb(a2); k++) a2 += 6;
        if (k == 128) return;
        shle_ww(V95_SMPW, shle_mw(a2 + 4));
        shle_wb(V95_NVAL, shle_mb(V95_B->notes + ((uint8_t)(d4 + shle_mb(a2 + 3)) & 0x7Fu)));
    }
    uint8_t ch = v95_ch(), nv = shle_rb(V95_NVAL);
    uint16_t smp = shle_rw(V95_SMPW);
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        uint8_t f = shle_rb(v + 2);
        if (shle_rb(v + 3) != nv || shle_rb(v + 4) != ch || shle_rw(v + 6) != smp || (f & 0x12)) continue;
        shle_wb(v + 2, f & 0x7F);
        if (f & 0x7F) rl_release(v, ch);
    }
}

static void rl_note_off_ev(uint32_t t, uint32_t p) {   /* 0x60140C */
    g_shle.cost += V95_COST_SCAN;
    uint8_t raw = shle_rb(p), d4 = (uint8_t)(raw + shle_rb(t + 4));
    if (!(d4 & 0x80)) rl_note_off(t, raw, d4);
}

/* 0x60153C: the voices bend only when the channel has a melodic track */
static void rl_bend(uint32_t t, uint32_t p) {
    g_shle.cost += V95_COST_SCAN - 300u;
    uint16_t v = (uint16_t)(((shle_rb(p + 1) & 0x7Fu) << 7 | (shle_rb(p) & 0x7Fu)) >> 5);
    uint8_t d2 = (uint8_t)v;
    if (!(v & 0x100)) { d2 = (uint8_t)-d2; if (!d2) d2 = 0xFF; }
    int16_t d0 = shle_mb(shle_ml(V95_B->bend + (uint8_t)(shle_rb(t + 7) << 2)) + d2);
    if (!(v & 0x100)) d0 = (int16_t)-d0;
    uint8_t ch = v95_ch();
    shle_ww(V95_BENDW, (uint16_t)d0);
    shle_ww(V95_BEND + ch * 2u, (uint16_t)d0);
    uint32_t u = V95_TRACKS;
    while (u < V95_TRACKS + 20u * 16u && !(shle_rb(u + 1) == ch && !(shle_rb(u) & 8))) u += 16;
    if (u == V95_TRACKS + 20u * 16u) return;
    for (uint32_t w = V95_VOICES; w < V95_VEND; w += 16) {
        if (shle_rb(w + 4) != ch || !shle_rb(w + 2)) continue;
        uint32_t slot = shle_rw(w);
        shle_ww(V95_RECS + slot + 0x1A, rl_freq(w, d0, shle_rb(t + 5), slot, true));
    }
}

/* ---- controllers (0x601654's jump) ------------------------------------------------- */

static void rl_cc_lfo(uint8_t d0, bool pitch) {       /* CC 1, CC 2 */
    uint8_t ch = v95_ch(), n = (d0 >> 4) & 7;
    uint32_t a = (pitch ? V95_CC1 : V95_CC2) + ch;
    uint8_t old = shle_rb(a);
    if ((old & 0x7F) == n) return;
    shle_wb(a, (old & 0x80) | n);
    uint8_t d6 = pitch ? (uint8_t)(d0 << 1) & 0xE0 : (d0 >> 4) & 7;
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        if (shle_rb(v + 4) != ch || !shle_rb(v + 2)) continue;
        uint32_t s = shle_rw(v) + 0x13;
        shle_sw8(s, (shle_sr8(s) & (pitch ? 0x1Fu : 0xF8u)) | d6);
    }
}

static void rl_cc_pan(uint32_t t, uint8_t d0) {       /* CC 10 */
    uint8_t pan = shle_mb(V95_B->pan + (d0 & 0x7Fu)), ch = v95_ch();
    shle_wb(t + 6, pan);
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        if (shle_rb(v + 4) != ch || !shle_rb(v + 2)) continue;
        uint32_t s = shle_rw(v) + 0x16;
        shle_sw8(s, (shle_sr8(s) & 0xE0u) | pan);
    }
}

static void rl_cc_master(uint8_t d0) {                /* CC 16 */
    shle_wb(V95_SEQS + 0xE, d0);
    d0 = v95_x2(d0);
    shle_wb(V95_MVOL, d0);
    for (uint32_t t = V95_TRACKS; t < V95_TRACKS + 20u * 16u; t += 16) {
        if ((shle_rb(t) & 0x81) != 0x80) continue;
        shle_wb(t + 0xA, v95_level(shle_rb(t + 3), d0));
        for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16)
            if (shle_rb(t + 1) == shle_rb(v + 4) && (shle_rb(v + 2) & 0x40)) v95_voice_tl(v, shle_rb(t + 0xA));
    }
}

/* a track set: its tracks, cleared ones to 20 less the fixed tail */
static bool rl_trackset_copy(uint8_t d0) {
    if ((int16_t)d0 > (int16_t)shle_mw(V95_B->trk)) return false;
    uint32_t a = shle_ml(V95_B->trk + 2 + d0 * 4u);
    uint32_t n = shle_mb(a) + 1u, keep = 20u - shle_mb(V95_B->trk2);
    if (n > keep) n = keep;
    shle_copy(V95_TRACKS, a + 1, n * 16u);
    for (uint32_t t = V95_TRACKS + n * 16u; t < V95_TRACKS + keep * 16u; t += 16) {
        shle_wb(t, 0);
        shle_wb(t + 1, 0xFF);
    }
    g_shle.cost += n * 16u * V95_COST_BYTE;
    return true;
}

static void rl_cc_trackset(uint8_t d0) {              /* CC 32 */
    if (!rl_trackset_copy(d0 & 0x7F)) return;
    for (uint32_t t = V95_TRACKS; t < V95_TRACKS + 20u * 16u; t += 16)
        if (shle_rb(t) & 0x80) shle_wb(t + 0xA, v95_level(shle_rb(t + 3), (shle_rb(t) & 1) ? 0xFF : shle_rb(V95_MVOL)));
}

static void rl_cc_tone(uint8_t cc, uint8_t d0) {      /* CC 41, 42 over 20 tracks, 43 over 21 */
    uint8_t ch = v95_ch();
    uint32_t end = V95_TRACKS + (cc == 43 ? 21u : 20u) * 16u;
    if (cc == 41) d0 = (uint8_t)((d0 & 0xF) << 3 | (d0 & 0x70) >> 4);
    else if (cc == 42) d0 = ((d0 & 0x7F) >> 4) & 7;
    else d0 = (uint8_t)((d0 & 0xF) << 3) & 0x78;
    for (uint32_t t = V95_TRACKS; t < end; t += 16) {
        if (shle_rb(t + 1) != ch) continue;
        if (cc == 41) shle_wb(t + 0xD, d0);
        else if (cc == 42) shle_wb(t + 0xC, (shle_rb(t + 0xC) & 0x78) | d0);
        else shle_wb(t + 0xC, (shle_rb(t + 0xC) & 7) | d0);
    }
}

/* the sample table, then preloaded set d0 (0x601E1C, CC 58; 0x600406 at boot) */
static void rl_samples(uint8_t d0, uint32_t top) {
    uint32_t n = shle_mw(V95_B->smptbl) + 1u;
    shle_copy(V95_SMPTBL, V95_B->smptbl + 2, n);
    g_shle.cost += n * V95_COST_BYTE;
    if (d0 == 0x7F || (int16_t)d0 > (int16_t)shle_mw(V95_B->preload)) return;
    uint32_t a1 = shle_ml(V95_B->preload + 2 + d0 * 4u);
    uint16_t cnt = shle_mw(a1);
    if (cnt & 0x8000) return;
    uint32_t a3 = V95_PRELOAD;
    for (uint32_t i = 0; i <= cnt; i++) {
        uint32_t d1 = shle_mw(a1 + 2 + i * 2u) * 20u;
        uint32_t src = V95_B->smptbl + 2 + d1;
        uint32_t ptr = shle_ml(src), lp = shle_ml(src + 8), len = shle_ml(src + 4);
        if (shle_ml(src + 0xC)) lp -= ptr;
        uint32_t e = V95_SMPTBL + d1;
        shle_wl(e + 8, lp);
        shle_wl(e, a3 | 0x80000000u);
        uint32_t nb = ((len - 1u) & 0xFFFFu) + 1u;
        shle_copy(a3, ptr, nb);
        a3 += nb;
        g_shle.cost += nb * V95_COST_BYTE;
        if (a3 > top) { LOG_WARN("sound HLE: preloaded samples ran past 0x%X (the driver halts here)", top); return; }
    }
}

static void rl_cc_pedal(uint8_t d0) {                 /* CC 64 */
    uint8_t ch = v95_ch();
    if (d0) { shle_wb(V95_CC1 + ch, shle_rb(V95_CC1 + ch) | 0x80); return; }
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        uint8_t f = shle_rb(v + 2);
        if (shle_rb(v + 4) != ch || !(f & 2)) continue;
        uint32_t slot = shle_rw(v);
        shle_sw8(slot, 0x10);
        f = (uint8_t)((f & ~2u) | 1u);
        shle_wb(v + 2, f);
        if (!(f & 4)) continue;
        uint16_t c = shle_rw(RL_LOOPS + slot + 0xA);
        if (c < shle_rw(v + 0xE)) shle_ww(v + 0xE, c);
    }
    shle_wb(V95_CC1 + ch, shle_rb(V95_CC1 + ch) & 0x7F);
}

/* the channel's controllers cleared, its keyed voices let go (0x6020D6, 0x60212C) */
static void rl_all_off(bool cut) {
    uint8_t ch = v95_ch();
    v95_clear_channels(ch, ch + 1u);
    for (uint32_t v = V95_VOICES; v < V95_VEND; v += 16) {
        if (!(shle_rb(v + 2) & 0x40) || shle_rb(v + 4) != ch) continue;
        uint32_t slot = shle_rw(v);
        if (cut) shle_sw8(slot + 0xB, 0x1F);
        shle_sw8(slot, 0x10);
        shle_wb(v + 2, shle_rb(v + 2) | 1);
    }
}

static void rl_cc_track(uint32_t t, uint8_t cc, uint8_t d0) {   /* CC 8, 9, 11-14: the track's */
    uint16_t w = shle_rw(t + 8);
    switch (cc) {
    case 8:  shle_wb(t + 4, (uint8_t)((d0 & 0x7F) - 64)); return;
    case 9: {
        uint8_t x = shle_mb(V95_B->pan + (d0 & 0x7Fu));
        shle_wb(t + 5, (x & 0x10) ? (uint8_t)-(x & 0xF) : x);
        return;
    }
    case 11: shle_wb(t + 7, d0 & 0xF); return;
    case 12: w = (uint16_t)((w & ~0x300u) | ((d0 & 3u) << 8)); break;
    case 13: w = (uint16_t)((w & ~0x18u) | ((d0 & 3u) << 3)); break;
    default: w = (uint16_t)((w & ~0x7C00u) | ((d0 & 31u) << 10)); break;
    }
    shle_ww(t + 8, w);
}

static void rl_cc(uint32_t t, uint32_t p) {
    uint8_t cc = shle_rb(p) & 0x7F, d0 = shle_rb(p + 1);
    g_shle.cost += v95_cc_cost(cc);
    switch (cc) {
    case 1:   rl_cc_lfo(d0, true); break;
    case 2:   rl_cc_lfo(d0, false); break;
    case 5:   if (d0 == 127) shle_wb(RL_METER, 0x80); else if (!d0) shle_wb(RL_METER, 0); break;
    case 7:   v95_cc_volume(t, d0); break;
    case 8: case 9: case 11: case 12: case 13: case 14: rl_cc_track(t, cc, d0); break;
    case 10:  rl_cc_pan(t, d0); break;
    case 16:  rl_cc_master(d0); break;
    case 26:  shle_wb(V95_SEQS + 0xC, d0); shle_wb(V95_SEQS + 0xD, d0); break;
    case 32:  rl_cc_trackset(d0); break;
    case 41: case 42: case 43: rl_cc_tone(cc, d0); break;
    case 44:  v95_cc_dsp(d0); break;
    case 45:  v95_efsdl(d0); break;
    case 46: case 47: v95_cc_send(cc, d0); break;
    case 58:  rl_samples(d0 & 0x7F, 0x6FFFFu); g_shle.stall = true; break;   /* at SR 0x2300 */
    case 64:  rl_cc_pedal(d0); break;
    case 80:  v95_cc_stop(d0); break;
    case 123: rl_all_off(false); break;
    default:  break;
    }
}

static void rl_program(uint32_t t, uint32_t p) {      /* 0x602188: its data says drums, effects */
    uint8_t v = shle_rb(p);
    if ((int16_t)v > (int16_t)shle_mw(V95_B->drums)) return;
    uint32_t a = shle_ml(V95_B->drums + 2 + v * 4u);
    shle_wb(t, !(shle_mb(a) & 0x80) ? 0x80 : (shle_mb(a + 1) & 0x80) ? 0x89 : 0x8A);
    shle_wb(t + 2, v);
    rl_all_off(true);
}

/* ---- the engines (A? 7x) ------------------------------------------------------------ */

/* layer k of engine a0 off (0x60263E's step); slot: its slot byte cleared too */
static void rl_layer_off(uint32_t a0, uint32_t k, bool slot) {
    if (!shle_rb(a0 + k)) return;
    uint32_t s = shle_rb(a0 + 0x10 + k) & 31u;
    shle_sw8(s * 32u, 0x10);
    shle_wb(V95_VOICES + s * 16u + 2, shle_rb(V95_VOICES + s * 16u + 2) | 1);
    shle_wb(a0 + k, 0);
    if (slot) shle_wb(a0 + 0x10 + k, 0);
}

static void rl_layers_off(uint32_t a0, uint32_t n) {
    for (uint32_t k = 0; k < n; k++) rl_layer_off(a0, k, true);
}

/* a layer's level and pitch at value v: its pace */
static uint16_t rl_layer_sound(uint32_t L, uint8_t v, uint32_t slot) {
    uint8_t d3 = shle_mb(shle_ml(L + 4) + v);
    if (shle_mb(L + 13)) d3 = (uint8_t)(d3 + shle_rb(RL_ENGCUT));
    shle_sw8(slot + 0xD, (uint8_t)~d3 & 0x7Fu);
    uint16_t p = shle_mw(shle_ml(L + 8) + v * 2u) & 0x7BFFu;
    shle_sw16(slot + 0x10, p);
    return shle_mw(shle_ml(V95_B->phase + ((p & 0x7800u) >> 9)) + ((p & 0x3FFu) / 5u) * 2u);
}

/* an effect voice for a layer past 16 effects: the oldest that is a drum's (0x602484) */
static uint32_t rl_engine_voice(void) {
    if (shle_rb(V95_NEFF) < 16) return rl_alloc();
    for (uint8_t d4 = 0; d4 < 16; d4++) {
        uint32_t v = V95_VOICES;
        while (v < V95_VEND && !((shle_rb(v + 2) & 8) && shle_rb(v + 5) == d4)) v += 16;
        if (v == V95_VEND) { shle_wb(V95_NEFF, 0); return rl_alloc(); }
        if (!shle_rb(v + 0xC)) continue;
        rl_take(v);
        shle_wb(v + 5, 16);
        rl_renum_eff(d4);
        v95_inc(V95_NEFF, -1);
        return v;
    }
    return 0;
}

static void rl_engine_keyon(uint32_t a0, uint32_t k, uint32_t L, uint8_t val, uint32_t v) {   /* 0x602516 */
    g_shle.keyons++;
    g_shle.cost += V95_COST_KEYON;
    uint32_t slot = shle_rw(v);
    shle_wb(v + 2, 0xC8);
    shle_sw8(slot, 0x10);
    shle_wb(a0 + 0x10 + k, slot >> 5);
    shle_wb(a0 + k, shle_rb(a0 + k) | 1);
    shle_wb(v + 0xA, k);
    shle_ww(v + 6, shle_mw(L));
    shle_wb(v + 3, val);
    shle_wb(v + 8, 0);
    shle_wb(v + 0xC, 0);
    shle_wb(v + 4, 0xFF);
    shle_wb(v + 9, 0xFF);
    shle_wb(v + 5, shle_rb(V95_NEFF));
    v95_inc(V95_NEFF, 1);
    uint16_t d2 = rl_layer_sound(L, val, slot);
    shle_sw32(slot + 4, 0x1FFF);
    shle_sw8(slot + 0xC, 0);
    shle_sw16(slot + 0xE, 0);
    shle_sw16(slot + 0x12, 0);
    shle_sw8(slot + 0x15, 0);
    shle_sw8(slot + 0x16, 0x60);
    rl_keyon_sample(v, slot, d2);
}

/* 0x602374: engine n to value val, its layers keyed, moved or let go by range */
static void rl_engine(uint32_t n, uint8_t val) {
    v95_inc(RL_ENGBUSY, 1);
    uint32_t a0 = RL_ENG + n * 32u;
    uint8_t idx = shle_rb(a0 + 15);
    if ((int16_t)idx <= (int16_t)shle_mw(V95_B->engines) && val != shle_rb(a0 + 31)) {
        uint32_t a2 = shle_ml(V95_B->engines + 2 + idx * 4u);
        uint32_t cnt = shle_mw(a2) + 1u;
        shle_wb(a0 + 31, val);
        if (!val) rl_layers_off(a0, cnt);
        for (uint32_t k = 0; val && k < cnt; k++) {
            uint32_t L = a2 + 2 + k * 14u;
            g_shle.cost += RL_COST_LAYER;
            if (val < shle_mb(L + 2)) { rl_layer_off(a0, k, false); continue; }
            if ((int8_t)val > (int8_t)shle_mb(L + 3)) { rl_layer_off(a0, k, true); continue; }
            if (shle_rb(a0 + k)) {
                uint32_t slot = shle_rw(V95_VOICES + (shle_rb(a0 + 0x10 + k) & 31u) * 16u);
                shle_ww(V95_RECS + slot + 0x1A, rl_layer_sound(L, val, slot));
                continue;
            }
            uint32_t v = rl_engine_voice();
            if (v) rl_engine_keyon(a0, k, L, val, v);
        }
    }
    shle_wb(RL_ENGBUSY, 0);
}

static bool rl_layers_idle(uint32_t a0) {
    for (uint32_t k = 0; k < 9; k++) if (shle_rb(a0 + k)) return false;
    return true;
}

/* 0x6021D0: A? group song (< 0x70), or an engine's command */
/* A? 7x: the engines */
static void rl_engine_command(uint8_t d0, uint8_t b2) {
    uint32_t stop = 0;
    switch (d0 & 15) {
    case 0:  stop = 7; break;
    case 7:  stop = 0; break;
    case 11: stop = 13; break;
    case 13: stop = 11; break;
    case 4: case 6: rl_engine(d0 & 15u, b2); return;
    case 2: case 9: shle_wb(RL_ENGCUT, b2 > 32 ? 32 : b2); return;
    case 3: if (rl_layers_idle(RL_ENG + 4 * 32u)) shle_wb(RL_ENG + 4 * 32u + 15, (b2 & 3u) + 1u); return;
    case 5: if (rl_layers_idle(RL_ENG + 6 * 32u)) shle_wb(RL_ENG + 6 * 32u + 15, (b2 & 1u) + 5u); return;
    default: return;
    }
    uint32_t a = RL_ENG + stop * 32u;                    /* the other car's engine stops first */
    if (shle_rb(a + 31)) { shle_wb(a + 31, 0); rl_layers_off(a, 15); }
    rl_engine(d0 & 15u, b2);
}

/* an effect starts on its own sequencer if that one plays, else on the first free one */
static void rl_effect_start(uint32_t q, uint8_t f) {
    uint8_t id = f & 15;
    uint32_t s = 0;
    if (f & 0x10)
        for (uint32_t u = V95_SEQS + 16; u < V95_SEQS + 0x80 && !s; u += 16)
            if (shle_rb(u + 15) == id) s = u;
    if (!s) {
        for (s = V95_SEQS + 16; s < V95_SEQS + 0x70 && (shle_rb(s) & 0x80); s += 16) {}
    }
    for (uint32_t k = 0; k < 16; k++) shle_wb(s + k, 0);
    shle_wl(s + 4, q + 1);
    shle_ww(s + 2, 1);
    shle_wb(s, 0x80);
    shle_wb(s + 15, id);
}

static void rl_command(uint32_t p) {
    g_shle.cost += V95_COST_SONG;
    uint8_t d0 = shle_rb(p) & 0x7F, b2 = shle_rb(p + 1);
    if (d0 >= 0x70) { rl_engine_command(d0, b2); return; }
    if ((int16_t)d0 > (int16_t)shle_mw(V95_B->songs)) return;
    uint32_t g = shle_ml(V95_B->songs + 2 + d0 * 4u);
    uint8_t n = b2 & 0x7F;
    if ((int16_t)n > (int16_t)shle_mw(g)) return;
    uint32_t q = shle_ml(g + 2 + n * 4u);
    uint8_t f = shle_mb(q);
    if (!(f & 0x80)) {                                  /* the music */
        for (uint32_t k = 0; k < 16; k++) shle_wb(V95_SEQS + k, 0);
        shle_wl(V95_SEQS + 4, shle_ml(q + 4));
        shle_wl(V95_SEQS + 8, q + 8);
        shle_ww(V95_SEQS + 2, 1);
        shle_wb(V95_SEQS, 0x88);
        return;
    }
    rl_effect_start(q, f);
}

static void rl_event(uint32_t t, uint32_t p) {        /* 0x600872 */
    switch (shle_rb(V95_STATUS) & 0xF0) {
    case 0x90: rl_note_on(t, p); break;
    case 0x80: rl_note_off_ev(t, p); break;
    case 0xE0: rl_bend(t, p); break;
    case 0xB0: rl_cc(t, p); break;
    case 0xC0: rl_program(t, p); break;
    default: break;
    }
}

/* 0x6007EC's body: an A? event once, the rest to every track on the channel */
static void rl_dispatch_event(uint8_t st, uint32_t p) {
    if ((st & 0xF0) == 0xA0) { rl_command(p); return; }
    for (uint32_t t = V95_TRACKS; t < V95_TRACKS + 20u * 16u; t += 16)
        if (shle_rb(t + 1) == (st & 0xF) && (shle_rb(t) & 0x80)) rl_event(t, p);
}

/* ---- the main loop's records (0x602A20) -------------------------------------------------- */

/* the chip has played past the window's last chunk: the countdown on (0x602D76) */
static void rl_rec_finish(uint32_t r) {
    uint32_t slot = (uint32_t)shle_rb(r + 0x12) * 16u;
    shle_sw8(slot + 1, shle_sr8(slot + 1) & 0x9Fu);
    shle_sw16(slot + 6, shle_rw(r + 0x1E));
    shle_wb(r, 0);
    uint32_t v = V95_VOICES + slot / 2u;
    uint8_t d3 = (uint8_t)(((shle_rb(v + 2) & 8) ? 96u : 65u) - (shle_rw(r + 0x1A) >> 8));
    if (!(d3 & 0x80)) shle_ww(v + 0xE, shle_rw(v + 0xE) + d3);
    shle_wb(v + 2, shle_rb(v + 2) | 1);
}

/* a short loop's own registers: the chip loops in the window from here (0x602D24) */
static void rl_rec_loop_regs(uint32_t r, uint32_t slot) {
    uint32_t T = RL_LOOPS + slot;
    shle_sw32(slot + 4, (uint32_t)shle_rw(T + 2) << 16 | shle_rw(T + 6));
    shle_wb(r, shle_rb(r) & 0x7F);
}

/* past the end: the window goes on from the loop start */
static void rl_rec_wrap(uint32_t r, uint8_t r0, uint8_t d2, uint16_t d0, uint32_t a1, uint32_t a2, uint32_t d5) {
    uint32_t slot = (uint32_t)shle_rb(r + 0x12) * 16u, T = RL_LOOPS + slot;
    uint32_t w = (0x200u - d5) & 0xFFFFu;
    shle_ww(r + 0x1C, d0 & 0x7FFFu);
    shle_wl(r + 2, (shle_rl(r + 0x12) & 0xFFFFFFu) + w);
    shle_wb(r + 1, d2 + 0x10u);
    shle_wl(r + 0xA, (shle_rl(r + 0x16) & 0xFFFFFFu) - w);
    shle_chunk_copy(a2, a1);
    shle_wl(r + 6, a2 + 0x200u);
    if (!(r0 & 2)) return;
    if (shle_rl(T) & 0x80000000u) {                     /* a loop shorter than a chunk wrapped */
        shle_wl(T, shle_rl(T + 4));
        shle_ww(T + 6, shle_rw(T + 6) + shle_rw(r + 0x18));
        shle_wl(r + 2, shle_rl(r + 0x12));
        shle_wl(r + 6, shle_rl(T) + shle_rl(r + 0xE));
    } else if ((d2 & 0x80) || (shle_rb(T + 0x10) & 1)) rl_rec_loop_regs(r, slot);
    else shle_wb(r, (shle_rb(r) | 4) & ~1u);
}

static void rl_rec_step(unsigned k) {
    uint32_t r = V95_RECS + k * 0x20u;
    uint8_t r0 = shle_rb(r);
    if (!(r0 & 0x80)) return;
    uint16_t d0 = (uint16_t)(shle_rw(r + 0x1C) + shle_rw(r + 0x1A));
    if (d0 & 0x8000) { shle_ww(r + 0x1C, d0); return; }
    uint8_t d2 = shle_rb(r + 1);
    uint32_t slot = (uint32_t)shle_rb(r + 0x12) * 16u, T = RL_LOOPS + slot, a2;
    if (d2 == 0) {
        if (!shle_ca_upper(k)) return;
        shle_wl(T, shle_rl(T) - 0x2000u);
        shle_wl(T + 4, shle_rl(T + 4) - 0x2000u);
        a2 = shle_rl(r + 0xE);
    } else if (d2 == 0x80) { if (shle_ca_upper(k)) return; a2 = shle_rl(r + 6); }
    else a2 = shle_rl(r + 6);
    uint32_t a1 = shle_rl(r + 2), d5 = shle_rl(r + 0xA);
    if (d5 > 0x200u) {
        shle_wl(r + 0xA, d5 - 0x200u);
        shle_wb(r + 1, d2 + 0x10u);
        shle_ww(r + 0x1C, d0 & 0x7FFFu);
        shle_chunk_copy(a2, a1);
        shle_wl(r + 2, a1 + 0x200u);
        shle_wl(r + 6, a2 + 0x200u);
    } else if (r0 & 1) rl_rec_wrap(r, r0, d2, d0, a1, a2, d5);
    else if (r0 & 4) {
        if (!shle_ca_upper(k)) rl_rec_loop_regs(r, slot);
    } else if (r0 & 8) {
        if (!shle_ca_upper(k)) rl_rec_finish(r);
    } else {                                            /* the last chunk: where it ends */
        shle_ww(r + 0x1E, (uint16_t)(a2 - shle_rl(r + 0xE) + d5));
        shle_chunk_copy(a2, a1);
        if (!(d2 & 0x80)) {
            shle_wb(r, r0 | 8);
            shle_wb(r + 1, d2 + 0x10u);
        } else rl_rec_finish(r);
    }
}

/* ---- boot (0x600100) ---------------------------------------------------------------- */

static void rl_boot_tracks(void) {                     /* 0x6002C6 */
    rl_trackset_copy(0);
    uint32_t tail = shle_mb(V95_B->trk2);
    shle_copy(V95_TRACKS + (20u - tail) * 16u, V95_B->trk2 + 1, tail * 16u);
    shle_wb(V95_MVOL, 0xFF);
    for (uint32_t t = V95_TRACKS; t < V95_TRACKS + 20u * 16u; t += 16)
        if (shle_rb(t)) shle_wb(t + 0xA, v95_level(shle_rb(t + 3), 0xFF));
    shle_wl(V95_NEXTV, V95_VOICES);
}

static void rl_boot_engines(void) {                    /* 0x60048C */
    uint32_t n = shle_mw(V95_B->engines) + 1u;
    for (uint32_t k = 0; k < n && k < 16; k++)
        if (shle_mw(shle_ml(V95_B->engines + 2 + k * 4u)) != 0xFFFF) shle_wb(RL_ENG + k * 32u + 15, k);
}

#endif /* SOUND_HLE_RALLY_H */
