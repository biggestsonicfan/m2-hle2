#ifndef AGES_H
#define AGES_H
/*
 * Sega's own C for two Model 2 games' hot code, from Sega Ages 2500:
 *
 *   fvipers  Vol. 19 Fighting Vipers (PS2, SLPM-62606), Revision D
 *   vf2      Vol. 16 Virtua Fighter 2 (PS2, SLPM-62584), Version 2.1
 *
 * The PS2 runs the arcade's i960 program (byte-identical to that set) under an
 * interpreter, as Sonic Gems Collection runs STF's. At start-up sysPatch
 * writes an extension opcode over the hot i960 functions (46 on FV; set_obj's
 * 140 call sites and 11 more on VF2); the extension switch (sysExtend) calls a
 * native C function for each (calc_unit_mat, osage_dsp, set_obj, ...). One
 * option runs that C here instead:
 *
 *   --ages-i960    the patched i960 functions (a hook at each patch site)
 *   --ages-verify  each one both ways, as --gems-verify (implies --ages-i960)
 *
 * The C is Sega's and is not in this repository. A build that has it is
 * configured with -DM2HLE_AGES_DIR=<dir> (the owner's ages-c), and
 * <dir>/ages_impl.h supplies ages_sets[] (each set's patch table), written
 * against the runtime API below. Without it the options say so and do nothing.
 *
 * VF2 is a Model 2A board: its coprocessor is the TGP (board/tgp.h), which the
 * i960 drives over the bus (FIFO 0x884000 at g11 + g12). Its C does the same
 * loads and stores through ages_ld32/ages_st32, never the 2B COP's ages_cop_*;
 * the verifier's snapshot holds the TGP, so a word sent differently shows.
 *
 * The ABI is Gems': sysExtend calls the function with 1, the C works on the
 * i960 register file and bus, pops the frame itself (i960ReturnProc) and
 * returns 0. So this runs on gems.h's runtime (g_gems), its gems_run, and its
 * verifier, and a patch is a gems_trap_t. The AGES_* / ages_* names below are
 * that runtime under this file's names, for the C in ages-c.
 *
 * Neither option is the board, for gems.h's reasons: a patched function is
 * charged as one i960 instruction. A netplay session turns them off.
 *
 * gems.h and this file share the hook layer's one extra-hook slot
 * (g_hle_extra_*). They serve different sets, so ages_apply runs after
 * gems_apply and takes the slot only for a set ages_impl.h serves.
 */

#include "gems.h"

static bool g_ages_i960   = false;   /* --ages-i960 */
static bool g_ages_verify = false;   /* --ages-verify (implies --ages-i960) */

/* ---- Runtime for the converted C (gems.h's) ---------------------------------- */

#define AGES_R(n) GEMS_R(n)
#define AGES_G(n) GEMS_G(n)
#define AGES_AC   GEMS_AC
#define ages_u2f     gems_u2f
#define ages_f2u     gems_f2u
#define ages_ld8     gems_ld8
#define ages_ld8s    gems_ld8s
#define ages_ld16    gems_ld16
#define ages_ld16s   gems_ld16s
#define ages_ld32    gems_ld32
#define ages_ldf     gems_ldf
#define ages_st8     gems_st8
#define ages_st16    gems_st16
#define ages_st32    gems_st32
#define ages_stf     gems_stf
#define ages_ldn     gems_ldn
#define ages_stn     gems_stn
#define ages_cop_w   gems_cop_w
#define ages_cop_wf  gems_cop_wf
#define ages_cop_r   gems_cop_r
#define ages_cop_rf  gems_cop_rf
/* The PS2's i960ReturnProc(): pop the frame, as `ret`. */
#define ages_i960_ret gems_i960_ret
#define ages_branch   gems_branch

/* ---- What ages_impl.h supplies ------------------------------------------------ */

#define AGES_PATCHES_MAX 256

/* One set's patches. site: the i960 address sysPatch writes the extension
 * over; fn(1) pops the frame and returns 0, or returns R and the i960 resumes
 * at site + R/2 (gems_run). One function can patch many sites (VF2's set_obj). */
typedef struct {
    const char        *set;      /* the MAME set the program is byte-identical to */
    const gems_trap_t *patches;
    size_t             count;
} ages_set_t;

#ifdef M2HLE_AGES
#include "ages_impl.h"
/* ages_impl.h defines:
 *   static const ages_set_t ages_sets[];  static const size_t ages_set_count; */
