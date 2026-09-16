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

#include <stdbool.h>
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
    /* Not a burst device: a multi-word load/store (ldl/ldt/ldq, stl/stt/stq)
     * hits the SAME address for every word instead of walking +4. Mirrors the
     * regions MAME's model2 map leaves without i960_cpu_device::BURST. STF
     * leans on it: `ldl TIMER_03, r14` snapshots one timer into two registers
     * (osage's frame-time budget); walking +4 read timer 3 instead and killed
     * the hair/cape physics two frames in three. */
    int          no_burst;
    /* An earlier region overlaps this one, so a lookup must not trust a cached
     * hit on it — the linear scan's first-match order decides (TILE_MIRROR
     * before TILE, TILE before H_SYNC). */
    int          shadowed;
    /* Change counter this region's writes feed (NULL: untracked). A write that
     * changes a byte bumps it, so a consumer that recorded the value can skip
     * work nothing changed: tile compositing (bus->gen_tile, gen_gfx, gen_pal),
     * the texture atlas (gen_tex), the luma/colorxlat tables (gen_lut). */
    volatile uint32_t *change_gen;
    /* One flag per KB of the region (NULL: untracked), set by a write that
     * changes a byte in that KB, after the bytes are stored; the consumer clears
     * a flag and then reads the KB, so no change is lost. Texture RAM, for the
     * atlas (game_render_upload_atlas). */
    volatile uint8_t  *dirty_kb;
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
    /* The two most recent unshadowed hits: code fetches and data accesses
     * alternate between two regions, and each lookup would otherwise scan. */
    mem_region_t *hit[2];
    /* The plain memory region the CPU last fetched an instruction from
     * (mem_fetch2): reading the two words at IP straight out of it skips two
     * whole bus reads on every instruction. */
    mem_region_t *fetch;

    /* Bumped by every write that changes a tracked region (see change_gen):
     * tile RAM, tile graphics, palette, texture RAM, luma + colorxlat. */
    volatile uint32_t gen_tile, gen_gfx, gen_pal, gen_tex, gen_lut;
    /* Per-KB change flags of texture RAM bank 0 and 1, aliases included (see
     * dirty_kb). All set at init: nothing decoded matches the new bus yet. One
     * spare entry takes a multi-byte write that starts in the last byte. */
    volatile uint8_t  tex_dirty[2][(TEXRAM0_SIZE >> 10) + 1];

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
    r->no_burst = 0;
    r->shadowed = 0;
    r->change_gen = NULL;
    r->dirty_kb = NULL;
    for (int i = 0; i < bus->region_count - 1; i++) {
        const mem_region_t *e = &bus->regions[i];
        if ((uint64_t)e->base < (uint64_t)base + size && (uint64_t)base < (uint64_t)e->base + e->size)
            r->shadowed = 1;
    }
    bus->hit[0] = bus->hit[1] = NULL;
    bus->fetch = NULL;
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

/* ---- GEO display list (board-level) -------------------------------------
 * The i960 does not hand the geometrizer draw calls: it builds a display list in
 * bufferram (0x900000, the SHARC's DM 0x1400000) and the GEO walks it once a
 * frame (MAME model2.cpp geo_w / push_geo_data, model2_v.cpp geo_parse):
 *   GEO + 0x000..0xFFF  a write here pushes one encoded word at the write
 *                       pointer: function (offset >> 4) & 0x3F in bits 23..28,
 *                       the data's low 20 bits (bit 31 set: a jump, 0x800FFFFF)
 *   GEO + 0x1008        write pointer (read back at + 0x2008)
 *   GEO + 0x3008        read pointer: where the next frame's walk starts
 *   GEO_PROGRAM 0x804000  a raw word pushed at the write pointer (window data),
 *                       unless the GEO is taking a firmware upload (geo_ctl1,
 *                       0x980008, bit 31)
 * The COP writes into the same list: put_poly lays a matrix command and an
 * object command at the pointer the i960 passes it, and the i960 links lists
 * with jump words it stores straight into bufferram.
 *
 * STF finishes a frame's list in set_end_mark: it pushes END, points the read
 * pointer at the list and moves the write pointer on to the next of four
 * buffers. So the moment the read pointer is written, the list it names is
 * complete; that is when it is copied out for the renderer, which walks the
 * copy from the UI thread. Two copies alternate so a walk never reads one being
 * written. */
