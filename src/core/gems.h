#ifndef GEMS_H
#define GEMS_H
/*
 * Sega's own C for STF's hot code, from Sonic Gems Collection (GameCube).
 *
 * The Gems port runs the arcade's i960 program under an interpreter, but
 * patches ~45 of its heaviest functions with a trap word whose handler is
 * native C (calc_unit_mat, get_frame_dat, coli_cont_cop, osage_dsp, ...), and
 * replaces the coprocessor's SHARC firmware with one C function per command,
 * under the firmware's own Fn_* names. Two options run that C here instead:
 *
 *   --gems-i960   the trapped i960 functions (a hook at each trap site)
 *   --gems-cop    the COP commands (in place of sharc_exec)
 *
 * The C itself is Sega's and is not in this repository. A build that has it
 * is configured with -DM2HLE_GEMS_DIR=<dir>, and <dir>/gems_impl.h supplies
 * the tables below (gems_traps[], gems_cop_ops[]) written against this
 * header's runtime API. Without it the options say so and do nothing.
 *
 * What the board can tell:
 *   - The i960 functions stand in for thousands of instructions each and are
 *     charged one, so the timers see less time in a frame. rand (0x66B0) reads
 *     the timers.
 *   - Where Gems' C leaves the board differently from the i960 (a scratch
 *     register, the debug command history), the conversion follows the i960.
 *   - Gems' COP is PowerPC single precision with fused multiply-add, not the
 *     SHARC's arithmetic (CLAUDE.md, "The COP's arithmetic is not libm").
 * So neither option is the board, and a netplay session turns both off.
 *
 * --gems-verify runs each trapped function both ways: the C on a copy of the
 * board, then the i960 from the same state up to where the C resumed, and logs
 * what the two left different (registers, RAM, bufferram, tiles, palette,
 * backup RAM, the COP's state). The board carries on from the i960's run, so
 * a verify run plays as the ROM does.
 *
 * Trap ABI (stf.elf, the i960 interpreter's trap handler at 0x800262D8): an
 * entry function is called with 1 and returns R. R != 0 resumes the i960 at
 * site + R / 2 (Gems decodes 8 bytes per i960 word); R == 0 means the C set
 * the IP itself, with i960_ret() (gems_i960_ret) or a branch (gems_branch).
 *
 * A profile's own hook inside a trapped function (sfight_console's head tilt
 * in get_frame_dat) would be skipped by the C, so the C runs it: gems_inner()
 * below. A trap whose C does not serve such a hook stays with the i960.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "../board/i960_exec.h"
#include "../board/memory.h"
#include "../board/cop.h"
#include "hle_hooks.h"
#include "log.h"

static bool g_gems_i960   = false;   /* --gems-i960 */
static bool g_gems_cop    = false;   /* --gems-cop */
static bool g_gems_verify = false;   /* --gems-verify (implies --gems-i960) */

/* ---- Runtime for the converted C ----------------------------------------- */

typedef struct {
    i960_cpu_t   *cpu;
    memory_bus_t *bus;
    bool          ip_set;      /* the C set the IP (i960_ret or a branch) */
    /* COP: the command's argument words, read in order */
    const uint32_t *in;
    int             in_n, in_i;
    uint32_t        cmd;
} gems_rt_t;

static gems_rt_t g_gems;

/* i960 registers: r0-r15 are the frame's locals, g0-g15 the globals. */
#define GEMS_R(n) (g_gems.cpu->locals.r[(n)])
#define GEMS_G(n) (g_gems.cpu->globals.g[(n)])
/* The arithmetic controls (Gems' cpu+0x114); bits 0-2 are the condition code:
 * 4 less, 2 equal, 1 greater. */
#define GEMS_AC   (g_gems.cpu->sfr.ac)

static inline float    gems_u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t gems_f2u(float f)    { uint32_t u; memcpy(&u, &f, 4); return u; }
/* The i960's real-to-integer conversions of a single in a word, as the core
 * does them: cvtri rounds by the AC rounding mode, cvtzri truncates; out of
 * range or NaN gives 0x80000000. Gems' fctiwz truncates and saturates. */
