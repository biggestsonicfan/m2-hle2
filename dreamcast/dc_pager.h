/*
 * dc_pager.h -- demand paging for the Dreamcast build (Pinboard #340, #443).
 *
 * A Dreamcast has 16 MB and a set is ~60 MB, so the ROM stays on the disc and
 * comes in 16 KB at a time. Every region is a window of addresses
 * (dc_paged.h) that only names its pages; nothing dereferences them. The
 * i960's bus is the TLB: its slow path asks pg_bus_in for the page, which
 * reads it off the disc into a frame if it is not resident and puts the frame
 * in the bus's direct page tables, so the next access to it is a plain load.
 * The 3D decoder and the profile ask dc_rom_at. A frame that goes takes its
 * entries out of the tables with it.
 *
 * It used to be the SH-4's MMU (4 KB pages, a TLB miss into pg_map), which
 * the board could not tell from memory. Flycast emulates that MMU only for
 * Windows CE, and with it on charges the first memory operations of every
 * block extra: a third of a fight frame (DREAMCAST-PORT.md, #394 part 12).
 *
 * Windows:
 *   - ROM: every region of the layout (dc_layout.h), one after another. A page
 *     is eight sectors of one of the ROM files on the disc, or a fill byte.
 *     Frames are evicted by a clock: a frame used since the hand last passed
 *     is kept. A page the board writes (MAIN_DATA, XTRA_DATA) is pinned: it
 *     would come back off the disc without the write.
 *   - anonymous: zero until first touched, then pinned (vid_ext_ram, 7 MB of
 *     which a game uses a little). pg_anon_clear() zeroes them for a reset.
 *
 * The model pack (MODELS.PAK, tools/dc_mdlpack.py): the polygon and texture
 * ROM bytes the 3D decoder reads, scene by scene, in the order the game first
 * reads them. In the ROM one scene's meshes and UV streams lie scattered over
 * 32 MB, a page or two per model. dc_rom_at serves a read the pack holds from
 * the pack's own window, after the ROM's; anything else comes from the ROM.
 * Without the file on the disc everything does.
 *
 * The disc: pg_read() talks to the GD-ROM syscalls directly and polls (it was
 * written for the miss handler, an exception, where KOS's cdrom driver may not
 * be used). Nothing else may use the drive once the pager is up.
 */
#ifndef DC_PAGER_H
#define DC_PAGER_H

#include <kos.h>
#include <dc/cdrom.h>
#include <dc/syscalls.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dc_paged.h"
#include "dc_layout.h"
#include "dc_strips.h"

#define PG_SHIFT      14
#define PG_SIZE       (1u << PG_SHIFT)
#define PG_SRC_ANON   0xFFFFFFFEu    /* src: anonymous; below 0x100: a fill byte; else a FAD */
#define PG_MAX_ANON   8
#define PG_MAX_PART   32             /* pages a file ends inside */
#define PG_SLOTS      4              /* bus pages a frame can be seen at (XTRA_DATA mirrors MAIN_DATA) */
#define PG_RECENT     4              /* frames dc_rom_at handed out last: not evicted */

typedef struct {
    uint32_t vpage;                  /* window page it holds, or ~0 */
    uint8_t  ref, pinned, dirty, nslot;
    uint16_t slot[PG_SLOTS];         /* the bus's direct pages that point at it */
} pg_frame_t;

