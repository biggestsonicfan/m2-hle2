/*
 * model1io.h — the Model 1 I/O board on an original Model 2 (board-level).
 *
 * Daytona USA's cabinet reads its wheel, pedals, gear lever and buttons
 * through the I/O board Sega built for Model 1 (837-10539; MAME model1io.cpp):
 * a Z80 at 4 MHz (32 MHz / 8) running its own program, epr-14869c (in MAME's
 * model1io.zip, which the profile names as its parent set), a 315-5338A I/O
 * chip, an MSM6253 A/D converter and a 93C46 EEPROM. The CPU board sees none
 * of it but an MB8421 dual-port RAM, 2 KB at 0x01C00000, one byte in lanes 0
 * and 2 of each word (MAME model2o_mem, umask32 0x00FF00FF): the Z80 writes
 * the ports there and the i960 reads them.
 *
 * Z80 map (model1io_device::mem_map): 0x0000-0x3FFF ROM, 0x4000-0x5FFF RAM,
 * 0x8000-0x800F the 315-5338A, 0xC000-0xC003 the MSM6253; nothing else, and no
 * interrupt is wired. The 315-5338A (MAME 315_5338a.cpp):
 *   0-6  ports A-G: a read gives the input when the port's bit of reg 8 is
 *        set, else the last value written; a write stores and drives it
 *   8    port direction; a port turned to output drives its stored value
 *   9    command: 0x00/0x01 load the DPRAM address low/high from reg 0x0A,
 *        0x07 writes reg 0x0A to DPRAM[address], 0x70-0x77 to DPRAM[cmd & 7]
 *   0x0A the serial byte (read back), 0x0B the command (read back),
 *   0x0C DPRAM[address], 0x0D status (0x08)
 * The ports (model1io_device): A out = EEPROM CLK/CS/DI on bits 7/6/5 and bit
 * 0 the "secondary controls" switch, which turns B-D to the DIP switches (all
 * off: 0xFF); B in = IN0, C in = IN1, D in = 0xFF; E = the drive board (reads
 * 0xFF; what is written is the force feedback, kept as `drive`); F out = the
 * lamps; G in = EEPROM DO << 7 | 0x70 | the board's four buttons (up: 0x0F).
 * The MSM6253: a write to 0xC000+n latches channel n into its shift register,
 * and each read shifts out bit 7 in D0 (the other bits read 0, unmapped).
 * Channels 0-2 are the wheel, accelerator and brake (io_5649.h's model of
 * them), 3 reads 0xFF.
 *
 * Time: the Z80 runs only when the CPU board touches the DPRAM, caught up to
 * the board's clock (eeprom_now's units, 1/(25 MHz * 60) s: one Z80 T-state
 * is 375 of them) one instruction at a time, so what the i960 reads depends
 * on nothing but board time. The EEPROM keeps the Z80's time while it runs.
 * A gap of more than MODEL1IO_CATCHUP_MAX is skipped rather than run.
 */
#ifndef M2HLE_MODEL1IO_H
#define M2HLE_MODEL1IO_H

#include <stdint.h>
#include <string.h>

#include "log.h"
#include "memory.h"
#include "irq_timer.h"
#include "eeprom_93c46.h"
#include "io_5649.h"
#include "z80.h"

#define MODEL1IO_BASE          0x01C00000u
#define MODEL1IO_WINDOW        0x00001000u   /* the i960's window: 2 KB in byte lanes 0 and 2 */
#define MODEL1IO_DPRAM_SIZE    0x800u
#define MODEL1IO_RAM_SIZE      0x2000u
#define MODEL1IO_ROM_SIZE      0x4000u       /* what the map shows of the 64 KB EPROM */
#define MODEL1IO_UNITS_PER_T   ((int64_t)IRQT_CPU_HZ * IRQT_VBLANK_HZ / 4000000)
#define MODEL1IO_CATCHUP_MAX   ((int64_t)IRQT_CPU_HZ * 4)   /* four frames */

_Static_assert((int64_t)IRQT_CPU_HZ * IRQT_VBLANK_HZ % 4000000 == 0, "a Z80 T-state is a whole number of board units");

