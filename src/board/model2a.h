/*
 * model2a.h — what a Model 2A board changes on the bus (board-level).
 *
 * mem_init builds the 2B map. A 2A game's install calls model2a_install after
 * it, which points the coprocessor ports at the TGP (tgp.h) instead of the
 * SHARC HLE and adds the regions 2A has where 2B has others (MAME model2.cpp,
 * model2_tgp_mem and model2a_crx_mem):
 *
 *   0x00880000..0x00887FFF  TGP function port and FIFO   (2B: SHARC FIFO)
 *   0x00980000 / 0x00980004 TGP control / reply-empty    (2B: SHARC control)
 *   0x00980030..0x0098003F  coprocessor ID bytes
 *   0x12000000..0x123FFFFF  texture RAM 0, 16 bits a word (2B: 0x11000000, 32)
 *   0x12400000..0x127FFFFF  texture RAM 1, 16 bits a word (2B: 0x11200000)
 *   0x12800000..0x1281FFFF  luma RAM, 8 bits a word      (2B: 0x11400000, 8 of 16)
 *   0x10800000              polygon count, reads 0
 *
 * Texture and luma land in the same buffers the 2B regions fill, in the same
 * layout, so the renderer reads one format whichever board wrote it.
 *
 * The I/O board is a 315-5649 with analog inputs (Sega Rally's wheel), served
 * by io_5649.h when the profile sets quirks.io_5649.
 *
 * Still to do for 2A: the sound UART sits at 0x01C80000 (2B: 0x009C0000).
 */
#ifndef MODEL2A_H
#define MODEL2A_H

#include <stdint.h>

#include "memory.h"
#include "irq_timer.h"
#include "tgp.h"

/* ---- Coprocessor ports --------------------------------------------------- */

static uint32_t model2a_copro_read_cb(mem_region_t *r, uint32_t addr, int size) {
    (void)r; (void)size;
    if (addr >= 0x00884000u && addr < 0x00888000u) return tgp_fifo_r();
    return 0u;
}

static void model2a_copro_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    (void)r;
    if (size != 4) return;
    if (addr < 0x00884000u)      tgp_function_port_w(addr - 0x00880000u, val);
    else if (addr < 0x00888000u) tgp_fifo_w(val);
}

/* COPRO_CTL (0x980000..0x980023): the TGP's control word and the reply FIFO's
 * empty flag are live; the rest (geo_ctl1 at +8, videoctl at +0xC) stays the
 * plain storage mem_init made it, which other code reads straight out of
 * bus->copro_ctl. */
static uint32_t model2a_ctl_read_cb(mem_region_t *r, uint32_t addr, int size) {
    uint32_t off = addr - r->base;
    uint32_t v;
    if (off < 4)       v = g_tgp.coproctl >> (off * 8);
    else if (off < 8)  v = off == 4 ? (tgp_out_empty() ? 1u : 0u) : 0u;
    else if (off == 0xC) {
        /* videoctl_r (irqt_videoctl). Served by the 2A map and the 2B map with
         * a SHARC COP (model2b_cop.h); STF's map leaves it plain storage. */
        const memory_bus_t *bus = (const memory_bus_t *)r->user;
        v = irqt_videoctl(bus && (bus->unknown_vid[0] & 0x04u), r->data[0xC]);
    } else {
        v = 0;
        for (int k = 0; k < size && off + (uint32_t)k < r->size; k++)
            v |= (uint32_t)r->data[off + (uint32_t)k] << (8 * k);
    }
    if (size == 1) return v & 0xFFu;
    if (size == 2) return v & 0xFFFFu;
    return v;
}

static void model2a_ctl_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - r->base;
    for (int k = 0; k < size && off + (uint32_t)k < r->size; k++)
        r->data[off + (uint32_t)k] = (uint8_t)(val >> (8 * k));
    if (off == 0 && size == 4) tgp_ctl_w(val);
}

/* An address MAME maps but answers nothing at. */
static uint32_t model2a_zero_read_cb(mem_region_t *r, uint32_t addr, int size) {
    (void)r; (void)addr; (void)size;
    return 0;
}

/* ---- Texture and luma RAM ------------------------------------------------ */

/* The regions carry no data pointer (their windows are twice the buffer, and
 * a raw reader such as the memory viewer would run off the end): 0x12000000
 * and its mirror are bank 0, 0x12400000 and its mirror bank 1. */
static inline uint8_t *model2a_tex_buf(memory_bus_t *bus, uint32_t addr) {
    return ((addr - 0x12000000u) & 0x400000u) ? bus->texram1 : bus->texram0;
}

/* model2.cpp tex0_w / tex1_w: dword n of the window holds 16 bits, the half
 * (n & 1) of 32-bit word n >> 1 -- byte 2n of the buffer. The handler ignores
 * the write's mask and keeps the low 16 bits of the dword as the bus puts them
 * on the lanes, so a byte or halfword written to the upper lanes stores 0. */
static void model2a_tex_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    memory_bus_t *bus = (memory_bus_t *)r->user;
    uint32_t off = (addr - r->base) & 0x1FFFFFu;
    uint32_t n = off >> 2;
    uint32_t at = (n * 2u) & (TEXRAM0_SIZE - 1u);
    uint8_t *buf = model2a_tex_buf(bus, addr);
    if (size != 4) val <<= 8u * (off & 3u);
    uint8_t lo = (uint8_t)val, hi = (uint8_t)(val >> 8);
    if (buf[at] != lo || buf[at + 1] != hi) {
        buf[at] = lo; buf[at + 1] = hi;
        bus->gen_tex++;
        bus->tex_dirty[buf == bus->texram1 ? 1 : 0][at >> 10] = 1;
    }
}

