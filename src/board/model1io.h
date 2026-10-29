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
 *
 * The Advanced board (837-11130 with 837-11131; MAME model1io2.cpp, Pinboard
 * #1286) is Virtua Cop's: model1io_install_advanced. The same 315-5338A and
 * dual-port RAM, but a TMPZ84C015 (a Z80 with a CTC, SIO and PIO inside) at
 * 9.8304 MHz (19.6608 MHz / 2) running epr-17181 (model1io2.zip), and an FPGA
 * that reads the two light guns. Its map (model1io2_device::mem_map):
 * 0x0000-0x7FFF ROM, 0x8000-0x800F the 315-5338A, 0x8040 the board's port
 * (bits 0-3 its buttons, up; 4 and 5 the ROM_EMU and MODE jumpers, off; 6
 * EEPROM DO; 7 high), 0x8080 DSW1 (all off), 0x8100-0x810F the FPGA,
 * 0x8200-0x8203 (mirror 0x8204) an MSM6253 with nothing on it, 0xE000-0xFFFF
 * RAM. Its ports: A in = IN0, B in = IN1 (the triggers, lightgun.h), C in =
 * IN2 (all high: "No Enemies" off), D out = the lamps, E = CN6 (reads 0xFF;
 * the diagnostic LCD's data), F out = EEPROM CS/CLK/DI on bits 4/5/6 (CLK is
 * driven first: the edge sees the old DI, as MAME's io_pf_w), G out = the
 * watchdog (bit 7) and the secondary-controls switch (bit 6).
 *
 * The FPGA reads 0x80 until the Z80 has written it 0x1400 bytes (its
 * configuration); then offsets 0-7 are the guns' 10-bit positions, P1 Y, P1 X,
 * P2 Y, P2 X, low byte first, and 8 the off-screen byte: lightgun_data with
 * the mux at the offset, the bytes HOTD's gun board gives the same way.
 *
 * The program runs on interrupts (IM 2, I = 0): CTC channel 1, a timer of
 * 256 x 38 T-states (1010.5 Hz), vector 0xEA (the CTC's base 0xE8, written to
 * channel 0), whose handler (ROM 0x0A05) polls the inputs and fills the DPRAM.
 * The CTC here is its timer mode, the vector and the daisy chain's
 * in-service bit, cleared by RETI. The SIO's two channels have nothing on
 * them (MAME: cn7 and cn8 empty): no character arrives, so its receive
 * interrupts never fire; RR0 reads 0x04 (transmit buffer empty). The PIO's A
 * and B are DSW2 and DSW3 (all off), with no interrupt. One T-state is 78125/512
 * board units; the fraction is kept.
 */
#ifndef M2HLE_MODEL1IO_H
#define M2HLE_MODEL1IO_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "memory.h"
#include "irq_timer.h"
#include "eeprom_93c46.h"
#include "io_5649.h"
#include "z80.h"
#include "lightgun.h"

#define MODEL1IO_BASE          0x01C00000u
#define MODEL1IO_WINDOW        0x00001000u   /* the i960's window: 2 KB in byte lanes 0 and 2 */
#define MODEL1IO_DPRAM_SIZE    0x800u
#define MODEL1IO_RAM_SIZE      0x2000u
#define MODEL1IO_ROM_SIZE      0x4000u       /* what the map shows of the 64 KB EPROM */
#define MODEL1IO_UNITS_PER_T   ((int64_t)IRQT_CPU_HZ * IRQT_UNIT / 4000000)
#define MODEL1IO_CATCHUP_MAX   (IRQT_FRAME_UNITS * 4)   /* four frames */
/* The Advanced board's: ten seconds. Virtua Cop's i960 counts out a delay
 * loop for its first 40-70 frames without touching the window, while the
 * Z80 runs its own start-up from power-on (MAME); with four frames the Z80
 * lost that time and the boot's handshake came 100 frames late. */
#define MODEL1IO2_CATCHUP_MAX  (IRQT_FRAME_UNITS * 600)

_Static_assert((int64_t)IRQT_CPU_HZ * IRQT_UNIT % 4000000 == 0, "a Z80 T-state is a whole number of board units");

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

/* ---- The Advanced board's own parts (Virtua Cop) ------------------------- */

#define MODEL1IO2_ROM_SIZE     0x8000u
#define MODEL1IO2_FPGA_BYTES   0x1400u        /* MAME fpga_r: its upload's length */
#define MODEL1IO2_T_NUM        78125          /* one T-state at 9.8304 MHz: 78125/512 units */
#define MODEL1IO2_T_SHIFT      9

_Static_assert((int64_t)IRQT_CPU_HZ * IRQT_UNIT * 512 == (int64_t)MODEL1IO2_T_NUM * 9830400,
               "a T-state at 9.8304 MHz is 78125/512 board units");

/* The TMPZ84C015's CTC, the FPGA's upload count and the clock's fraction
 * (savestated by the profile as M1IO2). */
typedef struct {
    uint8_t  ctrl[4], tc[4], count[4];
    uint8_t  want_tc;        /* bit n: channel n's next write is its time constant */
    uint8_t  running;        /* bit n: channel n counts */
    uint8_t  pending;        /* bit n: channel n has reached zero with its interrupt on */
    uint8_t  ius;            /* bit n: channel n's interrupt is in service (until RETI) */
    uint8_t  vector;         /* the CTC's vector base (channel 0 write, bit 0 clear) */
    uint8_t  port_g;
    uint8_t  pad[2];
    uint16_t pre[4];         /* T-states into the channel's prescaler */
    uint32_t fpga;           /* bytes written to the FPGA */
    uint32_t t_frac;         /* the clock's 1/512 units */
} model1io2_t;

static model1io2_t g_model1io2;
static bool        g_model1io_advanced;   /* the board is the Advanced one (set at install) */

/* ---- Inputs -------------------------------------------------------------- */

static inline uint8_t model1io2_port_in(int port) {
    uint32_t held = lightgun_held();
    switch (port) {
        case 0: return (uint8_t)~(held & 0xFFu);   /* IN0 */
        case 1: return lightgun_in1(held);          /* IN1: the triggers */
        default: return 0xFF;                       /* IN2, CN6 */
    }
}

static inline uint8_t model1io_port_in(int port) {
    if (g_model1io_advanced) return model1io2_port_in(port);
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
    if (g_model1io_advanced) {
        switch (port) {
            case 3: m->lamps = v; break;
            case 5: eeprom_pins_clk_first((v >> 5) & 1u, (v >> 6) & 1u, (v >> 4) & 1u); break;
            case 6: g_model1io2.port_g = v; m->secondary = (v >> 6) & 1u; break;
            default: break;    /* E: the LCD's data, nothing on CN6 */
        }
        return;
    }
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

/* ---- The Advanced board's Z80 bus ---------------------------------------- */

/* The FPGA: 0x80 until its configuration is in, then the guns. */
static inline uint8_t model1io2_fpga_read(uint8_t off) {
    if (g_model1io2.fpga < MODEL1IO2_FPGA_BYTES) return 0x80;
    if (off > 8) return 0xFF;
    g_lightgun.mux = off;
    return lightgun_data();
}

static uint8_t model1io2_z80_read(void *user, uint16_t a) {
    (void)user;
    if (a < MODEL1IO2_ROM_SIZE) return a < g_model1io_rom_size ? g_model1io_rom[a] : 0xFF;
    if (a >= 0xE000) return g_model1io.ram[a - 0xE000];
    if (a >= 0x8000 && a < 0x8010) return model1io_chip_read((uint8_t)(a & 0x0F));
    if (a == 0x8040) return (uint8_t)(0xBF | eeprom_do() << 6);
    if (a == 0x8080) return 0xFF;
    if (a >= 0x8100 && a < 0x8110) return model1io2_fpga_read((uint8_t)(a & 0x0F));
    if ((a & ~0x0007u) == 0x8200) {          /* the MSM6253: nothing on its inputs */
        uint8_t bit = g_model1io.adc_shift >> 7;
        g_model1io.adc_shift = (uint8_t)(g_model1io.adc_shift << 1);
        return bit;
    }
    return 0xFF;
}

static void model1io2_z80_write(void *user, uint16_t a, uint8_t v) {
    (void)user;
    if (a >= 0xE000) g_model1io.ram[a - 0xE000] = v;
    else if (a >= 0x8000 && a < 0x8010) model1io_chip_write((uint8_t)(a & 0x0F), v);
    else if (a >= 0x8100 && a < 0x8110) g_model1io2.fpga++;
    else if ((a & ~0x0007u) == 0x8200) g_model1io.adc_shift = 0xFF;
}

/* The CTC (I/O 0x10-0x13). A control word has bit 0 set: bit 7 interrupt,
 * 6 counter mode, 5 prescaler 256 (else 16), 2 a time constant follows, 1
 * reset. A word with bit 0 clear, to channel 0, is the vector base. */
static inline void model1io2_ctc_write(int ch, uint8_t v) {
    model1io2_t *c = &g_model1io2;
    uint8_t bit = (uint8_t)(1u << ch);
    if (c->want_tc & bit) {
        c->tc[ch] = v;
        c->count[ch] = v;
        c->pre[ch] = 0;
        c->want_tc &= (uint8_t)~bit;
        if (!(c->ctrl[ch] & 0x40)) c->running |= bit;   /* timer mode starts (no trigger wait) */
        return;
    }
    if (!(v & 1u)) { if (ch == 0) c->vector = v & 0xF8u; return; }
    c->ctrl[ch] = v;
    if (v & 0x04) c->want_tc |= bit;
    if (v & 0x02) c->running &= (uint8_t)~bit;
    if (!(v & 0x80)) c->pending &= (uint8_t)~bit;
}

/* T-states pass on the CTC's timers. A channel in counter mode has nothing
 * on its CLK/TRG pin and keeps still. */
static inline void model1io2_ctc_run(int t) {
    model1io2_t *c = &g_model1io2;
    for (int ch = 0; ch < 4; ch++) {
        if (!((c->running >> ch) & 1u)) continue;
        unsigned pre = (c->ctrl[ch] & 0x20) ? 256u : 16u;
        unsigned acc = c->pre[ch] + (unsigned)t;
        while (acc >= pre) {
            acc -= pre;
            if (--c->count[ch] == 0) {
                c->count[ch] = c->tc[ch];
                if (c->ctrl[ch] & 0x80) c->pending |= (uint8_t)(1u << ch);
            }
        }
        c->pre[ch] = (uint16_t)acc;
    }
}

/* The daisy chain: channel 0 first; a channel in service holds off itself
 * and every channel after it. -1 when none may interrupt. */
static inline int model1io2_ctc_next(void) {
    model1io2_t *c = &g_model1io2;
    for (int ch = 0; ch < 4; ch++) {
        if ((c->ius >> ch) & 1u) return -1;
        if ((c->pending >> ch) & 1u) return ch;
    }
    return -1;
}

static uint8_t model1io2_z80_in(void *user, uint16_t a) {
    (void)user;
    uint8_t p = (uint8_t)a;
    if (p >= 0x10 && p < 0x14) return g_model1io2.count[p & 3];
    if (p == 0x19 || p == 0x1B) return 0x04;            /* SIO RR0: transmit buffer empty */
    return 0xFF;                                        /* SIO data, PIO A/B (DSW2/3), the rest */
}

static void model1io2_z80_out(void *user, uint16_t a, uint8_t v) {
    (void)user;
    uint8_t p = (uint8_t)a;
    if (p >= 0x10 && p < 0x14) model1io2_ctc_write(p & 3, v);
    /* The SIO (0x18-0x1B), the PIO (0x1C-0x1F), the watchdog and the
     * interrupt priority (0xF0-0xF4): set up, with nothing to drive. */
}

static const z80_bus_t k_model1io2_bus = {
    model1io2_z80_read, model1io2_z80_in, model1io2_z80_write, model1io2_z80_out, NULL,
};

/* Is the instruction at PC a RETI (ED 4D)? Code runs from ROM or RAM only. */
static inline bool model1io2_at_reti(uint16_t pc) {
    if (pc < MODEL1IO2_ROM_SIZE)
        return (size_t)pc + 1 < g_model1io_rom_size && g_model1io_rom[pc] == 0xED && g_model1io_rom[pc + 1] == 0x4D;
    if (pc >= 0xE000 && pc != 0xFFFF) return g_model1io.ram[pc - 0xE000] == 0xED && g_model1io.ram[pc - 0xE000 + 1] == 0x4D;
    return false;
}

/* One instruction (or an interrupt's acceptance) of the Advanced board. */
static inline void model1io2_step(void) {
    model1io_t  *m = &g_model1io;
    model1io2_t *c = &g_model1io2;
    z80_state_t *z = &m->z;
    int ch = model1io2_ctc_next();
    z80_set_irq(z, ch >= 0);
    if (ch >= 0) z->irq_vector = (uint8_t)(c->vector | ch << 1);
    bool taking = ch >= 0 && z->iff1 && !z->ei_delay && !z->nmi_pending;
    bool reti   = !taking && !z->halted && model1io2_at_reti(z->pc);
    int t = z80_run(z, &k_model1io2_bus, 1);
    if (taking) { c->pending &= (uint8_t)~(1u << ch); c->ius |= (uint8_t)(1u << ch); }
    if (reti && c->ius) c->ius &= (uint8_t)(c->ius - 1u);   /* the chain's first in service */
    model1io2_ctc_run(t);
    uint64_t f = (uint64_t)c->t_frac + (uint64_t)t * MODEL1IO2_T_NUM;
    m->t += (int64_t)(f >> MODEL1IO2_T_SHIFT);
    c->t_frac = (uint32_t)(f & ((1u << MODEL1IO2_T_SHIFT) - 1u));
}

/* Run the Z80 up to the board's clock. */
static inline void model1io_catch_up(void) {
    model1io_t *m = &g_model1io;
    if (!g_model1io_rom || m->running) return;
    irqt_flush();
    int64_t now = (int64_t)g_irqt.vbl_count * IRQT_FRAME_UNITS + g_irqt.vbl_phase;
    if (m->t > now + IRQT_FRAME_UNITS) m->t = now;            /* the clock was reset */
    int64_t cap = g_model1io_advanced ? MODEL1IO2_CATCHUP_MAX : MODEL1IO_CATCHUP_MAX;
    if (now - m->t > cap) m->t = now - cap;
    m->running = 1;
    g_eeprom_clock_fn = model1io_eeprom_clock;
    if (g_model1io_advanced) while (m->t < now) model1io2_step();
    else while (m->t < now) m->t += (int64_t)z80_run(&m->z, &k_model1io_bus, 1) * MODEL1IO_UNITS_PER_T;
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
    memset(&g_model1io2, 0, sizeof g_model1io2);
    g_model1io_advanced = false;
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

/* The Advanced board (Virtua Cop): the same window, its own Z80 map. */
static inline void model1io_install_advanced(memory_bus_t *bus, const uint8_t *rom, size_t rom_size) {
    model1io_install(bus, rom, rom_size);
    g_model1io_advanced = true;
    if (!rom) LOG_ERROR("model1io: no Advanced I/O board program (epr-17181.6 from model1io2.zip)");
}

#endif /* M2HLE_MODEL1IO_H */