/* Board state (savestated by the profile, model1io_savestate). */
typedef struct {
    z80_state_t z;
    uint8_t  ram[MODEL1IO_RAM_SIZE];
    uint8_t  dpram[MODEL1IO_DPRAM_SIZE];
    uint8_t  port_value[7];
    uint8_t  port_config, serial_output, cmd;
    uint16_t address;
    uint8_t  adc_shift;
    uint8_t  secondary;      /* port A bit 0 */
    uint8_t  lamps;          /* port F */
    uint8_t  drive;          /* port E: the drive board's command */
    uint8_t  running;        /* inside model1io_catch_up: the EEPROM reads `t` */
    uint8_t  pad[3];
    int64_t  t;              /* board time the Z80 has run to */
} model1io_t;

static model1io_t     g_model1io;
static const uint8_t *g_model1io_rom;
static size_t         g_model1io_rom_size;
/* The held bit IN1 bit 0 reads (Daytona's VR4); the rest is io_5649.h's. */
static uint32_t       g_model1io_vr4;

static inline int64_t model1io_eeprom_clock(void) { return g_model1io.t; }

/* ---- Inputs -------------------------------------------------------------- */

static inline uint8_t model1io_port_in(int port) {
    model1io_t *m = &g_model1io;
    uint32_t held = io5649_held();
    switch (port) {
        case 1: return m->secondary ? 0xFF : (uint8_t)~(held & 0xFFu);      /* IN0 */
        case 2:                                                             /* IN1 */
            if (m->secondary) return 0xFF;
            io5649_advance();
            return (uint8_t)((io5649_gearbox() & 0xFEu) | ((held & g_model1io_vr4) ? 0 : 1));
        case 6: return (uint8_t)(eeprom_do() << 7 | 0x70 | 0x0F);
        default: return 0xFF;                                               /* D, E (drive board) */
    }
}

static inline void model1io_port_out(int port, uint8_t v) {
    model1io_t *m = &g_model1io;
    switch (port) {
        case 0: eeprom_port_a(v); m->secondary = v & 1u; break;
        case 4: m->drive = v; break;
        case 5: m->lamps = v; break;
        default: break;
    }
}

static inline uint8_t model1io_analog(int ch) {
    if (ch >= 3 || g_model1io.secondary) return 0xFF;
    io5649_advance();
    return io5649_analog(ch);
}

/* ---- The 315-5338A ------------------------------------------------------- */

static inline uint8_t model1io_chip_read(uint8_t off) {
    model1io_t *m = &g_model1io;
    if (off < 7) return (m->port_config >> off) & 1u ? model1io_port_in(off) : m->port_value[off];
    switch (off) {
        case 0x08: return m->port_config;
        case 0x0A: return m->serial_output;
        case 0x0B: return m->cmd;
        case 0x0C: return m->dpram[m->address & (MODEL1IO_DPRAM_SIZE - 1)];
        case 0x0D: return 0x08;
        default:   return 0xFF;
    }
}

static inline void model1io_chip_write(uint8_t off, uint8_t v) {
    model1io_t *m = &g_model1io;
    if (off < 7) { m->port_value[off] = v; model1io_port_out(off, v); return; }
    switch (off) {
        case 0x08: {
            uint8_t to_out = (uint8_t)((v ^ m->port_config) & ~v);
            for (int i = 0; i < 7; i++) if ((to_out >> i) & 1u) model1io_port_out(i, m->port_value[i]);
            m->port_config = v;
            break;
        }
        case 0x09:
            m->cmd = v;
            if (v == 0x00)      m->address = (uint16_t)((m->address & 0xFF00u) | m->serial_output);
            else if (v == 0x01) m->address = (uint16_t)((m->address & 0x00FFu) | m->serial_output << 8);
            else if (v == 0x07) m->dpram[m->address & (MODEL1IO_DPRAM_SIZE - 1)] = m->serial_output;
            else if ((v & 0xF8u) == 0x70) m->dpram[v & 7u] = m->serial_output;
            else if (v != 0x87) LOG_WARN("model1io: unknown 315-5338A command 0x%02X", v);
            break;
        case 0x0A: m->serial_output = v; break;
        default: break;
    }
}

/* ---- The Z80's bus ------------------------------------------------------- */

static uint8_t model1io_z80_read(void *user, uint16_t a) {
    (void)user;
    if (a < MODEL1IO_ROM_SIZE) return a < g_model1io_rom_size ? g_model1io_rom[a] : 0xFF;
    if (a >= 0x4000 && a < 0x6000) return g_model1io.ram[a - 0x4000];
    if (a >= 0x8000 && a < 0x8010) return model1io_chip_read((uint8_t)(a & 0x0F));
    if (a >= 0xC000 && a < 0xC004) {
        uint8_t bit = g_model1io.adc_shift >> 7;
        g_model1io.adc_shift = (uint8_t)(g_model1io.adc_shift << 1);
        return bit;
    }
    return 0x00;
}