#define AGES_AVAILABLE 1
#else
#define AGES_AVAILABLE 0
static const ages_set_t ages_sets[1];
static const size_t     ages_set_count = 0;
#endif

/* ---- --ages-i960 and --ages-verify --------------------------------------------- */

static const ages_set_t  *s_ages_set;   /* the running set's, while the hook is in */
static uint32_t           s_ages_sites[AGES_PATCHES_MAX];
static gems_verify_stat_t s_ages_stat[AGES_PATCHES_MAX];

static inline size_t ages_count(void) {
    size_t n = s_ages_set ? s_ages_set->count : 0;
    return n < AGES_PATCHES_MAX ? n : AGES_PATCHES_MAX;
}

static inline const gems_trap_t *ages_patch_at(uint32_t ip) {
    for (size_t i = 0; i < ages_count(); i++)
        if (s_ages_set->patches[i].site == ip) return &s_ages_set->patches[i];
    return NULL;
}

static int ages_hook(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (s_gems_suspend) return 1;   /* the verifier's i960 run */
    const gems_trap_t *t = ages_patch_at(cpu->sfr.ip);
    if (!t) return 1;
    if (g_ages_verify) return gems_verify_in(cpu, bus, t, &s_ages_stat[t - s_ages_set->patches], "ages");
    gems_run(cpu, bus, t);
    return 0;
}

/* One row per function: a function patched at many sites (VF2's set_obj at
 * 140) is summed over them. */
static void ages_verify_report(void) {
    if (!g_ages_verify || !s_ages_set) return;
    static gems_trap_t        fn[AGES_PATCHES_MAX];
    static gems_verify_stat_t sum[AGES_PATCHES_MAX];
    size_t n = 0;
    for (size_t i = 0; i < ages_count(); i++) {
        const gems_trap_t *t = &s_ages_set->patches[i];
        const gems_verify_stat_t *s = &s_ages_stat[i];
        size_t k = 0;
        while (k < n && fn[k].fn != t->fn) k++;
        if (k == n) { fn[n] = *t; memset(&sum[n], 0, sizeof sum[n]); n++; }
        sum[k].calls += s->calls; sum[k].exact += s->exact; sum[k].close += s->close;
        sum[k].regs += s->regs; sum[k].differ += s->differ; sum[k].lost += s->lost;
    }
    gems_verify_report_rows("ages", fn, n, sum);
}

/* A netplay session needs the board as the ROM runs it on both machines. */
static void ages_off_for_session(void) {
    if (!g_hle_extra_hook) return;
    LOG_INFO("ages: off for the netplay session");
    g_hle_extra_hook = NULL;
    g_hle_filter_gen++;
}

/* Only the program the PS2 patches: fvipers (Revision D), vf2 (Version 2.1),
 * not their clones. */
static const ages_set_t *ages_for_set(const char *set_name) {
    if (!AGES_AVAILABLE || !set_name) return NULL;
    for (size_t i = 0; i < ages_set_count; i++)
        if (!strcmp(set_name, ages_sets[i].set)) return &ages_sets[i];
    return NULL;
}

/* Apply the flags, after gems_apply (which resets the extra-hook slot). Call
 * after the ROM is loaded, and again when they change. */
static bool ages_apply(const char *set_name) {
    if ((g_ages_i960 || g_ages_verify) && !AGES_AVAILABLE)
        LOG_WARN("ages: this build has no Sega Ages code (configure with -DM2HLE_AGES_DIR)");
    bool homebrew = g_active_profile && g_active_profile->any_program;
    const ages_set_t *set = ages_for_set(set_name);
    if (!set || homebrew || !(g_ages_i960 || g_ages_verify)) return false;
    if (set != s_ages_set) memset(s_ages_stat, 0, sizeof s_ages_stat);
    s_ages_set = set;
    size_t n = ages_count();
    for (size_t i = 0; i < n; i++) s_ages_sites[i] = set->patches[i].site;
    g_hle_extra_sites = s_ages_sites;
    g_hle_extra_count = n;
    g_hle_extra_hook  = ages_hook;
    g_hle_extra_session_off = ages_off_for_session;
    g_hle_filter_gen++;
    LOG_INFO("ages: %s: %zu i960 patch site%s%s", set->set, n, n == 1 ? "" : "s",
             g_ages_verify ? " (verify)" : "");
    return true;
}

#endif /* AGES_H */
