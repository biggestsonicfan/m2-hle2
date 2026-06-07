/*
 * memory.h — board-level memory bus.
 *
 * The Model 2 address space is ~48 named regions: ROM, work RAM, geometry
 * RAM, COP buffer RAM, tile RAM, palette, texture RAM, framebuffer, plus
 * a handful of MMIO blocks (IRQ, timers, IO, IAC).
 *
 * Dispatch model: a linear-scanned region table. Each region has a backing
 * buffer and OPTIONAL read/write callbacks. If a callback is set it runs
 * INSTEAD of the plain buffer access — use callbacks for MMIO side effects
 * (timer tick, IRQ ack, IO port latch). Default reads/writes hit the buffer.
 *
 * Endian: i960 is little-endian. mem_read{16,32} / mem_write{16,32} use LE.
 * Register-pair (reg_quad) byte-swizzle is the CPU core's concern, not the
 * bus's — see CLAUDE.md.
 *
 * Region table ORDER MATTERS for overlapping regions (TILE before H_SYNC).
 *
 * PHASE 2 NOTE: every region is created here as a plain backing buffer. The
 * MMIO side-effect callbacks (COPROGRAM→cop_write, GEO clip-window capture,
 * IRQ ack, board timers) and watchpoint hooks attach in their own phases
 * (COP = Phase 7, IRQ/timers = Phase 11, watchpoints alongside breakpoints).
 * Until then those regions behave as plain RAM, which is correct for the
 * read/write round-trip checkpoint.
 */
#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "../core/log.h"
#include "../core/watchpoint.h"
#include "cop.h"
#include "irq_timer.h"

/* ---- Region descriptor --------------------------------------------------- */

#define MEM_REGIONS_MAX 64

struct mem_region;
typedef uint32_t (*mem_read_cb)(struct mem_region *r, uint32_t addr, int size);
typedef void     (*mem_write_cb)(struct mem_region *r, uint32_t addr, uint32_t val, int size);

typedef struct mem_region {
    const char  *name;
    uint32_t     base;
    uint32_t     size;
    uint8_t     *data;       /* backing buffer (may be NULL for pure-MMIO) */
    int          readonly;
    mem_read_cb  read_cb;    /* NULL = read from `data` */
    mem_write_cb write_cb;   /* NULL = write to `data` (unless readonly) */
    void        *user;       /* opaque for callbacks (cop state, irq controller, ...) */
} mem_region_t;

/* ---- Bus ----------------------------------------------------------------- */

typedef struct memory_bus {
    /* ROM (lifetime managed by rom_loader) */
    uint8_t  *rom;
    size_t    rom_size;

    /* Work RAM */
    uint8_t   ram2[RAM2_SIZE];
    uint8_t   ram[RAM_SIZE];

    /* Geometry / 3D */
    uint8_t   geo[GEO_SIZE];
    uint8_t   geo_program[GEO_PROGRAM_SIZE];
    uint8_t   geo_cmd[GEO_CMD_SIZE];

    /* Coprocessor — backing only; HLE state lives in cop.h */
    uint8_t   coprogram[COPROGRAM_SIZE];
    uint8_t   copro_sharc[COPRO_SHARC_IOP_SIZE];
    uint8_t   buff_ram[BUFF_RAM_SIZE];
    uint8_t   copro_ctl[COPRO_CONTROL1_SIZE];

    /* Audio */
    uint8_t   midi[MIDI_SIZE];

    /* CPU control / IRQ */
    uint8_t   cpu_ctrl[CPU_CTRL_SIZE];
    uint8_t   irq[IRQ_REQUEST_SIZE + IRQ_ENABLE_SIZE];

    /* Timers */
    uint8_t   timers[TIMERS_SIZE];

    /* Video */
    uint8_t   tile[TILE_SIZE];          /* covers scroll regs + h/v sync overlap */
    uint8_t   tmapgfx[TMAPGFX_SIZE];
    uint8_t   palette[PALETTE_SIZE];
    uint8_t   colorxlat[COLORXLAT_SIZE];
    uint8_t   zclip_3d[ZCLIP_3D_SIZE];

    /* I/O — initialized to 0xFF (idle), not 0x00 */
    uint8_t   io[IO_SIZE];
    uint8_t   serial[SERIAL_SIZE];
    uint8_t   back[BACK_SIZE];
    uint8_t   unknown_vid[UNKNOWN_VID_SIZE];

    /* IAC */
    uint8_t   iac[IAC_SIZE];

    /* Heap-allocated large regions */
    uint8_t  *main_data;
    uint8_t  *xtra_data;
    uint8_t  *vid_ext_ram;  /* 0x01100000–0x017FFFFF: collision tables, display lists */
    uint8_t  *texram0;
    uint8_t  *texram1;
    uint8_t   luma[LUMA_SIZE];
    uint8_t   luma2[LUMA2_SIZE];
    uint8_t  *framebuffer;

    /* Region table — linear-scanned in declaration order. */
    mem_region_t regions[MEM_REGIONS_MAX];
    int          region_count;

    /* Bus stats */
    uint64_t    reads;
    uint64_t    writes;
    uint64_t    unmapped_reads;
    uint64_t    unmapped_writes;

    /* Set by i960_step before each instruction dispatch — included in unmapped warnings. */
    uint32_t    cpu_ip;
} memory_bus_t;

