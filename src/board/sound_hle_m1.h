/*
 * sound_hle_m1.h — the Model 1 sound board's driver (Daytona USA's epr-14..,
 * model1_sound.h's board) in C in place of the 68000. The two MultiPCMs and
 * the YM3438, whose timer B paces the driver, stay emulated; this is what the
 * 68000 did with them. sound_hle.h includes it and runs it when the program
 * ROM's code is this build (shle_drv_t.m1); sound.h's sound_run calls
 * m1hle_run for a Model 1 board under the HLE.
 *
 * The driver's RAM (0xF00000-0xF0FFFF, g_sound.ram's low 64 KB):
 *   0xF00000 the MIDI ring, 0x1000 bytes: the UART's interrupt and the
 *            sequencers write whole messages (a0), the main loop reads them (a1)
 *   0xF01000 the message being received: length, left, running status;
 *            0xF01007 messages in the ring, 0xF01008 the status being played
 *   0xF01010 the drum countdown's divider    0xF01035 the note value
 *   0xF01037 the engine's level (CC 0x17)    0xF01050.. the engines' state
 *   0xF01240 a pitch bend word a channel     0xF01260 CC 1 a channel (bit 7: held)
 *   0xF01300 16 channels x 10                0xF01500 56 voices x 10 (28 a chip)
 *   0xF02000 the music's sequencer, then five effects' (32 bytes each)
 * Channel: +0 flags (7 on, 0 fixed level), +1 MIDI channel, +2 volume,
 *   +3 program (>= 0xF0 drums), +4 transpose, +6 LFO, +7 pan (bit 0: chip 2),
 *   +8 level, +9 bend range.
 * Voice: +0 flags (6 on, 4 drum, 3 chip 2, 1 held, 0 to release), +1 slot,
 *   +2 sample, +3 channel, +4 a drum's countdown, +5 velocity level, +6 age,
 *   +7 tune, +8 split transpose, +9 the split's sample bank.
 * Sequencer: +0 flags (7 on, 6 no delta follows, 5 a list), +2 position,
 *   +6 the delta's countdown (ticks), +9 volume, +10 fade step, +11 its
 *   countdown, +13 running status, +16 the list's position.
 *
 * The 68000's main loop is a pass of three parts with a poll of the YM3438
 * between them (timer B: the tick, 0x5F2, the sequencers and the drum
 * countdowns): the engine voices (255A, 262A); every 64 passes the engines'
 * other voices; then the releases (1538, 15B8) and one ring message (8BE).
 * The port runs the same parts at the board's times (M1H_*): a part's
 * effects land when it starts, and what it did decides when the next one
 * starts. The interrupt (a byte off the UART) is taken at the sample and
 * delays the part under way. Measured on MAME's board (snd_replay traces).
 */
#ifndef SOUND_HLE_M1_H
#define SOUND_HLE_M1_H

/* the init (0x300), from its start: the RAM clear, then the chips */
#define M1H_INIT_PCM1     721080u     /* chip 1's 28 slots cut */
#define M1H_INIT_PCM2     739972u     /* chip 2's */
#define M1H_INIT_YM       758884u     /* the YM3438's tables (0x860, 0x876) */
#define M1H_INIT_RAM      787288u     /* the voices, the channels, the banks */
#define M1H_INIT_TIMER    797248u     /* YM 0x27 = 0x2A: timer B runs */
#define M1H_INIT_UART     798150u     /* 0x6AC: the receiver on */
#define M1H_INIT_LOOP     798216u     /* the main loop's first poll */
#define M1H_RESTART_LAT   60u         /* a 0xFF's interrupt -> the init */

/* the main loop's parts, idle: poll A + the engine voices, poll B + the
 * pass counter, poll C + the releases and the ring (to the next poll A) */
#define M1H_SEG_A         736u
#define M1H_SEG_B         74u
#define M1H_SEG_B_BLOCK   120u        /* ...to the block's middle poll */
#define M1H_SEG_MID       220u        /* ...the middle poll + 2444, 26FA */
#define M1H_SEG_C         3080u       /* poll C + the releases' scans */
#define M1H_SEG_D         56u         /* the ring empty, back to poll A */
#define M1H_D_LEAD        4700u       /* a message's key-on, from the ring's read */
#define M1H_ENGINES       1500u       /* the block's six engine routines, the level changed */
/* the tick (0x5F2): its base, the drum countdowns' pass, a sequencer event */
#define M1H_TICK          864u
#define M1H_TICK_DRUMS    5626u
#define M1H_TICK_EVENT    450u
/* a ring message (8BE), over the empty pass's: the walk over the channels,
 * then each channel's handler, its loops over the chip's 28 voices apart */
#define M1H_DISPATCH      900u
#define M1H_NOTE_ON       3988u
#define M1H_NOTE_OFF      1930u       /* + the voice loop */
#define M1H_PROGRAM       1077u       /* + the voice loop */
#define M1H_CC            1436u       /* + its loops */
#define M1H_START         2274u
#define M1H_VOICE         62u         /* a voice the loop passes over */
#define M1H_VOICE_HIT     260u        /* ...one whose registers it writes */
#define M1H_CHANNEL       260u        /* CC 0x10, 0x20: a channel's level */
/* the releases: a voice keyed off, a drum's */
#define M1H_REL           2490u
#define M1H_REL_DRUM      458u
/* the interrupt: a byte, the byte that completes a message */
#define M1H_IRQ_BYTE      148u
#define M1H_IRQ_MSG       226u

enum { M1H_OFF, M1H_BOOT, M1H_RUN };
enum { M1H_S_A, M1H_S_B, M1H_S_MID, M1H_S_C, M1H_S_D };

/* ---- the bus ---------------------------------------------------------------------- */

#define M1R(a)  g_sound.ram[(a) & 0xFFFFu]