static struct {
    uint32_t       wstart, rstart;        /* 20-bit byte offsets into bufferram */
    uint8_t       *buff;                  /* memory_bus_t::buff_ram */
    const uint8_t *copro_ctl;             /* for geo_ctl1's upload bit */
} g_geo;

static uint32_t     g_geodl_snaps[2][BUFF_RAM_SIZE / 4];
static uint32_t    *g_geodl_snap          = g_geodl_snaps[0];
static uint32_t     g_geodl_snap_rstart   = 0;
static volatile int g_geodl_snap_ready    = 0;
static volatile int g_geodl_snap_seq      = 0;

/* What a display list leaves behind in the geometrizer for later frames
 * (model2_v.cpp): texture RAM (command 4, addresses with bit 23), the two
 * polygon RAMs objects can be built in (command 5; bit 24 of the address picks
 * the fast one), the 32 material slots (command 6: diffuse, ambient, specular
 * scale and control, and the slot's LOD distance coefficient) and log RAM
 * (command 0x14, or command 4 to an address without bit 23 — the rasterizer's
 * log2 table for the mantissa of a polygon's LOD distance). A list can upload
 * once and draw for many frames, so these are applied on the emulator thread
 * to every list as it is published, not by the renderer, which only ever sees
 * the latest. */
static uint16_t g_geo_texram_words[0x10000];
static uint32_t g_geo_polyram[2][0x8000];          /* [0] slow, [1] fast */
static float    g_geo_texparam[32][4];             /* diffuse, ambient, specular scale, specular control */
static float    g_geo_coef[32];                    /* distance coefficient, indexed by attribute >> 27 */
static uint8_t  g_geo_logram[0x8000];

static inline void geodl_apply_state(const uint32_t *L, uint32_t nw, uint32_t rstart) {
    uint32_t p = (rstart & 0x1FFFFu) >> 2;
    for (uint32_t guard = 0; guard < 0x8000u && p < nw; guard++) {
        uint32_t op = L[p];
        if (op & 0x80000000u) { p = (op & 0x1FFFFu) >> 2; continue; }
        uint32_t cmd = (op >> 23) & 0x1F;
        #define LA(k) (p + 1u + (k) < nw ? L[p + 1u + (k)] : 0u)
        uint32_t len = 0;
        switch (cmd) {
            case 0x01: case 0x11: len = 4; break;
            case 0x03: case 0x13: len = 6; break;
            case 0x04: {
                uint32_t addr = LA(0), cnt = LA(1);
                len = 2 + cnt;
                for (uint32_t k = 0; k < cnt; k++) {
                    if (addr & 0x800000u) g_geo_texram_words[(addr + k) & 0xFFFFu] = (uint16_t)LA(2 + k);
                    else                  g_geo_logram[(addr + k) & 0x7FFFu] = (uint8_t)LA(2 + k);
                }
                break;
            }
            case 0x14: {                    /* log data: a byte at a time, through command 4's write */
                uint32_t addr = LA(0), cnt = LA(1);
                len = 2 + cnt;
                for (uint32_t k = 0; k < 4u * cnt; k++) {
                    uint8_t b = (uint8_t)(LA(2 + k / 4u) >> (8u * (k % 4u)));
                    if (addr & 0x800000u) g_geo_texram_words[(addr + k) & 0xFFFFu] = b;
                    else                  g_geo_logram[(addr + k) & 0x7FFFu] = b;
                }
                break;
            }
            case 0x05: case 0x15: {
                uint32_t addr = LA(0), cnt = LA(1);
                len = 2 + cnt;
                uint32_t *ram = g_geo_polyram[(addr & 0x01000000u) ? 1 : 0];
                for (uint32_t k = 0; k < cnt; k++) ram[(addr + k) & 0x7FFFu] = LA(2 + k);
                break;
            }
            case 0x06: {
                uint32_t index = LA(0) >> 2, cnt = LA(1);
                len = 2 + 2 * cnt;
                for (uint32_t k = 0; k < cnt; k++, index++) {
                    uint32_t param = LA(2 + 2 * k), coef = LA(3 + 2 * k);
                    g_geo_texparam[index & 0x1F][0] = (float)(param & 0xFF);
                    g_geo_texparam[index & 0x1F][1] = (float)((param >> 8) & 0xFF);
                    g_geo_texparam[index & 0x1F][2] = (float)((param >> 16) & 0xFF);
                    g_geo_texparam[index & 0x1F][3] = (float)((param >> 24) & 0xFF);
                    memcpy(&g_geo_coef[index & 0x1F], &coef, 4);
                }
                break;
            }
            case 0x07: case 0x17: case 0x08: case 0x18: case 0x10: case 0x16: case 0x1E: len = 1; break;
            case 0x09: case 0x19: case 0x0D: len = 2; break;
            case 0x0A: case 0x1A: case 0x0C: case 0x1C: len = 3; break;
            case 0x0B: case 0x1B: len = 12; break;
            case 0x1D: len = 2 + 3 * LA(1); break;
            case 0x02: case 0x12: case 0x0F: case 0x1F: return;   /* inline polygons (unwalkable) / END */
            default: break;
        }
        #undef LA
        p += 1u + len;
    }
}