/* ---- Region builder ------------------------------------------------------ */

static inline mem_region_t *mem_add_region(memory_bus_t *bus,
                                           const char *name,
                                           uint32_t base, uint32_t size,
                                           uint8_t *data, int readonly) {
    if (bus->region_count >= MEM_REGIONS_MAX) {
        LOG_ERROR("mem: region table full, dropping %s", name);
        return NULL;
    }
    mem_region_t *r = &bus->regions[bus->region_count++];
    r->name = name;
    r->base = base;
    r->size = size;
    r->data = data;
    r->readonly = readonly;
    r->read_cb = NULL;
    r->write_cb = NULL;
    r->user = NULL;
    return r;
}

/* ---- COPROGRAM / GEO MMIO callbacks -------------------------------------- */

/* The COPROGRAM region (0x00880000) is the COP's command/arg stream. For HLE
 * purposes ALL 32-bit writes are forwarded to cop_write() regardless of offset
 * (the i960 uses (g11)[g12] addressing where g12 varies — function port vs FIFO
 * — but both carry the same sequential command+arg stream the SHARC must see).
 * Reads serve cop_read() (result FIFO). */
static uint32_t coprogram_read_cb(mem_region_t *r, uint32_t addr, int size) {
    (void)r; (void)addr; (void)size;
    return cop_read();
}
static void coprogram_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    (void)r; (void)addr;
    if (size == 4) cop_write(val);
}

/* GEO base (0x800000): set_window writes 0x303 to offset 0x30, signalling the
 * start of a 6-word clip-window sequence. */
static void geo_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - GEO_BASE;
    if (r->data && off + 4 <= r->size)
        memcpy(r->data + off, &val, 4);
    if (size == 4 && off == 0x30 && val == 0x303)
        geo_win_start();
}

/* GEO_PROGRAM FIFO port (0x804000): the 6 packed (x,y) clip words arrive here. */
static void geo_program_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - GEO_PROGRAM_BASE;
    if (r->data && off + 4 <= r->size)
        memcpy(r->data + off, &val, 4);
    if (size == 4 && off == 0)
        geo_win_push(val);
}

/* ---- IRQ controller / board timer MMIO callbacks ------------------------ */

