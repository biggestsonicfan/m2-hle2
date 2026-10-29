/*
 * dsb2.h — Sega's Digital Sound Board 2: the MPEG music beside the SCSP.
 *
 * Top Skater (Model 2C) plays its music off a second sound board: a 68000
 * with its own program (romset.mpegcpu, mpr-19760) that listens to the same
 * serial line the i960 drives the SCSP board with, and an MPEG-1 Layer II
 * decoder playing windows of a 16 MB data ROM (romset.mpeg). This is MAME's
 * src/mame/sega/dsb2.cpp (the oracle) on our 68000 core:
 *
 *   0x000000-0x01FFFF  the program ROM
 *   0xC00000-0xC00003  i8251, the low byte of each word (data, then control)
 *   0xD00001           system control (bit 3, inverted: the bank of a ROM
 *                      above 16 MB; none of ours is)
 *   0xE00001           the program writes it; MAME maps nothing there
 *                      ("acknowledge FIFO writes?"), nor does this
 *   0xE00003           the decoder's command FIFO
 *   0xE80001           decoder status: reads 0x01
 *   0xF00000-0xF1FFFF  work RAM
 *
 * Level 1 is the UART's RxRDY, level 2 a 1 kHz timer (HOLD_LINE: on until the
 * 68000 takes it). The FIFO takes 0x14/0x15 + three bytes (start), 0x24/0x25
 * + three (end), 0x74/0x75 (play the window) and 0x84/0x85 (stop); the low
 * bit is the player and MAME ignores it. The output is 32 kHz stereo at
 * volume 0x7F of 128, mixed at 1.0 into both speakers beside the SCSP
 * (model2.cpp topskatr), here resampled to the board's 44.1 kHz.
 *
 * The board's TX line goes nowhere (MAME connects nothing back to the i960),
 * so nothing here is read by the main board: it is audio only, and leaves
 * every frame and det_digest as they were.
 *
 * Its time: the 68000 runs ahead of each 44.1 kHz output sample, in
 * sound_out_push (sound.h), so it keeps whatever pace the board's samples are
 * made at. The SCSP makes its samples late and in runs (scsp.h, "The chip's
 * own time"), so the bytes the serial line delivers are queued here and handed
 * to the 8251 one at a time, a byte time apart, as the line would.
 */
#ifndef DSB2_H
#define DSB2_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "m68k.h"
#include "m68k_exec.h"
#include "mpeg_l2.h"

#define DSB2_CPU_HZ     8000000u
#define DSB2_OUT_HZ     44100u        /* SOUND_RATE */
#define DSB2_MPEG_HZ    32000u
#define DSB2_ROM_SIZE   0x20000u
#define DSB2_RAM_SIZE   0x20000u
#define DSB2_RX_FIFO    256u          /* a power of two */
/* One byte on the 31.25 kbaud line (10 bits), in the 68000's clocks. */
#define DSB2_BYTE_CLKS  (DSB2_CPU_HZ / 3125u)
#define DSB2_TIMER_CLKS (DSB2_CPU_HZ / 1000u)

enum { DSB2_IDLE, DSB2_START_HI, DSB2_START_MD, DSB2_START_LO, DSB2_END_HI, DSB2_END_MD, DSB2_END_LO };

/* Everything a savestate keeps but the 68000's registers (and mpa.base). */
typedef struct {
    uint8_t  ram[DSB2_RAM_SIZE];      /* big-endian, as the 68000 reads it */
    /* the 8251's receiver, and the line's bytes not yet handed to it */
    uint8_t  rx_data, rxrdy, mode_done, rxe;
    uint8_t  rxq[DSB2_RX_FIFO];
    uint32_t rxq_r, rxq_w;
    uint64_t rx_next;                 /* the clock the next queued byte may land at */
    uint64_t timer_at;                /* the 1 kHz timer's next tick */
    uint8_t  irq2;                    /* the timer's line, held until taken */
    uint8_t  sys_ctl;
    int32_t  budget;
    uint32_t cyc_frac;
    /* the decoder */
    int      command, playing;
    uint32_t start, end, mp_start, mp_end;
    int      mp_vol, mp_pan;
    int      mp_pos;                  /* in bits */
    int16_t  audio_buf[1152 * 2];
    int      audio_pos, audio_avail, audio_ch;
    /* 32 kHz to 44.1 kHz: the two decoder samples either side, and how far
     * between them the output is, in 44100ths */
    int16_t  prev_l, prev_r, cur_l, cur_r;
    uint32_t phase;
    mpa_t    mpa;
    uint32_t warned;                  /* unmapped accesses logged */
} dsb2_state_t;