static inline uint32_t gems_cvtri(uint32_t w)  { return i960_real_to_int32(i960_round_ac(g_gems.cpu, i960_single_to_double(w))); }
static inline uint32_t gems_cvtzri(uint32_t w) { return i960_real_to_int32(i960_single_to_double(w)); }

/* i960 memory, through the bus as the CPU sees it (MMIO side effects
 * included: COP FIFO, GEO, tile and palette generation counters). */
static inline uint32_t gems_ld8(uint32_t a)   { return mem_read8(g_gems.bus, a); }
static inline int32_t  gems_ld8s(uint32_t a)  { return (int8_t)mem_read8(g_gems.bus, a); }
static inline uint32_t gems_ld16(uint32_t a)  { return mem_read16(g_gems.bus, a); }
static inline int32_t  gems_ld16s(uint32_t a) { return (int16_t)mem_read16(g_gems.bus, a); }
static inline uint32_t gems_ld32(uint32_t a)  { return mem_read32(g_gems.bus, a); }
static inline float    gems_ldf(uint32_t a)   { return gems_u2f(mem_read32(g_gems.bus, a)); }
static inline void gems_st8(uint32_t a, uint32_t v)  { mem_write8(g_gems.bus, a, v & 0xFFu); }
static inline void gems_st16(uint32_t a, uint32_t v) { mem_write16(g_gems.bus, a, v & 0xFFFFu); }
static inline void gems_st32(uint32_t a, uint32_t v) { mem_write32(g_gems.bus, a, v); }
static inline void gems_stf(uint32_t a, float f)     { mem_write32(g_gems.bus, a, gems_f2u(f)); }
/* ldl/ldt/ldq and stl/stt/stq: n words, lowest address first. The registers
 * are arrays, so &GEMS_G(4) names g4 onwards. */
static inline void gems_ldn(uint32_t *dst, uint32_t a, int n) {
    for (int i = 0; i < n; i++) dst[i] = mem_read32(g_gems.bus, a + 4u * (uint32_t)i);
}
static inline void gems_stn(uint32_t a, const uint32_t *src, int n) {
    for (int i = 0; i < n; i++) mem_write32(g_gems.bus, a + 4u * (uint32_t)i, src[i]);
}

/* The i960 side of the COP FIFOs (Gems' cop_write / cop_read). */
static inline void     gems_cop_w(uint32_t v) { cop_write(v); }
static inline void     gems_cop_wf(float f)   { cop_write(gems_f2u(f)); }
static inline uint32_t gems_cop_r(void)       { return cop_read(); }
static inline float    gems_cop_rf(void)      { return gems_u2f(cop_read()); }
/* Gems' cop_writeN_from / cop_readN_to. */
static inline void gems_cop_wn(const uint32_t *src, int n) { for (int i = 0; i < n; i++) cop_write(src[i]); }
static inline void gems_cop_rn(uint32_t *dst, int n)       { for (int i = 0; i < n; i++) dst[i] = cop_read(); }

/* A hook of the active profile on an instruction inside a trapped function.
 * The C asks for it by the instruction's address (NULL: this profile has none
 * there, carry on as the ROM), puts the registers the instruction would hold
 * into the CPU, calls it, and takes back what the hook wrote. Only a hook that
 * edits registers or memory and lets its instruction run (returns 1) can be
 * served this way. gems_impl.h names the addresses its C serves in
 * GEMS_INNER_SITES (a comma list); GEMS_INNER tells it this header has these. */
