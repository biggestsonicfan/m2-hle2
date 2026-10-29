/*
 * sound_hle_vcop.h — Virtua Cop's Model 1 sound board driver (epr-18022.ic2,
 * code hash 0x5857DA20) in C in place of the 68000. sound_hle_m1.h includes
 * it and hands its board over when the build is this one (shle_drv_t.m1 2);
 * the bus, the chips' writes and the ring's bytes are that file's.
 *
 * Daytona USA's driver, a year older, rearranged: 20 channels, voices of 12
 * bytes, the drums on MIDI channel 15, no engines, and a main loop that only
 * polls timer B. The driver's RAM (0xF00000-0xF0FFFF):
 *   0xF00000 the MIDI ring, 0x1000 bytes (a0 in, a1 out)
 *   0xF01000 the message being received: length, left, running status;
 *            0xF01006 messages in the ring, 0xF01007 the status being played,
 *            0xF01048 the message's bytes, 0xF0104C where the next goes
 *   0xF01008 the note's sample word (+6 of its voice)
 *   0xF01010 the drum countdown's divider    0xF01021/22 the chips' ages
 *   0xF01050/54 the chips' next voice        0xF01100.. CC 0x16-0x1F, 0x34-0x37
 *   0xF012D0 a pitch bend word a channel     0xF012F0 CC 1 a channel (bit 7: held)
 *   0xF01300 20 channels x 10                0xF01500 56 voices x 12 (28 a chip)
 *   0xF017A0 the "engine" records A0 70+ walks: never written, all zero
 *   0xF02000 the music's sequencer, then seven effects' (32 bytes each)
 * Channel: Daytona's (+3 program, >= 0xF0 drums; +7 pan, bit 0 chip 2).
 * Voice: +0 flags (6 on, 4 drum with a countdown, 3 chip 2, 1 held, 0 to
 *   release), +1 slot, +2 sample, +3 channel, +4 a drum's countdown, +5
 *   velocity level, +6 the sample word, +8 age (0xFF: none), +9 tune, +10
 *   split transpose.
 *
 * The main loop (0x4F2) polls timer B: set, the tick (YM 0x27, the drum
 * countdowns 0x524, the sequencers 0x18C6); clear, a pass: the two chips'
 * releases (0x17C6, 0x1846) and one ring message (0x800). The parts run at
 * the board's times (VCH_*, clock periods, measured on MAME's board with
 * snd_replay traces of the shoot capture); the interrupt delays the part
 * under way, as in sound_hle_m1.h.
 */
#ifndef SOUND_HLE_VCOP_H
#define SOUND_HLE_VCOP_H

/* the init (0x300), from its start */
#define VCH_INIT_PCM1     721060u     /* chip 1's 28 slots cut (0x764) */
#define VCH_INIT_PCM2     753076u     /* chip 2's (0x780) */
#define VCH_INIT_YM       785092u     /* the YM3438's tables (0x7A2, 0x7B8) */
#define VCH_INIT_RAM      813496u     /* the voices, the channels, the banks */
#define VCH_INIT_TIMER    824210u     /* YM 0x27 = 0x2A */
#define VCH_INIT_UART     824400u     /* 0x5C8 */
#define VCH_INIT_LOOP     824882u     /* the main loop's first poll */

/* the poll, and the tick behind it: the YM write, 0x524's countdown, its
 * pass over the 56 voices' drum countdowns (+ a voice it marks), then the
 * sequencers: all idle, one waiting, one that plays (+ an event, an end) */
#define VCH_POLL          26u
#define VCH_TICK          172u
#define VCH_TICK_COUNT    108u
#define VCH_TICK_ARM      186u
#define VCH_TICK_DRUMS    5734u
#define VCH_TICK_MARK     84u
#define VCH_SEQS          530u
#define VCH_SEQ_WAIT      52u
#define VCH_SEQ_RUN       224u
#define VCH_SEQ_EVENT     440u
#define VCH_SEQ_END       158u
/* a pass: a chip's release scan (+ a voice keyed off, a drum's), the ring
 * empty, a message's walk over the 20 channels, then each handler */
#define VCH_REL_SCAN      1510u
#define VCH_REL           2640u
#define VCH_REL_DRUM      600u
#define VCH_MSG_NONE      76u
#define VCH_DISPATCH      2130u
#define VCH_D_LEAD        5900u       /* a message's key-on, from the ring's read */
#define VCH_NOTE_ON       4060u
#define VCH_STEAL         2000u       /* no voice free: the ages' scan */
#define VCH_NOTE_OFF      614u        /* + the voice loop */
#define VCH_CC            430u        /* + its loops */
#define VCH_START         1330u
#define VCH_VOICE         56u         /* a voice a loop passes over */
#define VCH_VOICE_OFF     79u         /* ...note off's */
#define VCH_VOICE_HIT     250u        /* ...one whose registers it writes */
#define VCH_MASTER_CH     2083u       /* CC 0x10: a channel's level and its voice loop */
#define VCH_MASTER_HIT    630u        /* ...a voice it writes */
/* the interrupt: a byte, the byte that completes a message */
#define VCH_IRQ_BYTE      358u
#define VCH_IRQ_MSG       550u

