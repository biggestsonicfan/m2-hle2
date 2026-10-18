/*
 * crypt_0229.h — the Sega 315-5838 / 317-0229 compression chip, as Dead or
 * Alive's board has it (board-level).
 *
 * A port of MAME's sega_315_5838_comp_device (src/mame/sega/
 * 315-5838_317-0229_comp.cpp, BSD-3-Clause, David Haywood, Samuel Neves,
 * Peter Wilhelmsen, Morten Shearman Kirkegaard) and its Model 2 map
 * (model2.cpp, model2_0229_mem). On Model 2 the chip sees a 32 KB window of
 * RAM at 0x01D80000, its source, and has three registers at the window's top:
 *
 *   0x01D87FF0  write  source address (a word index into the window)
 *   0x01D87FF4  write  table upload: the high half the data, the low half the
 *                      mode (data_w_doa, the halves reversed from ST-V's)
 *   0x01D87FF8  read   the next two plain bytes, a 16-bit half at a time
 *   0x01D8400C  read   a busy flag that toggles each read: 0xFFFF, 0xFFF0
 *
 * DOA's cipher is not known. What the game takes from the chip is one string
 * it checks at boot (the pass at 0x2C20, the fail at 0x2C28 in MAME's notes),
 * so the chip answers with that string, MAME's HACK_MODE_DOA, whatever the
 * source holds. The tables the game uploads are kept, as MAME keeps them, and
 * not used.
 */
#ifndef CRYPT_0229_H
#define CRYPT_0229_H

#include <stdint.h>
#include <string.h>

#include "log.h"
#include "memory.h"

#define C0229_RAM_BASE  0x01D80000u
#define C0229_RAM_SIZE  0x00008000u
#define C0229_REG_BASE  0x01D87FF0u
#define C0229_BUSY      0x01D8400Cu

/* Everything a board reset clears and a savestate keeps. */
typedef struct {
    uint8_t  ram[C0229_RAM_SIZE];
    uint32_t srcoffset;
    uint16_t mode;
    uint8_t  tree_len[13], tree_idx[13];
    uint16_t tree_pattern[13];
    int32_t  it2, id;
    uint8_t  dictionary[256];
    uint8_t  busy;          /* MAME m_prot_a */
} crypt_0229_t;

static crypt_0229_t g_c0229;

/* The string DOA checks, two spaces first (a dummy read, as the 5881's). */
static const char c0229_doa_string[51] = "  TECMO LTD.  DEAD OR ALIVE  1996.10.22  VER. 1.00";

static inline uint8_t c0229_byte(void) {
    crypt_0229_t *c = &g_c0229;
    uint8_t v = c->srcoffset < 50 ? (uint8_t)c0229_doa_string[c->srcoffset] : 0;
    c->srcoffset = (c->srcoffset + 1) & 0x007fffffu;
    return v;
}

static inline uint16_t c0229_data_r(void) {
    uint16_t hi = c0229_byte();
    return (uint16_t)(hi << 8 | c0229_byte());
}

static inline void c0229_upload(uint16_t v) {
    crypt_0229_t *c = &g_c0229;
    if (!(c->mode & 0x80)) {
        if (c->it2 / 2 >= 12) { LOG_WARN("317-0229: tree upload past 12 entries"); return; }
        if (!(c->it2 & 1)) {
            c->tree_len[c->it2 / 2] = (uint8_t)(v >> 8);
            c->tree_idx[c->it2 / 2] = (uint8_t)v;
        } else {
            c->tree_pattern[c->it2 / 2] = v;
        }
        c->it2++;
    } else {
        if (c->id >= 255) { LOG_WARN("317-0229: dictionary upload past 256 bytes"); return; }
        c->dictionary[c->id++] = (uint8_t)(v >> 8);
        c->dictionary[c->id++] = (uint8_t)v;
    }
}

static inline void c0229_mode(uint16_t v) {
    g_c0229.mode = v;
    if (!(v & 0x80)) g_c0229.it2 = 0;
    else             g_c0229.id = 0;
}

/* One 16-bit half of a register (half 0 the low, 1 the high). */
static inline void c0229_reg_w(uint32_t reg, int half, uint16_t v) {
    crypt_0229_t *c = &g_c0229;
    if (reg == 0) {             /* srcaddr_w: COMBINE_DATA into the offset */
        c->srcoffset = half ? (c->srcoffset & 0xffffu) | (uint32_t)v << 16
                            : (c->srcoffset & 0xffff0000u) | v;
        c->srcoffset &= 0x007fffffu;
    } else if (reg == 1) {      /* data_w_doa: high half data, low half mode */
        if (half) c0229_upload(v);
        else      c0229_mode(v);
    }
}

static uint32_t c0229_reg_read_cb(mem_region_t *r, uint32_t addr, int size) {
    uint32_t off = addr - r->base;
    if ((off & ~3u) != 8) return 0;
    /* doa_prot_r: each half the access covers takes two bytes. */
    if (size == 4) {
        uint32_t hi = c0229_data_r();
        return hi << 16 | c0229_data_r();
    }
    uint32_t v = c0229_data_r();
    return size == 1 ? (v >> ((off & 1) * 8)) & 0xFFu : v;
}

static void c0229_reg_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - r->base;
    uint32_t reg = off >> 2;
    if (size == 4) {
        c0229_reg_w(reg, 0, (uint16_t)val);
        c0229_reg_w(reg, 1, (uint16_t)(val >> 16));
    } else if (size == 2) {
        c0229_reg_w(reg, (off >> 1) & 1, (uint16_t)val);
    } else {
        LOG_WARN("317-0229: byte write 0x%02X to +0x%X", val & 0xFFu, off);
    }
}

/* doa_unk_r: the low half toggles between 0xFFFF and 0xFFF0 on every read. */
static uint32_t c0229_busy_read_cb(mem_region_t *r, uint32_t addr, int size) {
    uint32_t off = addr - r->base;
    g_c0229.busy ^= 1;
    uint32_t v = g_c0229.busy ? 0xffffu : 0xfff0u;
    if (size == 4) return v;
    if (size == 2) return off & 2 ? 0 : v;
    return off < 2 ? (v >> (off * 8)) & 0xFFu : 0;
}

/* Clear the chip (MAME device_reset) and map its window. Called by a
 * profile's install, after mem_init. The registers go in before the RAM, so
 * they win where the two overlap. */
static inline void crypt_0229_install(memory_bus_t *bus) {
    memset(&g_c0229, 0, sizeof g_c0229);
    mem_region_t *r = mem_add_region(bus, "C0229", C0229_REG_BASE, 0x10, NULL, 0);
    if (r) {
        r->read_cb  = c0229_reg_read_cb;
        r->write_cb = c0229_reg_write_cb;
        r->no_burst = 1;
    }
    r = mem_add_region(bus, "C0229_BUSY", C0229_BUSY, 4, NULL, 0);
    if (r) {
        r->read_cb  = c0229_busy_read_cb;
        r->no_burst = 1;
    }
    mem_add_region(bus, "C0229_RAM", C0229_RAM_BASE, C0229_RAM_SIZE, g_c0229.ram, 0);
    mem_regions_changed(bus);
}

#endif /* CRYPT_0229_H */