/* A read is plain RAM (MAME .ram() over the same share): dword n is texels 2n
 * and 2n + 1, bytes 4n.. of the buffer. The writes fill only the first half of
 * the 2 MB share, so the rest reads 0. */
static uint32_t model2a_tex_read_cb(mem_region_t *r, uint32_t addr, int size) {
    uint32_t off = (addr - r->base) & 0x1FFFFFu;
    if (off >= TEXRAM0_SIZE) return 0;
    const uint8_t *buf = model2a_tex_buf((memory_bus_t *)r->user, addr);
    uint32_t v = 0;
    for (int k = 0; k < size && off + (uint32_t)k < TEXRAM0_SIZE; k++)
        v |= (uint32_t)buf[off + (uint32_t)k] << (8 * k);
    return v;
}

/* model2.cpp lumaram (umask32 0x000000FF): luma byte n is the low byte of
 * dword n. The 2B buffer keeps it at byte 2n (umask16 0x00FF). */
static void model2a_luma_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - r->base;
    if (off & 3u) return;                      /* only the low byte lane is wired */
    (void)size;
    memory_bus_t *bus = (memory_bus_t *)r->user;
    uint32_t at = ((off >> 2) * 2u) & (LUMA_SIZE - 1u);
    if (bus->luma[at] != (uint8_t)val) { bus->luma[at] = (uint8_t)val; bus->gen_lut++; }
}

static uint32_t model2a_luma_read_cb(mem_region_t *r, uint32_t addr, int size) {
    (void)size;
    uint32_t off = addr - r->base;
    if (off & 3u) return 0u;
    memory_bus_t *bus = (memory_bus_t *)r->user;
    return bus->luma[((off >> 2) * 2u) & (LUMA_SIZE - 1u)];
}

/* ---- Install ------------------------------------------------------------- */

static inline mem_region_t *model2a_region_named(memory_bus_t *bus, const char *name) {
    for (int i = 0; i < bus->region_count; i++)
        if (strcmp(bus->regions[i].name, name) == 0) return &bus->regions[i];
    return NULL;
}

/* Call after mem_init. `tables` is the CPU board's copro_tgp_tables ROM
 * (opr-14742a/43a, 0x40000 bytes), `copro_data` the game's coprocessor data
 * ROM (Sega Rally's course collision), both kept alive by the caller. */
static inline void model2a_install(memory_bus_t *bus,
                                   const uint8_t *tables,
                                   const uint8_t *copro_data, size_t copro_data_size) {
    tgp_reset();
    g_tgp_tables     = tables;
    g_tgp_copro_data = copro_data;
    g_tgp_copro_words = 0;
    for (uint32_t w = 1; copro_data && (size_t)w * 4u <= copro_data_size; w <<= 1) g_tgp_copro_words = w;
    g_tgp_bufferram  = bus->buff_ram;

    mem_region_t *r;
    if ((r = model2a_region_named(bus, "COPROGRAM"))) {
        r->read_cb = model2a_copro_read_cb;
        r->write_cb = model2a_copro_write_cb;
    }
    if ((r = model2a_region_named(bus, "COPRO_CTL"))) {
        r->user = bus;
        r->read_cb = model2a_ctl_read_cb;
        r->write_cb = model2a_ctl_write_cb;
    }
    static const struct { const char *name; uint32_t base; } tex[] = {
        { "TEXRAM0_2A", 0x12000000u }, { "TEXRAM0_2A_M", 0x12200000u },
        { "TEXRAM1_2A", 0x12400000u }, { "TEXRAM1_2A_M", 0x12600000u },
    };
    for (size_t i = 0; i < sizeof tex / sizeof tex[0]; i++) {
        r = mem_add_region(bus, tex[i].name, tex[i].base, 0x200000u, NULL, 0);
        if (!r) continue;
        r->user = bus;
        r->read_cb = model2a_tex_read_cb;
        r->write_cb = model2a_tex_write_cb;
    }

    /* The polygon count register reads 0 (MAME model2_tgp_mem, nopr). Sega
     * Rally keeps the largest it has seen in backup RAM (0x5A7F64). */
    if ((r = mem_add_region(bus, "POLY_COUNT", 0x10800000u, 4u, NULL, 0))) {
        r->read_cb = model2a_zero_read_cb;
        r->write_cb = mem_ignore_write_cb;
        r->no_burst = 1;
    }

    /* The sound UART: 0x01C80000 here, where a 2B has it at 0x9C0000 (MAME
     * model2a_crx_mem). The 2A map has nothing at 0x9C0000, so the region moves;
     * it comes before SERIAL in the table, so it wins the lookup. sound_attach
     * finds it by name. */
    if ((r = model2a_region_named(bus, "MIDI"))) {
        r->name = "UART_2A";
        r->base = 0x01C80000u;
        r->size = 4u;
        r->no_burst = 1;
    }

    /* mem_init's LUMA2 covers these addresses as plain bytes; this one comes
     * first in lookups only if LUMA2 is taken off, so retarget LUMA2 itself. */
    if ((r = model2a_region_named(bus, "LUMA2"))) {
        r->user = bus;
        r->read_cb = model2a_luma_read_cb;
        r->write_cb = model2a_luma_write_cb;
    }
    mem_regions_changed(bus);
    LOG_INFO("model2a: TGP coprocessor on the bus (tables %s, copro_data %u words)",
             tables ? "loaded" : "MISSING", g_tgp_copro_words);
}

#endif /* MODEL2A_H */