enum { VCH_S_POLL, VCH_S_REL1, VCH_S_REL2, VCH_S_MSG };

#define VCH_V1   0x1500u              /* chip 1's voices */
#define VCH_V2   0x1650u              /* chip 2's */
#define VCH_VEND 0x17A0u

static uint32_t s_vch_work;
#define VCH_LOOP(n, hits) (s_vch_work += (uint32_t)(n) * VCH_VOICE + (uint32_t)(hits) * VCH_VOICE_HIT)

static inline uint32_t vch_voices(uint32_t c) { return (M1R(c + 7u) & 1u) ? VCH_V2 : VCH_V1; }
static inline uint8_t  vch_ch(void) { return M1R(0x1007u) & 15u; }

static inline void vch_put(uint8_t b) { m1h_put(b); }

/* ---- the init (0x300) ------------------------------------------------------------- */

static void vch_init_pcm(int chip) {
    uint32_t ids = chip ? 0x780u : 0x764u;
    m1h_sel(chip, 3);    /* move.b #3,$C40013 (chip 2: $C60013) */
    for (uint32_t i = 0; i < 28; i++) {
        m1h_sel(chip, m1h_rb(ids + i));
        m1h_reg(chip, 0, 0x80);
        m1h_reg(chip, 4, 0x00);
        m1h_reg(chip, 5, 0xFE);
    }
}

static void vch_init_ym(uint64_t t) {
    for (uint32_t i = 0; i < 11; i++) m1h_ym(t, 0, m1h_rb(0x7A2u + 2 * i), m1h_rb(0x7A3u + 2 * i));
    for (uint32_t i = 0; i < 36; i++) {
        uint8_t a = m1h_rb(0x7B8u + 2 * i), d = m1h_rb(0x7B9u + 2 * i);
        m1h_ym(t, 0, a, d);
        m1h_ym(t, 1, a, d);
    }
}

static void vch_levels(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) M1R(0x1308u + 10u * i) = m1h_level(0x1300u + 10u * i);
}

static void vch_init_ram(void) {
    for (uint32_t i = 0; i < 28; i++) {
        M1R(VCH_V1 + 1u + 12u * i) = m1h_rb(0x764u + i);
        M1R(VCH_V2 + 1u + 12u * i) = m1h_rb(0x780u + i);
    }
    uint32_t p = m1h_rl(0x3F1Eu);
    for (uint32_t i = 0; i < 100; i++) M1R(0x1300u + i) = m1h_rb(p + i);
    m1h_bank(0, m1h_rb(p + 100u));
    p = m1h_rl(0x3F6Eu);
    for (uint32_t i = 0; i < 100; i++) M1R(0x1364u + i) = m1h_rb(p + i);
    m1h_bank(1, m1h_rb(p + 100u));
    M1R(0x101Eu) = 0xFF;
    vch_levels(20);
    m1h_wl(0x1050u, 0xF00000u + VCH_V1);
    m1h_wl(0x1054u, 0xF00000u + VCH_V2);
}

static void vch_boot(uint64_t now) {
    m1hle_t *h = &g_shle.m1;
    static const uint32_t at[] = { VCH_INIT_PCM1, VCH_INIT_PCM2, VCH_INIT_YM, VCH_INIT_RAM,
                                   VCH_INIT_TIMER, VCH_INIT_UART, VCH_INIT_LOOP };
    while (h->phase == M1H_BOOT && now >= h->t0 + at[h->stage]) {
        uint64_t t = h->t0 + at[h->stage];
        switch (h->stage++) {
        case 0: vch_init_pcm(0); break;
        case 1: vch_init_pcm(1); break;
        case 2: vch_init_ym(t); break;
        case 3: vch_init_ram(); break;
        case 4: m1h_ym(t, 0, 0x27, 0x2A); break;
        case 5: {
            static const uint8_t u[] = { 0, 0, 0, 0x40, 0x4E, 0x37 };
            M1R(0x1028u) = 1;
            for (size_t i = 0; i < sizeof u; i++) m1snd_uart_write(&g_sound.m1, 1, u[i]);
            break;
        }
        default:
            h->phase = M1H_RUN;
            h->dc = t;
            h->seg = VCH_S_POLL;
        }
    }
}

/* ---- the interrupt (0x120): a byte off the UART ------------------------------------ */

