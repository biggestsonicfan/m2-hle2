/*
 * dc_pager.h -- demand paging for the Dreamcast build (Pinboard #340).
 *
 * The board reads its ROM through plain pointers: the i960 bus through its page
 * table, the COP its tables, the 3D decoder its models, the 68000 its samples.
 * A Dreamcast has 16 MB and a set is ~60 MB, so the ROM stays on the disc and
 * the SH-4's MMU does the paging, the way an OS would: every region is a window
 * of P0 virtual addresses, a TLB miss lands in pg_map(), and a page that is not
 * resident is read off the disc into a 4 KB frame first. The board cannot tell,
 * and no reader had to change (tests/rom_touch.c measured what it reads).
 *
 * Windows:
 *   - ROM: every region of the layout (dc_layout.h), one after another. A page
 *     is two sectors of one of the ROM files on the disc, or a fill byte.
 *     Frames are evicted by a clock: a frame whose page took a TLB refill
 *     since the hand last passed is kept.
 *   - anonymous: zero until first touched, then pinned (vid_ext_ram, 7 MB of
 *     which a game uses a little). pg_anon_clear() zeroes them for a reset.
 *
 * The cache: the SH-4's operand cache is indexed by virtual address bits 13:5
 * and tagged by physical address, so a 4 KB page seen at two addresses whose
 * bits 13:12 differ can sit in the cache twice. Frames are coloured: page v
 * only ever goes into a frame with the same bits 13:12 as its window address,
 * and the frame is written through its P1 address, which has them too. So
 * nothing ever needs flushing.
 *
 * The disc: the miss handler runs as an exception, where KOS's cdrom driver
 * (semaphores) may not be used. pg_read() talks to the GD-ROM syscalls directly
 * and polls. Nothing else may use the drive once the pager is up.
 */
#ifndef DC_PAGER_H
#define DC_PAGER_H

#include <kos.h>
#include <arch/mmu.h>
#include <dc/cdrom.h>
#include <dc/syscalls.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dc_layout.h"

#define PG_VA_BASE    0x10000000u
#define PG_SHIFT      12
#define PG_SRC_ANON   0xFFFFFFFEu    /* src: anonymous; below 0x100: a fill byte; else a FAD */
#define PG_MAX_ANON   8
#define PG_MAX_PART   32             /* pages a file ends inside */

typedef struct {
    uint32_t pteh, ptel;             /* what gen_tlb_miss loads (mmupage_t's tail) */
} pg_tlb_t;

typedef struct {
    mmupage_t pg;                    /* handed to KOS's miss handler */
    uint32_t  vpage;                 /* window page it holds, or ~0 */
    uint32_t  sum;                   /* a writable page's contents at load (pg_sum) */
    uint8_t   ref, pinned, dirty;
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
    uint8_t          *pool;          /* P1 address of frame 0 (16 KB aligned) */
    uint32_t          nframes;
    uint32_t          hand[4];       /* clock hand per colour */
    uint32_t          wr_lo, wr_hi;  /* window pages the board may write (main_data) */
    mmucontext_t     *cxt;
    struct { uint32_t va, size; } anon[PG_MAX_ANON];
    int               nanon;
    /* counters */
    volatile uint32_t refills, loads, zero_fills, anon_fills, evictions, read_errors, rom_writes, flushes;
    volatile uint64_t read_ns;
} pager_t;

static pager_t g_pg;

/* ---- the drive, from the exception ------------------------------------------- */

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
    return -1;
}

/* ---- the miss handler ------------------------------------------------------- */

static inline void pg_tlb_drop(uint32_t vpage) {
    /* Associative write to the UTLB address array: the entry for this VPN
     * (ASID 0) loses its valid bit. KOS's mmu_page_uninstall does the same. */
    *(volatile uint32_t *)(0xF6000000u | 0x80u) = (PG_VA_BASE + (vpage << PG_SHIFT));
}

/* Every TLB entry goes, and KOS's two wired store-queue entries come back.
 * The SH-4 can drop one entry (an associative write to the UTLB address
 * array), but Flycast's fast MMU keeps every entry it was ever given in a
 * table of its own and empties that only on MMUCR.TI: a page dropped one at a
 * time stayed mapped to its old frame, and read whatever page had moved in. */
static void pg_tlb_flush(void) {
    volatile uint32_t *mmucr = (volatile uint32_t *)0xFF000010u;
    *mmucr = *mmucr | 0x04u;                         /* TI */
    mmu_init_basic();
    g_pg.flushes++;
}