typedef struct {
    const dc_layout_t *lay;
    uint32_t          first[DC_REGIONS];   /* each region's first window page */
    uint32_t         *src;           /* per window page: FAD, fill byte or PG_SRC_ANON */
    struct { uint32_t v; uint16_t valid; uint8_t fill; } part[PG_MAX_PART];
    int               npart;
    uint16_t         *frame;         /* per window page: frame + 1, 0 = not resident */
    uint32_t          npages;        /* window pages (ROM + anonymous) */
    uint32_t          nrom;          /* the first nrom are the ROM's */
    pg_frame_t       *fr;
    uint8_t          *pool;
    uint32_t          nframes;
    uint32_t          hand;          /* the clock's */
    uint32_t          recent[PG_RECENT], rhead;
    memory_bus_t     *bus;           /* whose tables hold the frames' slots */
    struct { uint32_t va, size; } anon[PG_MAX_ANON];
    int               nanon;
    /* counters */
    uint32_t refills, loads, zero_fills, anon_fills, evictions, read_errors, rom_writes, bounces;
    uint64_t read_ns;
    uint32_t rg_loads[DC_REGIONS];   /* loads by the region the page is in */
    uint32_t at_loads;               /* loads for dc_rom_at, not the bus */
    uint32_t pak_loads;              /* of the model pack's pages */
    uint8_t  in_at;
    /* the model pack */
    uint32_t          pak_first, pak_pages;    /* its window pages, after the ROM's */
    const uint32_t   *pak;           /* npak entries: address (polygons, then textures), length, pack offset */
    uint32_t          npak;
    /* the strip pack (dc_strips.h), read only by pg_sp_copy */
    uint32_t          sp_first, sp_pages, sp_loads;
    const dcs_head_t  *sp;           /* its header and, after it, its index */
} pager_t;

static pager_t g_pg;

/* The bus hands its page tables' pages to pg_bus_in: they must be the same. */
_Static_assert(MEM_PAGE_SHIFT == PG_SHIFT, "build with -DMEM_PAGE_SHIFT=14");

/* ---- the drive -------------------------------------------------------------- */

static int pg_read(void *dst, uint32_t fad, uint32_t nsec) {
    cd_read_params_t p = { .start_sec = fad, .num_sec = nsec, .buffer = dst, .is_test = 0 };
    cd_cmd_chk_status_t st;
    gdc_cmd_hnd_t h;
    for (int tries = 0; (h = syscall_gdrom_send_command(CD_CMD_PIOREAD, &p)) <= 0; tries++) {
        syscall_gdrom_exec_server();
        if (tries > 1000000) return -1;
    }
    for (uint32_t spin = 0; spin < 100000000u; spin++) {
        syscall_gdrom_exec_server();
        int r = syscall_gdrom_check_command(h, &st);
        if (r == CD_CMD_COMPLETED) return 0;
        if (r == CD_CMD_FAILED || r == CD_CMD_NOT_FOUND) return -1;
    }
    syscall_gdrom_abort_command(h);   /* or the drive is still busy with it at the next read */
    syscall_gdrom_exec_server();
    return -1;
}

/* A page the board reads has to be the ROM's: a few tries, then stop, rather
 * than run on with made-up bytes (the board would quietly be another game). */
#define PG_READ_TRIES 4
static void pg_read_page(void *dst, uint32_t fad, uint32_t nsec, uint32_t v) {
    for (int t = 0; t < PG_READ_TRIES; t++) {
        if (pg_read(dst, fad, nsec) == 0) return;
        g_pg.read_errors++;
    }
    printf("pager: page %lu unreadable at FAD %lu after %d tries\n",
           (unsigned long)v, (unsigned long)fad, PG_READ_TRIES);
    arch_abort();
}

/* ---- the frames --------------------------------------------------------------- */

/* The bus's direct pages that point at frame f go. */
static void pg_unslot(pg_frame_t *f) {
    memory_bus_t *bus = g_pg.bus;
    for (int i = 0; i < f->nslot; i++) {
        bus->rd_page[f->slot[i]] = NULL;
        bus->wr_page[f->slot[i]] = NULL;
    }
    f->nslot = 0;
}

static int pg_victim(void) {
    pager_t *g = &g_pg;
    for (uint32_t step = 0; step < 2 * g->nframes + 1; step++) {
        uint32_t i = g->hand;
        g->hand = i + 1 < g->nframes ? i + 1 : 0;
        pg_frame_t *f = &g->fr[i];
        if (f->pinned) continue;
        if (f->ref) { f->ref = 0; continue; }
        bool recent = false;
        for (uint32_t k = 0; k < PG_RECENT; k++) recent |= g->recent[k] == i;
        if (recent) continue;
        return (int)i;
    }
    return -1;
}