#define GEMS_INNER 1
typedef hle_hook_fn gems_inner_fn;
static inline gems_inner_fn gems_inner(uint32_t ip) {
    const game_profile_t *p = g_active_profile;
    for (size_t i = 0; p && i < p->hook_count; i++)
        if (p->hooks[i].addr == ip) return p->hooks[i].fn;
    return NULL;
}
static inline void gems_inner_call(gems_inner_fn f) {
    if (f(g_gems.cpu, g_gems.bus) != 1) {
        static bool said;
        if (!said) LOG_WARN("gems: a profile hook inside a trapped function skipped its instruction; the C ran it");
        said = true;
    }
}

/* Gems' i960_ret(): pop the frame, as `ret`. */
static inline void gems_i960_ret(void) { hle_ret(g_gems.cpu); g_gems.ip_set = true; }
/* Gems' FUN_8002d34c: carry on at an i960 address (a branch). */
static inline void gems_branch(uint32_t ip) { g_gems.cpu->sfr.ip = ip; g_gems.ip_set = true; }

/* The COP: the argument words of the command in flight, and its replies. */
static inline uint32_t gems_in_w(void) {
    if (g_gems.in_i < g_gems.in_n) return g_gems.in[g_gems.in_i++];
    g_gems.in_i++;
    return 0;
}
static inline float gems_in_f(void) { return gems_u2f(gems_in_w()); }
static inline void  gems_out_w(uint32_t v) { sharc_push_u(v); }
/* A NaN goes out as the SHARC writes one (all ones), so a reply does not
 * depend on the host's NaN sign: the wasm and native builds must agree. */
static inline void  gems_out_f(float f)    { sharc_push_u(sharc_float_to_bits(f)); }

/* The firmware's data memory. Gems keeps its COP state as an image of the
 * SHARC's DM from 0x30000 (its state pointer is DM 0x30000: +0xCFC is
 * DM 0x3033F, the current matrix's index), which is what g_sharc.dm holds. */
#define GEMS_DM_BASE  0x30000u
#define GEMS_DM_WORDS ((uint32_t)(sizeof g_sharc.dm / sizeof g_sharc.dm[0]))
static inline uint32_t *gems_dm(uint32_t addr) {
    uint32_t i = addr - GEMS_DM_BASE;
    if (i >= GEMS_DM_WORDS) {
        /* Callers index a whole matrix or slot from what we hand back, so the
         * stand-in is a span, not one word. */
        static uint32_t junk[64];
        LOG_WARN("gems: DM 0x%X outside the image (cmd 0x%08X)", addr, g_gems.cmd);
        memset(junk, 0, sizeof junk);
        return junk;
    }
    return &g_sharc.dm[i];
}
static inline float *gems_dmf(uint32_t addr) { return (float *)(void *)gems_dm(addr); }

/* Bufferram from the SHARC's side: a word index (DM 0x01400000 + idx). */
static inline uint32_t gems_bram_rd(uint32_t idx) {
    uint32_t v = 0;
    if (g_sharc.sharc_dm_ext)
        memcpy(&v, g_sharc.sharc_dm_ext + ((idx * 4u) & (g_sharc.sharc_dm_ext_size - 1u)), 4);
    return v;
}
static inline void gems_bram_wr(uint32_t idx, uint32_t v) {
    if (g_sharc.sharc_dm_ext)
        memcpy(g_sharc.sharc_dm_ext + ((idx * 4u) & (g_sharc.sharc_dm_ext_size - 1u)), &v, 4);
}
static inline float gems_bram_rdf(uint32_t idx)        { return gems_u2f(gems_bram_rd(idx)); }
static inline void  gems_bram_wrf(uint32_t idx, float f) { gems_bram_wr(idx, sharc_float_to_bits(f)); }

/* ---- What gems_impl.h supplies --------------------------------------------- */

typedef struct {
    uint32_t    site;               /* the i960 address Gems traps */
    uint32_t  (*fn)(int entry);     /* Gems' native function; returns R */
    const char *name;
} gems_trap_t;

typedef struct {
    void      (*fn)(void);          /* NULL: not converted */
    int         args;               /* argument words; COP_ARGS_STREAM for 0x80 */
    const char *name;
} gems_cop_op_t;