typedef struct {
    bool           present;           /* the set has the board */
    const uint8_t *rom;               /* romset.mpegcpu (the caller keeps it) */
    const uint8_t *mpeg;              /* romset.mpeg */
    uint32_t       mpeg_size;
    m68k_state_t   m68k;
    dsb2_state_t   st;
} dsb2_t;

static dsb2_t g_dsb2;

static inline void dsb2_warn(const char *what, uint32_t a, uint32_t v) {
    if (g_dsb2.st.warned >= 16) return;
    if (++g_dsb2.st.warned == 16) LOG_WARN("dsb2: %s 0x%06X = 0x%X (no more of these)", what, a, v);
    else                          LOG_WARN("dsb2: %s 0x%06X = 0x%X", what, a, v);
}

static inline uint8_t dsb2_uart_read(dsb2_state_t *s, uint32_t reg) {
    if (reg == 0) { s->rxrdy = 0; return s->rx_data; }
    return (uint8_t)(0x85 | (s->rxrdy ? 0x02 : 0));    /* TxRDY, TxEMPTY, DSR; RxRDY */
}

static inline void dsb2_uart_write(dsb2_state_t *s, uint32_t reg, uint8_t v) {
    if (reg == 0) return;                               /* TX: not wired anywhere */
    if (!s->mode_done) { s->mode_done = 1; return; }    /* the mode, then commands */
    if (v & 0x40) { s->mode_done = 0; s->rxe = 0; return; }
    s->rxe = (v >> 2) & 1;
}

/* MAME dsb2_device::fifo_w. */
static inline void dsb2_fifo_w(dsb2_t *d, uint8_t data) {
    dsb2_state_t *s = &d->st;
    switch (s->command) {
    case DSB2_START_HI: s->start = (s->start & 0x00FFFFu) | (uint32_t)data << 16; s->command = DSB2_START_MD; return;
    case DSB2_START_MD: s->start = (s->start & 0xFF00FFu) | (uint32_t)data << 8;  s->command = DSB2_START_LO; return;
    case DSB2_START_LO: s->start = (s->start & 0xFFFF00u) | data; s->mp_start = s->start; s->command = DSB2_IDLE; return;
    case DSB2_END_HI:   s->end = (s->end & 0x00FFFFu) | (uint32_t)data << 16; s->command = DSB2_END_MD; return;
    case DSB2_END_MD:   s->end = (s->end & 0xFF00FFu) | (uint32_t)data << 8;  s->command = DSB2_END_LO; return;
    case DSB2_END_LO:   s->end = (s->end & 0xFFFF00u) | data; s->mp_end = s->end; s->command = DSB2_IDLE; return;
    default: break;
    }
    switch (data & 0xFEu) {
    case 0x14: s->command = DSB2_START_HI; break;
    case 0x24: s->command = DSB2_END_HI; break;
    case 0x74: {
        uint32_t start = s->mp_start & 0xFFFFFFu, end = s->mp_end & 0xFFFFFFu;
        if (start >= d->mpeg_size || end > d->mpeg_size || end <= start) {
            LOG_WARN("dsb2: refusing the play window 0x%06X-0x%06X (ROM 0x%X)", start, end, d->mpeg_size);
            s->playing = 0;
            s->audio_pos = s->audio_avail = 0;
        } else {
            s->mp_start = start;
            s->mp_end = end;
            s->mp_pos = (int)(start * 8u);
            s->playing = 1;
            LOG_INFO("dsb2: plays 0x%06X-0x%06X", start, end);
        }
        break;
    }
    case 0x84: s->playing = 0; s->audio_pos = s->audio_avail = 0; break;
    default: break;
    }
}

