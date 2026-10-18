#ifndef AGES_H
#define AGES_H
/*
 * Sega's own C for Fighting Vipers' hot code, from Sega Ages 2500 Vol. 19:
 * Fighting Vipers (PS2, SLPM-62606).
 *
 * The PS2 runs the arcade's i960 program (byte-identical to fvipers,
 * Revision D) under an interpreter, as Sonic Gems Collection runs STF's. At
 * start-up sysPatch writes an extension opcode over 46 i960 functions; the
 * extension switch (sysExtend) calls a native C function for each
 * (calc_unit_mat, play_motion, rob_disp, osage_dsp, ...). One option runs
 * that C here instead:
 *
 *   --ages-i960    the patched i960 functions (a hook at each patch site)
 *   --ages-verify  each one both ways, as --gems-verify (implies --ages-i960)
 *
 * The C is Sega's and is not in this repository. A build that has it is
 * configured with -DM2HLE_AGES_DIR=<dir> (the owner's ages-c), and
 * <dir>/ages_impl.h supplies ages_patches[], written against the runtime API
 * below. Without it the options say so and do nothing.
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
 * gems_apply and takes the slot only for fvipers.
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

#define AGES_PATCHES_MAX 64

#ifdef M2HLE_AGES
#include "ages_impl.h"
/* ages_impl.h defines:
 *   static const gems_trap_t ages_patches[];  static const size_t ages_patch_count;
 * site: the i960 address sysPatch writes the extension over; fn(1) returns 0. */
#define AGES_AVAILABLE 1
#else
#define AGES_AVAILABLE 0
static const gems_trap_t ages_patches[1];
static const size_t      ages_patch_count = 0;
#endif

/* ---- --ages-i960 and --ages-verify --------------------------------------------- */

static uint32_t           s_ages_sites[AGES_PATCHES_MAX];
static gems_verify_stat_t s_ages_stat[AGES_PATCHES_MAX];

static inline const gems_trap_t *ages_patch_at(uint32_t ip) {
    for (size_t i = 0; i < ages_patch_count && i < AGES_PATCHES_MAX; i++)
        if (ages_patches[i].site == ip) return &ages_patches[i];
    return NULL;
}

static int ages_hook(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (s_gems_suspend) return 1;   /* the verifier's i960 run */
    const gems_trap_t *t = ages_patch_at(cpu->sfr.ip);
    if (!t) return 1;
    if (g_ages_verify) return gems_verify_in(cpu, bus, t, &s_ages_stat[t - ages_patches], "ages");
    gems_run(cpu, bus, t);
    return 0;
}

static void ages_verify_report(void) {
    if (g_ages_verify) gems_verify_report_rows("ages", ages_patches, ages_patch_count, s_ages_stat);
}

/* A netplay session needs the board as the ROM runs it on both machines. */
static void ages_off_for_session(void) {
    if (!g_hle_extra_hook) return;
    LOG_INFO("ages: off for the netplay session");
    g_hle_extra_hook = NULL;
    g_hle_filter_gen++;
}

/* Only fvipers (Revision D) is the program the PS2 patches. */
static bool ages_for_set(const char *set_name) {
    return AGES_AVAILABLE && set_name && !strcmp(set_name, "fvipers");
}

/* Apply the flags, after gems_apply (which resets the extra-hook slot). Call
 * after the ROM is loaded, and again when they change. */
static bool ages_apply(const char *set_name) {
    if ((g_ages_i960 || g_ages_verify) && !AGES_AVAILABLE)
        LOG_WARN("ages: this build has no Sega Ages code (configure with -DM2HLE_AGES_DIR)");
    bool homebrew = g_active_profile && g_active_profile->any_program;
    if (!ages_for_set(set_name) || homebrew || !(g_ages_i960 || g_ages_verify)) return false;
    size_t n = 0;
    for (size_t i = 0; i < ages_patch_count && n < AGES_PATCHES_MAX; i++)
        s_ages_sites[n++] = ages_patches[i].site;
    g_hle_extra_sites = s_ages_sites;
    g_hle_extra_count = n;
    g_hle_extra_hook  = ages_hook;
    g_hle_extra_session_off = ages_off_for_session;
    g_hle_filter_gen++;
    LOG_INFO("ages: %zu i960 function%s%s", n, n == 1 ? "" : "s", g_ages_verify ? " (verify)" : "");
    return true;
}

#endif /* AGES_H */