/* Window page v, resident: its frame's index. */
static uint32_t pg_fault(uint32_t v) {
    pager_t *g = &g_pg;
    if (g->frame[v]) {
        uint32_t fi = g->frame[v] - 1u;
        g->fr[fi].ref = 1;
        return fi;
    }
    int fi = pg_victim();
    if (fi < 0) {                                    /* every frame pinned */
        printf("pager: no frame for page %lu (%lu pinned)\n", (unsigned long)v, (unsigned long)g->nframes);
        arch_abort();
    }
    pg_frame_t *f = &g->fr[fi];
    if (f->vpage != ~0u) {
        g->frame[f->vpage] = 0;
        pg_unslot(f);
        g->evictions++;
    }
    uint8_t *p = g->pool + (uint32_t)fi * PG_SIZE;
    uint32_t src = g->src[v];
    f->dirty = f->pinned = 0;
    if (src < 0x100u) {
        memset(p, (int)src, PG_SIZE);
        g->zero_fills++;
    } else if (src == PG_SRC_ANON) {
        memset(p, 0, PG_SIZE);
        f->dirty = f->pinned = 1;
        g->anon_fills++;
    } else {
        uint64_t t0 = timer_ns_gettime64();
        uint32_t valid = PG_SIZE;
        int part = -1;
        for (int i = 0; i < g->npart; i++)           /* a file's last page: the rest is fill */
            if (g->part[i].v == v) { part = i; valid = g->part[i].valid; }
        pg_read_page(p, src, (valid + DC_SECTOR - 1) / DC_SECTOR, v);
        if (part >= 0) memset(p + valid, g->part[part].fill, PG_SIZE - valid);
        g->read_ns += timer_ns_gettime64() - t0;
        g->loads++;
        if (v - g->pak_first < g->pak_pages) g->pak_loads++;
        else if (v - g->sp_first < g->sp_pages) g->sp_loads++;
        else {
            int r = DC_REGIONS - 1;
            while (r > 0 && v < g->first[r]) r--;
            g->rg_loads[r]++;
        }
        g->at_loads += g->in_at;
    }
    f->vpage = v;
    f->ref = 1;
    g->frame[v] = (uint16_t)(fi + 1);
    return (uint32_t)fi;
}

/* MEM_HOST_PAGE_IN: the frame holding the bus address addr of region r, put
 * in the direct page tables where they name only r. A write pins it. */
static uint8_t *pg_bus_in(memory_bus_t *bus, mem_region_t *r, uint32_t addr, int write) {
    pager_t *g = &g_pg;
    uint32_t v = ((uint32_t)(uintptr_t)r->data - PG_VA_BASE + (addr - r->base)) >> PG_SHIFT;
    if (v >= g->npages) return NULL;
    g->bus = bus;
    uint32_t fi = pg_fault(v);
    pg_frame_t *f = &g->fr[fi];
    uint8_t *p = g->pool + fi * PG_SIZE;
    if (write && !f->dirty) {
        f->dirty = f->pinned = 1;
        g->rom_writes++;
    }
    uint32_t slot = addr >> MEM_PAGE_SHIFT, e = bus->page[addr >> 16];
    if (slot < MEM_PAGES && e != MEM_PAGE_NONE && e != MEM_PAGE_MIXED && &bus->regions[e - 1u] == r) {
        int k = 0;
        while (k < f->nslot && f->slot[k] != slot) k++;
        if (k == f->nslot && k < PG_SLOTS) f->slot[f->nslot++] = (uint16_t)slot;
        if (k < f->nslot) {
            if (!r->read_cb) bus->rd_page[slot] = p;
            /* Only a pinned frame takes writes straight: the first write to a
             * page has to come through here. */
            if (f->dirty && !r->write_cb && !r->readonly && !r->change_gen && !r->dirty_kb)
                bus->wr_page[slot] = p;
        }
    }
    g->refills++;
    return p;
}