static uint32_t dsb2_read_cb(void *ctx, uint32_t a, int sz) {
    dsb2_t *d = (dsb2_t *)ctx;
    dsb2_state_t *s = &d->st;
    a &= 0xFFFFFFu;
    if (a < DSB2_ROM_SIZE || (a >= 0xF00000u && a < 0xF00000u + DSB2_RAM_SIZE)) {
        const uint8_t *p = a < DSB2_ROM_SIZE ? d->rom + a : s->ram + (a - 0xF00000u);
        uint32_t room = a < DSB2_ROM_SIZE ? DSB2_ROM_SIZE - a : 0xF00000u + DSB2_RAM_SIZE - a;
        uint32_t v = 0;
        for (int i = 0; i < sz; i++) v = v << 8 | ((uint32_t)i < room ? p[i] : 0xFFu);
        return v;
    }
    if (sz == 4) return dsb2_read_cb(ctx, a, 2) << 16 | dsb2_read_cb(ctx, a + 2, 2);
    if ((a & ~3u) == 0xC00000u) {                      /* umask 0x00ff: the odd bytes */
        if (sz == 1 && !(a & 1)) return 0;
        return dsb2_uart_read(s, (a >> 1) & 1);
    }
    if ((a | 1u) == 0xE80001u) return (sz == 1 && !(a & 1)) ? 0 : 0x01;
    dsb2_warn("read of unmapped", a, (uint32_t)sz);
    return 0;
}

static void dsb2_write_cb(void *ctx, uint32_t a, uint32_t v, int sz) {
    dsb2_t *d = (dsb2_t *)ctx;
    dsb2_state_t *s = &d->st;
    a &= 0xFFFFFFu;
    if (a >= 0xF00000u && a < 0xF00000u + DSB2_RAM_SIZE) {
        uint32_t o = a - 0xF00000u;
        for (int i = sz - 1; i >= 0; i--, v >>= 8)
            if (o + (uint32_t)i < DSB2_RAM_SIZE) s->ram[o + (uint32_t)i] = (uint8_t)v;
        return;
    }
    if (sz == 4) { dsb2_write_cb(ctx, a, v >> 16, 2); dsb2_write_cb(ctx, a + 2, v & 0xFFFFu, 2); return; }
    if (sz == 1 && !(a & 1)) {                         /* an even byte: nothing there listens */
        if ((a & ~3u) == 0xC00000u || (a | 1u) == 0xD00001u || (a & ~3u) == 0xE00000u) return;
    } else {
        uint8_t b = (uint8_t)v;
        if ((a & ~3u) == 0xC00000u) { dsb2_uart_write(s, (a >> 1) & 1, b); return; }
        if ((a | 1u) == 0xD00001u)  { s->sys_ctl = b; return; }
        if ((a | 1u) == 0xE00003u)  { dsb2_fifo_w(d, b); return; }
        if ((a | 1u) == 0xE00001u)  return;
    }
    if (a < DSB2_ROM_SIZE) return;                     /* a write to ROM */
    dsb2_warn("write to unmapped", a, v);
}

static inline void dsb2_map(dsb2_t *d) {
    memset((void *)d->m68k.rmap, 0, sizeof d->m68k.rmap);
    memset(d->m68k.wmap, 0, sizeof d->m68k.wmap);
    for (uint32_t pg = 0; pg < DSB2_ROM_SIZE >> 16; pg++) d->m68k.rmap[pg] = d->rom + (pg << 16);
    for (uint32_t pg = 0; pg < DSB2_RAM_SIZE >> 16; pg++) d->m68k.rmap[0xF0 + pg] = d->st.ram + (pg << 16);
    d->m68k.read_cb = dsb2_read_cb;
    d->m68k.write_cb = dsb2_write_cb;
    d->m68k.mem_ctx = d;
}