#define GEMS_COP_OPS 0x88

#ifdef M2HLE_GEMS
#include "gems_impl.h"
/* gems_impl.h defines:
 *   static const gems_trap_t   gems_traps[];  static const size_t gems_trap_count;
 *   static const gems_cop_op_t gems_cop_ops[GEMS_COP_OPS];
 *   static void gems_cop_impl_reset(void);       the firmware's init of its DM
 *   static void gems_zanzou_begin(void);         Fn_zanzou_reserve as a stream
 *   static bool gems_zanzou_feed(uint32_t w);    true once the stream ends */
#define GEMS_AVAILABLE 1
#else
#define GEMS_AVAILABLE 0
static const gems_trap_t   gems_traps[1];
static const size_t        gems_trap_count = 0;
static const gems_cop_op_t gems_cop_ops[GEMS_COP_OPS];
static void gems_cop_impl_reset(void) {}
static void gems_zanzou_begin(void) {}
static bool gems_zanzou_feed(uint32_t w) { (void)w; return true; }
#endif

/* ---- --gems-cop -------------------------------------------------------------- */

static inline int gems_cop_op_of(uint32_t cmd) {
    uint32_t n = (cmd >> 23) & 0x1FFu;
    if (n >= GEMS_COP_OPS || cmd != ((n << 23) | (n << 8) | n)) return -1;
    return (int)n;
}

/* Commands m2-hle2 keeps its own handler for even under --gems-cop. Gems' C
 * for them is converted all the same, for the record, but not dispatched:
 * Fn_put_poly (0x78) lays each object into the GEO display list, and that is
 * what our renderer reads (sharc_exec.h). */
static inline bool gems_cop_ours(int op) { return op == 0x78; }

/* Our Fn_put_poly reads g_sharc.rot / pos; under --gems-cop the matrix stack
 * is Gems' (DM 0x30000 on, the current index at +0xCFC: col0, col1, col2, T). */
static void gems_cop_sync_matrix(void) {
    const float *m = gems_dmf(0x30000u + *gems_dm(0x30000u + 0xCFCu / 4u));
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) g_sharc.rot[c][r] = m[c * 3 + r];
    for (int r = 0; r < 3; r++) g_sharc.pos[r] = m[9 + r];
}

static int gems_cop_args_cb(uint32_t cmd) {
    int op = gems_cop_op_of(cmd);
    if (op >= 0 && gems_cop_ours(op)) {
        gems_cop_sync_matrix();
        return GEMS_COP_NOT_MINE;
    }
    if (op < 0 || !gems_cop_ops[op].fn) return GEMS_COP_NOT_MINE;
    return gems_cop_ops[op].args;
}

static void gems_cop_exec_cb(uint32_t cmd, const uint32_t *args, int n) {
    const gems_cop_op_t *o = &gems_cop_ops[gems_cop_op_of(cmd)];
    g_sharc.reply_count = 0;
    g_sharc.reply_idx   = 0;
    g_gems.cmd  = cmd;
    g_gems.in   = args;
    g_gems.in_n = n;
    g_gems.in_i = 0;
    o->fn();
    if (g_gems.in_i != n)
        LOG_WARN("gems: %s took %d of its %d argument words", o->name, g_gems.in_i, n);
}

static void gems_cop_stream_begin_cb(void) { g_gems.cmd = 0x40008080u; gems_zanzou_begin(); }
static bool gems_cop_stream_feed_cb(uint32_t w) { return gems_zanzou_feed(w); }

/* ---- --gems-i960 and --gems-verify ------------------------------------------------ */

static uint32_t s_gems_sites[256];
static int      s_gems_suspend = 0;   /* the verifier's native run: no traps */

static inline const gems_trap_t *gems_trap_at(uint32_t ip) {
    for (size_t i = 0; i < gems_trap_count; i++)
        if (gems_traps[i].site == ip) return &gems_traps[i];
    return NULL;
}

