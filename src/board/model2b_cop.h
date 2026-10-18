/*
 * model2b_cop.h — the Model 2B COP as a SHARC that runs the program the game
 * uploads (Pinboard #935).
 *
 * cop.h answers Sonic the Fighters' COP program (cpres1) in C. A game with a
 * program of its own, such as Virtual-On's (Model 1/2 TGP functions: fadd,
 * matrix stack, sin/cos tables in copro_data; cop.md, "Virtual-On"), would sit
 * on its first reply forever there. Its install calls model2b_cop_install after
 * mem_init, and the COPROGRAM and COPRO_CTL regions then talk to the
 * interpreter in sharc_cpu.h instead, the way model2a.h wires the TGP:
 *
 *   0x880000..0x883FFF write  function port: (data & 0x800FFFFF) | dword << 23
 *   0x884000..0x887FFF write  FIFO in, or a boot halfword while CTL bit 31 is up
 *   0x884000..0x887FFF read   FIFO out
 *   0x980000                  CTL: bit 31 up starts an upload, down boots
 *   0x980004 read             1 when FIFO out is empty
 *   0x98000C read             the frame-number bit (irqt_videoctl)
 *   0x980014 read             -1 until a word of the program has come
 *
 * and the SHARC's data bus (model2b_state::copro_sharc_map):
 *   0x0400000..0x0BFFFFF read   FIFO in (stalls while empty)
 *   0x0C00000..0x13FFFFF write  FIFO out (stalls while full)
 *   0x1400000..0x1BFFFFF        buffer RAM, word & 0x7FFF
 *   0x1C00000..0x1DFFFFF read   copro_data ROM
 * FLAG0 in is "FIFO in empty", FLAG1 in "FIFO out full".
 *
 * The SHARC runs synchronously: after each word the i960 pushes it runs until
 * it blocks (a read of the empty FIFO, a poll of FLAG0, IDLE), so a reply is
 * there by the time the i960 reads. Both FIFOs are deeper than the board's 16
 * words: the board halts the i960 on a full FIFO in, which here would be a
 * SHARC waiting on its own output, and the deeper FIFOs leave it nothing to
 * wait for. What the i960 reads is the same either way.
 *
 * Every byte of state is in g_m2bc and g_shc, POD, saved as the profile's
 * savestate extras.
 */
#ifndef MODEL2B_COP_H
#define MODEL2B_COP_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "memory.h"
#include "irq_timer.h"
#include "sharc_cpu.h"

#define M2BC_FIFO 1024u   /* a power of 2 */

/* The most cycles one i960 access lets the SHARC run. A program that never
 * blocks (a bug, or a wait this file does not know) gives the i960 back its
 * turn after this. */
#define M2BC_RUN_MAX 2000000

typedef struct {
    uint32_t coproctl;              /* 0x980000 */
    uint32_t coprocnt;              /* halfwords uploaded since bit 31 rose */
    uint32_t fin[M2BC_FIFO], fin_r, fin_n;
    uint32_t fout[M2BC_FIFO], fout_r, fout_n;
    bool     halted;                /* held until a program is booted */
    uint32_t pushes, pops, empty_pops, overflows;
} m2bc_state_t;

static m2bc_state_t   g_m2bc;
static const uint8_t *g_m2bc_rom      = NULL;   /* copro_data, LE words */
static uint32_t       g_m2bc_rom_size = 0;      /* bytes */
static uint8_t       *g_m2bc_bufram   = NULL;   /* i960 0x900000 */

static inline uint32_t m2bc_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void m2bc_flags(void) {
    shc_set_flag_input(0, g_m2bc.fin_n == 0);
    shc_set_flag_input(1, g_m2bc.fout_n >= M2BC_FIFO);
}

/* ---- The SHARC's side ---------------------------------------------------- */