static uint32_t vch_irq(uint64_t now) {
    m1hle_t *h = &g_shle.m1;
    uint8_t b = m1snd_uart_read(&g_sound.m1, 0);
    h->irqs++;
    if (b == 0xFF) { m1hle_restart(now + M1H_RESTART_LAT); return 0; }
    if (b >= 0xF0u) return VCH_IRQ_BYTE;
    uint32_t p = m1h_rl(0xF0104Cu) & 0xFFFFu;
    if ((b & 0x80u) || M1R(0x1001u) == 0) {
        uint8_t led = (uint8_t)(M1R(0x1028u) << 1);   /* 0x73E: the LED walks */
        M1R(0x1028u) = led == 0x10 ? 1 : led;
        p = 0x1048u;
        if (!(b & 0x80u)) {                            /* running status: put it back in front */
            M1R(0x1001u) = (uint8_t)(M1R(0x1000u) - 1u);
            M1R(p++) = M1R(0x1002u);
        } else {
            M1R(0x1002u) = b;
            M1R(0x1001u) = M1R(0x1000u) = b >= 0xC0u && b < 0xE0u ? 2 : 3;
        }
    }
    M1R(p++) = b;
    m1h_wl(0x104Cu, 0xF00000u + p);
    if (--M1R(0x1001u)) return VCH_IRQ_BYTE;
    uint32_t n = (uint8_t)(M1R(0x1000u) - 1u) + 1u;
    for (uint32_t i = 0; i < n; i++) m1h_put(M1R(0x1048u + i));
    M1R(0x1006u)++;
    return VCH_IRQ_MSG;
}

/* ---- the sequencers (0x18C6) -------------------------------------------------------- */

/* 0x1B68: the music's fade */
static uint32_t vch_fade(uint32_t s) {
    if (!M1R(s + 10u) || --M1R(s + 11u)) return 0;
    M1R(s + 9u) -= 4u;
    if (M1R(s + 9u) & 0x80u) {
        M1R(s) &= 0x7Fu;
        M1R(s + 10u) = 0;
        vch_put(0xBE); vch_put(0x50); vch_put(0x7E);
    } else {
        vch_put(0xBE); vch_put(0x10); vch_put(M1R(s + 9u));
        M1R(s + 11u) = M1R(s + 10u);
    }
    M1R(0x1006u)++;
    return VCH_SEQ_EVENT;
}

/* the event's last data byte: bit 7 says no delta follows */
static inline uint8_t vch_last(uint32_t s, uint8_t d) {
    if (d & 0x80u) M1R(s) |= 0x40u;
    return d & 0x7Fu;
}

/* 0x192A: a sequencer's tick */
static uint32_t vch_seq(uint32_t s) {
    if (!(M1R(s) & 0x80u)) return 0;
    uint16_t cd = (uint16_t)(m1h_rw(0xF00000u + s + 6u) - 1u);
    m1h_ww(s + 6u, cd);
    if (cd) return VCH_SEQ_WAIT;
    uint32_t p = m1h_rl(0xF00000u + s + 2u), cost = VCH_SEQ_RUN;
    for (int guard = 0; guard < 4096; guard++) {
        M1R(s) &= ~0x40u;
        uint8_t st = m1h_rb(p++);
        if (!(st & 0x80u)) { st = M1R(s + 13u); p--; }
        M1R(s + 13u) = st;
        bool end = false;
        switch (st & 0xF0u) {
        case 0xB0: {
            vch_put(st);
            uint8_t ctl = m1h_rb(p++), v = m1h_rb(p++);
            vch_put(ctl);
            if (ctl == 0x10) {
                if (M1R(s + 10u)) v = M1R(s + 9u);
                else              M1R(s + 9u) = v & 0x7Fu;
            }
            vch_put(vch_last(s, v));
            break;
        }
        case 0x80:
            vch_put(st);
            vch_put(vch_last(s, m1h_rb(p++)));
            vch_put(0x7F);
            break;
        case 0xC0: case 0xD0:
            vch_put(st);
            vch_put(vch_last(s, m1h_rb(p++)));
            break;
        case 0xF0:
            if (st == 0xF7) {
                uint32_t n = m1h_rb(p++);
                p += n ? n : 256u;
            } else if (st == 0xF0) {
                do p += 2; while (m1h_rb(p - 1u) != 0xF7);
            } else if (st == 0xFF) {
                uint8_t ty = m1h_rb(p++);
                if (ty == 0x2F) { end = true; break; }
                uint32_t n = m1h_rb(p++);
                p += n ? n : 256u;
            } else {
                end = true;
            }
            g_shle.m1.metas++;
            if (end) break;
            goto delta;
        default:
            vch_put(st);
            vch_put(m1h_rb(p++));
            vch_put(vch_last(s, m1h_rb(p++)));
            break;
        }
        if (end) {                                     /* 0x1B04 */
            cost += VCH_SEQ_END;
            if (!(M1R(s) & 0x20u)) { M1R(s) = 0; return cost; }
            uint32_t l = m1h_rl(0xF00000u + s + 16u), d = m1h_rl(l);
            if (d == 0xFFFFFFFFu) { M1R(s) = 0; return cost; }
            if (d == 0xFFFFFFF2u) { memset(&M1R(0x2000u), 0, 224); return cost; }
            if (d == 0xFFFFFFF1u) {
                uint32_t a5 = m1h_rl(l + 4u);
                p = m1h_rl(a5);
                m1h_wl(s + 16u, a5 + 4u);
            } else {
                p = d;
                m1h_wl(s + 16u, l + 4u);
            }
            continue;
        }
        M1R(0x1006u)++;
        cost += VCH_SEQ_EVENT;
    delta:
        if (M1R(s) & 0x40u) continue;
        {
            uint16_t d = m1h_rb(p++);
            if (!d) continue;
            if (d & 0x80u) d = (uint16_t)((d & 0x7Fu) << 7 | m1h_rb(p++));
            m1h_ww(s + 6u, d);
            m1h_wl(s + 2u, p);
        }
        return cost;
    }
    M1R(s) = 0;
    return cost;
}

