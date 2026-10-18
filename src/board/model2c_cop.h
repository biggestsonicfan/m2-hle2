/*
 * model2c_cop.h — the Model 2C coprocessor on the bus: the TGPx4 of tgpx4.h
 * behind the i960's COP ports (Pinboard #1021).
 *
 * Same ports as the 2B (model2b_cop.h), a different chip behind them (MAME
 * model2c_state):
 *
 *   0x880000..0x883FFF write  function port: (data & 0x800FFFFF) | dword << 23
 *   0x884000..0x887FFF write  FIFO in, or a program word while CTL bit 31 is up
 *                             (64-bit words, low half first)
 *   0x884000..0x887FFF read   FIFO out
 *   0x980000                  CTL: bit 31 down boots; up starts an upload and
 *                             does nothing else (copro_halt is empty on the 2C)
 *   0x980004 read             1 when FIFO out is empty
 *   0x98000C read             the frame-number bit (irqt_videoctl)
 *   0x980014 read             -1 until a word of the program has come
 *
 * The TGPx4 runs synchronously: after each word the i960 pushes or pops it
 * runs until it waits (tgpx4.h, "Run"), so a reply is there by the time the
 * i960 reads. The board's FIFOs are 8 deep and halt whichever side would
 * overrun; these are deeper, so neither side waits on the other here, and the
 * i960 reads the same words.
 *
 * The rest of the 2C map is the 2B's but for the sound UART, at 0x01C80000
 * as on the 2A (MAME model2c_crx_mem).
 */
#ifndef MODEL2C_COP_H
#define MODEL2C_COP_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "memory.h"
#include "irq_timer.h"
#include "tgpx4.h"

/* ---- The i960's side ----------------------------------------------------- */

static inline void m2cc_push(uint32_t v) {
    g_tgx.pushes++;
    if (g_tgx.fin_n >= TGX_FIFO) {
        if (g_tgx.overflows++ < 16) LOG_WARN("COP: FIFO in overflow (TGPx4 halted or stuck)");
        return;
    }
    g_tgx.fin[(g_tgx.fin_r + g_tgx.fin_n++) & (TGX_FIFO - 1u)] = v;
    tgx_run();
}

static inline void m2cc_fifo_w(uint32_t data) {
    if (g_tgx.coproctl & 0x80000000u) {
        uint64_t *w = &g_tgx.prog[(g_tgx.coprocnt / 2u) & (TGX_PROG_WORDS - 1u)];
        if (g_tgx.coprocnt & 1u) *w = (*w & 0xFFFFFFFFull) | (uint64_t)data << 32;
        else                     *w = (*w & 0xFFFFFFFF00000000ull) | data;
        g_tgx.coprocnt++;
        return;
    }
    m2cc_push(data);
}

/* The board holds the i960 on an empty FIFO until the TGPx4 answers; it has
 * run as far as its input lets it, so here it is waiting on words the i960
 * has not sent. */
static inline uint32_t m2cc_fifo_r(void) {
    g_tgx.pops++;
    if (g_tgx.fout_n == 0) {
        if (g_tgx.empty_pops++ < 16)
            LOG_WARN("COP: i960 read an empty FIFO out (TGPx4 pc %03X)", g_tgx.pc);
        return 0u;
    }
    uint32_t v = g_tgx.fout[g_tgx.fout_r];
    g_tgx.fout_r = (g_tgx.fout_r + 1u) & (TGX_FIFO - 1u);
    g_tgx.fout_n--;
    tgx_run();          /* it may have been waiting for room */
    return v;
}

/* model2.cpp copro_ctl1_w: only a change of bit 31 alone acts. */
static inline void m2cc_ctl_w(uint32_t data) {
    if ((data ^ g_tgx.coproctl) == 0x80000000u) {
        if (data & 0x80000000u) g_tgx.coprocnt = 0;
        else {
            LOG_INFO("COP: boot TGPx4, %u words", g_tgx.coprocnt);
            g_tgx.coproctl = data;
            g_tgx.halted = false;
            tgx_run();
            return;
        }
    }
    g_tgx.coproctl = data;
}