static inline void geo_push(uint32_t word) {
    if (!g_geo.buff) return;
    uint32_t w = (g_geo.wstart >> 2) & (BUFF_RAM_SIZE / 4 - 1);
    memcpy(g_geo.buff + w * 4u, &word, 4);
    g_geo.wstart = (g_geo.wstart + 4u) & 0xFFFFFu;
}

/* Copy bufferram out as the list the next frame draws, starting at rstart. */
static inline void geodl_publish(uint32_t rstart) {
    if (!g_geo.buff) return;
    uint32_t *back = (g_geodl_snap == g_geodl_snaps[0]) ? g_geodl_snaps[1] : g_geodl_snaps[0];
    memcpy(back, g_geo.buff, sizeof g_geodl_snaps[0]);
    geodl_apply_state(back, BUFF_RAM_SIZE / 4, rstart);
    g_geodl_snap_rstart = rstart;
    g_geodl_snap        = back;
    g_geodl_snap_seq++;
    g_geodl_snap_ready  = 1;
}

/* GEO base (0x800000). set_window also writes 0x303 to offset 0x30 before its
 * six window words; the COP-stream scanner still keys clip windows off that. */
static void geo_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - GEO_BASE;
    if (r->data && off + 4 <= r->size)
        memcpy(r->data + off, &val, 4);
    if (size != 4) return;
    if (off == 0x30 && val == 0x303)
        geo_win_start();
    if (off < 0x1000) {
        uint32_t function = (off >> 4) & 0x3F;
        if (val & 0x80000000u) {
            geo_push((val & 0x800FFFFFu) | (function << 23));
        } else if ((off & 0xF) == 0) {
            uint32_t word = (val & 0x000FFFFFu) | (function << 23);
            if (((off >> 4) & 0xC0) && function == 1)
                word |= ((off >> 10) & 3u) << 29;          /* eye mode: which projection centre */
            geo_push(word);
        }
    } else if (off == 0x1008) {
        g_geo.wstart = val & 0xFFFFFu;
    } else if (off == 0x3008) {
        g_geo.rstart = val & 0xFFFFFu;
        geodl_publish(g_geo.rstart);
    }
}

static uint32_t geo_read_cb(mem_region_t *r, uint32_t addr, int size) {
    uint32_t off = addr - GEO_BASE;
    if (off == 0x2008) return g_geo.wstart;
    if (off == 0x3008) return g_geo.rstart;
    uint32_t v = 0;
    if (r->data && off + 4 <= r->size) memcpy(&v, r->data + off, 4);
    return size == 1 ? (v & 0xFF) : size == 2 ? (v & 0xFFFF) : v;
}

/* GEO_PROGRAM (0x804000): raw list words — set_window's six. */
static void geo_program_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - GEO_PROGRAM_BASE;
    if (r->data && off + 4 <= r->size)
        memcpy(r->data + off, &val, 4);
    if (size != 4) return;
    if (off == 0)
        geo_win_push(val);
    int uploading = g_geo.copro_ctl && (g_geo.copro_ctl[11] & 0x80);
    if (!uploading) geo_push(val);
}

/* Publish bufferram as it stands. The homebrew's frame path calls this at slice
 * end, where it is vblank-waiting just past its flush. */