/* 0x1BF6: A? b1 b2, on the ring's bytes after a1: a sequence started; from
 * 0x70 the records at 0xF017A0 (0x6504), which the driver never fills: it
 * meant the table at 0x65F4, so the walk selects chip 2's slot 0 and plays
 * nothing a data byte can reach */
static void vch_start(uint32_t *a1) {
    uint8_t b = m1h_get(a1);
    if (b >= 0x70u) {
        uint8_t val = m1h_get(a1);
        if (val == M1R(0x1103u)) return;
        M1R(0x1103u) = val;
        uint32_t n = m1h_rw(0x65F2u);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t r = VCH_VEND + 12u * i;
            uint8_t x = val & 0x7Fu;
            m1h_sel(1, M1R(r) & 0x7Fu);
            if (!val || x < M1R(r + 2u) || x > M1R(r + 3u)) {
                if (M1R(r) & 0x80u) { M1R(r) &= 0x7Fu; m1h_reg(1, 4, 0); }
                continue;
            }
            m1h_reg(1, 5, (uint8_t)(m1h_rb(m1h_rl(0xF00000u + r + 4u) + x) << 1));
            uint32_t pt = m1h_rl(0xF00000u + r + 8u) + 2u * x;
            m1h_reg(1, 3, m1h_rb(pt));
            m1h_reg(1, 2, m1h_rb(pt + 1u));
            if (M1R(r) & 0x80u) continue;
            M1R(r) |= 0x80u;
            m1h_reg(1, 1, M1R(r + 1u));
            m1h_reg(1, 0, 0);
            m1h_reg(1, 4, 0x80);
        }
        return;
    }
    uint32_t t = m1h_rl(0x1CC2u + 4u * (b & 0x7Fu));
    uint8_t n = m1h_get(a1);
    if ((int16_t)n > (int16_t)m1h_rw(t)) return;
    uint32_t a5 = m1h_rl(t + 2u + 4u * n);
    if (m1h_rb(a5) & 0x80u) {
        uint32_t s = 0x2020u;
        while (s < 0x20E0u && (M1R(s) & 0x80u)) s += 32u;
        memset(&M1R(s), 0, 32);
        m1h_wl(s + 2u, a5 + 1u);
        m1h_ww(s + 6u, 1);
        M1R(s) |= 0x80u;
    } else {
        memset(&M1R(0x2000u), 0, 32);
        m1h_wl(0x2002u, m1h_rl(a5 + 4u));
        m1h_wl(0x2010u, a5 + 8u);
        m1h_ww(0x2006u, 1);
        M1R(0x2000u) |= 0xA0u;
    }
}

/* ---- the tick: YM 0x27, 0x524, 0x18C6 ----------------------------------------------- */

static uint32_t vch_tick(uint64_t t) {
    uint32_t cost = VCH_POLL + VCH_TICK;
    m1h_ym(t, 0, 0x27, 0x2A);
    /* 0x524: every 36 ticks the drums' countdowns, all 56 voices in one */
    if (M1R(0x1017u) & 1u) {
        cost += VCH_TICK_DRUMS;
        for (uint32_t v = VCH_V1; v < VCH_VEND; v += 12u) {
            if (--M1R(v + 4u)) continue;
            uint8_t f = M1R(v);
            if (!(f & 0x81u) && (f & 0x70u) == 0x50u) { M1R(v) |= 1u; cost += VCH_TICK_MARK; }
        }
        M1R(0x1017u) &= ~1u;
        m1h_wl(0x1018u, 0xF00000u + VCH_VEND + 4u);
    } else if (--M1R(0x1010u) == 0) {
        cost += VCH_TICK_ARM;
        M1R(0x1010u) = 0x23;
        m1h_wl(0x1018u, 0xF01504u);
        M1R(0x101Cu) = 0x37;
        M1R(0x1017u) = 1;
    } else {
        cost += VCH_TICK_COUNT;
    }
    /* 0x18C6: the sequencers, while the ring has room */
    if (M1R(0x1006u) < 0xFFu) {
        cost += VCH_SEQS + vch_fade(0x2000u);
        for (uint32_t s = 0x2000u; s <= 0x20E0u; s += 0x20u) cost += vch_seq(s);
    }
    return cost;
}

/* ---- a ring message (0x800, 0x8AA) -------------------------------------------------- */

/* the melodic program's split for the note F01026: 0x529E */
static uint32_t vch_split(uint32_t c) {
    uint32_t a5 = m1h_rl(0x529Eu + 4u * (M1R(c + 3u) & 0x7Fu));
    while (M1R(0x1026u) >= m1h_rb(a5)) a5 += 6u;
    return a5;
}

/* a drum's entry for note d2: 0x58B2 */
static inline uint32_t vch_drum(uint32_t c, uint8_t d2) {
    return m1h_rl(0x58B2u + 4u * ((uint8_t)(M1R(c + 3u) + 0x10u) & 0x7Fu)) + 6u * (d2 & 0x7Fu);
}