/* A trap whose function holds one of the active profile's own hooks stays
 * with the i960 unless the C runs that hook itself (gems_inner): the C would
 * run the whole function and skip the hook. sfight_console zeroes the head
 * tilt at get_frame_dat+0x140 (0x30608, sfc_hook_head_tilt), so with a Gems
 * get_frame_dat that does not serve it the Console profile played the
 * Arcade's motion blend (--gems-verify: get_frame_dat differs at 0x514C30 on
 * sfight_console only). */
static bool gems_trap_left_to_i960(const gems_trap_t *t) {
    static const struct { const char *profile; uint32_t site, hook; } inner[] = {
        { "sfight_console", 0x000304C8u, 0x00030608u },   /* get_frame_dat */
    };
#ifdef GEMS_INNER_SITES
    static const uint32_t served[] = { GEMS_INNER_SITES };
    const size_t served_n = sizeof served / sizeof served[0];
#else
    static const uint32_t served[1] = { 0 };
    const size_t served_n = 0;
#endif
    if (!g_active_profile) return false;
    for (size_t k = 0; k < sizeof inner / sizeof inner[0]; k++) {
        if (t->site != inner[k].site || strcmp(g_active_profile->id, inner[k].profile)) continue;
        bool has = false;
        for (size_t i = 0; i < served_n; i++) has |= served[i] == inner[k].hook;
        if (!has) return true;
    }
    return false;
}

/* Run the C for the trap at the CPU's IP. */
static void gems_run(i960_cpu_t *cpu, memory_bus_t *bus, const gems_trap_t *t) {
    g_gems.cpu = cpu;
    g_gems.bus = bus;
    g_gems.ip_set = false;
    uint32_t r = t->fn(1);
    if (r) {
        cpu->sfr.ip = t->site + r / 2u;
    } else if (!g_gems.ip_set) {
        LOG_WARN("gems: %s returned 0 without setting the IP; returning", t->name);
        hle_ret(cpu);
    }
    cpu->cycles += 1;
}

typedef struct {
    i960_cpu_t     cpu;
    uint8_t        ram[RAM_SIZE];
    uint8_t        ram2[RAM2_SIZE];
    uint8_t        buff[BUFF_RAM_SIZE];
    uint8_t        tile[TILE_SIZE];
    uint8_t        pal[PALETTE_SIZE];
    uint8_t        back[BACK_SIZE];
    sharc_state_t  sharc;
    cop_state_t    cop;
    uint32_t       geo_w, geo_r;    /* the GEO port's list pointers (memory.h) */
} gems_snap_t;

typedef struct {
    uint64_t seen, calls, exact, close, regs, differ, lost;
    int      logged;
} gems_verify_stat_t;

#define GEMS_VERIFY_ALL    256u
#define GEMS_VERIFY_STRIDE 32u

static gems_snap_t        *s_gems_snap[2];
static gems_verify_stat_t  s_gems_stat[256];

static void gems_snap_save(gems_snap_t *s, const i960_cpu_t *cpu, const memory_bus_t *bus) {
    s->cpu = *cpu;
    memcpy(s->ram,  bus->ram,      sizeof s->ram);
    memcpy(s->ram2, bus->ram2,     sizeof s->ram2);
    memcpy(s->buff, bus->buff_ram, sizeof s->buff);
    memcpy(s->tile, bus->tile,     sizeof s->tile);
    memcpy(s->pal,  bus->palette,  sizeof s->pal);
    memcpy(s->back, bus->back,     sizeof s->back);
    s->sharc = g_sharc;
    s->cop   = g_cop;
    s->geo_w = g_geo.wstart;
    s->geo_r = g_geo.rstart;
}