/* Back to power-on: the board boots from its vectors. Keeps the ROMs. */
static inline void dsb2_reset(void) {
    dsb2_t *d = &g_dsb2;
    memset(&d->st, 0, sizeof d->st);
    d->st.mp_vol = 0x7F;
    d->st.audio_ch = 2;
    d->st.timer_at = DSB2_TIMER_CLKS;
    mpa_init(&d->st.mpa, d->mpeg);
    memset(&d->m68k, 0, sizeof d->m68k);
    if (!d->present) return;
    dsb2_map(d);
    m68k_reset(&d->m68k);
    m68k_startup(&d->m68k);
}

/* The set's board, or none (rom NULL). The caller keeps both ROMs alive. */
static inline void dsb2_load(const uint8_t *rom, size_t rom_size, const uint8_t *mpeg, size_t mpeg_size) {
    dsb2_t *d = &g_dsb2;
    d->present = rom && rom_size >= DSB2_ROM_SIZE && mpeg && mpeg_size > 0;
    d->rom = d->present ? rom : NULL;
    d->mpeg = d->present ? mpeg : NULL;
    d->mpeg_size = d->present ? (uint32_t)mpeg_size : 0;
    dsb2_reset();
    if (d->present)
        LOG_INFO("dsb2: MPEG board, program %u KB, data %u MB, SSP=0x%08X PC=0x%08X",
                 (unsigned)(rom_size >> 10), (unsigned)(mpeg_size >> 20), d->m68k.cpu.ssp, d->m68k.cpu.pc);
}

/* A byte off the i960's serial line (sound_uart_service hands it over as the
 * SCSP board gets it). */
static inline void dsb2_rx(uint8_t b) {
    dsb2_state_t *s = &g_dsb2.st;
    if (!g_dsb2.present) return;
    if (s->rxq_w - s->rxq_r >= DSB2_RX_FIFO) return;    /* far behind: drop */
    s->rxq[s->rxq_w++ & (DSB2_RX_FIFO - 1u)] = b;
}

/* The next queued byte into the 8251, a byte time after the last. */
static inline void dsb2_rx_land(dsb2_state_t *s, uint64_t now) {
    if (s->rxq_r == s->rxq_w || now < s->rx_next) return;
    uint8_t b = s->rxq[s->rxq_r++ & (DSB2_RX_FIFO - 1u)];
    s->rx_next = now + DSB2_BYTE_CLKS;
    if (!s->rxe) return;                               /* a receiver that is off ignores it */
    s->rx_data = b;                                    /* one still held is overrun */
    s->rxrdy = 1;
}

static inline void dsb2_run_cpu(dsb2_t *d, int32_t clocks) {
    dsb2_state_t *s = &d->st;
    m68k_state_t *m = &d->m68k;
    s->budget += clocks;
    while (s->budget > 0) {
        if (m->cpu.halted) { m->cpu.cycles += (uint64_t)s->budget; s->budget = 0; break; }
        uint64_t c0 = m->cpu.cycles;
        while (c0 >= s->timer_at) { s->irq2 = 1; s->timer_at += DSB2_TIMER_CLKS; }
        dsb2_rx_land(s, c0);
        int ipl = m68k_ipl(&m->cpu);
        if (s->irq2 && ipl < 2 && m68k_interrupt(m, 2)) s->irq2 = 0;
        else if (s->rxrdy && ipl < 1 && m68k_interrupt(m, 1)) {}
        else if (m->cpu.stopped) {
            /* asleep until the timer's next tick or the line's next byte */
            uint64_t wake = s->timer_at;
            if (s->rxq_r != s->rxq_w && s->rx_next < wake) wake = s->rx_next > c0 ? s->rx_next : c0 + 1;
            uint64_t till = c0 + (uint64_t)s->budget;
            m->cpu.cycles = wake < till ? wake : till;
            if (m->cpu.cycles == c0) m->cpu.cycles++;
        } else m68k_step_core(m);
        s->budget -= (int32_t)(m->cpu.cycles - c0);
    }
}