static void model1io_z80_write(void *user, uint16_t a, uint8_t v) {
    (void)user;
    if (a >= 0x4000 && a < 0x6000) g_model1io.ram[a - 0x4000] = v;
    else if (a >= 0x8000 && a < 0x8010) model1io_chip_write((uint8_t)(a & 0x0F), v);
    else if (a >= 0xC000 && a < 0xC004) g_model1io.adc_shift = model1io_analog(a & 3);
}

static uint8_t model1io_z80_in(void *user, uint16_t a) { (void)user; (void)a; return 0x00; }
static void    model1io_z80_out(void *user, uint16_t a, uint8_t v) { (void)user; (void)a; (void)v; }

static const z80_bus_t k_model1io_bus = {
    model1io_z80_read, model1io_z80_in, model1io_z80_write, model1io_z80_out, NULL,
};

/* Run the Z80 up to the board's clock. */
static inline void model1io_catch_up(void) {
    model1io_t *m = &g_model1io;
    if (!g_model1io_rom || m->running) return;
    irqt_flush();
    int64_t now = (int64_t)g_irqt.vbl_count * IRQT_CPU_HZ + g_irqt.vbl_phase;
    if (m->t > now + IRQT_CPU_HZ) m->t = now;            /* the clock was reset */
    if (now - m->t > MODEL1IO_CATCHUP_MAX) m->t = now - MODEL1IO_CATCHUP_MAX;
    m->running = 1;
    g_eeprom_clock_fn = model1io_eeprom_clock;
    while (m->t < now) m->t += (int64_t)z80_run(&m->z, &k_model1io_bus, 1) * MODEL1IO_UNITS_PER_T;
    g_eeprom_clock_fn = NULL;
    m->running = 0;
}

/* ---- The i960's window --------------------------------------------------- */

/* Byte `off` of the window: lane 0 of each word is DPRAM[2n], lane 2 DPRAM[2n+1]. */
static inline int model1io_dpram_index(uint32_t off) {
    if (off & 1u) return -1;
    return (int)(((off >> 2) << 1) | ((off >> 1) & 1u));
}

static uint32_t model1io_window_read(mem_region_t *r, uint32_t addr, int size) {
    model1io_catch_up();
    uint32_t off = addr - r->base, v = 0;
    for (int i = 0; i < size; i++) {
        int k = model1io_dpram_index((off + (uint32_t)i) & (MODEL1IO_WINDOW - 1));
        if (k >= 0) v |= (uint32_t)g_model1io.dpram[k] << (8 * i);
    }
    return v;
}

static void model1io_window_write(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    model1io_catch_up();
    uint32_t off = addr - r->base;
    for (int i = 0; i < size; i++) {
        int k = model1io_dpram_index((off + (uint32_t)i) & (MODEL1IO_WINDOW - 1));
        if (k >= 0) g_model1io.dpram[k] = (uint8_t)(val >> (8 * i));
    }
}

/* Power-on, and the window in place of the 315-5649's I/O region. The ROM
 * stays the caller's (the romset's iocpu region). */
static inline void model1io_install(memory_bus_t *bus, const uint8_t *rom, size_t rom_size) {
    memset(&g_model1io, 0, sizeof g_model1io);
    memset(g_model1io.port_value, 0xFF, sizeof g_model1io.port_value);
    z80_reset(&g_model1io.z);
    g_model1io_rom      = rom;
    g_model1io_rom_size = rom_size;
    if (!rom) LOG_ERROR("model1io: no I/O board program (epr-14869c.25 from model1io.zip): no inputs");
    for (int i = 0; i < bus->region_count; i++) {
        mem_region_t *r = &bus->regions[i];
        if (r->base != MODEL1IO_BASE) continue;
        r->size     = MODEL1IO_WINDOW;
        r->data     = NULL;
        r->read_cb  = model1io_window_read;
        r->write_cb = model1io_window_write;
        r->no_burst = 1;
        mem_regions_changed(bus);
        return;
    }
    LOG_ERROR("model1io: no I/O region at 0x%08X", MODEL1IO_BASE);
}

#endif /* M2HLE_MODEL1IO_H */
