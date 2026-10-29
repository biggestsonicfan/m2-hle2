/*
 * eeprom_93c46.h — the board's 93C46 serial EEPROM (board-level).
 *
 * Every Model 2 has one, 64 words of 16 bits, on the I/O chip's port A (MAME
 * model2.cpp eeprom_w / in0_r): bit 5 DI, bit 6 CS, bit 7 CLK. Bit 0 of the
 * same byte switches IN0 to "control mode": the top half then reads 0xC0 |
 * DO << 5 | 0x10. Dynamite Cop keeps its settings there (an "S32A" block,
 * which it writes itself when the chip reads erased); without one it read
 * all-ones settings: two credit counters, a demo that never wins.
 *
 * The protocol is MAME's eeprom_serial_93cxx_device: a start bit, two opcode
 * bits and six address bits clocked in on rising CLK, then 16 data bits in
 * or out. While CS is up and no command runs DO is READY: low for the write
 * cycle after a write or erase (MAME's 1.75 ms write, 1 ms erase, 8 ms for
 * the whole chip), on the board's clock, so every machine sees it end at the
 * same instruction.
 *
 * The words are board state (savestates, rollback) but not kept on disk yet:
 * mem_init, which every install and netplay reset runs, erases the chip
 * (0xFFFF), so every machine starts a session from the same one.
 */
#ifndef M2HLE_EEPROM_93C46_H
#define M2HLE_EEPROM_93C46_H

#include <stdint.h>
#include <string.h>

#include "irq_timer.h"

#define EEPROM_WORDS      64
#define EEPROM_ADDR_BITS  6

/* Board time, 1/(IRQT_CPU_HZ * IRQT_UNIT) s; MAME eeprom_base_device. */
#define EEPROM_US(us)     ((int64_t)(us) * (IRQT_CPU_HZ / 1000000) * IRQT_UNIT)
#define EEPROM_T_WRITE    EEPROM_US(1750)
#define EEPROM_T_ERASE    EEPROM_US(1000)
#define EEPROM_T_ALL      EEPROM_US(8000)

enum { EEP_RESET, EEP_START, EEP_COMMAND, EEP_READ, EEP_DATA, EEP_BUSY };
enum { EEP_CMD_LOCK, EEP_CMD_WRITEALL, EEP_CMD_ERASEALL, EEP_CMD_UNLOCK,
       EEP_CMD_WRITE, EEP_CMD_READ, EEP_CMD_ERASE };

typedef struct {
    uint16_t word[EEPROM_WORDS];
    int32_t  state, command, address, bits;
    uint32_t accum, shift;
    uint8_t  cs, clk, di, locked;
    uint8_t  port_a;       /* the last byte written to port A (bit 0: control mode) */
    uint8_t  pad[3];
    int64_t  busy_until;   /* board time the write cycle ends */
} eeprom_t;

static eeprom_t g_eeprom;

/* A new board: the chip erased. */
static inline void eeprom_power_on(void) {
    memset(&g_eeprom, 0, sizeof g_eeprom);
    memset(g_eeprom.word, 0xFF, sizeof g_eeprom.word);
    g_eeprom.locked = 1;
}

/* A clock other than the CPU board's, while a CPU of the I/O board runs the
 * chip (model1io.h: the Z80 runs behind the board's clock and catches up). */
static int64_t (*g_eeprom_clock_fn)(void);

static inline int64_t eeprom_now(void) {
    if (g_eeprom_clock_fn) return g_eeprom_clock_fn();
    irqt_flush();
    return (int64_t)g_irqt.vbl_count * IRQT_FRAME_UNITS + g_irqt.vbl_phase;
}

static inline int eeprom_ready(void) { return eeprom_now() >= g_eeprom.busy_until; }

static inline int eeprom_do(void) {
    if (g_eeprom.state == EEP_START) return eeprom_ready();
    if (g_eeprom.state == EEP_READ)  return (int)(g_eeprom.shift >> 31);
    return 1;
}

static inline void eeprom_busy(int64_t t) {
    g_eeprom.busy_until = eeprom_now() + t;
    g_eeprom.state = EEP_BUSY;
}