/* The decoder's next 32 kHz sample pair (MAME sound_stream_update). */
static inline void dsb2_mpeg_next(dsb2_t *d, int16_t *l, int16_t *r) {
    dsb2_state_t *s = &d->st;
    while (s->audio_pos >= s->audio_avail) {
        if (!s->playing) { *l = *r = 0; return; }
        int n = 0, rate = 0, ch = 0;
        s->mpa.base = d->mpeg;                       /* not kept by a state: set before every use */
        if (mpa_decode(&s->mpa, &s->mp_pos, (int)(s->mp_end * 8u), s->audio_buf, &n, &rate, &ch) && n > 0) {
            s->audio_pos = 0;
            s->audio_avail = n;
            s->audio_ch = ch;
        } else {
            s->audio_pos = s->audio_avail = 0;          /* the end of the window */
            s->playing = 0;
        }
    }
    const int16_t *p = s->audio_buf + s->audio_pos * s->audio_ch;
    int32_t a = p[0], b = s->audio_ch == 2 ? p[1] : p[0];
    if (s->mp_pan == 1) b = a;
    else if (s->mp_pan == 2) a = b;
    *l = (int16_t)(a * s->mp_vol / 128);
    *r = (int16_t)(b * s->mp_vol / 128);
    s->audio_pos++;
}

/* One 44.1 kHz output sample: run the 68000 to it, then the music into l, r
 * (added, saturating). Called once per board sample from sound_out_push. */
static inline void dsb2_sample(int16_t *l, int16_t *r) {
    dsb2_t *d = &g_dsb2;
    dsb2_state_t *s = &d->st;
    s->cyc_frac += DSB2_CPU_HZ % DSB2_OUT_HZ;
    int32_t n = (int32_t)(DSB2_CPU_HZ / DSB2_OUT_HZ);
    if (s->cyc_frac >= DSB2_OUT_HZ) { s->cyc_frac -= DSB2_OUT_HZ; n++; }
    dsb2_run_cpu(d, n);

    s->phase += DSB2_MPEG_HZ;
    while (s->phase >= DSB2_OUT_HZ) {
        s->phase -= DSB2_OUT_HZ;
        s->prev_l = s->cur_l; s->prev_r = s->cur_r;
        dsb2_mpeg_next(d, &s->cur_l, &s->cur_r);
    }
    int32_t f = (int32_t)s->phase;                     /* 0..44099 past prev */
    int32_t ml = s->prev_l + (int32_t)(((int64_t)(s->cur_l - s->prev_l) * f) / (int32_t)DSB2_OUT_HZ);
    int32_t mr = s->prev_r + (int32_t)(((int64_t)(s->cur_r - s->prev_r) * f) / (int32_t)DSB2_OUT_HZ);
    int32_t ol = *l + ml, or_ = *r + mr;
    *l = (int16_t)(ol > 32767 ? 32767 : ol < -32768 ? -32768 : ol);
    *r = (int16_t)(or_ > 32767 ? 32767 : or_ < -32768 ? -32768 : or_);
}

/* ---- savestates (core/savestate.h, entry "DSB2") ----------------------- */

typedef struct {
    m68k_cpu_t   cpu;
    dsb2_state_t st;
} dsb2_save_t;

static inline void dsb2_save(dsb2_save_t *o) {
    o->cpu = g_dsb2.m68k.cpu;
    o->st = g_dsb2.st;
    o->st.mpa.base = NULL;
}

static inline void dsb2_restore(const dsb2_save_t *o) {
    if (!g_dsb2.present) return;
    g_dsb2.m68k.cpu = o->cpu;
    g_dsb2.st = o->st;
    g_dsb2.st.mpa.base = g_dsb2.mpeg;
    dsb2_map(&g_dsb2);
}

#endif /* DSB2_H */