static inline void geodl_capture(const memory_bus_t *bus) {
    if (!bus) return;
    geodl_publish(g_geo.rstart);
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
    /* Every (re)init is new content: start past any generation a consumer
     * could have recorded from the previous bus. */
    static uint32_t s_init_count = 0;
    bus->gen_tile = bus->gen_gfx = bus->gen_pal = bus->gen_tex = bus->gen_lut = ++s_init_count << 20;

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
      if (r) { r->read_cb = geo_read_cb; r->write_cb = geo_write_cb; } }
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
    g_geo.wstart = g_geo.rstart = 0;
    g_geo.buff      = bus->buff_ram;
    g_geo.copro_ctl = bus->copro_ctl;
    g_geodl_snap_ready = 0;
    mem_add_region(bus, "COPRO_CTL",       COPRO_CONTROL1_BASE,  COPRO_CONTROL1_SIZE,  bus->copro_ctl,     0);
    mem_add_region(bus, "MIDI",            MIDI_BASE,            MIDI_SIZE,            bus->midi,          0);
    mem_add_region(bus, "CPU_CTRL",        CPU_CTRL_BASE,        CPU_CTRL_SIZE,        bus->cpu_ctrl,      0);
    /* IRQ controller (0xE80000) + 4 board timers (0xF00000) — real hardware model. */
    { mem_region_t *r = mem_add_region(bus, "IRQ", IRQ_REQUEST_BASE, sizeof(bus->irq), bus->irq, 0);
      if (r) { r->read_cb = irq_region_read; r->write_cb = irq_region_write; } }
    { mem_region_t *r = mem_add_region(bus, "TIMERS", TIMERS_BASE, TIMERS_SIZE, bus->timers, 0);
      if (r) { r->read_cb = timers_region_read; r->write_cb = timers_region_write; } }
    /* Tile RAM is 64K and repeats at 0x01010000 (MAME: mirror 0x110000). Games
     * lean on it: STF's NEXT MATCH places a nameplate one tile left with a
     * zero-extended -2 byte offset (0xFFFE), so Knuckles' and Metal Sonic's
     * plates are written at 0x0101xxxx. It shares TILE's buffer and must come
     * before TILE, whose flat span also covers these addresses. */
    mem_add_region(bus, "TILE_MIRROR",     TILE_BASE + 0x10000u, 0x10000u,             bus->tile,          0);
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
    mem_add_region(bus, "TEXRAM0_A",       TEXRAM0_ALIAS_BASE,   TEXRAM0_SIZE,         bus->texram0,       0);
    mem_add_region(bus, "TEXRAM0",         TEXRAM0_BASE,         TEXRAM0_SIZE,         bus->texram0,       0);
    mem_add_region(bus, "TEXRAM0_M",       TEXRAM0_MIRROR_BASE,  TEXRAM0_SIZE,         bus->texram0,       0);
    mem_add_region(bus, "TEXRAM1",         TEXRAM1_BASE,         TEXRAM1_SIZE,         bus->texram1,       0);
    mem_add_region(bus, "TEXRAM1_M",       TEXRAM1_MIRROR_BASE,  TEXRAM1_SIZE,         bus->texram1,       0);
    mem_add_region(bus, "LUMA",            LUMA_BASE,            LUMA_SIZE,            bus->luma,          0);
    mem_add_region(bus, "LUMA2",           LUMA2_BASE,           LUMA2_SIZE,           bus->luma2,         0);
    mem_add_region(bus, "FRAMEBUFFER",     FRAMEBUFFER_BASE,     FRAMEBUFFER_SIZE,     bus->framebuffer,   0);
    mem_add_region(bus, "IAC",             IAC_BASE,             IAC_SIZE,             bus->iac,           0);

    /* MMIO the board does not burst (model2.cpp: no i960_cpu_device::BURST). */
    static const char *const no_burst[] = {
        "GEO", "GEO_PROGRAM", "GEO_CMD", "COPROGRAM", "COPRO_SHARC_IOP", "COPRO_CTL",
        "MIDI", "CPU_CTRL", "IRQ", "TIMERS", "ZCLIP_3D", "IO", "UNKNOWN_VID",
    };
    for (int i = 0; i < bus->region_count; i++)
        for (size_t k = 0; k < sizeof no_burst / sizeof no_burst[0]; k++)
            if (strcmp(bus->regions[i].name, no_burst[k]) == 0) bus->regions[i].no_burst = 1;
    for (int i = 0; i < bus->region_count; i++) {
        mem_region_t *r = &bus->regions[i];
        if (!strcmp(r->name, "TILE") || !strcmp(r->name, "TILE_MIRROR"))   /* one buffer */
            r->change_gen = &bus->gen_tile;
        else if (!strcmp(r->name, "TMAPGFX"))
            r->change_gen = &bus->gen_gfx;
        else if (!strcmp(r->name, "PALETTE"))
            r->change_gen = &bus->gen_pal;
        else if (!strncmp(r->name, "TEXRAM", 6)) { /* both banks and all their aliases */
            r->change_gen = &bus->gen_tex;
            r->dirty_kb   = bus->tex_dirty[r->data == bus->texram1 ? 1 : 0];
        }
        else if (!strcmp(r->name, "LUMA") || !strcmp(r->name, "COLORXLAT"))
            r->change_gen = &bus->gen_lut;
    }

    memset((uint8_t *)bus->tex_dirty, 1, sizeof bus->tex_dirty);

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
    /* A cached region is never shadowed, so no earlier region can contain addr:
     * it is exactly what the scan below would return. */
    mem_region_t *c = bus->hit[0];
    if (c && (addr - c->base) < c->size) return c;
    c = bus->hit[1];
    if (c && (addr - c->base) < c->size) {
        bus->hit[1] = bus->hit[0];
        bus->hit[0] = c;
        return c;
    }
    for (int i = 0; i < bus->region_count; i++) {
        mem_region_t *r = &bus->regions[i];
        if (addr >= r->base && (addr - r->base) < r->size) {
            if (!r->shadowed) {
                bus->hit[1] = bus->hit[0];
                bus->hit[0] = r;
            }
            return r;
        }
    }
    return NULL;
}

