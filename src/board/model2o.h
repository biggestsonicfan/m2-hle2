/*
 * model2o.h — the original Model 2 CPU board (board-level; MAME model2o).
 *
 * Daytona USA and Virtua Cop (Pinboard #1035, #1286). MAME's model2o_mem is
 * model2_tgp_mem, the TGP's map (model2a.h), with RAM at 0x200000-0x21FFFF,
 * the program's second 128 KB mirrored at 0x220000, a Model 1 I/O board's
 * dual-port RAM at 0x01C00000 (model1io.h: the profile installs its board
 * between the two calls here) and M2COMM. The processor starts as MAME's
 * i960 device_reset: the PRCB's interrupt stack and start IP.
 */
#ifndef M2HLE_MODEL2O_H
#define M2HLE_MODEL2O_H

#include <stdint.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "rom_loader.h"
#include "memory.h"
#include "model2a.h"
#include "m2comm.h"
#include "i960.h"

static inline uint32_t model2o_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

/* The bus: the 2B map mem_init builds, made model2o's. */
static inline void model2o_install_bus(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2a_install(bus, rs->tgp_tables, rs->copro_data, rs->copro_data_size);
    mem_region_t *r = model2a_region_named(bus, "RAM2");
    if (r) r->size = 0x20000u;
    if (rs->maincpu_size >= 0x40000u)
        mem_add_region(bus, "ROM_MIRROR", 0x00220000u, 0x20000u, rs->maincpu + 0x20000u, 1);
}

/* After the I/O board: the comm board, alone (the boot's network check waits
 * for it to answer), the data ROMs, and the processor's start. */
static inline void model2o_install_finish(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    m2comm_install(bus, 0x1C0u);
    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
    }
    uint32_t prcb = model2o_read32(rs->maincpu, 0x04);
    if (prcb + 0x2C < rs->maincpu_size) {
        uint32_t isp = model2o_read32(rs->maincpu, prcb + PRCB_INTR_STACK);
        cpu->globals.fp = isp;
        cpu->locals.sp  = isp + 64;
        uint32_t start_ip_ptr = model2o_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = model2o_read32(rs->maincpu, start_ip_ptr);
    }
    LOG_INFO("model2o_install: PRCB=0x%08X initial IP=0x%08X", prcb, cpu->sfr.ip);
}

#endif /* M2HLE_MODEL2O_H */