/* MEM_HOST_PAGES_RESET: the bus emptied its tables. */
static void pg_slots_reset(void) {
    for (uint32_t i = 0; i < g_pg.nframes; i++) g_pg.fr[i].nslot = 0;
}

/* Window address a of the polygons or textures, moved to the model pack's
 * window if the pack holds its n bytes. */
static uint32_t pg_pak_at(uint32_t a, uint32_t n) {
    pager_t *g = &g_pg;
    uint32_t pa = a - (g->first[3] << PG_SHIFT);
    if (pa >= (g->first[5] - g->first[3]) << PG_SHIFT) return a;
    uint32_t lo = 0, hi = g->npak;                   /* the last entry at or below pa */
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (g->pak[mid * 3] <= pa) lo = mid; else hi = mid;
    }
    const uint32_t *e = &g->pak[lo * 3];
    if (pa < e[0] || pa + n > e[0] + e[1]) return a;
    return (g->pak_first << PG_SHIFT) + e[2] + (pa - e[0]);
}

/* The n bytes at p as plain memory: p itself outside the windows, else their
 * place in a frame (good until the next PG_RECENT calls), or a copy when they
 * straddle two pages. */
static const uint8_t *dc_rom_at(const void *p, uint32_t n) {
    uint32_t a = (uint32_t)(uintptr_t)p - PG_VA_BASE;
    if (a >= PG_VA_SPAN) return (const uint8_t *)p;
    pager_t *g = &g_pg;
    if (g->npak) a = pg_pak_at(a, n ? n : 1);
    uint32_t v = a >> PG_SHIFT, off = a & (PG_SIZE - 1u);
    if (v >= g->npages) return (const uint8_t *)p;
    if (off + n <= PG_SIZE) {
        g->in_at = 1;
        uint32_t fi = pg_fault(v);
        g->in_at = 0;
        g->recent[g->rhead++ % PG_RECENT] = fi;
        return g->pool + fi * PG_SIZE + off;
    }
    static uint8_t bounce[PG_RECENT][64];
    static uint32_t bi;
    if (n > sizeof bounce[0]) {   /* the callers read a word or a vertex pair; more would be cut short */
        printf("pager: a %lu-byte read across pages\n", (unsigned long)n);
        arch_abort();
    }
    uint8_t *b = bounce[bi++ % PG_RECENT];
    uint32_t n0 = PG_SIZE - off;                     /* the first page's part, then the next's */
    g->in_at = 1;
    memcpy(b, g->pool + pg_fault(v) * PG_SIZE + off, n0);
    if (v + 1u < g->npages) memcpy(b + n0, g->pool + pg_fault(v + 1u) * PG_SIZE, n - n0);
    else memset(b + n0, 0, n - n0);
    g->in_at = 0;
    g->bounces++;
    return b;
}

/* ---- set up -------------------------------------------------------------------- */

/* FAD of the extent of `name` in the disc's ISO9660 root, its size in *size. */
static uint32_t pg_find_file(const char *name, uint32_t *size) {
    static uint8_t sec[2048] __attribute__((aligned(32)));
    cd_toc_t toc;
    uint32_t base = 0;
    /* A GD-ROM's file system is in the high-density area (track 3 on); its
     * low-density TOC only names track 1, a stub. A CD-R has only the low. */
    if (cdrom_read_toc(&toc, true) == ERR_OK) base = cdrom_locate_data_track(&toc);
    if (!base && cdrom_read_toc(&toc, false) == ERR_OK) base = cdrom_locate_data_track(&toc);
    if (!base) return 0;
    if (cdrom_read_sectors(sec, base + 16, 1) != ERR_OK) return 0;
    if (memcmp(sec + 1, "CD001", 5)) return 0;
    uint32_t root = sec[156 + 2] | sec[156 + 3] << 8 | sec[156 + 4] << 16 | (uint32_t)sec[156 + 5] << 24;
    uint32_t rlen = sec[156 + 10] | sec[156 + 11] << 8 | sec[156 + 12] << 16 | (uint32_t)sec[156 + 13] << 24;
    size_t nlen = strlen(name);
    for (uint32_t s = 0; s < (rlen + 2047) / 2048; s++) {
        if (cdrom_read_sectors(sec, root + s + 150, 1) != ERR_OK) return 0;
        for (uint32_t o = 0; o < 2048 && sec[o];) {
            const uint8_t *d = sec + o;
            if (d[32] >= nlen && !memcmp(d + 33, name, nlen) && (d[32] == nlen || d[33 + nlen] == ';')) {
                *size = d[10] | d[11] << 8 | d[12] << 16 | (uint32_t)d[13] << 24;
                return (d[2] | d[3] << 8 | d[4] << 16 | (uint32_t)d[5] << 24) + 150;
            }
            o += d[0];
        }
    }
    return 0;
}