/* a voice's pitch: its sample's table at `d` (0xBE0, 0xEB0) */
static void vch_pitch(uint32_t v, int32_t d, uint8_t hi) {
    uint32_t fp = m1h_rl(0x470Au + 4u * (M1R(v + 2u) & 15u)) + (uint32_t)(2 * d);
    m1h_vreg(v, 3, (uint8_t)((M1R(v + 2u) & 0xF0u) + m1h_rb(fp)));
    m1h_vreg(v, 2, (uint8_t)((hi & 3u) | m1h_rb(fp + 1u)));
}

/* 0x169A / 0x1730: a free voice on the chip, or the oldest */
static uint32_t vch_alloc(int chip) {
    uint32_t lo = chip ? VCH_V2 : VCH_V1, hi = lo + 336u, rr = chip ? 0x1054u : 0x1050u;
    uint32_t v = m1h_rl(0xF00000u + rr) & 0xFFFFu;
    for (int i = 0; i < 28; i++) {
        if (v == hi) { v = lo; m1h_wl(rr, 0xF00000u + v); }
        if (M1R(v) == 0) { m1h_wl(rr, 0xF00000u + v + 12u); return v; }
        v += 12u;
    }
    s_vch_work += VCH_STEAL;
    for (v = lo; v < hi; v += 12u) {
        if (M1R(v + 8u)) continue;
        M1R(v + 8u) = 28;
        for (uint32_t k = lo; k < hi; k += 12u)
            if (M1R(k + 8u) != 0xFFu) M1R(k + 8u)--;
        M1R(chip ? 0x1022u : 0x1021u)--;
        return v;
    }
    return 0;
}

static void vch_note_off_at(uint32_t c, uint8_t d2);

/* 0x8F2: note on */
static void vch_note_on(uint32_t c, uint8_t d2, uint32_t *a1) {
    if (d2 & 0x80u) d2 = m1h_get(a1);
    M1R(0x1026u) = (uint8_t)(d2 + M1R(c + 4u));
    uint8_t vel = m1h_get(a1);
    if (!vel) { vch_note_off_at(c, d2); return; }
    /* 0xC00001 bit 0 (open on this board: reads 0) would pick 0x521E */
    M1R(0x1013u) = m1h_rb(0x519Eu + vel);
    uint8_t prog = M1R(c + 3u), ch = vch_ch(), d1;
    if (prog >= 0xF0u) {
        uint32_t e = vch_drum(c, d2);
        d1 = m1h_rb(e + 5u);
        if (d1 == 0x80u) d1 = M1R(c + 7u);
        M1R(0x1027u) = d1 & 1u;
        M1R(0x1016u) = d1 & 0xFEu;
        m1h_ww(0x1058u, m1h_rw(e + 2u));
        M1R(0x1029u) = m1h_rb(e + 4u);
        m1h_ww(0x1008u, m1h_rw(e));
        M1R(0x1012u) = m1h_rb(0x50EAu + (M1R(0x1026u) & 0x7Fu));
        if (m1h_rw(e) == 0xFFu) return;
        if (ch == 15 && (M1R(0x1012u) == 0xF6u || M1R(0x1012u) == 0xF8u)) {   /* the hi-hat's choke */
            uint32_t lo = M1R(0x1027u) ? VCH_V2 : VCH_V1;
            VCH_LOOP(28, 0);
            for (uint32_t v = lo; v < lo + 336u; v += 12u)
                if (M1R(v + 2u) == 0xFAu && M1R(v + 3u) == 15u) M1R(v) |= 1u;
        }
    } else {
        uint32_t a5 = vch_split(c);
        m1h_ww(0x1008u, m1h_rw(a5 + 4u));
        M1R(0x1024u) = m1h_rb(a5 + 2u);
        M1R(0x1025u) = m1h_rb(a5 + 3u);
        M1R(0x1012u) = m1h_rb(0x50EAu + ((uint8_t)(M1R(0x1026u) + m1h_rb(a5 + 3u)) & 0x7Fu));
        d1 = M1R(c + 7u);
        M1R(0x1027u) = d1 & 1u;
        M1R(0x1016u) = d1 & 0xFEu;
    }
    int chip = M1R(0x1027u) & 1;
    uint32_t v = vch_alloc(chip);
    if (!v) return;
    /* 0xAD4 */
    uint8_t *age = &M1R(chip ? 0x1022u : 0x1021u);
    M1R(v) = (uint8_t)(0x40u | (chip ? 8u : 0u));
    if (prog >= 0xF0u) {
        if (M1R(0x1029u)) { M1R(v) |= 0x10u; M1R(v + 4u) = M1R(0x1029u); }
        m1h_vsel(v);
        M1R(v + 2u) = M1R(0x1012u);
        M1R(v + 5u) = M1R(0x1013u);
        M1R(v + 3u) = ch;
        M1R(v + 8u) = ch == 15 ? (*age)++ : 0xFFu;
        m1h_vreg(v, 3, M1R(0x1058u));
        m1h_vreg(v, 2, (uint8_t)((M1R(0x1008u) & 3u) | M1R(0x1059u)));
    } else {
        m1h_vsel(v);
        M1R(v + 2u) = M1R(0x1012u);
        M1R(v + 5u) = M1R(0x1013u);
        M1R(v + 3u) = ch;
        M1R(v + 8u) = (*age)++;
        M1R(v + 9u) = M1R(0x1024u);
        M1R(v + 10u) = M1R(0x1025u);
        vch_pitch(v, (int16_t)((int16_t)m1h_rw(0xF012D0u + 2u * ch) + (int8_t)M1R(0x1024u)), M1R(0x1008u));
    }
    m1h_vreg(v, 1, M1R(0x1009u));
    M1R(v + 6u) = M1R(0x1008u);
    M1R(v + 7u) = M1R(0x1009u);
    m1h_vreg(v, 5, (uint8_t)(~((M1R(v + 5u) * M1R(c + 8u)) >> 8) | 1u));
    m1h_vreg(v, 0, M1R(0x1016u));
    m1h_vreg(v, 4, 0x80);
    g_shle.keyons++;
}