static uint32_t irq_region_read(mem_region_t *r, uint32_t addr, int size) {
    (void)size;
    return ((addr - r->base) < IRQ_REQUEST_SIZE) ? irqt_request_read()
                                                  : irqt_enable_read();
}
static void irq_region_write(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    (void)size;
    if ((addr - r->base) < IRQ_REQUEST_SIZE) irqt_request_ack(val);  /* write = ACK */
    else                                     irqt_enable_write(val);
}
static uint32_t timers_region_read(mem_region_t *r, uint32_t addr, int size) {
    (void)size; return irqt_timer_read(addr - r->base);
}
static void timers_region_write(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    (void)size; irqt_timer_write(addr - r->base, val);
}

/* ---- Init / shutdown ----------------------------------------------------- */

static inline int mem_init(memory_bus_t *bus, uint8_t *rom_data, size_t rom_size) {
    /* Re-init is safe — free any previously-allocated heap regions first. */
    free(bus->main_data);
    free(bus->xtra_data);
    free(bus->vid_ext_ram);
    free(bus->texram0);
    free(bus->texram1);
    free(bus->framebuffer);

    memset(bus, 0, sizeof(*bus));
    bus->rom = rom_data;
    bus->rom_size = rom_size;

    /* IO idles HIGH on the Model 2 (hardware pull-ups). */
    memset(bus->io, IO_IDLE_FILL, sizeof(bus->io));

    cop_reset();   /* clears g_cop / g_sharc and resets rot[] to identity */

    bus->main_data   = (uint8_t *)calloc(1, MAIN_DATA_SIZE);
    bus->xtra_data   = (uint8_t *)calloc(1, XTRA_DATA_SIZE);
    bus->vid_ext_ram = (uint8_t *)calloc(1, VID_EXT_RAM_SIZE);
    bus->texram0     = (uint8_t *)calloc(1, TEXRAM0_SIZE);
    bus->texram1     = (uint8_t *)calloc(1, TEXRAM1_SIZE);
    bus->framebuffer = (uint8_t *)calloc(1, FRAMEBUFFER_SIZE);
    if (!bus->main_data || !bus->xtra_data || !bus->vid_ext_ram || !bus->texram0 || !bus->texram1 || !bus->framebuffer) {
        LOG_ERROR("mem: heap allocation failed");
        return 0;
    }

    /* Region table — ORDER MATTERS. TILE must precede H_SYNC/V_SYNC so the
     * overlapping sync addresses route to the TILE region (where the scroll
     * regs / sync flags actually live in our model). */
    /* ROM region is sized to the actual loaded image (e.g. STF maincpu is
     * 2MB even though ROM_SIZE defaults to 1MB). When rom_size is 0 (boot
     * before any ROM has been loaded) fall back to ROM_SIZE so the region
     * still exists; reads return zero because data is NULL. */
    mem_add_region(bus, "ROM",             ROM_BASE,             rom_size ? (uint32_t)rom_size : ROM_SIZE, rom_data, 1);
    mem_add_region(bus, "RAM2",            RAM2_BASE,            RAM2_SIZE,            bus->ram2,          0);
    mem_add_region(bus, "RAM",             RAM_BASE,             RAM_SIZE,             bus->ram,           0);
    /* GEO / GEO_PROGRAM capture the set_window clip stream; COPROGRAM forwards
     * the command/arg stream to the SHARC HLE (cop.h). */
    { mem_region_t *r = mem_add_region(bus, "GEO", GEO_BASE, GEO_SIZE, bus->geo, 0);
      if (r) r->write_cb = geo_write_cb; }
    { mem_region_t *r = mem_add_region(bus, "GEO_PROGRAM", GEO_PROGRAM_BASE, GEO_PROGRAM_SIZE, bus->geo_program, 0);
      if (r) r->write_cb = geo_program_write_cb; }
    mem_add_region(bus, "GEO_CMD",         GEO_CMD_BASE,         GEO_CMD_SIZE,         bus->geo_cmd,       0);
    { mem_region_t *r = mem_add_region(bus, "COPROGRAM", COPROGRAM_BASE, COPROGRAM_SIZE, bus->coprogram, 0);
      if (r) { r->read_cb = coprogram_read_cb; r->write_cb = coprogram_write_cb; } }
    mem_add_region(bus, "COPRO_SHARC_IOP", COPRO_SHARC_IOP_BASE, COPRO_SHARC_IOP_SIZE, bus->copro_sharc,   0);
    mem_add_region(bus, "BUFF_RAM",        BUFF_RAM_BASE,        BUFF_RAM_SIZE,        bus->buff_ram,      0);
    /* Wire BUFF_RAM into the SHARC as DM[0x01400000]: the i960 writes animation
     * blobs to BUFF_RAM (0x00900000); 0x25004A4A reads them by word offset.
     * cop_reset() zeroed the field, so set it here (after reset, after alloc). */
    g_sharc.sharc_dm_ext      = bus->buff_ram;
    g_sharc.sharc_dm_ext_size = BUFF_RAM_SIZE;
    mem_add_region(bus, "COPRO_CTL",       COPRO_CONTROL1_BASE,  COPRO_CONTROL1_SIZE,  bus->copro_ctl,     0);
    mem_add_region(bus, "MIDI",            MIDI_BASE,            MIDI_SIZE,            bus->midi,          0);
    mem_add_region(bus, "CPU_CTRL",        CPU_CTRL_BASE,        CPU_CTRL_SIZE,        bus->cpu_ctrl,      0);
    /* IRQ controller (0xE80000) + 4 board timers (0xF00000) — real hardware model. */
    { mem_region_t *r = mem_add_region(bus, "IRQ", IRQ_REQUEST_BASE, sizeof(bus->irq), bus->irq, 0);
      if (r) { r->read_cb = irq_region_read; r->write_cb = irq_region_write; } }
    { mem_region_t *r = mem_add_region(bus, "TIMERS", TIMERS_BASE, TIMERS_SIZE, bus->timers, 0);
      if (r) { r->read_cb = timers_region_read; r->write_cb = timers_region_write; } }
    mem_add_region(bus, "TILE",            TILE_BASE,            TILE_SIZE,            bus->tile,          0);
    mem_add_region(bus, "TMAPGFX",         TMAPGFX_BASE,         TMAPGFX_SIZE,         bus->tmapgfx,       0);
    mem_add_region(bus, "VID_EXT_RAM",     VID_EXT_RAM_BASE,     VID_EXT_RAM_SIZE,     bus->vid_ext_ram,   0);
    mem_add_region(bus, "PALETTE",         PALETTE_BASE,         PALETTE_SIZE,         bus->palette,       0);
    mem_add_region(bus, "COLORXLAT",       COLORXLAT_BASE,       COLORXLAT_SIZE,       bus->colorxlat,     0);
    mem_add_region(bus, "ZCLIP_3D",        ZCLIP_3D_BASE,        ZCLIP_3D_SIZE,        bus->zclip_3d,      0);
    mem_add_region(bus, "IO",              IO_BASE,              IO_SIZE,              bus->io,            0);
    mem_add_region(bus, "SERIAL",          SERIAL_BASE,          SERIAL_SIZE,          bus->serial,        0);
    mem_add_region(bus, "BACK",            BACK_BASE,            BACK_SIZE,            bus->back,          0);
    mem_add_region(bus, "MAIN_DATA",       MAIN_DATA_BASE,       MAIN_DATA_SIZE,       bus->main_data,     0);
    mem_add_region(bus, "XTRA_DATA",       XTRA_DATA_BASE,       XTRA_DATA_SIZE,       bus->xtra_data,     0);
    mem_add_region(bus, "UNKNOWN_VID",     UNKNOWN_VID_BASE,     UNKNOWN_VID_SIZE,     bus->unknown_vid,   0);
    mem_add_region(bus, "TEXRAM0",         TEXRAM0_BASE,         TEXRAM0_SIZE,         bus->texram0,       0);
    mem_add_region(bus, "TEXRAM0_M",       TEXRAM0_MIRROR_BASE,  TEXRAM0_SIZE,         bus->texram0,       0);
    mem_add_region(bus, "TEXRAM1",         TEXRAM1_BASE,         TEXRAM1_SIZE,         bus->texram1,       0);
    mem_add_region(bus, "TEXRAM1_M",       TEXRAM1_MIRROR_BASE,  TEXRAM1_SIZE,         bus->texram1,       0);
    mem_add_region(bus, "LUMA",            LUMA_BASE,            LUMA_SIZE,            bus->luma,          0);
    mem_add_region(bus, "LUMA2",           LUMA2_BASE,           LUMA2_SIZE,           bus->luma2,         0);
    mem_add_region(bus, "FRAMEBUFFER",     FRAMEBUFFER_BASE,     FRAMEBUFFER_SIZE,     bus->framebuffer,   0);
    mem_add_region(bus, "IAC",             IAC_BASE,             IAC_SIZE,             bus->iac,           0);

    LOG_INFO("mem: bus initialized with %d regions, ROM=%zu bytes", bus->region_count, rom_size);
    return 1;
}