static uint32_t pg_sum(const uint8_t *p) {
    const uint32_t *w = (const uint32_t *)p;
    uint32_t h = 0;
    for (int i = 0; i < DC_PAGE / 4; i++) h = (h << 5 | h >> 27) ^ w[i];
    return h;
}

/* A written page must not be evicted: it would come back off the disc without
 * the write. The first-write trap reports most (pg_first_write); Flycast's
 * fast MMU never raises it, so a page of the writable region is also summed
 * when it comes in and again before it goes. */
static bool pg_written(pg_frame_t *f) {
    pager_t *g = &g_pg;
    if (f->dirty) return true;
    if (f->vpage < g->wr_lo || f->vpage >= g->wr_hi) return false;
    if (pg_sum(g->pool + (uint32_t)(f - g->fr) * DC_PAGE) == f->sum) return false;
    f->dirty = f->pinned = 1;
    g->rom_writes++;
    return true;
}

static int pg_victim(uint32_t colour) {
    pager_t *g = &g_pg;
    uint32_t per = g->nframes / 4;
    for (uint32_t step = 0; step < 2 * per + 1; step++) {
        uint32_t i = g->hand[colour];
        g->hand[colour] = (i + 1) % per;
        pg_frame_t *f = &g->fr[i * 4 + colour];
        if (f->pinned) continue;
        if (f->ref) { f->ref = 0; continue; }
        if (f->vpage != ~0u && pg_written(f)) continue;
        return (int)(i * 4 + colour);
    }
    return -1;
}

static mmupage_t *pg_map(mmucontext_t *cxt, int virtpage) {
    (void)cxt;
    pager_t *g = &g_pg;
    uint32_t v = (uint32_t)virtpage - (PG_VA_BASE >> PG_SHIFT);
    if (v >= g->npages) return NULL;                 /* KOS reports it and panics */
    if (g->frame[v]) {                               /* resident: a TLB refill */
        pg_frame_t *f = &g->fr[g->frame[v] - 1];
        f->ref = 1;
        g->refills++;
        return &f->pg;
    }
    int fi = pg_victim(v & 3u);
    if (fi < 0) return NULL;                         /* every frame of the colour pinned */
    pg_frame_t *f = &g->fr[fi];
    if (f->vpage != ~0u) {
        g->frame[f->vpage] = 0;
        pg_tlb_flush();
        g->evictions++;
    }
    uint8_t *p1 = g->pool + (uint32_t)fi * DC_PAGE;
    uint32_t src = g->src[v];
    if (src < 0x100u) {
        memset(p1, (int)src, DC_PAGE);
        g->zero_fills++;
    } else if (src == PG_SRC_ANON) {
        memset(p1, 0, DC_PAGE);
        f->pinned = 1;
        g->anon_fills++;
    } else {
        uint64_t t0 = timer_ns_gettime64();
        if (pg_read(p1, src, DC_PAGE / DC_SECTOR) != 0) {
            g->read_errors++;
            memset(p1, 0xA5, DC_PAGE);
        }
        for (int i = 0; i < g->npart; i++)       /* a file's last page: the rest is fill */
            if (g->part[i].v == v) memset(p1 + g->part[i].valid, g->part[i].fill, DC_PAGE - g->part[i].valid);
        g->read_ns += timer_ns_gettime64() - t0;
        g->loads++;
    }
    if (v >= g->wr_lo && v < g->wr_hi) f->sum = pg_sum(p1);
    f->vpage = v;
    f->ref = 1;
    f->dirty = src == PG_SRC_ANON;
    g->frame[v] = (uint16_t)(fi + 1);
    uint32_t va = PG_VA_BASE + (v << PG_SHIFT);
    uint32_t pa = ((uint32_t)(uintptr_t)p1) & 0x1FFFFFFFu;
    f->pg.pteh = va & 0xFFFFFC00u;
    /* V, 4 KB (SZ0), PR 3 (read/write), cached; D only for an anonymous page,
     * so the first write to a ROM page traps (pg_first_write). */
    f->pg.ptel = (pa & 0x1FFFFC00u) | 0x100u | 0x10u | (3u << 5) | 0x08u | (f->dirty ? 0x04u : 0);
    return &f->pg;
}