static void gems_snap_load(const gems_snap_t *s, i960_cpu_t *cpu, memory_bus_t *bus) {
    *cpu = s->cpu;
    memcpy(bus->ram,      s->ram,  sizeof s->ram);
    memcpy(bus->ram2,     s->ram2, sizeof s->ram2);
    memcpy(bus->buff_ram, s->buff, sizeof s->buff);
    memcpy(bus->tile,     s->tile, sizeof s->tile);
    memcpy(bus->palette,  s->pal,  sizeof s->pal);
    memcpy(bus->back,     s->back, sizeof s->back);
    g_sharc = s->sharc;
    g_cop   = s->cop;
    g_geo.wstart = s->geo_w;
    g_geo.rstart = s->geo_r;
}

/* A word the two runs left different: the same float to a few parts in a
 * million (PowerPC FMA against the i960's or the SHARC's rounding) or not. */
static inline bool gems_float_close(uint32_t a, uint32_t b) {
    float fa = gems_u2f(a), fb = gems_u2f(b);
    if (!isfinite(fa) || !isfinite(fb)) return false;
    float d = fabsf(fa - fb), m = fmaxf(fabsf(fa), fabsf(fb));
    return d <= 1e-4f * m || d < 1e-6f;
}

typedef struct { int words, close; uint32_t first, a, b; } gems_diff_t;
typedef struct { uint32_t lo, hi; } gems_skip_t;

static void gems_diff_mem(gems_diff_t *d, const uint8_t *a, const uint8_t *b, size_t n,
                          uint32_t base, const gems_skip_t *skip, int nskip) {
    for (size_t o = 0; o + 4 <= n; o += 4) {
        uint32_t wa, wb;
        memcpy(&wa, a + o, 4);
        memcpy(&wb, b + o, 4);
        if (wa == wb) continue;
        uint32_t addr = base + (uint32_t)o;
        bool skipped = false;
        for (int k = 0; k < nskip; k++)
            if (addr >= skip[k].lo && addr < skip[k].hi) skipped = true;
        if (skipped) continue;
        if (gems_float_close(wa, wb)) { d->close++; continue; }
        if (!d->words) { d->first = addr; d->a = wa; d->b = wb; }
        d->words++;
    }
}

/* Run the trap both ways (see the top of the file). Returns 0: the board
 * carries on from the i960's run; 1 for a call it lets the i960 run alone. */