/* 0xCB2: the note F01026 (d2 as received) off */
static void vch_note_off_at(uint32_t c, uint8_t d2) {
    if (M1R(c + 3u) >= 0xF0u) {
        m1h_ww(0x1008u, m1h_rw(vch_drum(c, d2)));
        M1R(0x1012u) = m1h_rb(0x50EAu + (M1R(0x1026u) & 0x7Fu));
    } else {
        uint32_t a5 = vch_split(c);
        m1h_ww(0x1008u, m1h_rw(a5 + 4u));
        M1R(0x1012u) = m1h_rb(0x50EAu + ((uint8_t)(M1R(0x1026u) + m1h_rb(a5 + 3u)) & 0x7Fu));
    }
    uint8_t ch = vch_ch();
    uint16_t w = m1h_rw(0xF01008u);
    uint32_t lo = vch_voices(c);
    s_vch_work += 28u * VCH_VOICE_OFF;
    for (uint32_t v = lo; v < lo + 336u; v += 12u) {
        if (M1R(v + 2u) != M1R(0x1012u) || M1R(v + 3u) != ch || m1h_rw(0xF00000u + v + 6u) != w ||
            (M1R(v) & 0x12u))
            continue;
        if (!(M1R(v) &= 0x7Fu)) continue;
        M1R(v) |= (M1R(0x12F0u + ch) & 0x80u) ? 2u : 1u;   /* 0xDCA: held, or released */
    }
}

/* 0x1600: every voice of the channel released */
static void vch_all_off(uint32_t c) {
    uint8_t ch = vch_ch();
    uint32_t lo = vch_voices(c);
    VCH_LOOP(28, 0);
    for (uint32_t v = lo; v < lo + 336u; v += 12u)
        if ((M1R(v) & 0x40u) && M1R(v + 3u) == ch) M1R(v) |= 1u;
}

/* the voice level, CC 7 and 0x10 (0x13CC, 0x1478) */
static inline void vch_vlevel(uint32_t v, uint32_t c) {
    s_vch_work += VCH_VOICE_HIT;
    m1h_vsel(v);
    m1h_vreg(v, 5, (uint8_t)(~((M1R(v + 5u) * M1R(c + 8u)) >> 8) & 0xFEu));
}