/* How far a multi-word load/store advances after the word at `addr`. */
static inline uint32_t mem_burst_step(memory_bus_t *bus, uint32_t addr) {
    mem_region_t *r = mem_find_region(bus, addr);
    return (r && r->no_burst) ? 0 : 4;
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

/* The two words at addr, as mem_read32(addr) and mem_read32(addr + 4) return
 * them: the CPU's instruction fetch. When both lie in the region the last fetch
 * came from and that region is plain memory (unshadowed, no read callback, with
 * backing), they are read from it directly; otherwise through the bus, and the
 * region is remembered if it qualifies. The callback is checked on every fetch
 * because a region can be given one after it is added. */
static inline void mem_fetch2(memory_bus_t *bus, uint32_t addr, uint32_t *w1, uint32_t *w2) {
    mem_region_t *r = bus->fetch;
    if (r) {
        uint32_t off = addr - r->base;
        if ((uint64_t)off + 8u <= r->size && !r->read_cb) {
            const uint8_t *p = r->data + off;
            bus->reads += 2;
            *w1 = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            *w2 = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
            return;
        }
    }
    *w1 = mem_read32(bus, addr);
    *w2 = mem_read32(bus, addr + 4);
    r = mem_find_region(bus, addr);
    if (r && !r->shadowed && !r->read_cb && r->data && (uint64_t)(addr - r->base) + 8u <= r->size)
        bus->fetch = r;
}

/* Set on every bus write to the issuing CPU's IP, so region write_cb handlers
 * (which only receive the region) can attribute the access to a code address. */
static uint32_t g_mem_last_write_ip = 0;

/* ---- Display-list tap ------------------------------------------------------
 * Every write the i960 makes to the geometry processor and the coprocessor,
 * 0x800000..0x8CFFFF, in the order it makes them, with a mark at each frame
 * edge. That is exactly what the explorer toolkit's MAME tap records
 * (stf-tools/mame-dl-capture.lua), in the same shape, so its display-list
 * checks read a capture from here unchanged: both ports in write order, which
 * is what places a part handed straight to the geometry processor at the
 * matrix the coprocessor had built by then.
 *
 * The recording is armed by the capture_dl bridge command, starts at the next
 * frame edge so the first slice is a whole frame, and stops itself once it has
 * the marks it was asked for. The probes read at each edge are addresses the
 * caller names — which game variables tie a frame to the game's own clock is
 * the caller's knowledge, not the bus's. */
#define DL_TAP_LO        0x00800000u
#define DL_TAP_HI        0x008D0000u
#define DL_MAX_PROBES    96
#define DL_MAX_BLOCKS    8
#define DL_BLOCK_BYTES   0x20000u        /* per mark, all blocks together */
#define DL_SLOT_WORDS    (2 * 16 * 12)   /* both fighters' TGP slots, bufferram words 0x3A00.. / 0x3B00.. */

typedef struct { uint32_t addr, val; } dl_rec_t;
typedef struct { uint32_t frame, index, probe[DL_MAX_PROBES]; } dl_mark_t;

static struct {
    volatile int armed, active, done;
    uint32_t     want;                  /* frames wanted, so want + 1 marks */
    dl_rec_t    *recs;
    size_t       n, cap;
    int          overflow;
    dl_mark_t   *marks;
    size_t       nmarks, capmarks;
    int          nprobes;
    uint32_t     probe_addr[DL_MAX_PROBES];
    uint8_t      probe_size[DL_MAX_PROBES];
    uint32_t     lo, hi;                /* the part of DL_TAP_LO..HI to record */
    float       *tgp;                   /* NULL, or capmarks × 32 × 12 bone-slot floats */
    uint32_t    *slots;                 /* NULL, or capmarks × DL_SLOT_WORDS words out of bufferram */
    float       *unit;                  /* NULL, or capmarks × 32 × 12 unit-matrix cache floats */
    int          nblocks;               /* whole RAM ranges copied at every mark */
    uint32_t     block_addr[DL_MAX_BLOCKS], block_len[DL_MAX_BLOCKS];
    uint32_t     block_bytes;           /* sum of block_len */
    uint8_t     *blocks;                /* NULL, or capmarks × block_bytes */
    /* cop: record the coprocessor conversation instead of FIFO writes — cop.h's
     * g_cop_tap tags plus the i960's 32-bit bufferram writes, a MAME SHARC-side
     * capture's format — with bufferram and SHARC DM 0x30000-0x30FFF as they
     * stood when the capture began. */
    int          cop;
    uint8_t     *cop_bufram;            /* BUFF_RAM_SIZE bytes */
    uint32_t    *cop_dm;                /* 0x1000 words */
} g_dl;

static inline void dl_record(uint32_t addr, uint32_t val) {
    if (g_dl.n < g_dl.cap) {
        g_dl.recs[g_dl.n].addr = addr;
        g_dl.recs[g_dl.n].val  = val;
        g_dl.n++;
    } else {
        g_dl.overflow = 1;
    }
}

static inline void dl_tap(uint32_t addr, uint32_t val) {
    if (!g_dl.active || g_dl.cop || addr - g_dl.lo >= g_dl.hi - g_dl.lo) return;
    dl_record(addr, val);
}

static void dl_cop_tap(uint32_t tag, uint32_t val) {
    if (g_dl.active) dl_record(tag, val);
}

/* A plain write changed bytes off..off+len-1 of r, already stored: bump the
 * region's change counter, then flag the KBs it touched. */
static inline void mem__note_change(mem_region_t *r, uint32_t off, uint32_t len) {
    (*r->change_gen)++;
    if (r->dirty_kb) {
        r->dirty_kb[off >> 10] = 1;
        r->dirty_kb[(off + len - 1u) >> 10] = 1;
    }
}

static inline void mem_write8(memory_bus_t *bus, uint32_t addr, uint32_t val) {
    bus->writes++;
    g_mem_last_write_ip = bus->cpu_ip;
    if (g_wp.count) wp_check(addr, val, true, bus->cpu_ip);
    dl_tap(addr, val & 0xFF);
    mem_region_t *r = mem_find_region(bus, addr);
    if (!r) {
        bus->unmapped_writes++;
        LOG_WARN("mem: unmapped write8 @ 0x%08X = 0x%02X (IP=0x%08X)", addr, val & 0xFF, bus->cpu_ip);
        return;
    }
    if (r->write_cb) { r->write_cb(r, addr, val, 1); return; }
    if (r->readonly) { LOG_WARN("mem: write8 to RO %s @ 0x%08X", r->name, addr); return; }
    if (!r->data)    return;
    uint32_t off = addr - r->base;
    bool changed = r->change_gen && r->data[off] != (uint8_t)val;
    r->data[off] = (uint8_t)val;
    if (changed) mem__note_change(r, off, 1);
}

static inline void mem_write16(memory_bus_t *bus, uint32_t addr, uint32_t val) {
    bus->writes++;
    g_mem_last_write_ip = bus->cpu_ip;
    if (g_wp.count) wp_check(addr, val, true, bus->cpu_ip);
    dl_tap(addr, val & 0xFFFF);
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
    bool changed = r->change_gen && (r->data[off] | (r->data[off + 1] << 8)) != (val & 0xFFFF);
    r->data[off]     = (uint8_t)(val & 0xFF);
    r->data[off + 1] = (uint8_t)((val >> 8) & 0xFF);
    if (changed) mem__note_change(r, off, 2);
}

static inline void mem_write32(memory_bus_t *bus, uint32_t addr, uint32_t val) {
    bus->writes++;
    g_mem_last_write_ip = bus->cpu_ip;
    if (g_wp.count) wp_check(addr, val, true, bus->cpu_ip);
    dl_tap(addr, val);
    if (g_dl.active && g_dl.cop && addr - BUFF_RAM_BASE < BUFF_RAM_SIZE) dl_record(addr, val);
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
    bool changed = r->change_gen && ((uint32_t)r->data[off] | ((uint32_t)r->data[off + 1] << 8)
                                     | ((uint32_t)r->data[off + 2] << 16) | ((uint32_t)r->data[off + 3] << 24)) != val;
    r->data[off]     = (uint8_t)(val & 0xFF);
    r->data[off + 1] = (uint8_t)((val >> 8) & 0xFF);
    r->data[off + 2] = (uint8_t)((val >> 16) & 0xFF);
    r->data[off + 3] = (uint8_t)((val >> 24) & 0xFF);
    if (changed) mem__note_change(r, off, 4);
}

/* A frame edge: the run loop calls this, under the emu mutex, when the game's
 * frame has ended and before the next instruction runs. */
static inline void dl_frame_edge(memory_bus_t *bus, uint32_t frame) {
    if (g_dl.armed && !g_dl.active && !g_dl.done) {
        g_dl.active = 1;
        if (g_dl.cop_bufram) memcpy(g_dl.cop_bufram, bus->buff_ram, BUFF_RAM_SIZE);
        if (g_dl.cop_dm)
            for (uint32_t k = 0; k < 0x1000u; k++) g_dl.cop_dm[k] = sharc_dm_get(0x30000u + k);
    }
    if (!g_dl.active) return;
    if (g_dl.nmarks < g_dl.capmarks) {
        dl_mark_t *m = &g_dl.marks[g_dl.nmarks++];
        m->frame = frame;
        m->index = (uint32_t)g_dl.n;
        for (int i = 0; i < g_dl.nprobes; i++) {
            uint32_t a = g_dl.probe_addr[i];
            m->probe[i] = g_dl.probe_size[i] == 1 ? mem_read8(bus, a)
                        : g_dl.probe_size[i] == 2 ? mem_read16(bus, a)
                        :                           mem_read32(bus, a);
        }
        /* Both fighters' TGP bone slots as the coprocessor HLE holds them at the
         * frame edge — the rig the frame was drawn with. */
        if (g_dl.tgp)
            memcpy(g_dl.tgp + (g_dl.nmarks - 1) * sizeof g_sharc.tgp_bone / sizeof(float),
                   g_sharc.tgp_bone, sizeof g_sharc.tgp_bone);
        /* The same slots as the i960 can see them — bufferram, which is SHARC
         * DM 0x1400000: what op 0x67 stored, laid out the way a MAME capture
         * reads them out of i960 0x90E800 / 0x90EC00. */
        if (g_dl.slots)
            memcpy(g_dl.slots + (g_dl.nmarks - 1) * DL_SLOT_WORDS,
                   bus->buff_ram + 0x3A00u * 4u, DL_SLOT_WORDS * 4u);
        /* The coprocessor's unit-matrix cache (op 0x35 stores, 0x36/0x37 loads). */
        if (g_dl.unit)
            memcpy(g_dl.unit + (g_dl.nmarks - 1) * sizeof g_sharc.rot_cache / sizeof(float),
                   g_sharc.rot_cache, sizeof g_sharc.rot_cache);
        /* Whole RAM ranges, byte for byte, in the order they were asked for. */
        if (g_dl.blocks) {
            uint8_t *dst = g_dl.blocks + (g_dl.nmarks - 1) * (size_t)g_dl.block_bytes;
            for (int b = 0; b < g_dl.nblocks; b++)
                for (uint32_t k = 0; k < g_dl.block_len[b]; k++) *dst++ = mem_read8(bus, g_dl.block_addr[b] + k);
        }
    }
    if (g_dl.nmarks > g_dl.want || g_dl.nmarks >= g_dl.capmarks || g_dl.overflow) {
        g_dl.active = 0;
        g_dl.done   = 1;
    }
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