static inline void mem_shutdown(memory_bus_t *bus) {
    free(bus->main_data);   bus->main_data   = NULL;
    free(bus->xtra_data);   bus->xtra_data   = NULL;
    free(bus->vid_ext_ram); bus->vid_ext_ram = NULL;
    free(bus->texram0);     bus->texram0     = NULL;
    free(bus->texram1);     bus->texram1     = NULL;
    free(bus->framebuffer); bus->framebuffer = NULL;
}

/* ---- Lookup -------------------------------------------------------------- */

static inline mem_region_t *mem_find_region(memory_bus_t *bus, uint32_t addr) {
    for (int i = 0; i < bus->region_count; i++) {
        mem_region_t *r = &bus->regions[i];
        if (addr >= r->base && (addr - r->base) < r->size) {
            return r;
        }
    }
    return NULL;
}

/* ---- Read / Write -------------------------------------------------------- */

/* All accesses are little-endian. Unaligned accesses are permitted by the
 * i960 (with a performance penalty); we emulate that by issuing byte-wise
 * reads/writes when the underlying region has no callback. */

/* A region with no callback AND no backing buffer (e.g. ROM before any ROM
 * has been loaded) silently reads as zero and ignores writes. Without this
 * the memview window would deref NULL the moment it scrolls into ROM. */