/* The model pack's index, if the disc has one; its data's FAD and size, and
 * the bytes the index takes in memory. */
static int pg_pak_open(uint32_t *fad, uint32_t *size, uint32_t *held) {
    static uint8_t sec[2048] __attribute__((aligned(32)));
    pager_t *g = &g_pg;
    uint32_t fsize = 0, f = pg_find_file("MODELS.PAK", &fsize);
    if (!f || pg_read(sec, f, 1) != 0 || memcmp(sec, "M2PK", 4)) return 0;
    uint32_t n, off;
    memcpy(&n, sec + 4, 4);
    memcpy(&off, sec + 8, 4);
    if (!n || n > fsize / 12u || off % DC_SECTOR || off < 12 + 12 * n || off >= fsize) return 0;   /* 12 * n must not wrap */
    uint8_t *idx = memalign(32, off);
    if (!idx || pg_read(idx, f, off / DC_SECTOR) != 0) { free(idx); return 0; }
    g->pak = (const uint32_t *)(idx + 12);
    g->npak = n;
    *fad = f + off / DC_SECTOR;
    *size = fsize - off;
    *held = off;
    return 1;
}

/* The strip pack's header and index, if the disc has one; its blobs' FAD and size. */
static int pg_sp_open(uint32_t *fad, uint32_t *size) {
    static uint8_t sec[2048] __attribute__((aligned(32)));
    uint32_t fsize = 0, f = pg_find_file("STRIPS.PAK", &fsize);
    if (!f || pg_read(sec, f, 1) != 0 || memcmp(sec, "M2SP", 4)) return 0;
    const dcs_head_t *h = (const dcs_head_t *)sec;
    uint32_t n = h->n, off = h->data_off;
    if (!n || n > fsize / sizeof(dcs_index_t) || off % DC_SECTOR || off < sizeof *h + sizeof(dcs_index_t) * n ||
            off >= fsize) return 0;
    uint8_t *idx = memalign(32, off);
    if (!idx || pg_read(idx, f, off / DC_SECTOR) != 0) { free(idx); return 0; }
    g_pg.sp = (const dcs_head_t *)idx;
    *fad = f + off / DC_SECTOR;
    *size = fsize - off;
    return 1;
}

/* Window pages first .. first + pages - 1 onto the file at fad (size bytes). */
static int pg_map_file(uint32_t first, uint32_t pages, uint32_t fad, uint32_t size) {
    pager_t *g = &g_pg;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t v = first + i;
        g->src[v] = fad + i * (PG_SIZE / DC_SECTOR);
        if (size - i * PG_SIZE < PG_SIZE) {
            if (g->npart == PG_MAX_PART) { printf("pager: more than %d part pages\n", PG_MAX_PART); return -1; }
            g->part[g->npart].v = v;
            g->part[g->npart].valid = (uint16_t)(size - i * PG_SIZE);
            g->part[g->npart].fill = 0;
            g->npart++;
        }
    }
    return 0;
}

/* len bytes of the strip pack's blobs from off, copied to dst. */
static void pg_sp_copy(void *dst, uint32_t off, uint32_t len) {
    pager_t *g = &g_pg;
    uint8_t *d = dst;
    while (len) {
        uint32_t v = g->sp_first + (off >> PG_SHIFT), o = off & (PG_SIZE - 1u);
        uint32_t n = PG_SIZE - o < len ? PG_SIZE - o : len;
        memcpy(d, g->pool + pg_fault(v) * PG_SIZE + o, n);
        d += n; off += n; len -= n;
    }
}

