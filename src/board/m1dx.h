/*
 * m1dx.h — a second sound board on the line: the Model 1 one in Manx TT's
 * Deluxe cabinet (Pinboard #1928).
 *
 * MAME's manxttdx machine (model2.cpp) is the Model 2A board with a
 * SEGAM1AUDIO beside the CRX's SCSP board, on the same serial line: the i960's
 * UART drives both. Its program is epr-18742.7 (romset.m1cpu, 128 KB in a
 * 256 KB space, the rest zero) and its first MultiPCM's samples mpr-18743.32
 * (romset.m1pcm, 2 MB); MAME's second MultiPCM region is empty, so that chip
 * plays zeros. The chips and the map are Daytona's board (model1_sound.h,
 * sound.h's sound_m1_*): a 68000 at 10 MHz, its i8251's RxRDY on level 2, the
 * YM3438 for its timers only.
 *
 * Measured in MAME (manxtt, a race in each cabinet type):
 *   - TWIN: the i960 sends the line 8128 bytes and this board keys no voice.
 *     It sounds in a DELUXE cabinet only (TEST MODE > GAME ASSIGNMENTS).
 *   - DELUXE: 308 MultiPCM key-ons by frame 3900, no YM3438 key-on, and the
 *     board's TX (which MAME wires back to the i960's receiver) sends nothing.
 * So nothing here is read by the main board: it is audio only, and leaves
 * every frame and det_digest as they were. A byte the 68000 does send is
 * dropped and logged once.
 *
 * Its time is the DSB2's (dsb2.h): the 68000 runs ahead of each 44.1 kHz
 * output sample in sound_out_push (sound.h), and the line's bytes are queued
 * and land in the 8251 a byte time apart. The two MultiPCMs come out at 0.5
 * each (m1snd_sample) and are added at 1.0 to the SCSP's, as MAME's speakers
 * sum the two boards, saturating.
 */
#ifndef M1DX_H
#define M1DX_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "m68k.h"
#include "m68k_exec.h"
#include "model1_sound.h"

#define M1DX_ROM_SIZE   0x40000u
#define M1DX_RAM_SIZE   0x10000u
#define M1DX_RX_FIFO    256u          /* a power of two */
/* One byte on the 31.25 kbaud line (10 bits), in the 68000's clocks. */
#define M1DX_BYTE_CLKS  (M1SND_CPU_HZ / 3125u)

/* Everything a savestate keeps but the 68000's registers. */
typedef struct {
    uint8_t  ram[M1DX_RAM_SIZE];      /* big-endian, as the 68000 reads it */
    m1snd_t  chips;                   /* the MultiPCMs, the YM3438, the 8251 */
    uint8_t  rxq[M1DX_RX_FIFO];       /* the line's bytes not yet in the 8251 */
    uint32_t rxq_r, rxq_w;
    uint64_t rx_next;                 /* the clock the next queued byte may land at */
    int32_t  budget;
    uint32_t warned;                  /* unmapped accesses logged */
    uint8_t  tx_warned;
} m1dx_state_t;

typedef struct {
    bool           present;           /* the set has the board */
    const uint8_t *rom;               /* romset.m1cpu, 256 KB (the caller keeps it) */
    const uint8_t *pcm;               /* romset.m1pcm */
    uint32_t       pcm_size;
    m68k_state_t   m68k;
    m1dx_state_t   st;
} m1dx_t;

static m1dx_t g_m1dx;

static inline void m1dx_warn(const char *what, uint32_t a, uint32_t v) {
    if (g_m1dx.st.warned >= 16) return;
    if (++g_m1dx.st.warned == 16) LOG_WARN("m1dx: %s 0x%06X = 0x%X (no more of these)", what, a, v);
    else                          LOG_WARN("m1dx: %s 0x%06X = 0x%X", what, a, v);
}

/* The YM3438's clock, 8 MHz to the 68000's 10. */
static inline uint64_t m1dx_ym_clock(const m1dx_t *d) { return d->m68k.cpu.cycles * 4u / 5u; }

/* MultiPCM `chip`: the first plays romset.m1pcm, the second nothing. */
static inline void m1dx_pcm_write(m1dx_t *d, int chip, uint32_t reg, uint8_t v) {
    if (chip) mpcm_write(&d->st.chips.pcm[1], NULL, 0, reg, v);
    else      mpcm_write(&d->st.chips.pcm[0], d->pcm, d->pcm_size, reg, v);
}