static inline uint32_t mem_read8(memory_bus_t *bus, uint32_t addr) {
    bus->reads++;
    mem_region_t *r = mem_find_region(bus, addr);
    if (!r) {
        bus->unmapped_reads++;
        LOG_WARN("mem: unmapped read8 @ 0x%08X", addr);
        return 0;
    }
    if (r->read_cb) return r->read_cb(r, addr, 1) & 0xFF;
    if (!r->data)   return 0;
    return r->data[addr - r->base];
}

static inline uint32_t mem_read16(memory_bus_t *bus, uint32_t addr) {
    bus->reads++;
    mem_region_t *r = mem_find_region(bus, addr);
    if (!r) {
        bus->unmapped_reads++;
        LOG_WARN("mem: unmapped read16 @ 0x%08X (IP=0x%08X)", addr, bus->cpu_ip);
        return 0;
    }
    if (r->read_cb) return r->read_cb(r, addr, 2) & 0xFFFF;
    if (!r->data)   return 0;
    uint32_t off = addr - r->base;
    return (uint32_t)r->data[off] | ((uint32_t)r->data[off + 1] << 8);
}

static inline uint32_t mem_read32(memory_bus_t *bus, uint32_t addr) {
    bus->reads++;
    mem_region_t *r = mem_find_region(bus, addr);
    if (!r) {
        bus->unmapped_reads++;
        LOG_WARN("mem: unmapped read32 @ 0x%08X (IP=0x%08X)", addr, bus->cpu_ip);
        return 0;
    }
    if (r->read_cb) return r->read_cb(r, addr, 4);
    if (!r->data)   return 0;
    uint32_t off = addr - r->base;
    return  (uint32_t)r->data[off]
         | ((uint32_t)r->data[off + 1] << 8)
         | ((uint32_t)r->data[off + 2] << 16)
         | ((uint32_t)r->data[off + 3] << 24);
}