/* The first write to a ROM page (initial page write exception). The board's
 * MAIN_DATA and XTRA_DATA are writable regions, and an evicted page would come
 * back off the disc without the write, so a written page stays in memory for
 * good. Its TLB entry goes; the write runs again, misses, and pg_map hands back
 * the frame with D set. */
static void pg_first_write(irq_t code, irq_context_t *ctx, void *data) {
    (void)code; (void)ctx; (void)data;
    pager_t *g = &g_pg;
    uint32_t tea = *(volatile uint32_t *)0xFF00000Cu;
    uint32_t v = (tea - PG_VA_BASE) >> PG_SHIFT;
    if (tea < PG_VA_BASE || v >= g->npages || !g->frame[v]) {
        printf("pager: page write at %08lx outside the windows\n", (unsigned long)tea);
        arch_abort();
    }
    pg_frame_t *f = &g->fr[g->frame[v] - 1];
    f->dirty = f->pinned = 1;
    f->pg.ptel |= 0x04u;
    g->rom_writes++;
    pg_tlb_drop(v);
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

/* mmu_init, so that Flycast translates too. Flycast emulates the MMU only for
 * Windows CE: when MMUCR.AT goes up it looks for "SH-4 Kernel" (UTF-16) at
 * 0x8C0110A8 or 0x8C011118 (core/hw/sh4/modules/mmu.cpp, mmu_set_state), and
 * otherwise maps only the store queues, so every page here read as zeros. KOS
 * raises AT once, in mmu_init, and Flycast looks only then: the words go there
 * for the call and the code they cover (KOS's, or ours) comes back after. A
 * real Dreamcast never looks.
 *
 * Then a TLB miss has to reach us. Flycast's fast MMU (USE_WINCE_HACK,
 * fastmmu.cpp) first tries to answer it from Windows CE's own page tables,
 * found through TTB, and only raises the exception when that walk finds no
 * page. KOS never sets TTB, so the walk read whatever RAM it named and handed
 * back made-up pages: the board read zeros and the pager never heard of it.
 * TTB names a table here whose every group is a zeroed block, which the walk
 * reads as "not present". TTB is only a scratch register to the SH-4. */
static uint32_t pg_ttb_group[512] __attribute__((aligned(32)));
static uint32_t pg_ttb[128] __attribute__((aligned(32)));
static void pg_mmu_on(void) {
    static const char sig[] = { 'S',0,'H',0,'-',0,'4',0,' ',0,'K',0,'e',0,'r',0,'n',0,'e',0,'l',0 };
    uint8_t *at = (uint8_t *)0x8C011118u, keep[sizeof sig];
    int old = irq_disable();
    memcpy(keep, at, sizeof sig);
    memcpy(at, sig, sizeof sig);
    dcache_flush_range((uintptr_t)at, sizeof sig);
    mmu_init();
    for (int i = 0; i < 128; i++) pg_ttb[i] = (uint32_t)(uintptr_t)pg_ttb_group;
    dcache_flush_range((uintptr_t)pg_ttb, sizeof pg_ttb);
    *(volatile uint32_t *)0xFF000008u = (uint32_t)(uintptr_t)pg_ttb;   /* TTB */
    memcpy(at, keep, sizeof sig);
    dcache_flush_range((uintptr_t)at, sizeof sig);
    icache_flush_range((uintptr_t)at, sizeof sig);
    irq_restore(old);
}

/* Lay the regions out as windows, every page pointing at its sectors, and
 * make the frame pool. anon_bytes reserves window space for pg_anon(). */
static int pg_init(const dc_layout_t *lay, uint32_t cache_bytes, uint32_t anon_bytes) {
    pager_t *g = &g_pg;
    g->lay = lay;
    g->nrom = 0;
    for (int r = 0; r < DC_REGIONS; r++) {
        g->first[r] = g->nrom;
        g->nrom += (lay->rg[r].size + DC_PAGE - 1) / DC_PAGE;
    }
    g->npages = g->nrom + anon_bytes / DC_PAGE + PG_MAX_ANON * 4;
    g->src   = malloc(g->npages * 4u);
    g->frame = calloc(g->npages, 2);
    if (!g->src || !g->frame) return -1;
    for (uint32_t i = g->nrom; i < g->npages; i++) g->src[i] = PG_SRC_ANON;
    for (int r = 0; r < DC_REGIONS; r++) {
        const dc_region_t *rg = &lay->rg[r];
        uint32_t n = (rg->size + DC_PAGE - 1) / DC_PAGE;
        for (uint32_t i = 0; i < n; i++) g->src[g->first[r] + i] = rg->fill;
        for (int k = 0; k < DC_MAX_SEGS && rg->seg[k].file; k++) {
            const dc_seg_t *sg = &rg->seg[k];
            uint32_t fsize = 0, fad = pg_find_file(sg->file, &fsize);
            if (!fad) { printf("pager: %s not on the disc\n", sg->file); return -1; }
            uint32_t len = sg->len;
            if (sg->file_off >= fsize) len = 0;
            else if (len > fsize - sg->file_off) len = fsize - sg->file_off;
            uint32_t reps = sg->count ? sg->count : 1;
            for (uint32_t c = 0; c < reps; c++)
                for (uint32_t o = 0; o < len; o += DC_PAGE) {
                    uint32_t ro = sg->reg_off + c * sg->period + o;
                    if (ro >= rg->size) break;
                    uint32_t v = g->first[r] + ro / DC_PAGE;
                    g->src[v] = fad + (sg->file_off + o) / DC_SECTOR;
                    if (len - o < DC_PAGE && g->npart < PG_MAX_PART) {
                        g->part[g->npart].v = v;
                        g->part[g->npart].valid = (uint16_t)(len - o);
                        g->part[g->npart].fill = rg->fill;
                        g->npart++;
                    }
                }
        }
        if (!strcmp(rg->name, "main_data")) {
            g->wr_lo = g->first[r];
            g->wr_hi = g->wr_lo + n;
        }
    }

    g->nframes = (cache_bytes / DC_PAGE) & ~3u;
    g->pool = memalign(16384, g->nframes * DC_PAGE);
    g->fr   = calloc(g->nframes, sizeof *g->fr);
    if (!g->pool || !g->fr) return -1;
    for (uint32_t i = 0; i < g->nframes; i++) g->fr[i].vpage = ~0u;

    /* The drive is the pager's from here: KOS's driver must not start a
     * command the miss handler would interleave with. */
    pg_mmu_on();
    g->cxt = mmu_context_create(0);
    mmu_use_table(g->cxt);
    mmu_map_set_callback(pg_map);
    irq_set_handler(EXC_INITIAL_PAGE_WRITE, pg_first_write, NULL);
    printf("pager: %s, %u ROM pages, %u KB cache\n", lay->profile,
           (unsigned)g->nrom, (unsigned)(g->nframes * 4));
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
    for (int i = 0; i < g->nanon; i++) start += (g->anon[i].size + DC_PAGE - 1) / DC_PAGE + 4;
    if (start + (size + DC_PAGE - 1) / DC_PAGE > g->npages) return NULL;
    g->anon[idx].va = PG_VA_BASE + (start << PG_SHIFT);
    g->anon[idx].size = size;
    g->nanon = idx + 1;
    return (uint8_t *)(uintptr_t)g->anon[idx].va;
}

/* Zero what the anonymous window holds, for a board reset (through P1). */
static void pg_anon_clear(int idx) {
    pager_t *g = &g_pg;
    if (idx < 0 || idx >= g->nanon) return;
    uint32_t v0 = (g->anon[idx].va - PG_VA_BASE) >> PG_SHIFT;
    uint32_t n = (g->anon[idx].size + DC_PAGE - 1) / DC_PAGE;
    for (uint32_t v = v0; v < v0 + n; v++)
        if (g->frame[v]) memset(g->pool + (uint32_t)(g->frame[v] - 1) * DC_PAGE, 0, DC_PAGE);
}

/* Forget every write to a ROM page, for a board reset: elsewhere the install
 * copies the ROM over MAIN_DATA again, here the window is the ROM. */
static void pg_rom_revert(void) {
    pager_t *g = &g_pg;
    for (uint32_t i = 0; i < g->nframes; i++) {
        pg_frame_t *f = &g->fr[i];
        if (f->vpage == ~0u || g->src[f->vpage] == PG_SRC_ANON || !pg_written(f)) continue;
        g->frame[f->vpage] = 0;
        f->vpage = ~0u;
        f->dirty = f->pinned = f->ref = 0;
    }
    int old = irq_disable();
    pg_tlb_flush();
    irq_restore(old);
}

static uint32_t pg_pinned(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_pg.nframes; i++) n += g_pg.fr[i].pinned;
    return n;
}

#endif /* DC_PAGER_H */