/* MAME parse_command_and_address + execute_command. */
static inline void eeprom_command(void) {
    eeprom_t *e = &g_eeprom;
    uint32_t op = e->accum >> EEPROM_ADDR_BITS;
    e->address = (int32_t)(e->accum & (EEPROM_WORDS - 1));
    e->command = op == 0 ? (int32_t)(e->address >> (EEPROM_ADDR_BITS - 2)) : (int32_t)(EEP_CMD_WRITE + op - 1);
    e->bits = 0;
    switch (e->command) {
        case EEP_CMD_READ:     e->shift = 0; e->state = EEP_READ; break;
        case EEP_CMD_WRITE:
        case EEP_CMD_WRITEALL: e->shift = 0; e->state = EEP_DATA; break;
        case EEP_CMD_LOCK:     e->locked = 1; e->state = EEP_RESET; break;
        case EEP_CMD_UNLOCK:   e->locked = 0; e->state = EEP_RESET; break;
        case EEP_CMD_ERASE:
            if (e->locked) { e->state = EEP_RESET; break; }
            e->word[e->address] = 0xFFFF;
            eeprom_busy(EEPROM_T_ERASE);
            break;
        default:               /* ERASEALL */
            if (e->locked) { e->state = EEP_RESET; break; }
            memset(e->word, 0xFF, sizeof e->word);
            eeprom_busy(EEPROM_T_ALL);
            break;
    }
}

/* MAME execute_write_command: WRITEALL ANDs, as after an ERASEALL. */
static inline void eeprom_write_data(void) {
    eeprom_t *e = &g_eeprom;
    if (e->locked) { e->state = EEP_RESET; return; }
    uint16_t v = (uint16_t)e->shift;
    if (e->command == EEP_CMD_WRITE) {
        e->word[e->address] = v;
        eeprom_busy(EEPROM_T_WRITE);
    } else {
        for (int i = 0; i < EEPROM_WORDS; i++) e->word[i] &= v;
        eeprom_busy(EEPROM_T_ALL);
    }
}

/* A rising CLK with CS up (MAME handle_event). */
static inline void eeprom_clock(void) {
    eeprom_t *e = &g_eeprom;
    switch (e->state) {
        case EEP_START:
            if (e->di && eeprom_ready()) { e->accum = 0; e->bits = 0; e->state = EEP_COMMAND; }
            break;
        case EEP_COMMAND:
            e->accum = (e->accum << 1) | e->di;
            if (++e->bits == 2 + EEPROM_ADDR_BITS) eeprom_command();
            break;
        case EEP_READ: {
            int i = e->bits++;
            if (i == 0) e->shift = (uint32_t)e->word[e->address] << 16;
            else        e->shift = (e->shift << 1) | 1u;
            break;
        }
        case EEP_DATA:
            e->shift = (e->shift << 1) | e->di;
            if (++e->bits == 16) eeprom_write_data();
            break;
        default: break;
    }
}

/* A port A write: DI, then CLK, then CS, as MAME's eeprom_w sets them. */
static inline void eeprom_port_a(uint8_t v) {
    eeprom_t *e = &g_eeprom;
    e->port_a = v;
    e->di = (v >> 5) & 1u;
    uint8_t clk = (v >> 7) & 1u, cs = (v >> 6) & 1u;
    if (clk != e->clk) {
        e->clk = clk;
        if (clk && e->state != EEP_RESET) eeprom_clock();
    }
    if (cs != e->cs) {
        e->cs = cs;
        e->state = cs ? (e->state == EEP_RESET ? EEP_START : e->state) : EEP_RESET;
    }
}

/* The pins set one by one, CLK first, DI, then CS, as MAME's model1io2
 * io_pf_w does (the Advanced Model 1 I/O board's port F): a rising CLK
 * clocks in the DI the pin held before this write. */
static inline void eeprom_pins_clk_first(uint8_t clk, uint8_t di, uint8_t cs) {
    eeprom_t *e = &g_eeprom;
    if (clk != e->clk) {
        e->clk = clk;
        if (clk && e->state != EEP_RESET) eeprom_clock();
    }
    e->di = di;
    if (cs != e->cs) {
        e->cs = cs;
        e->state = cs ? (e->state == EEP_RESET ? EEP_START : e->state) : EEP_RESET;
    }
}

/* IN0 as the board reads it: in control mode the top half is the EEPROM's. */
static inline uint8_t eeprom_in0(uint8_t in0) {
    if (!(g_eeprom.port_a & 1u)) return in0;
    return (uint8_t)(0xC0 | eeprom_do() << 5 | 0x10 | (in0 & 0x0F));
}

#endif /* M2HLE_EEPROM_93C46_H */