static int gems_verify(i960_cpu_t *cpu, memory_bus_t *bus, const gems_trap_t *t) {
    for (int k = 0; k < 2; k++)
        if (!s_gems_snap[k] && !(s_gems_snap[k] = (gems_snap_t *)malloc(sizeof(gems_snap_t)))) {
            gems_run(cpu, bus, t);
            return 0;
        }
    gems_snap_t *before = s_gems_snap[0], *c_run = s_gems_snap[1];
    gems_verify_stat_t *st = &s_gems_stat[t - gems_traps];
    /* Every call to start with, then one in GEMS_VERIFY_STRIDE: a whole-board
     * snapshot per call is slow, and a hot trap runs hundreds of times a frame. */
    static int every = -1;   /* M2HLE_GEMS_VERIFY_ALL=1: every call, however slow */
    if (every < 0) every = getenv("M2HLE_GEMS_VERIFY_ALL") != NULL;
    if (!every && st->seen++ >= GEMS_VERIFY_ALL && st->seen % GEMS_VERIFY_STRIDE) return 1;
    st->calls++;

    gems_snap_save(before, cpu, bus);
    gems_run(cpu, bus, t);
    uint32_t stop_ip = cpu->sfr.ip;
    int      stop_d  = cpu->frame_depth;
    gems_snap_save(c_run, cpu, bus);
    gems_snap_load(before, cpu, bus);

    /* The i960 from the same state, to where the C resumed. */
    s_gems_suspend++;
    uint64_t n = 0;
    const uint64_t cap = 20u * 1000u * 1000u;
    i960_step(cpu, bus);   /* the trap site itself, so a loop back to it stops */
    while (!(cpu->sfr.ip == stop_ip && cpu->frame_depth == stop_d) && !cpu->halted && ++n < cap)
        i960_step(cpu, bus);
    s_gems_suspend--;
    if (n >= cap || cpu->halted) {
        st->lost++;
        if (st->lost <= 3)
            LOG_WARN("gems-verify: %s: the i960 never reached 0x%08X (depth %d) from 0x%08X",
                     t->name, stop_ip, stop_d, t->site);
        return 0;
    }

    gems_diff_t ram = {0}, ram2 = {0}, buff = {0}, tile = {0}, pal = {0}, back = {0}, dm = {0};
    /* The i960's dead frames above SP are where its callees kept their
     * locals, which the C keeps on the host. */
    const gems_skip_t skip[1] = { { cpu->locals.sp, cpu->locals.sp + 0x4000u } };
    gems_diff_mem(&ram,  c_run->ram,  bus->ram,      RAM_SIZE,       0x00500000u, skip, 1);
    gems_diff_mem(&ram2, c_run->ram2, bus->ram2,     RAM2_SIZE,      0x00200000u, skip, 1);
    gems_diff_mem(&buff, c_run->buff, bus->buff_ram, BUFF_RAM_SIZE,  0x00900000u, NULL, 0);
    gems_diff_mem(&tile, c_run->tile, bus->tile,     TILE_SIZE,      0x01000000u, NULL, 0);
    gems_diff_mem(&pal,  c_run->pal,  bus->palette,  PALETTE_SIZE,   0x01800000u, NULL, 0);
    gems_diff_mem(&back, c_run->back, bus->back,     BACK_SIZE,      0x01D00000u, NULL, 0);
    gems_diff_mem(&dm, (const uint8_t *)c_run->sharc.dm, (const uint8_t *)g_sharc.dm,
                  sizeof g_sharc.dm, GEMS_DM_BASE, NULL, 0);
    uint32_t gmask = 0;
    for (int i = 0; i < 16; i++) {
        if (c_run->cpu.globals.g[i] != cpu->globals.g[i]) gmask |= 1u << (16 + i);
        if (c_run->cpu.locals.r[i]  != cpu->locals.r[i])  gmask |= 1u << i;
    }
    /* The display-list pointers: a run that laid a different amount moved them. */
    if (c_run->geo_w != g_geo.wstart || c_run->geo_r != g_geo.rstart) {
        if (!buff.words) { buff.first = 0x00801008u; buff.a = c_run->geo_w; buff.b = g_geo.wstart; }
        buff.words++;
    }
    int words = ram.words + ram2.words + buff.words + tile.words + pal.words + back.words + dm.words;
    int close = ram.close + ram2.close + buff.close + tile.close + pal.close + back.close + dm.close;
    if (!words && !gmask) {
        if (close) st->close++; else st->exact++;
        return 0;
    }
    /* Registers alone: often a scratch register the C never needed. */
    if (!words) st->regs++; else st->differ++;
    if (st->logged++ < 4) {
        LOG_WARN("gems-verify: %s (0x%08X) differs: regs g%04X r%04X; words ram %d ram2 %d "
                 "buff %d tile %d pal %d back %d dm %d (+%d float-close)",
                 t->name, t->site, gmask >> 16, gmask & 0xFFFF, ram.words, ram2.words,
                 buff.words, tile.words, pal.words, back.words, dm.words, close);
        const gems_diff_t *all[] = { &ram, &ram2, &buff, &tile, &pal, &back, &dm };
        for (int k = 0; k < 7; k++)
            if (all[k]->words)
                LOG_WARN("gems-verify:   first at 0x%08X: C 0x%08X, i960 0x%08X",
                         all[k]->first, all[k]->a, all[k]->b);
        for (int i = 0; i < 32; i++)
            if (gmask & (1u << i))
                LOG_WARN("gems-verify:   %c%d: C 0x%08X, i960 0x%08X", i < 16 ? 'r' : 'g', i & 15,
                         i < 16 ? c_run->cpu.locals.r[i] : c_run->cpu.globals.g[i - 16],
                         i < 16 ? cpu->locals.r[i] : cpu->globals.g[i - 16]);
    }
    return 0;
}