static inline uint8_t m1h_rb(uint32_t a) {
    a &= 0xFFFFFFu;
    if (a < 0x40000u) return g_sound.rom[a];
    if (a >= 0x80000u && a < 0xA0000u) return g_sound.rom[a - 0x60000u];
    if (a >= 0xF00000u && a < 0xF10000u) return M1R(a);
    return 0;
}
static inline uint16_t m1h_rw(uint32_t a) { return (uint16_t)(m1h_rb(a) << 8 | m1h_rb(a + 1u)); }
static inline uint32_t m1h_rl(uint32_t a) { return (uint32_t)m1h_rw(a) << 16 | m1h_rw(a + 2u); }
static inline void m1h_ww(uint32_t a, uint16_t v) { M1R(a) = (uint8_t)(v >> 8); M1R(a + 1u) = (uint8_t)v; }
static inline void m1h_wl(uint32_t a, uint32_t v) { m1h_ww(a, (uint16_t)(v >> 16)); m1h_ww(a + 2u, (uint16_t)v); }

/* MultiPCM `chip`: a slot selected (reg 1), a register written (reg 2, then
 * the data, reg 0): 0x1652 and 0x73E; a voice's chip is its +0 bit 3. */
static inline void m1h_sel(int chip, uint8_t slot) { sound_m1_pcm_write(&g_sound, chip, 1, slot); }
static inline void m1h_reg(int chip, uint8_t r, uint8_t v) {
    sound_m1_pcm_write(&g_sound, chip, 2, r);
    sound_m1_pcm_write(&g_sound, chip, 0, v);
}
static inline int  m1h_vchip(uint32_t v) { return (M1R(v) >> 3) & 1; }
static inline void m1h_vsel(uint32_t v) { m1h_sel(m1h_vchip(v), M1R(v + 1u)); }
static inline void m1h_vreg(uint32_t v, uint8_t r, uint8_t d) { m1h_reg(m1h_vchip(v), r, d); }

static inline void m1h_bank(int chip, uint8_t v) {
    g_sound.m1.pcm[chip].bank = v & 3u;
    if (sndcap_on()) sndcap_put(7, (uint32_t)chip, v & 3u, 0);
}

/* The YM3438 at the driver's clock t: a register written (0x7B0, 0x7D6) */
static inline void m1h_ym(uint64_t t, int bank, uint8_t addr, uint8_t v) {
    m1ym_t *y = &g_sound.m1.ym;
    m1ym_advance(y, t * 4u / 5u);
    m1ym_write(y, bank ? 2u : 0u, addr);
    if (sndcap_on()) sndcap_put(6, y->addr, v, 0);
    m1ym_write(y, bank ? 3u : 1u, v);
}

/* ...and the poll: timer B's flag */
static inline bool m1h_timer_b(uint64_t t) {
    m1ym_t *y = &g_sound.m1.ym;
    m1ym_advance(y, t * 4u / 5u);
    return (m1ym_read(y, 0) & 2u) != 0;
}

/* the ring: a byte in (0x2BB2, a0) and out (0x1638, a1) */
static inline void m1h_put(uint8_t b) {
    M1R(g_shle.m1.a0) = b;
    g_shle.m1.a0 = (g_shle.m1.a0 + 1u) & 0xFFFu;
}
static inline uint8_t m1h_get(uint32_t *a1) {
    uint8_t b = M1R(*a1);
    *a1 = (*a1 + 1u) & 0xFFFu;
    return b;
}

/* the clock periods the handler under way has taken in its loops */
static uint32_t s_m1h_work;
#define M1H_LOOP(n, hits) (s_m1h_work += (uint32_t)(n) * M1H_VOICE + (uint32_t)(hits) * M1H_VOICE_HIT)

/* ---- the init (0x300) ------------------------------------------------------------- */

static void m1h_init_pcm(int chip) {
    uint32_t ids = chip ? 0x83Eu : 0x822u;
    m1h_sel(chip, 3);    /* move.b #3,$C40013 (chip 2: $C60013) */
    for (uint32_t i = 0; i < 28; i++) {
        m1h_sel(chip, m1h_rb(ids + i));
        m1h_reg(chip, 0, 0x80);
        m1h_reg(chip, 4, 0x00);
        m1h_reg(chip, 5, 0xFE);
    }
}

static void m1h_init_ym(uint64_t t) {
    for (uint32_t i = 0; i < 11; i++) m1h_ym(t, 0, m1h_rb(0x860u + 2 * i), m1h_rb(0x861u + 2 * i));
    for (uint32_t i = 0; i < 36; i++) {
        uint8_t a = m1h_rb(0x876u + 2 * i), d = m1h_rb(0x877u + 2 * i);
        m1h_ym(t, 0, a, d);
        m1h_ym(t, 1, a, d);
    }
}

/* a channel's level: its volume, by the master's unless it is fixed (+0 bit 0) */
static inline uint8_t m1h_level(uint32_t c) {
    unsigned m = (M1R(c) & 1u) ? 255u : M1R(0x101Eu);
    unsigned l = (M1R(c + 2u) * m) >> 8 & 0xFFu;
    return (uint8_t)(l ? l + 1u : 0u);
}

static void m1h_init_ram(void) {
    for (uint32_t i = 0; i < 28; i++) {
        M1R(0x1501u + 10u * i) = m1h_rb(0x822u + i);
        M1R(0x1619u + 10u * i) = m1h_rb(0x83Eu + i);
    }
    uint32_t p = m1h_rl(0x50A2u);
    for (uint32_t i = 0; i < 100; i++) M1R(0x1300u + i) = m1h_rb(p + i);
    m1h_bank(0, m1h_rb(p + 100u));
    m1h_bank(1, m1h_rb(p + 100u));
    p = m1h_rl(0x50A6u);
    for (uint32_t i = 0; i < 60; i++) M1R(0x1364u + i) = m1h_rb(p + i);
    M1R(0x101Eu) = 0xFF;
    for (uint32_t i = 0; i < 16; i++) M1R(0x1308u + 10u * i) = m1h_level(0x1300u + 10u * i);
    m1h_wl(0x102Au, 0xF01500u);
    m1h_wl(0x102Eu, 0xF01618u);
    m1h_wl(0x105Cu, 0x40u);
    /* chip 2's voices 18-27 belong to the engines: never allocated */
    for (uint32_t v = 0x16CCu; v < 0x1730u; v += 10u) {
        M1R(v) |= 0x40u;
        M1R(v + 3u) = 10;
        M1R(v + 6u) = 0xFF;
    }
}