/* Lay the regions out as windows, every page pointing at its sectors, and
 * make the frame pool. anon_bytes reserves window space for pg_anon(). */
static int pg_init(const dc_layout_t *lay, uint32_t cache_bytes, uint32_t anon_bytes) {
    pager_t *g = &g_pg;
    g->lay = lay;
    g->nrom = 0;
    for (int r = 0; r < DC_REGIONS; r++) {
        g->first[r] = g->nrom;
        g->nrom += (lay->rg[r].size + PG_SIZE - 1) / PG_SIZE;
    }
    uint32_t pak_fad = 0, pak_size = 0, pak_held = 0;
    if (pg_pak_open(&pak_fad, &pak_size, &pak_held)) {
        /* The index comes out of the frame pool: the caller sized the pool
         * with the heap's headroom kept, and the index (12 bytes a run, a
         * whole-group pack has ~11000) must not eat that. */
        uint32_t pages = (pak_held + PG_SIZE - 1) / PG_SIZE;
        if (cache_bytes > (pages + 16) * PG_SIZE) cache_bytes -= pages * PG_SIZE;
        g->pak_first = g->nrom;
        g->pak_pages = (pak_size + PG_SIZE - 1) / PG_SIZE;
        g->nrom += g->pak_pages;
    }
    uint32_t sp_fad = 0, sp_size = 0;
    if (pg_sp_open(&sp_fad, &sp_size)) {
        g->sp_first = g->nrom;
        g->sp_pages = (sp_size + PG_SIZE - 1) / PG_SIZE;
        g->nrom += g->sp_pages;
    }
    g->npages = g->nrom + anon_bytes / PG_SIZE + PG_MAX_ANON * 4;
    if ((uint64_t)g->npages << PG_SHIFT > PG_VA_SPAN) { printf("pager: the windows overflow\n"); return -1; }
    g->src   = malloc(g->npages * 4u);
    g->frame = calloc(g->npages, 2);
    if (!g->src || !g->frame) return -1;
    for (uint32_t i = g->nrom; i < g->npages; i++) g->src[i] = PG_SRC_ANON;
    for (int r = 0; r < DC_REGIONS; r++) {
        const dc_region_t *rg = &lay->rg[r];
        uint32_t n = (rg->size + PG_SIZE - 1) / PG_SIZE;
        for (uint32_t i = 0; i < n; i++) g->src[g->first[r] + i] = rg->fill;
        for (int k = 0; k < DC_MAX_SEGS && rg->seg[k].file; k++) {
            const dc_seg_t *sg = &rg->seg[k];
            if ((sg->file_off | sg->reg_off | sg->period) & (PG_SIZE - 1u)) {
                printf("pager: %s is not on a %u KB page\n", sg->file, (unsigned)(PG_SIZE >> 10));
                return -1;
            }
            uint32_t fsize = 0, fad = pg_find_file(sg->file, &fsize);
            if (!fad) { printf("pager: %s not on the disc\n", sg->file); return -1; }
            uint32_t len = sg->len;
            if (sg->file_off >= fsize) len = 0;
            else if (len > fsize - sg->file_off) len = fsize - sg->file_off;
            uint32_t reps = sg->count ? sg->count : 1;
            for (uint32_t c = 0; c < reps; c++)
                for (uint32_t o = 0; o < len; o += PG_SIZE) {
                    uint32_t ro = sg->reg_off + c * sg->period + o;
                    if (ro >= rg->size) break;
                    uint32_t v = g->first[r] + ro / PG_SIZE;
                    g->src[v] = fad + (sg->file_off + o) / DC_SECTOR;
                    if (len - o < PG_SIZE) {
                        if (g->npart == PG_MAX_PART) { printf("pager: more than %d part pages\n", PG_MAX_PART); return -1; }
                        g->part[g->npart].v = v;
                        g->part[g->npart].valid = (uint16_t)(len - o);
                        g->part[g->npart].fill = rg->fill;
                        g->npart++;
                    }
                }
        }
    }

    if (pg_map_file(g->pak_first, g->pak_pages, pak_fad, pak_size) != 0 ||
            pg_map_file(g->sp_first, g->sp_pages, sp_fad, sp_size) != 0) return -1;

    g->nframes = cache_bytes / PG_SIZE;
    g->pool = memalign(32, g->nframes * PG_SIZE);
    g->fr   = calloc(g->nframes, sizeof *g->fr);
    if (!g->pool || !g->fr) return -1;
    for (uint32_t i = 0; i < g->nframes; i++) g->fr[i].vpage = ~0u;
    for (int i = 0; i < PG_RECENT; i++) g->recent[i] = ~0u;
    printf("pager: %s, %u ROM pages, %u KB cache, model pack: %u runs, strip pack: %u meshes\n", lay->profile,
           (unsigned)g->nrom, (unsigned)(g->nframes * (PG_SIZE >> 10)), (unsigned)g->npak,
           (unsigned)(g->sp ? g->sp->n : 0));
    return 0;
}