/* 0xF2E: a controller, by the jump table at 0xF4E */
static void vch_cc(uint32_t c, uint8_t d2, uint32_t *a1) {
    if (d2 & 0x80u) d2 = m1h_get(a1);
    uint8_t d0 = m1h_get(a1), ch = vch_ch();
    uint32_t lo = vch_voices(c);
    switch (d2) {
    case 0x01: {                                       /* LFO depth (0x1258) */
        uint8_t m = (d0 >> 4) & 7u;
        M1R(0x101Du) = m;
        uint8_t o = M1R(0x12F0u + ch);
        if ((o & 0x7Fu) == m) break;
        M1R(0x12F0u + ch) = (uint8_t)((o & 0x80u) | m);
        VCH_LOOP(28, 0);
        for (uint32_t v = lo; v < lo + 336u; v += 12u)
            if (M1R(v + 3u) == ch && (M1R(v) & 0x7Fu)) {
                VCH_LOOP(0, 1);
                m1h_vsel(v);
                m1h_vreg(v, 6, (uint8_t)(M1R(0x101Du) | M1R(c + 6u)));
            }
        break;
    }
    case 0x07: {                                       /* 0x140A */
        uint8_t x = (uint8_t)(d0 << 1);
        M1R(c + 2u) = x ? x + 1u : 0;
        M1R(c + 8u) = m1h_level(c);
        VCH_LOOP(28, 0);
        for (uint32_t v = lo; v < lo + 336u; v += 12u)
            if ((M1R(v) & 0x40u) && M1R(v + 3u) == ch) vch_vlevel(v, c);
        break;
    }
    case 0x0A: {                                       /* pan (0x11C4) */
        uint8_t x = d0 >> 3;
        x = x >= 8u ? x & ~8u : x ? x | 8u : 9u;
        x = (uint8_t)(x << 4 | (M1R(c + 7u) & 1u));
        M1R(c + 7u) = x;
        lo = vch_voices(c);
        VCH_LOOP(28, 0);
        for (uint32_t v = lo; v < lo + 336u; v += 12u)
            if (M1R(v + 3u) == ch && (M1R(v) & 0x7Fu)) { VCH_LOOP(0, 1); m1h_vsel(v); m1h_vreg(v, 0, x); }
        break;
    }
    case 0x10: {                                       /* master volume: the first 10 channels (0x1358) */
        uint8_t x = (uint8_t)(d0 << 1);
        M1R(0x101Eu) = x ? x + 1u : 0;
        for (uint32_t k = 0x1300u; k < 0x1364u; k += 10u) {
            M1R(k + 8u) = m1h_level(k);
            s_vch_work += VCH_MASTER_CH;
            uint32_t l2 = vch_voices(k);
            for (uint32_t v = l2; v < l2 + 336u; v += 12u)
                if (M1R(k + 1u) == M1R(v + 3u) && (M1R(v) & 0x40u)) {
                    vch_vlevel(v, k);
                    s_vch_work += VCH_MASTER_HIT - VCH_VOICE_HIT;
                }
        }
        break;
    }
    case 0x40:                                         /* the pedal: below 0x40 holds (0x12EA) */
        if (d0 < 0x40u) { M1R(0x12F0u + ch) |= 0x80u; break; }
        VCH_LOOP(28, 0);
        for (uint32_t v = lo; v < lo + 336u; v += 12u)
            if (M1R(v + 3u) == ch && (M1R(v) & 2u)) M1R(v) |= 1u;
        M1R(0x12F0u + ch) &= 0x7Fu;
        break;
    case 0x50:                                         /* everything off: 7D the effects, 7E the music (0x14AA) */
        VCH_LOOP(56, 0);
        for (uint32_t v = VCH_V1; v < VCH_VEND; v += 12u) {
            if (!(M1R(v) & 0x40u)) continue;
            uint8_t k = M1R(v + 3u);
            if (d0 == 0x7Du ? k >= 9u && k != 15u : d0 == 0x7Eu ? k <= 9u || k == 15u : true) M1R(v) |= 1u;
        }
        memset(&M1R(0x12F0u), 0, 16);
        memset(&M1R(0x12D0u), 0, 32);
        s_vch_work += 1180u;   /* the 56 voices' tests, the clears */
        break;
    case 0x20: {                                       /* a channel set (0x1574) */
        uint32_t p = m1h_rl(0x3F1Eu + 4u * (d0 & 31u));
        for (uint32_t i = 0; i < 100; i++) M1R(0x1300u + i) = m1h_rb(p + i);
        m1h_bank(0, m1h_rb(p + 100u));
        vch_levels(10);
        s_vch_work += 4230u;   /* the copy, the levels */
        break;
    }
    case 0x21: m1h_bank(M1R(c + 7u) & 1u, d0); break;  /* 0x15DA */
    case 0x7B: vch_all_off(c); break;
    case 0x16: M1R(0x1101u) = d0; break;
    case 0x17: M1R(0x1100u) = d0; break;
    case 0x18: M1R(0x1112u) = d0; break;
    case 0x19: M1R(0x1111u) = d0; break;
    case 0x1A: M1R(0x200Au) = d0; M1R(0x200Bu) = d0; break;   /* the music's fade */
    case 0x1B: M1R(0x110Fu) = d0; break;
    case 0x1C: M1R(0x110Eu) = d0; break;
    case 0x1D: M1R(0x110Du) = d0; break;
    case 0x1E: M1R(0x110Cu) = d0; break;
    case 0x1F: M1R(0x110Au) = d0; break;
    case 0x34: M1R(0x110Bu) = d0; break;
    case 0x35: M1R(0x1110u) = d0; break;
    case 0x36: M1R(0x1108u) = d0; break;
    case 0x37: M1R(0x1109u) = d0; break;
    default: break;
    }
}

/* 0xDEA: pitch bend */
static void vch_bend(uint32_t c, uint8_t d2, uint32_t *a1) {
    if (d2 & 0x80u) d2 = m1h_get(a1);
    uint8_t msb = m1h_get(a1);
    uint16_t v = (uint16_t)(((msb & 0x7Fu) << 7 | (d2 & 0x7Fu)) >> 5);
    uint8_t i = (uint8_t)v;
    if (!(v & 0x100u)) { i = (uint8_t)-i; if (!i) i = 0xFF; }
    uint8_t t = m1h_rb(m1h_rl(0x4BBAu + 4u * (M1R(c + 9u) & 0x7Fu)) + i);
    int16_t bend = (v & 0x100u) ? (int16_t)t : (int16_t)(int8_t)(uint8_t)-t;
    m1h_ww(0x1014u, (uint16_t)bend);
    uint8_t ch = vch_ch();
    m1h_ww(0x12D0u + 2u * ch, (uint16_t)bend);
    uint32_t lo = vch_voices(c);
    VCH_LOOP(28, 0);
    for (uint32_t k = lo; k < lo + 336u; k += 12u)
        if (M1R(k + 3u) == ch && (M1R(k) & 0x7Fu)) {
            VCH_LOOP(0, 1);
            m1h_vsel(k);
            vch_pitch(k, (int16_t)(bend + (int8_t)M1R(k + 9u)), M1R(k + 6u));
        }
}