static uint32_t m1dx_read_cb(void *ctx, uint32_t a, int sz) {
    m1dx_t *d = (m1dx_t *)ctx;
    a &= 0xFFFFFFu;
    if (sz == 4) return (m1dx_read_cb(ctx, a, 2) << 16) | m1dx_read_cb(ctx, a + 2u, 2);
    const uint8_t *p = NULL;
    if (a < M1DX_ROM_SIZE)                    p = d->rom + a;
    else if (a >= 0x80000u && a < 0xA0000u)   p = d->rom + (a - 0x60000u);
    else if (a >= 0xF00000u && a < 0xF10000u) p = d->st.ram + (a & 0xFFFFu);
    if (p) return sz == 1 ? p[0] : ((uint32_t)p[0] << 8) | p[1];
    if (sz == 1 && !(a & 1u)) return 0;               /* the chips sit on the low byte */
    uint32_t reg = (a & 0xFu) >> 1;
    if (a >= 0xC20000u && a < 0xC20004u) return m1snd_uart_read(&d->st.chips, reg);
    if (a >= 0xD00000u && a < 0xD00008u) {
        m1ym_advance(&d->st.chips.ym, m1dx_ym_clock(d));
        return m1ym_read(&d->st.chips.ym, reg);
    }
    if ((a >= 0xC40000u && a < 0xC40008u) || (a >= 0xC60000u && a < 0xC60008u)) return 0;
    if ((a | 1u) == 0xC00001u) return 0;              /* the program polls it; MAME maps nothing there */
    m1dx_warn("read of unmapped", a, (uint32_t)sz);
    return 0;
}