static uint32_t m2cc_copro_read_cb(mem_region_t *r, uint32_t addr, int size) {
    (void)r; (void)size;
    return addr >= 0x00884000u && addr < 0x00888000u ? m2cc_fifo_r() : 0u;
}

static void m2cc_copro_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    (void)r;
    if (size != 4) return;
    uint32_t off = addr - 0x00880000u;
    if (off < 0x4000u)      m2cc_push((val & 0x800FFFFFu) | (((off >> 4) & 0xFFu) << 23));
    else if (off < 0x8000u) m2cc_fifo_w(val);
}

/* CTL: the live words at +0, +4, +0xC and +0x14; the rest stays the plain storage
 * mem_init made it (geo_ctl1 at +8 is read straight out of bus->copro_ctl). */
static uint32_t m2cc_ctl_read_cb(mem_region_t *r, uint32_t addr, int size) {
    uint32_t off = addr - r->base, v = 0;
    uint32_t word = off & ~3u, sh = 8u * (off & 3u);
    if (word == 0)          v = g_tgx.coproctl >> sh;
    else if (word == 4)     v = (g_tgx.fout_n == 0 ? 1u : 0u) >> sh;
    else if (word == 0xC) {
        const memory_bus_t *bus = (const memory_bus_t *)r->user;
        v = irqt_videoctl(bus && (bus->unknown_vid[0] & 0x04u), r->data[0xC]) >> sh;
    } else if (word == 0x14)  v = (g_tgx.coprocnt == 0 ? 0xFFFFFFFFu : 0u) >> sh;
    else
        for (int k = 0; k < size && off + (uint32_t)k < r->size; k++)
            v |= (uint32_t)r->data[off + (uint32_t)k] << (8 * k);
    if (size == 1) return v & 0xFFu;
    if (size == 2) return v & 0xFFFFu;
    return v;
}

static void m2cc_ctl_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - r->base;
    for (int k = 0; k < size && off + (uint32_t)k < r->size; k++)
        r->data[off + (uint32_t)k] = (uint8_t)(val >> (8 * k));
    if (off == 0 && size == 4) m2cc_ctl_w(val);
}

/* ---- Install ------------------------------------------------------------- */

/* Call after mem_init. */
static inline void model2c_cop_install(memory_bus_t *bus, const uint8_t *copro_data, size_t copro_data_size) {
    tgx_reset();
    g_tgx_rom      = copro_data;
    g_tgx_rom_size = (uint32_t)copro_data_size;
    g_tgx_bufram   = bus->buff_ram;
    /* MAME model2_state::machine_reset's "sane default", as on the 2B. */
    for (uint32_t i = 0; bus->buff_ram && i + 4u <= BUFF_RAM_SIZE; i += 4u) {
        bus->buff_ram[i] = 0x0F; bus->buff_ram[i + 1] = 0x0F;
        bus->buff_ram[i + 2] = 0x80; bus->buff_ram[i + 3] = 0x07;
    }
    for (int i = 0; i < bus->region_count; i++) {
        mem_region_t *r = &bus->regions[i];
        if (strcmp(r->name, "COPROGRAM") == 0) {
            r->read_cb = m2cc_copro_read_cb;
            r->write_cb = m2cc_copro_write_cb;
        } else if (strcmp(r->name, "COPRO_CTL") == 0) {
            r->user = bus;
            r->read_cb = m2cc_ctl_read_cb;
            r->write_cb = m2cc_ctl_write_cb;
        } else if (strcmp(r->name, "MIDI") == 0) {
            /* The sound UART moves to 0x01C80000, as model2a.h moves it;
             * sound_attach finds it by this name. */
            r->name = "UART_2A";
            r->base = 0x01C80000u;
            r->size = 4u;
            r->no_burst = 1;
        }
    }
    mem_regions_changed(bus);
}

#endif /* MODEL2C_COP_H */