/* the init from its start at t: what it clears at once, the rest by stage */
static void m1hle_restart(uint64_t t) {
    m1hle_t *h = &g_shle.m1;
    memset(g_sound.ram, 0, 0x10000u);
    h->phase = M1H_BOOT;
    h->stage = 0;
    h->t0 = t;
    h->a0 = h->a1 = 0;
    h->seg = M1H_S_A;
}

static void m1h_boot(uint64_t now) {
    m1hle_t *h = &g_shle.m1;
    static const uint32_t at[] = { M1H_INIT_PCM1, M1H_INIT_PCM2, M1H_INIT_YM, M1H_INIT_RAM,
                                   M1H_INIT_TIMER, M1H_INIT_UART, M1H_INIT_LOOP };
    while (h->phase == M1H_BOOT && now >= h->t0 + at[h->stage]) {
        uint64_t t = h->t0 + at[h->stage];
        switch (h->stage++) {
        case 0: m1h_init_pcm(0); break;
        case 1: m1h_init_pcm(1); break;
        case 2: m1h_init_ym(t); break;
        case 3: m1h_init_ram(); break;
        case 4: m1h_ym(t, 0, 0x27, 0x2A); M1R(0x103Cu) = 1; break;
        case 5: {
            static const uint8_t u[] = { 0, 0, 0, 0x40, 0x4E, 0x37 };
            for (size_t i = 0; i < sizeof u; i++) m1snd_uart_write(&g_sound.m1, 1, u[i]);
            break;
        }
        default:
            h->phase = M1H_RUN;
            h->dc = t;
            h->seg = M1H_S_A;
        }
    }
}

/* ---- the interrupt (0x120): a byte off the UART ------------------------------------ */

/* Answers the clock periods it takes; a 0xFF restarts the driver. */
static uint32_t m1h_irq(uint64_t now) {
    m1hle_t *h = &g_shle.m1;
    uint8_t b = m1snd_uart_read(&g_sound.m1, 0);
    h->irqs++;
    if (b == 0xFF) { m1hle_restart(now + M1H_RESTART_LAT); return 0; }
    if (b == 0xF8) return M1H_IRQ_BYTE;
    uint32_t a2 = m1h_rl(0xF01060u);
    uint8_t d4;
    if ((b & 0x80u) || M1R(0x1001u) == 0) {
        uint8_t led = (uint8_t)(M1R(0x103Cu) << 1);   /* 0x7FC: the LED walks */
        M1R(0x103Cu) = led == 0x10 ? 1 : led;
        a2 = 0xF01064u;
        if (!(b & 0x80u)) {                            /* running status: put it back in front */
            d4 = (uint8_t)(M1R(0x1000u) - 1u);
            M1R(0x1001u) = d4;
            M1R(a2++) = M1R(0x1002u);
        } else {
            M1R(0x1002u) = b;
            d4 = b >= 0xC0u && b < 0xE0u ? 2 : 3;
            M1R(0x1000u) = d4;
        }
    } else {
        d4 = M1R(0x1001u);
    }
    M1R(a2++) = b;
    m1h_wl(0x1060u, a2);
    d4--;
    M1R(0x1001u) = d4;
    if (d4) return M1H_IRQ_BYTE;
    uint32_t n = M1R(0x1000u) ? M1R(0x1000u) : 256u;
    for (uint32_t i = 0; i < n; i++) m1h_put(M1R(0x1064u + i));
    M1R(0x1007u)++;
    return M1H_IRQ_MSG;
}

/* ---- the sequencers (0x27EE) -------------------------------------------------------- */

/* 0x2A58: the music's fade */
static uint32_t m1h_fade(uint32_t s) {
    if (!M1R(s + 10u) || --M1R(s + 11u)) return 0;
    M1R(s + 9u) -= 4u;
    if (M1R(s + 9u) & 0x80u) {
        M1R(s) &= 0x7Fu;
        M1R(s + 10u) = 0;
        m1h_put(0xBE); m1h_put(0x50); m1h_put(0x7E);
    } else {
        m1h_put(0xBE); m1h_put(0x10); m1h_put(M1R(s + 9u));
        M1R(s + 11u) = M1R(s + 10u);
    }
    M1R(0x1007u)++;
    return M1H_TICK_EVENT;
}

/* the event's last data byte: bit 7 says no delta follows */
static inline uint8_t m1h_last(uint32_t s, uint8_t d) {
    if (d & 0x80u) M1R(s) |= 0x40u;
    return d & 0x7Fu;
}