/* 0x8AA: the message for channel c; d2 its first byte */
static uint32_t vch_handle(uint32_t c, uint8_t d2, uint32_t *a1) {
    switch (M1R(0x1007u) & 0xF0u) {
    case 0x90: vch_note_on(c, d2, a1); return VCH_NOTE_ON;
    case 0x80:
        if (d2 & 0x80u) d2 = m1h_get(a1);
        M1R(0x1026u) = (uint8_t)((d2 & 0x7Fu) + M1R(c + 4u));
        m1h_get(a1);
        vch_note_off_at(c, d2);
        return VCH_NOTE_OFF;
    case 0xE0: vch_bend(c, d2, a1); return VCH_CC;
    case 0xB0: vch_cc(c, d2, a1); return VCH_CC;
    case 0xC0:
        if (d2 & 0x80u) d2 = m1h_get(a1);
        M1R(c + 3u) = d2 >= 0x46u ? d2 | 0x80u : d2;
        vch_all_off(c);
        return VCH_CC;
    case 0xA0: vch_start(a1); return VCH_START;
    default: return 0;   /* 0xD0 */
    }
}

/* 0x800: one message off the ring, to every channel on its MIDI channel */
static uint32_t vch_dispatch(void) {
    m1hle_t *h = &g_shle.m1;
    if (!M1R(0x1006u)) return VCH_MSG_NONE;
    M1R(0x1006u)--;
    uint8_t b = m1h_get(&h->a1), d0 = b;
    uint32_t after = h->a1, cost = 0;
    m1h_wl(0x1044u, 0xF00000u + after);
    if (b & 0x80u) M1R(0x1007u) = b;
    else           d0 = M1R(0x1007u);
    d0 &= 15u;
    for (uint32_t c = 0x1300u; c < 0x13C8u; c += 10u) {
        if (!(M1R(c) & 0x80u) || M1R(c + 1u) != d0) continue;
        uint32_t a1 = after;
        s_vch_work = 0;
        cost += vch_handle(c, b, &a1) + s_vch_work;
        m1h_wl(0x1040u, 0xF00000u + a1);
    }
    uint8_t d1;
    if (b & 0x80u) { d1 = b & 0xF0u; m1h_get(&h->a1); }
    else           d1 = M1R(0x1007u) & 0xF0u;
    if (d1 != 0xC0u && d1 != 0xD0u) m1h_get(&h->a1);
    h->events++;
    return VCH_DISPATCH + cost;
}

/* 0x17C6 / 0x1846: the chip's voices marked for release keyed off */
static uint32_t vch_release(int chip) {
    uint32_t lo = chip ? VCH_V2 : VCH_V1, cost = VCH_REL_SCAN;
    uint8_t *ages = &M1R(chip ? 0x1022u : 0x1021u);
    for (uint32_t v = lo; v < lo + 336u; v += 12u) {
        if (!(M1R(v) & 1u)) continue;
        m1h_vsel(v);
        m1h_vreg(v, 4, 0);
        uint8_t old = M1R(v), k = M1R(v + 3u);
        M1R(v) = 0;
        M1R(v + 3u) = 0;
        if (k != 15u && (old & 0x10u)) { cost += VCH_REL_DRUM; continue; }
        cost += VCH_REL;
        uint8_t age = M1R(v + 8u);
        (*ages)--;
        for (uint32_t j = lo; j < lo + 336u; j += 12u)
            if (M1R(j + 8u) != 0xFFu && M1R(j + 8u) > age) M1R(j + 8u)--;
    }
    return cost;
}

/* ---- the main loop (0x4F2) ---------------------------------------------------------- */

static uint32_t vch_segment(void) {
    m1hle_t *h = &g_shle.m1;
    uint64_t t = h->dc;
    switch (h->seg) {
    case VCH_S_POLL:
        if (m1h_timer_b(t)) return vch_tick(t);
        h->seg = VCH_S_REL1;
        return VCH_POLL;
    case VCH_S_REL1:
        h->seg = VCH_S_REL2;
        return vch_release(0);
    case VCH_S_REL2:
        h->seg = VCH_S_MSG;
        return vch_release(1);
    default:
        h->seg = VCH_S_POLL;
        return vch_dispatch() + 10u;
    }
}

/* sound_hle_m1.h's m1hle_events for this build */
static void vch_events(uint64_t now) {
    m1hle_t *h = &g_shle.m1;
    if (h->phase == M1H_BOOT) vch_boot(now);
    if (h->phase != M1H_RUN) return;
    while (h->dc + (h->seg == VCH_S_MSG && M1R(0x1006u) ? VCH_D_LEAD : 0u) <= now) {
        h->dc += vch_segment();
        if (h->phase != M1H_RUN) return;
    }
    if (g_sound.m1.rxrdy) {
        uint32_t c = vch_irq(now);
        if (h->phase == M1H_RUN) h->dc += c;
    }
}

#endif /* SOUND_HLE_VCOP_H */