static void gems_verify_report(void) {
    if (!g_gems_verify) return;
    for (size_t i = 0; i < gems_trap_count; i++) {
        const gems_verify_stat_t *s = &s_gems_stat[i];
        if (!s->calls) continue;
        LOG_INFO("gems-verify: %-28s calls %llu  exact %llu  float-close %llu  regs-only %llu  "
                 "differ %llu  lost %llu",
                 gems_traps[i].name, (unsigned long long)s->calls, (unsigned long long)s->exact,
                 (unsigned long long)s->close, (unsigned long long)s->regs,
                 (unsigned long long)s->differ, (unsigned long long)s->lost);
    }
}

static int gems_hook(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (s_gems_suspend) return 1;
    const gems_trap_t *t = gems_trap_at(cpu->sfr.ip);
    if (!t) return 1;
    if (g_gems_verify) return gems_verify(cpu, bus, t);
    gems_run(cpu, bus, t);
    return 0;
}

static void gems_off_for_session(void);

/* ---- Switching ------------------------------------------------------------------- */

/* Apply the flags. Call after the ROM is loaded, and again when they change.
 * Only STF has Gems code; another set leaves both off. */
static bool gems_apply(const char *set_name) {
    bool stf = set_name && (!strcmp(set_name, "sfight") || !strcmp(set_name, "sfight_console"));
    /* The traps are addresses in STF's program, not in homebrew on its board
     * (the any_program profile). The COP's commands are the set's firmware. */
    bool stf_code = stf && !(g_active_profile && g_active_profile->any_program);
    bool want_i960 = (g_gems_i960 || g_gems_verify) && GEMS_AVAILABLE && stf_code;
    bool want_cop  = g_gems_cop && GEMS_AVAILABLE && stf;
    if ((g_gems_i960 || g_gems_cop || g_gems_verify) && !GEMS_AVAILABLE)
        LOG_WARN("gems: this build has no Gems code (configure with -DM2HLE_GEMS_DIR)");

    size_t n = 0;
    for (size_t i = 0; i < gems_trap_count && n < 256; i++)
        if (!gems_trap_left_to_i960(&gems_traps[i])) s_gems_sites[n++] = gems_traps[i].site;
    g_hle_extra_sites = s_gems_sites;
    g_hle_extra_count = n;
    g_hle_extra_hook  = want_i960 ? gems_hook : NULL;
    g_hle_filter_gen++;

    g_gems_cop_args         = want_cop ? gems_cop_args_cb : NULL;
    g_gems_cop_exec         = want_cop ? gems_cop_exec_cb : NULL;
    g_gems_cop_stream_begin = want_cop ? gems_cop_stream_begin_cb : NULL;
    g_gems_cop_stream_feed  = want_cop ? gems_cop_stream_feed_cb : NULL;
    g_gems_cop_reset        = want_cop ? gems_cop_impl_reset : NULL;
    if (want_cop) gems_cop_impl_reset();   /* cop_reset ran before this was set */
    g_hle_extra_session_off = gems_off_for_session;
    if (want_i960 || want_cop)
        LOG_INFO("gems: %s%s%s", want_i960 ? "i960 functions" : "",
                 want_i960 && want_cop ? " + " : "", want_cop ? "COP commands" : "");
    return want_i960 || want_cop;
}

/* A netplay session needs the board as the ROM runs it on both machines. */
static void gems_off_for_session(void) {
    if (!g_hle_extra_hook && !g_gems_cop_args) return;
    LOG_INFO("gems: off for the netplay session");
    g_hle_extra_hook = NULL;
    g_hle_filter_gen++;
    g_gems_cop_args = NULL;
    g_gems_cop_exec = NULL;
    g_gems_cop_stream_begin = NULL;
    g_gems_cop_stream_feed  = NULL;
    g_gems_cop_reset        = NULL;
}

#endif /* GEMS_H */
