/*
 * hle_hooks.h — pre-instruction HLE hook dispatch.
 *
 * Each game profile carries a hooks[] table populated at compile time in the
 * profile header.  Before the CPU decodes an instruction at IP, hle_check()
 * walks the active profile's table.  If a match is found the hook is called
 * and 0 is returned so the step loop skips normal decode (the hook must
 * advance IP or simulate a return).  Returns 1 when no hook fires.
 *
 * hle_ret() — helper for hooks that bypass complete functions.  Mirrors the
 * i960 `ret` instruction: restores the previous register window and jumps to
 * the saved return address.  Call this inside any hook that short-circuits an
 * entire function (e.g. CoProcessorErr).
 */
#ifndef HLE_HOOKS_H
#define HLE_HOOKS_H

#include "i960.h"
#include "memory.h"
#include "log.h"
#include "game_profile.h"

/* Frame-pacing flag — set to 1 by the per-game frame-boundary hook.
 * Polled by emu_thread_run_loop after each step batch; when set the slice is
 * cut short and the thread sleeps until the next 16.67 ms tick. */
static volatile int g_frame_done = 0;

/* A versus match was just decided: 1 = the 1P side won, 2 = the 2P side, 0 =
 * nothing. Set by the profile's versus hook -- an observe-only hook on the
 * arcade's own "match over" path -- and taken by the emu thread at the end of
 * the frame (netplay_end_frame), so every board in a netplay room sees a result
 * on the same frame. Part of a board reset (emu_board_reset_state). */
static volatile int g_versus_result = 0;

/* The region the board powers up as, for games whose region is a backup-RAM
 * setting (STF's country_val: 0 Japan, 1 USA, 2 Export). A profile's hook
 * applies it where the game writes its factory default, so it holds for every
 * cold boot and for the test menu's INITIALIZE. USA by default; --region picks
 * another. MAME's sfight boots as Japan, so the graders ask for japan. Both
 * boards in a netplay session have to agree on it. */
typedef enum { GAME_REGION_JAPAN = 0, GAME_REGION_USA = 1, GAME_REGION_EXPORT = 2 } game_region_t;
static volatile int g_region = GAME_REGION_USA;

/* Versus mode: the cabinet setting Sega's console emulator calls VS mode. When
 * a two-player match is decided the board goes straight back to character
 * select with both players still in, rather than keeping the winner on against
 * the CPU. A profile that honours it says so (game_quirks_t.vs_rematch). Off by
 * default; --vs-mode turns it on, and in a netplay room the owner's setting is
 * the one every board plays by, like g_region. */
static volatile int g_vs_mode = 0;

/* The cabinet's DAMAGE setting (STF's GAME ASSIGNMENTS, flag byte bit 7):
 * 0 = NORMAL, the factory default, where a fighter who is behind hits harder
 * ("catch-up" damage); 1 = REAL, where every hit does what it says. Applied
 * where the game writes the factory default, like g_region. Only a netplay room
 * on the community server sets it -- the owner's choice, which every board in
 * the match plays by -- so everything else, the graders included, stays on
 * the factory NORMAL. */
static volatile int g_damage_real = 0;

/* "japan"/"jpn", "usa"/"us", "export"/"exp"; -1 for anything else. */
static inline int game_region_parse(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "japan")  || !strcmp(s, "jpn") || !strcmp(s, "jp")) return GAME_REGION_JAPAN;
    if (!strcmp(s, "usa")    || !strcmp(s, "us"))                      return GAME_REGION_USA;
    if (!strcmp(s, "export") || !strcmp(s, "exp"))                     return GAME_REGION_EXPORT;
    return -1;
}

/*
 * Cross-play with the PS3 port (net/ps3_link.h). A PS3 match drives the board
 * the way the PS3's own emulator does in its network mode, through three hooks
 * on the same instructions it traps (sfight.h). All three are inert unless
 * g_xplay_match is set.
 *
 *   g_xplay_match    a PS3 match owns the board
 *   g_xplay_barrier  SEL_INT: 0 = hold (skip it, as `ret`), 1 = release at the
 *                    next call, 2 = released (run it)
 *   g_xplay_events   what the board did, for ps3_link to take: bit 0 = the
 *                    forced START at ADV_DSP (a new generation), bit 1 = the
 *                    barrier released, bit 2 = VIC_INT after a versus match
 *
 * Part of a board reset (emu_board_reset_state).
 */
static volatile int g_xplay_match   = 0;
static volatile int g_xplay_barrier = 0;
static volatile int g_xplay_events  = 0;
#define XPLAY_EV_NEW_GENERATION 1
#define XPLAY_EV_BARRIER        2
#define XPLAY_EV_MATCH_OVER     4

/*
 * The match's settings, the way the PS3 build puts them on its board
 * (NetMatch_StateMachine -> NetGameMode_Set(2) -> Settings_ApplyRoomRules): the
 * room's rules go into the game's own settings block, the "game assignments"
 * the test menu edits (RAM 0x59C340 and backup RAM 0x1D03340, 0x42 bytes), once
 * the board has booted past WARNING. Until they are in, the forced START waits.
 *
 *   g_xplay_rules   +0x01 rounds to win, +0x04 energy (VS), +0x11 round time,
 *                   +0x13 the flag byte (AUTOMATIC, HYPER MODE, BARRIER RESET,
 *                   DAMAGE, ...), +0x18 barrier (also word 0x50A424)
 *   g_xplay_seed    the room's seed: the PS3 picks the stage from it (0xAF84)
 *   g_xplay_mode / g_xplay_also_mode   mode (0x50002A) and also_mode (0x50002B)
 *                   at the last frame boundary, -1 before the first
 */