static uint32_t m2bc_ext_rd(uint32_t addr, bool *stall) {
    if (addr >= 0x0400000u && addr < 0x0C00000u) {
        if (g_m2bc.fin_n == 0) { *stall = true; return 0; }
        uint32_t v = g_m2bc.fin[g_m2bc.fin_r];
        g_m2bc.fin_r = (g_m2bc.fin_r + 1u) & (M2BC_FIFO - 1u);
        g_m2bc.fin_n--;
        m2bc_flags();
        return v;
    }
    if (addr >= 0x1400000u && addr < 0x1C00000u)
        return g_m2bc_bufram ? m2bc_le32(g_m2bc_bufram + (addr & 0x7FFFu) * 4u) : 0u;
    if (addr >= 0x1C00000u && addr < 0x1E00000u) {
        uint32_t off = (addr - 0x1C00000u) * 4u;
        return g_m2bc_rom && off + 4u <= g_m2bc_rom_size ? m2bc_le32(g_m2bc_rom + off) : 0u;
    }
    return 0;
}

static void m2bc_ext_wr(uint32_t addr, uint32_t v, bool *stall) {
    if (addr >= 0x0C00000u && addr < 0x1400000u) {
        if (g_m2bc.fout_n >= M2BC_FIFO) { *stall = true; return; }
        g_m2bc.fout[(g_m2bc.fout_r + g_m2bc.fout_n++) & (M2BC_FIFO - 1u)] = v;
        m2bc_flags();
        return;
    }
    if (addr >= 0x1400000u && addr < 0x1C00000u && g_m2bc_bufram) {
        uint8_t *p = g_m2bc_bufram + (addr & 0x7FFFu) * 4u;
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    }
}

static inline void m2bc_run(void) {
    if (!g_m2bc.halted) shc_run(M2BC_RUN_MAX);
}

/* ---- The i960's side ----------------------------------------------------- */

static inline void m2bc_push(uint32_t v) {
    g_m2bc.pushes++;
    if (g_m2bc.fin_n >= M2BC_FIFO) {
        if (g_m2bc.overflows++ < 16) LOG_WARN("COP: FIFO in overflow (SHARC halted or stuck)");
        return;
    }
    g_m2bc.fin[(g_m2bc.fin_r + g_m2bc.fin_n++) & (M2BC_FIFO - 1u)] = v;
    m2bc_flags();
    m2bc_run();
}

static inline void m2bc_fifo_w(uint32_t data) {
    if (g_m2bc.coproctl & 0x80000000u) {
        shc_boot_write(g_m2bc.coprocnt++, (uint16_t)data);
        return;
    }
    m2bc_push(data);
}

/* An empty FIFO stalls the i960 on the board until the SHARC answers; the
 * SHARC has run as far as its input lets it, so here it is the i960 waiting on
 * words it has not sent. */
static inline uint32_t m2bc_fifo_r(void) {
    g_m2bc.pops++;
    if (g_m2bc.fout_n == 0) {
        if (g_m2bc.empty_pops++ < 16)
            LOG_WARN("COP: i960 read an empty FIFO out (SHARC pc %05X)", g_shc.pc);
        return 0u;
    }
    uint32_t v = g_m2bc.fout[g_m2bc.fout_r];
    g_m2bc.fout_r = (g_m2bc.fout_r + 1u) & (M2BC_FIFO - 1u);
    g_m2bc.fout_n--;
    m2bc_flags();
    m2bc_run();          /* it may have been waiting for room */
    return v;
}

/* model2.cpp copro_ctl1_w: only a change of bit 31 alone acts. */
static inline void m2bc_ctl_w(uint32_t data) {
    if ((data ^ g_m2bc.coproctl) == 0x80000000u) {
        if (data & 0x80000000u) {
            g_m2bc.coprocnt = 0;
            g_m2bc.halted = true;
            shc_reset();
        } else {
            LOG_INFO("COP: boot SHARC, %u halfwords", g_m2bc.coprocnt);
            g_m2bc.coproctl = data;
            g_m2bc.halted = false;
            shc_boot();
            m2bc_flags();
            m2bc_run();
            return;
        }
    }
    g_m2bc.coproctl = data;
}