/* Set on every bus write to the issuing CPU's IP, so region write_cb handlers
 * (which only receive the region) can attribute the access to a code address. */
static uint32_t g_mem_last_write_ip = 0;

static inline void mem_write8(memory_bus_t *bus, uint32_t addr, uint32_t val) {
    bus->writes++;
    g_mem_last_write_ip = bus->cpu_ip;
    if (g_wp.count) wp_check(addr, val, true, bus->cpu_ip);
    mem_region_t *r = mem_find_region(bus, addr);
    if (!r) {
        bus->unmapped_writes++;
        LOG_WARN("mem: unmapped write8 @ 0x%08X = 0x%02X (IP=0x%08X)", addr, val & 0xFF, bus->cpu_ip);
        return;
    }
    if (r->write_cb) { r->write_cb(r, addr, val, 1); return; }
    if (r->readonly) { LOG_WARN("mem: write8 to RO %s @ 0x%08X", r->name, addr); return; }
    if (!r->data)    return;
    r->data[addr - r->base] = (uint8_t)val;
}

static inline void mem_write16(memory_bus_t *bus, uint32_t addr, uint32_t val) {
    bus->writes++;
    g_mem_last_write_ip = bus->cpu_ip;
    if (g_wp.count) wp_check(addr, val, true, bus->cpu_ip);
    mem_region_t *r = mem_find_region(bus, addr);
    if (!r) {
        bus->unmapped_writes++;
        LOG_WARN("mem: unmapped write16 @ 0x%08X = 0x%04X (IP=0x%08X)", addr, val & 0xFFFF, bus->cpu_ip);
        return;
    }
    if (r->write_cb) { r->write_cb(r, addr, val, 2); return; }
    if (r->readonly) { LOG_WARN("mem: write16 to RO %s @ 0x%08X", r->name, addr); return; }
    if (!r->data)    return;
    uint32_t off = addr - r->base;
    r->data[off]     = (uint8_t)(val & 0xFF);
    r->data[off + 1] = (uint8_t)((val >> 8) & 0xFF);
}

static inline void mem_write32(memory_bus_t *bus, uint32_t addr, uint32_t val) {
    bus->writes++;
    g_mem_last_write_ip = bus->cpu_ip;
    if (g_wp.count) wp_check(addr, val, true, bus->cpu_ip);
    mem_region_t *r = mem_find_region(bus, addr);
    if (!r) {
        bus->unmapped_writes++;
        LOG_WARN("mem: unmapped write32 @ 0x%08X = 0x%08X (IP=0x%08X)", addr, val, bus->cpu_ip);
        return;
    }
    if (r->write_cb) { r->write_cb(r, addr, val, 4); return; }
    if (r->readonly) { LOG_WARN("mem: write32 to RO %s @ 0x%08X", r->name, addr); return; }
    if (!r->data)    return;
    uint32_t off = addr - r->base;
    r->data[off]     = (uint8_t)(val & 0xFF);
    r->data[off + 1] = (uint8_t)((val >> 8) & 0xFF);
    r->data[off + 2] = (uint8_t)((val >> 16) & 0xFF);
    r->data[off + 3] = (uint8_t)((val >> 24) & 0xFF);
}

/* Convenience: install MMIO callbacks on the region containing `addr`. */
static inline void mem_install_callbacks(memory_bus_t *bus, uint32_t addr,
                                         mem_read_cb rcb, mem_write_cb wcb, void *user) {
    mem_region_t *r = mem_find_region(bus, addr);
    if (!r) { LOG_ERROR("mem: install_callbacks: no region at 0x%08X", addr); return; }
    r->read_cb = rcb;
    r->write_cb = wcb;
    r->user = user;
}

#endif /* MEMORY_H */