/* The window of the region named `name`, or NULL. */
static uint8_t *pg_region(const char *name, size_t *size) {
    for (int r = 0; r < DC_REGIONS; r++)
        if (!strcmp(g_pg.lay->rg[r].name, name)) {
            if (size) *size = g_pg.lay->rg[r].size;
            return (uint8_t *)(uintptr_t)(PG_VA_BASE + (g_pg.first[r] << PG_SHIFT));
        }
    return NULL;
}

/* An anonymous window of `size` bytes (the same one again for the same size
 * and index): zero until touched, pinned once touched. */
static uint8_t *pg_anon(int idx, uint32_t size) {
    pager_t *g = &g_pg;
    if (idx < 0 || idx >= PG_MAX_ANON) return NULL;
    if (idx < g->nanon) return g->anon[idx].size == size ? (uint8_t *)(uintptr_t)g->anon[idx].va : NULL;
    uint32_t start = g->nrom;
    for (int i = 0; i < g->nanon; i++) start += (g->anon[i].size + PG_SIZE - 1) / PG_SIZE + 4;
    if (start + (size + PG_SIZE - 1) / PG_SIZE > g->npages) return NULL;
    g->anon[idx].va = PG_VA_BASE + (start << PG_SHIFT);
    g->anon[idx].size = size;
    g->nanon = idx + 1;
    return (uint8_t *)(uintptr_t)g->anon[idx].va;
}

/* Zero what the anonymous window holds, for a board reset. */
static void pg_anon_clear(int idx) {
    pager_t *g = &g_pg;
    if (idx < 0 || idx >= g->nanon) return;
    uint32_t v0 = (g->anon[idx].va - PG_VA_BASE) >> PG_SHIFT;
    uint32_t n = (g->anon[idx].size + PG_SIZE - 1) / PG_SIZE;
    for (uint32_t v = v0; v < v0 + n; v++)
        if (g->frame[v]) memset(g->pool + (uint32_t)(g->frame[v] - 1) * PG_SIZE, 0, PG_SIZE);
}

/* Forget every write to a ROM page, for a board reset: elsewhere the install
 * copies the ROM over MAIN_DATA again, here the window is the ROM. */
static void pg_rom_revert(void) {
    pager_t *g = &g_pg;
    for (uint32_t i = 0; i < g->nframes; i++) {
        pg_frame_t *f = &g->fr[i];
        if (f->vpage == ~0u || g->src[f->vpage] == PG_SRC_ANON || !f->dirty) continue;
        if (g->bus) pg_unslot(f);
        g->frame[f->vpage] = 0;
        f->vpage = ~0u;
        f->dirty = f->pinned = f->ref = 0;
    }
}

static uint32_t pg_pinned(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_pg.nframes; i++) n += g_pg.fr[i].pinned;
    return n;
}

#endif /* DC_PAGER_H */