static uint32_t m2bc_copro_read_cb(mem_region_t *r, uint32_t addr, int size) {
    (void)r; (void)size;
    return addr >= 0x00884000u && addr < 0x00888000u ? m2bc_fifo_r() : 0u;
}

static void m2bc_copro_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    (void)r;
    if (size != 4) return;
    uint32_t off = addr - 0x00880000u;
    if (off < 0x4000u)      m2bc_push((val & 0x800FFFFFu) | (((off >> 4) & 0xFFu) << 23));
    else if (off < 0x8000u) m2bc_fifo_w(val);
}

/* CTL: the live words at +0, +4, +0xC and +0x14; the rest stays the plain storage
 * mem_init made it (geo_ctl1 at +8 is read straight out of bus->copro_ctl). */
static uint32_t m2bc_ctl_read_cb(mem_region_t *r, uint32_t addr, int size) {
    uint32_t off = addr - r->base, v = 0;
    uint32_t word = off & ~3u, sh = 8u * (off & 3u);
    if (word == 0)          v = g_m2bc.coproctl >> sh;
    else if (word == 4)     v = (g_m2bc.fout_n == 0 ? 1u : 0u) >> sh;
    else if (word == 0xC) {
        const memory_bus_t *bus = (const memory_bus_t *)r->user;
        v = irqt_videoctl(bus && (bus->unknown_vid[0] & 0x04u), r->data[0xC]) >> sh;
    } else if (word == 0x14)  v = (g_m2bc.coprocnt == 0 ? 0xFFFFFFFFu : 0u) >> sh;
    else
        for (int k = 0; k < size && off + (uint32_t)k < r->size; k++)
            v |= (uint32_t)r->data[off + (uint32_t)k] << (8 * k);
    if (size == 1) return v & 0xFFu;
    if (size == 2) return v & 0xFFFFu;
    return v;
}

static void m2bc_ctl_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - r->base;
    for (int k = 0; k < size && off + (uint32_t)k < r->size; k++)
        r->data[off + (uint32_t)k] = (uint8_t)(val >> (8 * k));
    if (off == 0 && size == 4) m2bc_ctl_w(val);
}

/* ---- Install ------------------------------------------------------------- */

static inline void model2b_cop_reset(void) {
    memset(&g_m2bc, 0, sizeof g_m2bc);
    g_m2bc.halted = true;
    shc_reset();
    m2bc_flags();
}

/* Call after mem_init. */
static inline void model2b_cop_install(memory_bus_t *bus, const uint8_t *copro_data, size_t copro_data_size) {
    model2b_cop_reset();
    g_m2bc_rom      = copro_data;
    g_m2bc_rom_size = (uint32_t)copro_data_size;
    g_m2bc_bufram   = bus->buff_ram;
    /* MAME model2_state::machine_reset fills buffer RAM with this "sane
     * default", 0x07800F0F (STF's board leaves it 0). */
    for (uint32_t i = 0; bus->buff_ram && i + 4u <= BUFF_RAM_SIZE; i += 4u) {
        bus->buff_ram[i] = 0x0F; bus->buff_ram[i + 1] = 0x0F;
        bus->buff_ram[i + 2] = 0x80; bus->buff_ram[i + 3] = 0x07;
    }
    shc_ext_rd = m2bc_ext_rd;
    shc_ext_wr = m2bc_ext_wr;
    for (int i = 0; i < bus->region_count; i++) {
        mem_region_t *r = &bus->regions[i];
        if (strcmp(r->name, "COPROGRAM") == 0) {
            r->read_cb = m2bc_copro_read_cb;
            r->write_cb = m2bc_copro_write_cb;
        } else if (strcmp(r->name, "COPRO_CTL") == 0) {
            r->user = bus;
            r->read_cb = m2bc_ctl_read_cb;
            r->write_cb = m2bc_ctl_write_cb;
        }
    }
}

#endif /* MODEL2B_COP_H */