static void m1dx_write_cb(void *ctx, uint32_t a, uint32_t v, int sz) {
    m1dx_t *d = (m1dx_t *)ctx;
    m1dx_state_t *s = &d->st;
    a &= 0xFFFFFFu;
    if (sz == 4) { m1dx_write_cb(ctx, a, v >> 16, 2); m1dx_write_cb(ctx, a + 2u, v & 0xFFFFu, 2); return; }
    if (a >= 0xF00000u && a < 0xF10000u) {
        uint8_t *p = s->ram + (a & 0xFFFFu);
        if (sz == 1) p[0] = (uint8_t)v;
        else { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
        return;
    }
    /* The bank registers take the word; a byte at the even address lands in
     * its high half and leaves bits 0-1 clear (sound.h, sound_m1_write). */
    if ((a & ~1u) == 0xC50000u || (a & ~1u) == 0xC70000u) {
        uint32_t w = sz == 2 ? v : (a & 1u) ? v : v << 8;
        s->chips.pcm[a >= 0xC70000u].bank = w & 3u;
        return;
    }
    if (a < M1DX_ROM_SIZE || (a >= 0x80000u && a < 0xA0000u)) return;   /* a write to ROM */
    if (sz == 1 && !(a & 1u)) return;                 /* an even byte: nothing there listens */
    uint32_t reg = (a & 0xFu) >> 1;
    if (a >= 0xC20000u && a < 0xC20004u) { m1snd_uart_write(&s->chips, reg, (uint8_t)v); return; }
    if (a >= 0xC40000u && a < 0xC40008u) { m1dx_pcm_write(d, 0, reg, (uint8_t)v); return; }
    if (a >= 0xC60000u && a < 0xC60008u) { m1dx_pcm_write(d, 1, reg, (uint8_t)v); return; }
    if ((a | 1u) == 0xC40013u) return;                /* MAME: nopw */
    if ((a | 1u) == 0xC10001u) return;                /* the program writes it; MAME maps nothing there */
    if (a >= 0xD00000u && a < 0xD00008u) {
        m1ym_advance(&s->chips.ym, m1dx_ym_clock(d));
        m1ym_write(&s->chips.ym, reg, (uint8_t)v);
        return;
    }
    m1dx_warn("write to unmapped", a, v);
}

static inline void m1dx_map(m1dx_t *d) {
    memset((void *)d->m68k.rmap, 0, sizeof d->m68k.rmap);
    memset(d->m68k.wmap, 0, sizeof d->m68k.wmap);
    for (uint32_t pg = 0; pg < M1DX_ROM_SIZE >> 16; pg++) d->m68k.rmap[pg] = d->rom + (pg << 16);
    for (uint32_t pg = 0; pg < 2; pg++) d->m68k.rmap[8 + pg] = d->rom + 0x20000u + (pg << 16);
    d->m68k.rmap[0xF0] = d->st.ram;
    d->m68k.read_cb = m1dx_read_cb;
    d->m68k.write_cb = m1dx_write_cb;
    d->m68k.mem_ctx = d;
}

/* Back to power-on: the board boots from its vectors. Keeps the ROMs. */
static inline void m1dx_reset(void) {
    m1dx_t *d = &g_m1dx;
    memset(&d->st, 0, sizeof d->st);
    m1snd_reset(&d->st.chips);
    memset(&d->m68k, 0, sizeof d->m68k);
    if (!d->present) return;
    m1dx_map(d);
    m68k_reset(&d->m68k);
    m68k_startup(&d->m68k);
}

/* The set's board, or none (rom NULL). The caller keeps both ROMs alive. */
static inline void m1dx_load(const uint8_t *rom, size_t rom_size, const uint8_t *pcm, size_t pcm_size) {
    m1dx_t *d = &g_m1dx;
    d->present = rom && rom_size >= M1DX_ROM_SIZE && pcm && pcm_size > 0;
    d->rom = d->present ? rom : NULL;
    d->pcm = d->present ? pcm : NULL;
    d->pcm_size = d->present ? (uint32_t)(pcm_size < M1SND_PCM_SIZE ? pcm_size : M1SND_PCM_SIZE) : 0;
    /* m68k_step_core wants the timing tables, which nothing else builds while
     * the sound driver runs in C (dsb2_load, #1909). */
    if (d->present) m68k_tables_init();
    m1dx_reset();
    if (d->present)
        LOG_INFO("m1dx: Model 1 sound board, program %u KB, samples %u MB, SSP=0x%08X PC=0x%08X",
                 (unsigned)(rom_size >> 10), (unsigned)(pcm_size >> 20), d->m68k.cpu.ssp, d->m68k.cpu.pc);
}

/* A byte off the i960's serial line (sound_uart_service hands it over as the
 * SCSP board gets it). */
static inline void m1dx_rx(uint8_t b) {
    m1dx_state_t *s = &g_m1dx.st;
    if (!g_m1dx.present) return;
    if (s->rxq_w - s->rxq_r >= M1DX_RX_FIFO) return;   /* far behind: drop */
    s->rxq[s->rxq_w++ & (M1DX_RX_FIFO - 1u)] = b;
}

/* The next queued byte into the 8251, a byte time after the last. */
static inline void m1dx_rx_land(m1dx_state_t *s, uint64_t now) {
    if (s->rxq_r == s->rxq_w || now < s->rx_next) return;
    uint8_t b = s->rxq[s->rxq_r++ & (M1DX_RX_FIFO - 1u)];
    s->rx_next = now + M1DX_BYTE_CLKS;
    m1snd_rx(&s->chips, b);
}

static inline void m1dx_run_cpu(m1dx_t *d, int32_t clocks) {
    m1dx_state_t *s = &d->st;
    m68k_state_t *m = &d->m68k;
    s->budget += clocks;
    while (s->budget > 0) {
        if (m->cpu.halted) { m->cpu.cycles += (uint64_t)s->budget; s->budget = 0; break; }
        uint64_t c0 = m->cpu.cycles;
        m1dx_rx_land(s, c0);
        if (s->chips.rxrdy && m68k_ipl(&m->cpu) < 2 && m68k_interrupt(m, 2)) {}
        else if (m->cpu.stopped) {
            /* asleep until the line's next byte */
            uint64_t till = c0 + (uint64_t)s->budget, wake = till;
            if (s->rxq_r != s->rxq_w) wake = s->rx_next > c0 ? s->rx_next : c0 + 1;
            m->cpu.cycles = wake < till ? wake : till;
        } else m68k_step_core(m);
        s->budget -= (int32_t)(m->cpu.cycles - c0);
    }
    if (m1snd_tx_ready(&s->chips)) {                   /* nothing listens (above) */
        uint8_t b = m1snd_tx_pop(&s->chips);
        if (!s->tx_warned) { s->tx_warned = 1; LOG_WARN("m1dx: the board sent 0x%02X, which the i960 is not given", b); }
        while (m1snd_tx_ready(&s->chips)) (void)m1snd_tx_pop(&s->chips);
    }
}

/* One 44.1 kHz output sample: run the 68000 to it, then add its chips to l, r
 * (saturating). Called once per board sample from sound_out_push. */
static inline void m1dx_sample(int16_t *l, int16_t *r) {
    m1dx_t *d = &g_m1dx;
    m1dx_run_cpu(d, m1snd_sample_cycles(&d->st.chips));
    int16_t ml, mr;
    m1snd_sample(&d->st.chips, d->pcm, d->pcm_size, &ml, &mr);
    int32_t ol = *l + ml, or_ = *r + mr;
    *l = (int16_t)(ol > 32767 ? 32767 : ol < -32768 ? -32768 : ol);
    *r = (int16_t)(or_ > 32767 ? 32767 : or_ < -32768 ? -32768 : or_);
}

/* ---- savestates (core/savestate.h, entry "M1DX") ----------------------- */

typedef struct {
    m68k_cpu_t   cpu;
    m1dx_state_t st;
} m1dx_save_t;

static inline void m1dx_save(m1dx_save_t *o) {
    o->cpu = g_m1dx.m68k.cpu;
    o->st = g_m1dx.st;
}

static inline void m1dx_restore(const m1dx_save_t *o) {
    if (!g_m1dx.present) return;
    g_m1dx.m68k.cpu = o->cpu;
    g_m1dx.st = o->st;
    m1dx_map(&g_m1dx);
}

#endif /* M1DX_H */