/* 0x283E: a sequencer's tick */
static uint32_t m1h_seq(uint32_t s) {
    if (!(M1R(s) & 0x80u)) return 0;
    uint16_t cd = (uint16_t)(m1h_rw(0xF00000u + s + 6u) - 1u);
    m1h_ww(s + 6u, cd);
    if (cd) return 0;
    uint32_t p = m1h_rl(0xF00000u + s + 2u), cost = 0;
    for (int guard = 0; guard < 4096; guard++) {
        M1R(s) &= ~0x40u;
        uint8_t st = m1h_rb(p++);
        if (!(st & 0x80u)) { st = M1R(s + 13u); p--; }
        M1R(s + 13u) = st;
        bool end = false;
        switch (st & 0xF0u) {
        case 0xB0: {
            m1h_put(st);
            uint8_t ctl = m1h_rb(p++), v;
            m1h_put(ctl);
            if (ctl == 0x10) {
                v = m1h_rb(p++);
                if (M1R(s + 10u)) v = M1R(s + 9u);
                else              M1R(s + 9u) = v;
            } else {
                v = m1h_rb(p++);
            }
            m1h_put(m1h_last(s, v));
            break;
        }
        case 0x80:
            m1h_put(st);
            m1h_put(m1h_last(s, m1h_rb(p++)));
            m1h_put(0x7F);
            break;
        case 0xC0: case 0xD0:
            m1h_put(st);
            m1h_put(m1h_last(s, m1h_rb(p++)));
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
            m1h_put(st);
            m1h_put(m1h_rb(p++));
            m1h_put(m1h_last(s, m1h_rb(p++)));
            break;
        }
        if (end) {                                     /* 0x2A12 */
            if (!(M1R(s) & 0x20u)) { M1R(s) = 0; return cost; }
            uint32_t l = m1h_rl(0xF00000u + s + 16u), d = m1h_rl(l);
            if (d == 0xFFFFFFFFu) { M1R(s) = 0; return cost; }
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
        M1R(0x1007u)++;
        cost += M1H_TICK_EVENT;
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

/* 0x2AF2: a sequence started (A? b1 b2), on the ring's bytes after a1 */
static void m1h_start(uint32_t *a1) {
    uint32_t t = m1h_rl(0x2BC6u + 4u * (m1h_get(a1) & 0x7Fu));
    uint8_t n = m1h_get(a1);
    if ((int16_t)n > (int16_t)m1h_rw(t)) return;
    uint32_t a5 = m1h_rl(t + 2u + 4u * n);
    if (m1h_rb(a5) & 0x80u) {
        uint32_t s = 0x2020u + 32u * (uint8_t)((m1h_rb(a5 + 1u) & 15u) - 10u);
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

/* ---- the tick (0x5F2) --------------------------------------------------------------- */

static uint32_t m1h_tick(uint64_t t) {
    uint32_t cost = M1H_TICK;
    m1h_ym(t, 0, 0x27, 0x2A);
    /* 0x608: every 36 ticks the drums' countdowns, all 56 voices in one */
    if (M1R(0x1017u) & 1u) {
        for (uint32_t v = 0x1500u; v < 0x1730u; v += 10u) {
            if (--M1R(v + 4u)) continue;
            uint8_t f = M1R(v);
            if (!(f & 0x81u) && (f & 0x70u) >= 0x50u) M1R(v) |= 1u;
        }
        M1R(0x1017u) &= ~1u;
        m1h_wl(0x1018u, 0xF01734u);
        cost += M1H_TICK_DRUMS;
    } else if (--M1R(0x1010u) == 0) {
        M1R(0x1010u) = 0x23;
        m1h_wl(0x1018u, 0xF01504u);
        M1R(0x101Cu) = 0x37;
        M1R(0x1017u) = 1;
    }
    /* 0x27EE: the sequencers, while the ring has room */
    if (M1R(0x1007u) < 0xFFu) {
        cost += m1h_fade(0x2000u);
        for (uint32_t s = 0x2000u; s <= 0x20A0u; s += 0x20u) cost += m1h_seq(s);
    }
    return cost;
}

/* ---- a ring message (0x8BE, 0x968) -------------------------------------------------- */

static inline uint32_t m1h_voices(uint32_t c) { return (M1R(c + 7u) & 1u) ? 0x1618u : 0x1500u; }

/* the melodic program's split for the note F01035: 0x5E16 */
static uint32_t m1h_split(uint32_t c) {
    uint32_t a5 = m1h_rl(0x5E16u + 4u * (M1R(c + 3u) & 0x7Fu)) + 1u;
    while (M1R(0x1035u) >= m1h_rb(a5)) a5 += 4u;
    return a5;
}

/* a voice's pitch: its sample's table at the note plus `d` (0xB86, 0xEFC) */
static void m1h_pitch(uint32_t v, int32_t d) {
    uint32_t fp = m1h_rl(0x5282u + 4u * (M1R(v + 2u) & 15u)) + (uint32_t)(2 * d);
    m1h_vreg(v, 3, (uint8_t)((M1R(v + 2u) & 0xF0u) + m1h_rb(fp)));
    m1h_vreg(v, 2, m1h_rb(fp + 1u));
}

/* 0x1688 / 0x1714: a free voice on the chip, or the oldest */
static uint32_t m1h_alloc(int chip) {
    uint32_t lo = chip ? 0x1618u : 0x1500u, hi = lo + 280u, rr = chip ? 0x102Eu : 0x102Au;
    uint32_t v = m1h_rl(0xF00000u + rr) & 0xFFFFu;
    for (int i = 0; i < 28; i++) {
        if (v == hi) { v = lo; m1h_wl(rr, 0xF00000u + v); }
        if (M1R(v) == 0) { m1h_wl(rr, 0xF00000u + v + 10u); return v; }
        v += 10u;
    }
    for (uint32_t i = 0; i < 28; i++) {
        v = lo + 10u * i;
        if (M1R(v + 6u)) continue;
        M1R(v + 6u) = 28;
        /* chip 1's loop reuses the search's counter: the first 28 - i ages */
        uint32_t n = chip ? 28u : 28u - i;
        for (uint32_t k = 0; k < n; k++) M1R(lo + 10u * k + 6u)--;
        M1R(chip ? 0x103Du : 0x1021u)--;
        return v;
    }
    return 0;
}

/* 0x9A4: note on; d2 the note, or the status */
static void m1h_note_on(uint32_t c, uint8_t d2, uint32_t *a1);
static void m1h_note_off_at(uint32_t c);

static void m1h_note_on(uint32_t c, uint8_t d2, uint32_t *a1) {
    if (d2 & 0x80u) d2 = m1h_get(a1);
    M1R(0x1035u) = (uint8_t)(d2 + M1R(c + 4u));
    uint8_t vel = m1h_get(a1);
    if (!vel) { m1h_note_off_at(c); return; }
    /* 0xC00001 bit 0 (open on this board: reads 0) would pick 0x5D96 */
    M1R(0x1013u) = m1h_rb(0x5D16u + vel);
    uint8_t prog = M1R(c + 3u), d1 = 0;
    if (prog >= 0xF0u) {
        uint32_t e = m1h_rl(0x64A2u + 4u * ((uint8_t)(prog - 0xF0u) & 0x7Fu)) + 4u * (d2 & 0x7Fu);
        d1 = m1h_rb(e);
        if (d1 == 0x80u) d1 = M1R(c + 7u);
        M1R(0x103Bu) = d1 & 1u;
        M1R(0x1016u) = d1 & 0xFEu;
        M1R(0x101Fu) = m1h_rb(e + 1u);
        d1 = m1h_rb(e + 2u);
        M1R(0x100Du) = m1h_rb(e + 3u);
        M1R(0x1012u) = m1h_rb(0x5C62u + (M1R(0x1035u) & 0x7Fu));
        if (m1h_rb(e) == 0xFFu) return;
        /* (channel 22's hi-hat choke, 0xA84: no MIDI channel is 22) */
    } else {
        uint32_t a5 = m1h_split(c);
        M1R(0x100Du) = m1h_rb(a5 + 1u);
        M1R(0x1033u) = m1h_rb(a5 + 2u);
        M1R(0x1034u) = m1h_rb(a5 + 3u);
        M1R(0x1012u) = m1h_rb(0x5C62u + ((uint8_t)(M1R(0x1035u) + m1h_rb(a5 + 3u)) & 0x7Fu));
        M1R(0x103Bu) = M1R(c + 7u) & 1u;
        M1R(0x1016u) = M1R(c + 7u) & 0xFEu;
    }
    int chip = M1R(0x103Bu) & 1;
    uint32_t v = m1h_alloc(chip);
    if (!v) return;
    /* 0xB86 */
    uint8_t ch = M1R(0x1008u) & 15u, r3, r2;
    M1R(v) = (uint8_t)(0x40u | (chip ? 8u : 0u));
    if (prog >= 0xF0u) {
        M1R(v) |= 0x10u;
        m1h_vsel(v);
        M1R(v + 2u) = M1R(0x1012u);
        M1R(v + 3u) = ch;
        M1R(v + 4u) = d1;
        M1R(v + 5u) = M1R(0x1013u);
        M1R(v + 6u) = 0xF0;
        r3 = M1R(0x101Fu);
        r2 = 0;
        m1h_vreg(v, 3, r3);
        m1h_vreg(v, 2, r2);
    } else {
        m1h_vsel(v);
        M1R(v + 2u) = M1R(0x1012u);
        M1R(v + 3u) = ch;
        M1R(v + 5u) = M1R(0x1013u);
        M1R(v + 6u) = M1R(chip ? 0x103Du : 0x1021u)++;
        M1R(v + 7u) = M1R(0x1033u);
        M1R(v + 8u) = M1R(0x1034u);
        m1h_pitch(v, (int16_t)((int16_t)m1h_rw(0xF01240u + 2u * ch) + (int8_t)M1R(0x1033u)));
    }
    M1R(v + 9u) = M1R(0x100Du);
    m1h_vreg(v, 1, M1R(0x100Du));
    m1h_vreg(v, 5, (uint8_t)(~((M1R(0x1013u) * M1R(c + 8u)) >> 8) | 1u));
    m1h_vreg(v, 0, M1R(0x1016u));
    m1h_vreg(v, 4, 0x80);
    g_shle.keyons++;
}

/* 0xD4C: the note F01035 off */
static void m1h_note_off_at(uint32_t c) {
    if (M1R(c + 3u) >= 0xF0u) return;
    uint32_t a5 = m1h_split(c);
    M1R(0x100Du) = m1h_rb(a5 + 1u);
    M1R(0x1012u) = m1h_rb(0x5C62u + ((uint8_t)(M1R(0x1035u) + m1h_rb(a5 + 3u)) & 0x7Fu));
    uint8_t ch = M1R(0x1008u) & 15u;
    uint32_t lo = m1h_voices(c);
    M1H_LOOP(28, 0);
    for (uint32_t v = lo; v < lo + 280u; v += 10u) {
        if (M1R(v + 2u) != M1R(0x1012u) || M1R(v + 3u) != ch || M1R(v + 9u) != M1R(0x100Du) || (M1R(v) & 2u))
            continue;
        if (!(M1R(v) &= 0x7Fu)) continue;
        M1R(v) |= (M1R(0x1260u + ch) & 0x80u) ? 2u : 1u;   /* 0xE16: held, or released */
    }
}

/* 0x14EE: every voice of the channel released */
static void m1h_all_off(uint32_t c) {
    uint8_t ch = M1R(0x1008u) & 15u;
    uint32_t lo = m1h_voices(c);
    M1H_LOOP(28, 0);
    for (uint32_t v = lo; v < lo + 280u; v += 10u)
        if ((M1R(v) & 0x40u) && M1R(v + 3u) == ch) M1R(v) |= 1u;
}

/* the voice level, CC 7 and 0x10 (0x12F8, 0x1246) */
static inline void m1h_vlevel(uint32_t v, uint32_t c) {
    s_m1h_work += M1H_VOICE_HIT;
    m1h_vsel(v);
    m1h_vreg(v, 5, (uint8_t)(~((M1R(v + 5u) * M1R(c + 8u)) >> 8) & 0xFEu));
}

static void m1h_cc(uint32_t c, uint8_t d2, uint32_t *a1) {
    if (d2 & 0x80u) d2 = m1h_get(a1);
    uint8_t d0 = m1h_get(a1), ch = M1R(0x1008u) & 15u;
    uint32_t lo = m1h_voices(c);
    switch (d2) {
    case 0x17: M1R(0x1037u) = d0; break;
    case 0x01: {                                       /* LFO depth */
        uint8_t m = (d0 >> 4) & 7u;
        M1R(0x101Du) = m;
        uint8_t o = M1R(0x1260u + ch);
        if ((o & 0x7Fu) == m) break;
        M1R(0x1260u + ch) = (uint8_t)((o & 0x80u) | m);
        M1H_LOOP(28, 0);
        for (uint32_t v = lo; v < lo + 280u; v += 10u)
            if (M1R(v + 3u) == ch && (M1R(v) & 0x7Fu)) {
                M1H_LOOP(0, 1);
                m1h_vsel(v);
                m1h_vreg(v, 6, (uint8_t)(M1R(0x101Du) | M1R(c + 6u)));
            }
        break;
    }
    case 0x07: {
        uint8_t x = (uint8_t)(d0 << 1);
        M1R(c + 2u) = x ? x + 1u : 0;
        M1R(c + 8u) = m1h_level(c);
        M1H_LOOP(28, 0);
        for (uint32_t v = lo; v < lo + 280u; v += 10u)
            if ((M1R(v) & 0x40u) && M1R(v + 3u) == ch) m1h_vlevel(v, c);
        break;
    }
    case 0x10: {                                       /* master volume: the first 10 channels */
        uint8_t x = (uint8_t)(d0 << 1);
        M1R(0x101Eu) = x ? x + 1u : 0;
        for (uint32_t k = 0x1300u; k < 0x1364u; k += 10u) {
            M1R(k + 8u) = m1h_level(k);
            s_m1h_work += M1H_CHANNEL;
            M1H_LOOP(28, 0);
            uint32_t l2 = m1h_voices(k);
            for (uint32_t v = l2; v < l2 + 280u; v += 10u)
                if (M1R(k + 1u) == M1R(v + 3u) && (M1R(v) & 0x40u)) m1h_vlevel(v, k);
        }
        break;
    }
    case 0x40:                                         /* the pedal: below 0x40 holds (inverted) */
        if (d0 < 0x40u) { M1R(0x1260u + ch) |= 0x80u; break; }
        M1H_LOOP(28, 0);
        for (uint32_t v = lo; v < lo + 280u; v += 10u)
            if (M1R(v + 3u) == ch && (M1R(v) & 2u)) M1R(v) |= 1u;
        M1R(0x1260u + ch) &= 0x7Fu;
        break;
    case 0x7B: m1h_all_off(c); break;
    case 0x50: {                                       /* everything off: 7D the effects, 7E the music */
        uint32_t n = d0 == 0x7Du ? 46u : 56u;
        M1H_LOOP(n, 0);
        s_m1h_work += 32u * 22u;                       /* the two 16-byte clears */
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = 0x1500u + 10u * i;
            if (!(M1R(v) & 0x40u)) continue;
            uint8_t k = M1R(v + 3u);
            bool music = k <= 9u || k == 15u;
            if (d0 == 0x7Du ? !music : d0 == 0x7Eu ? music : true) M1R(v) |= 1u;
        }
        memset(&M1R(0x1260u), 0, 16);
        memset(&M1R(0x1240u), 0, 16);
        break;
    }
    case 0x20: {                                       /* a channel set */
        uint32_t p = m1h_rl(0x50A2u + 4u * d0);
        for (uint32_t i = 0; i < 100; i++) M1R(0x1300u + i) = m1h_rb(p + i);
        m1h_bank(0, m1h_rb(p + 100u));
        for (uint32_t k = 0x1300u; k < 0x1364u; k += 10u) M1R(k + 8u) = m1h_level(k);
        s_m1h_work += 100u * 22u + 10u * 120u;          /* the copy, the levels */
        break;
    }
    case 0x21: m1h_bank(M1R(c + 7u) & 1u, d0); break;
    case 0x16: M1R(0x1038u) = d0; break;
    case 0x18: M1R(0x103Fu) = d0; break;
    case 0x19: M1R(0x103Eu) = d0; break;
    case 0x1A: M1R(0x200Au) = d0; M1R(0x200Bu) = d0; break;   /* the music's fade */
    case 0x1B: M1R(0x1042u) = d0; break;
    case 0x1C: M1R(0x1045u) = d0; break;
    case 0x1D: M1R(0x1048u) = d0; break;
    case 0x1E: M1R(0x104Du) = d0; break;
    case 0x1F: M1R(0x1053u) = d0; break;
    case 0x34: M1R(0x1052u) = d0; break;
    case 0x35: M1R(0x1059u) = d0; break;
    case 0x36: M1R(0x1071u) = d0; break;
    case 0x37: M1R(0x1070u) = d0; break;
    case 0x0A: {                                       /* pan */
        uint8_t x = d0 >> 3;
        x = x >= 8u ? x & ~8u : x ? x | 8u : 9u;
        x = (uint8_t)(x << 4 | (M1R(c + 7u) & 1u));
        M1R(c + 7u) = x;
        lo = m1h_voices(c);
        M1H_LOOP(28, 0);
        for (uint32_t v = lo; v < lo + 280u; v += 10u)
            if (M1R(v + 3u) == ch && (M1R(v) & 0x7Fu)) { M1H_LOOP(0, 1); m1h_vsel(v); m1h_vreg(v, 0, x); }
        break;
    }
    default: break;
    }
}

/* 0xE36: pitch bend */
static void m1h_bend(uint32_t c, uint8_t d2, uint32_t *a1) {
    if (d2 & 0x80u) d2 = m1h_get(a1);
    uint8_t msb = m1h_get(a1);
    uint16_t v = (uint16_t)(((msb & 0x7Fu) << 7 | (d2 & 0x7Fu)) >> 5);
    uint8_t i = (uint8_t)v;
    if (!(v & 0x100u)) { i = (uint8_t)-i; if (!i) i = 0xFF; }
    uint8_t t = m1h_rb(m1h_rl(0x5732u + 4u * (M1R(c + 9u) & 0x7Fu)) + i);
    int16_t bend = (v & 0x100u) ? (int16_t)t : (int16_t)(int8_t)(uint8_t)-t;
    m1h_ww(0x1014u, (uint16_t)bend);
    uint8_t ch = M1R(0x1008u) & 15u;
    m1h_ww(0x1240u + 2u * ch, (uint16_t)bend);
    uint32_t lo = m1h_voices(c);
    M1H_LOOP(28, 0);
    for (uint32_t k = lo; k < lo + 280u; k += 10u)
        if (M1R(k + 3u) == ch && (M1R(k) & 0x7Fu)) {
            M1H_LOOP(0, 2);
            m1h_vsel(k);
            m1h_pitch(k, (int16_t)(bend + (int8_t)M1R(k + 7u)));
        }
}

/* 0x968: the message for channel c; d2 its first byte */
static uint32_t m1h_handle(uint32_t c, uint8_t d2, uint32_t *a1) {
    switch (M1R(0x1008u) & 0xF0u) {
    case 0x90: m1h_note_on(c, d2, a1); return M1H_NOTE_ON;
    case 0x80:
        if (d2 & 0x80u) d2 = m1h_get(a1);
        M1R(0x1035u) = (uint8_t)((d2 & 0x7Fu) + M1R(c + 4u));
        m1h_get(a1);
        m1h_note_off_at(c);
        return M1H_NOTE_OFF;
    case 0xE0: m1h_bend(c, d2, a1); return M1H_CC;
    case 0xB0: m1h_cc(c, d2, a1); return M1H_CC;
    case 0xC0:
        if (d2 & 0x80u) d2 = m1h_get(a1);
        M1R(c + 3u) = d2;
        m1h_all_off(c);
        return M1H_PROGRAM;
    case 0xA0: m1h_start(a1); return M1H_START;
    default: return 0;
    }
}

/* 0x8BE: one message off the ring, to every channel on its MIDI channel */
static uint32_t m1h_dispatch(void) {
    m1hle_t *h = &g_shle.m1;
    if (!M1R(0x1007u)) return 0;
    M1R(0x1007u)--;
    uint8_t b = m1h_get(&h->a1), d0 = b;
    uint32_t after = h->a1, cost = 0;
    m1h_wl(0x106Cu, 0xF00000u + after);
    if (b & 0x80u) M1R(0x1008u) = b;
    else           d0 = M1R(0x1008u);
    d0 &= 15u;
    for (uint32_t c = 0x1300u; c < 0x13A0u; c += 10u) {
        if (!(M1R(c) & 0x80u) || M1R(c + 1u) != d0) continue;
        uint32_t a1 = after;
        s_m1h_work = 0;
        cost += m1h_handle(c, b, &a1) + s_m1h_work;
        m1h_wl(0x1068u, 0xF00000u + a1);
    }
    uint8_t d1;
    if (b & 0x80u) { d1 = b & 0xF0u; m1h_get(&h->a1); }
    else           d1 = M1R(0x1008u) & 0xF0u;
    if (d1 != 0xC0u && d1 != 0xD0u) m1h_get(&h->a1);
    h->events++;
    return M1H_DISPATCH + cost;
}

/* 0x1538 / 0x15B8: the chip's voices marked for release keyed off */
static uint32_t m1h_release(int chip) {
    uint32_t lo = chip ? 0x1618u : 0x1500u, cost = 0;
    for (uint32_t v = lo; v < lo + 280u; v += 10u) {
        if (!(M1R(v) & 1u)) continue;
        m1h_vsel(v);
        m1h_vreg(v, 4, 0);
        uint8_t old = M1R(v);
        M1R(v) = 0;
        M1R(v + 3u) = 0;     /* (+3 == 22, the choke's channel, never is) */
        if (old & 0x10u) { cost += M1H_REL_DRUM; continue; }
        cost += M1H_REL;
        uint8_t age = M1R(v + 6u);
        M1R(chip ? 0x103Du : 0x1021u)--;
        for (uint32_t k = lo; k < lo + 280u; k += 10u)
            if (M1R(k + 6u) != 0xFFu && M1R(k + 6u) > age) M1R(k + 6u)--;
    }
    return cost;
}

/* ---- the engines -------------------------------------------------------------------- */

/* An engine voice on chip 2's raw slot: off at 0, else keyed on once with
 * its sample and kept at the pitch and level of F01050 (0x17A0..0x2210). */
typedef struct { uint8_t slot; uint16_t flag, smp, pitch, level; uint8_t pan; } m1h_engine_t;

static void m1h_engine_update(const m1h_engine_t *e) {
    uint16_t i = m1h_rw(0xF01050u), w = m1h_rw(e->pitch + i);
    m1h_reg(1, 3, (uint8_t)(w >> 8));
    m1h_reg(1, 2, (uint8_t)w);
    m1h_reg(1, 5, m1h_rb(e->level + (i >> 1)));
}

static void m1h_engine(const m1h_engine_t *e, uint8_t val, bool first) {
    m1h_sel(1, e->slot);
    if (!val) { M1R(e->flag) &= 0x7Fu; m1h_reg(1, 4, 0); return; }
    if (first) m1h_ww(0x1050u, (uint16_t)((val & 0x7Fu) << 1));   /* 0x17FA sets the index */
    if (M1R(e->flag) & 0x80u) { m1h_engine_update(e); return; }
    M1R(e->flag) |= 0x80u;
    m1h_engine_update(e);
    m1h_reg(1, 1, M1R(e->smp));
    m1h_reg(1, 0, 0);
    m1h_reg(1, 4, 0x80);
}

static const m1h_engine_t k_m1h_engines[6] = {
    { 0x1C, 0x1039, 0x1038, 0x18D4, 0x1854, 0 },
    { 0x1B, 0x1043, 0x1042, 0x18D4, 0x1A86, 0 },
    { 0x1A, 0x1046, 0x1045, 0x1D38, 0x1CB8, 0 },
    { 0x1E, 0x1049, 0x1048, 0x1F6C, 0x1EEC, 0 },
    { 0x1D, 0x104B, 0x1048, 0x2120, 0x1EEC, 0 },
    { 0x19, 0x104E, 0x104D, 0x2344, 0x22C4, 0 },
};

/* 0x255A / 0x262A: the two engine voices that run every pass, their level
 * their own (F01052, F01059; 0 is full) */
static void m1h_engine_lv(uint8_t slot, uint16_t flag, uint16_t lv, uint16_t lvsave, uint8_t pan) {
    uint8_t val = M1R(0x1037u);
    m1h_sel(1, slot);
    if (!val) { M1R(flag) &= 0x7Fu; m1h_reg(1, 4, 0); return; }
    bool on = M1R(flag) & 0x80u;
    M1R(flag) |= 0x80u;
    uint16_t w = m1h_rw(0x18D4u + m1h_rw(0xF01050u));
    m1h_reg(1, 3, (uint8_t)(w >> 8));
    m1h_reg(1, 2, (uint8_t)w);
    uint8_t l = M1R(lv);
    M1R(lvsave) = l;
    m1h_reg(1, 5, l ? l : 0xFF);
    if (on) return;
    m1h_reg(1, 1, M1R(0x1053u));
    m1h_reg(1, 0, pan);
    m1h_reg(1, 4, 0x80);
}

/* 0x2444 / 0x26FA: two more, on a change of their value, from 16-step tables */
static void m1h_engine_step(uint16_t valat, uint16_t last, uint8_t slot, uint16_t flag, uint16_t smp,
                            uint16_t pitch, uint16_t level, uint16_t panat, bool pan_on_key) {
    uint8_t val = M1R(valat);
    if (val == M1R(last)) return;
    M1R(last) = val;
    m1h_sel(1, slot);
    if (!val) { M1R(flag) &= 0x7Fu; m1h_reg(1, 4, 0); return; }
    bool on = M1R(flag) & 0x80u;
    M1R(flag) |= 0x80u;
    uint8_t i = val & 15u;
    uint16_t w = m1h_rw(pitch + 2u * i);
    m1h_reg(1, 3, (uint8_t)(w >> 8));
    m1h_reg(1, 2, (uint8_t)w);
    m1h_reg(1, 5, m1h_rb(level + i));
    if (panat) m1h_reg(1, 0, m1h_rb(panat + i));
    if (on) return;
    m1h_reg(1, 1, M1R(smp));
    if (pan_on_key) m1h_reg(1, 0, 0);
    m1h_reg(1, 4, 0x80);
}

/* ---- the main loop (0x51E) ---------------------------------------------------------- */

/* the poll: timer B's tick if it has fired */
static inline uint32_t m1h_poll(uint64_t t) { return m1h_timer_b(t) ? m1h_tick(t) : 0; }

static uint32_t m1h_segment(void) {
    m1hle_t *h = &g_shle.m1;
    uint64_t t = h->dc;
    uint32_t cost;
    switch (h->seg) {
    case M1H_S_A:
        cost = M1H_SEG_A + m1h_poll(t);
        m1h_engine_lv(0x16, 0x1055, 0x1052, 0x1056, 0x70);
        m1h_engine_lv(0x15, 0x1058, 0x1059, 0x105A, 0x90);
        h->seg = M1H_S_B;
        return cost;
    case M1H_S_B: {
        cost = m1h_poll(t);
        uint32_t n = m1h_rl(0xF0105Cu) - 1u;
        m1h_wl(0x105Cu, n);
        if (n) { h->seg = M1H_S_C; return cost + M1H_SEG_B; }
        cost += M1H_SEG_B_BLOCK;
        if (M1R(0x1037u) != M1R(0x103Au)) {
            uint8_t v = M1R(0x1037u);
            M1R(0x103Au) = v;
            for (int i = 0; i < 6; i++) m1h_engine(&k_m1h_engines[i], v, i == 0);
            cost += M1H_ENGINES;
        }
        h->seg = M1H_S_MID;
        return cost;
    }
    case M1H_S_MID:
        cost = M1H_SEG_MID + m1h_poll(t);
        m1h_engine_step(0x103E, 0x1041, 0x18, 0x1040, 0x103F, 0x2528, 0x2518, 0x254A, false);
        m1h_engine_step(0x1070, 0x1073, 0x14, 0x1072, 0x1071, 0x27CE, 0x27BE, 0, true);
        m1h_wl(0x105Cu, 0x40u);
        h->seg = M1H_S_C;
        return cost;
    default:
        cost = M1H_SEG_C + m1h_poll(t);
        cost += m1h_release(0);
        cost += m1h_release(1);
        h->seg = M1H_S_D;
        return cost;
    case M1H_S_D:
        cost = M1H_SEG_D + m1h_dispatch();
        h->seg = M1H_S_A;
        return cost;
    }
}

/* the board up to clock period `now`: the init's stages, the main loop's
 * parts that start by then, a byte the UART holds */
static void m1hle_events(uint64_t now) {
    m1hle_t *h = &g_shle.m1;
    if (h->phase == M1H_BOOT) m1h_boot(now);
    if (h->phase != M1H_RUN) return;
    /* a message's effects land with its key-on, M1H_D_LEAD into the part */
    while (h->dc + (h->seg == M1H_S_D && M1R(0x1007u) ? M1H_D_LEAD : 0u) <= now) {
        h->dc += m1h_segment();
        if (h->phase != M1H_RUN) return;
    }
    if (g_sound.m1.rxrdy) {
        uint32_t c = m1h_irq(now);
        if (h->phase == M1H_RUN) h->dc += c;
    }
}

#undef M1R

#endif /* SOUND_HLE_M1_H */