static volatile int      g_xplay_rules_pending = 0;
static volatile int      g_xplay_ready         = 0;
static uint8_t           g_xplay_rules[5];
static volatile uint32_t g_xplay_seed          = 0;
/* The room holds more than its two fighters (room match flag 0x40, the PS3's
 * session flag 0x400000): the match then runs on into the victory screen
 * (sfight.h, xplay_vic_dsp) instead of ending at VIC_INT. */
static volatile int      g_xplay_spectators    = 0;
static volatile int      g_xplay_mode          = -1;
static volatile int      g_xplay_also_mode     = -1;

/* Monotonic count of completed game frames. The emu thread bumps it at every
 * frame boundary (HLE pace hook or board vblank ACK). Tooling outside the
 * emulator needs a frame clock to pace a capture by — MAME's drivers use the
 * screen's frame notifier for exactly this — and steps/s is not one. */
static volatile unsigned g_emu_frames = 0;

/* Simulate an i960 `ret`: restore the previous register window and set IP to
 * the saved return address.  Use this in hooks that bypass whole functions. */
static inline void hle_ret(i960_cpu_t *cpu) {
    if (cpu->frame_depth > 0) {
        uint32_t ret_ip = cpu->locals.rip;
        cpu->frame_depth--;
        cpu->locals = cpu->frame_stack[cpu->frame_depth];
        if (cpu->frame_irq[cpu->frame_depth]) {   /* interrupt return, as `ret` */
            cpu->sfr.ac = cpu->frame_irq_ac[cpu->frame_depth];
            cpu->sfr.pc = cpu->frame_irq_pc[cpu->frame_depth];
            cpu->frame_irq[cpu->frame_depth] = 0;
        }
        cpu->sfr.ip = ret_ip;
        cpu->globals.fp = cpu->locals.pfp;
    } else {
        LOG_WARN("hle_ret: empty frame stack at IP=0x%08X", cpu->sfr.ip);
        cpu->halted = 1;
    }
}

/* Inject a call to target: push current frame with rip = ret_ip, redirect IP.
 * When target executes `ret`, it returns to ret_ip and the saved frame is restored. */
static inline void hle_call(i960_cpu_t *cpu, uint32_t target, uint32_t ret_ip) {
    if (cpu->frame_depth < FRAME_STACK_DEPTH) {
        cpu->frame_stack[cpu->frame_depth] = cpu->locals;
        cpu->frame_irq[cpu->frame_depth] = 0;
        cpu->locals.rip = ret_ip;
        cpu->frame_depth++;
    } else {
        LOG_WARN("hle_call: frame stack full at IP=0x%08X", cpu->sfr.ip);
    }
    cpu->sfr.ip = target;
}

/*
 * Deliver an interrupt: vector to handler between two instructions and resume
 * at the current IP when it returns.
 *
 * Not a call. The processor stores PC and AC in the interrupt frame and `ret`
 * puts both back, so the interrupted code finds its condition code exactly as
 * it left it. A call does not, and an interrupt landing between a compare and
 * its branch then branches on the handler's last compare instead.
 *
 * That is how STF crashed in attract: a timer interrupt arrived between
 * `cmpo r14, 0x10` and `bg` in unpack_lod_data's bit-buffer refill (0x4BAAC /
 * 0x4BAB4), the refill branch went the wrong way, the Huffman decode lost sync, and its
 * output ran off the end of the halfword buffer into the code tree.
 */
static inline void hle_interrupt(i960_cpu_t *cpu, uint32_t handler) {
    int d = cpu->frame_depth;
    hle_call(cpu, handler, cpu->sfr.ip);
    if (cpu->frame_depth == d + 1) {
        cpu->frame_irq[d]    = 1;
        cpu->frame_irq_ac[d] = cpu->sfr.ac;
        cpu->frame_irq_pc[d] = cpu->sfr.pc;
    }
}

/* One bit per (ip >> 2) & 0xFFFF, set for every hook address of the profile it
 * was built for (none without a profile): a clear bit means no hook can match,
 * so the table walk is skipped. 16 bits of instruction index, 8 KB: an address
 * shares a bit with a hook only once in 64 KB of code, where 12 bits sent one
 * instruction in a few dozen through the walk. */
static const game_profile_t *s_hle_filter_profile = NULL;
static uint8_t               s_hle_filter[65536 / 8];

/* Dispatch: walk the active profile's hook table and call the first match. */
static inline int hle_check(i960_cpu_t *cpu, memory_bus_t *bus) {
    const game_profile_t *p = g_active_profile;
    uint32_t ip = cpu->sfr.ip;
    if (s_hle_filter_profile != p) {
        memset(s_hle_filter, 0, sizeof(s_hle_filter));
        for (size_t i = 0; p && i < p->hook_count; i++) {
            uint32_t k = (p->hooks[i].addr >> 2) & 0xFFFFu;
            s_hle_filter[k >> 3] |= (uint8_t)(1u << (k & 7u));
        }
        s_hle_filter_profile = p;
    }
    uint32_t k = (ip >> 2) & 0xFFFFu;
    if (!(s_hle_filter[k >> 3] & (1u << (k & 7u))))
        return 1;
    const hle_hook_entry_t *h = p->hooks;
    size_t n = p->hook_count;
    for (size_t i = 0; i < n; i++) {
        if (h[i].addr == ip)
            return h[i].fn(cpu, bus);
    }
    return 1;
}

#endif /* HLE_HOOKS_H */
