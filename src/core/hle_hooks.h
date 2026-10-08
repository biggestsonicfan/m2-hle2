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

/* A versus match was just decided: 1 = the 1P side won, 2 = the 2P side, 0 =
 * nothing. Set by the profile's versus hook -- an observe-only hook on the
 * arcade's own "match over" path -- and taken by the emu thread at the end of
 * the frame (netplay_end_frame), so every board in a netplay room sees a result
 * on the same frame. Part of a board reset (emu_board_reset_state). */
static volatile int g_versus_result = 0;

/* match_replay's stage (--match-replay-stage): the stage the replay fight is
 * played on instead of its own. g_match_replay_stage is what was asked for;
 * the jump arms g_replay_stage_pin with it, and the profile's stage hook then
 * writes it where the replay hands stage_num to change_scene. MAME's side does
 * the same from the jump on (tools/mame/match-replay.lua, MR_STAGE). -1: off. */
static int          g_match_replay_stage = -1;
static volatile int g_replay_stage_pin   = -1;

/* match_replay: 0 off, 1 armed, 2 done (the jump was made), -1 the profile has
 * no attract replay. See game_quirks_t.attract_replay. */
static volatile int      g_match_replay = 0;
static volatile uint32_t g_match_replay_frame = 0;

/* At a game frame's end: if armed and attract mode is at the profile's movie
 * step with the movie set up, write the movie state a natural boot has when the
 * replay starts and move on to the replay step. It runs where the game's frame
 * ends (the profile's frame hook, or the vblank for a board_vblank profile),
 * the point MAME's tools/mame/match-replay.lua makes the same jump at: made at
 * a vblank in the middle of the game's frame, the fight split from MAME's
 * (Pinboard #253). */
static inline void hle_match_replay_edge(memory_bus_t *bus) {
    if (g_match_replay != 1 || !g_active_profile) return;
    const attract_replay_t *ar = &g_active_profile->quirks.attract_replay;
    if (!ar->step_addr) { g_match_replay = -1; return; }
    if (mem_read8(bus, ar->step_addr) != ar->from_step) return;
    if (ar->ready_addr && mem_read32(bus, ar->ready_addr) == 0) return;
    for (int i = 0; i < ar->state_count; i++)
        mem_write32(bus, ar->state_addr + 4u * (uint32_t)i, ar->state[i]);
    mem_write8(bus, ar->step_addr, ar->to_step);
    g_replay_stage_pin = g_match_replay_stage;
    g_match_replay = 2;
    g_match_replay_frame = g_dl_frame_now;
    LOG_INFO("match_replay: attract step %u -> %u at frame %u", ar->from_step, ar->to_step, g_dl_frame_now);
}

/* The game's frame hook: the display-list capture's frame mark and the
 * match_replay jump, both on the game's frame rather than the board's. */
static inline void hle_game_frame_edge(memory_bus_t *bus) {
    dl_game_frame_edge(bus);
    hle_match_replay_edge(bus);
}

/* The region the board powers up as, for games whose region is a backup-RAM
 * setting (STF's country_val: 0 Japan, 1 USA, 2 Export). A profile's hook
 * applies it where the game writes its factory default, so it holds for every
 * cold boot with blank backup RAM and for the test menu's INITIALIZE; saved
 * backup RAM (core/backup_ram.h) keeps the region it was saved with. USA by
 * default; --region picks another. MAME's sfight boots as Japan, so the graders ask for japan. Both
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

/* A room's PLAYER MATCH rules (the PS3 port's RULE MENU, ps3ui_app.h), for a
 * room on our server. Each is 0 for the factory setting, which is what every
 * board outside such a room plays, the graders included, and each is the
 * owner's for every board in the match, like g_damage_real.
 *   g_rounds_to_win  GAME ASSIGNMENTS +0x01 (factory 2), put in by
 *                    sfight.h rounds_default where the game writes its default
 *   g_round_time     seconds a round lasts (`time`, 0x500090), put in at
 *                    GAME_INT as the PS3 does (sfight.h xplay_game_time)
 *   g_game_type      Type A..D = 0..3: the flag byte's BARRIER RESET (b3, 1 =
 *                    ON) and HYPER MODE (b6, 1 = OFF) as the PS3 sets them
 *                    from its table at EBOOT 0x377AB0; Type A is factory 0 */
static volatile int g_rounds_to_win = 0;
static volatile int g_round_time    = 0;
static volatile int g_game_type     = 0;

/* Secret character (the PS3's rule row 0x173): whether Start on a fighter with
 * a hidden variant picks it (sfight_console.h, sfc_hidden_toggle). On, the
 * default, is the Console version as Sega's DLL ships it; a room on our server
 * sets it from its rules. A PS3 match takes the PS3 room's own (g_xplay_secret):
 * with it Off the PS3 offers no hidden fighter online. */
static volatile int g_hidden_chars = 1;

/* The CPU opponent's AI table (STF's match_enemy_rank_data, picked per fight by
 * sub_3B22C from the cabinet's ENEMY RANK). -1, the default, plays the
 * cabinet's; 0..5 plays that table instead: Easy, Normal, Hard, Hardest, and
 * the two the ROM carries but never points at, Extra 1 (0x93428) and Extra 2
 * (0x93728). Only the AI table changes (sfight.h enemy_rank_table); the rest
 * of what ENEMY RANK decides still follows the cabinet's setting. A netplay
 * session plays the cabinet's (g_hle_netplay_board), so it cannot split two
 * boards; --enemy-rank, the Profile menu and the PS3 menus' Difficulty set it. */
static volatile int g_enemy_rank = -1;
#define ENEMY_RANKS 6
static const char *const g_enemy_rank_names[ENEMY_RANKS] = {
    "Easy", "Normal", "Hard", "Hardest", "Extra 1", "Extra 2"
};

/* "easy", "normal", "hard", "hardest", "extra1" or "extra2" (or 0..5);
 * "cabinet" is -1. -2 for anything else. */
static inline int enemy_rank_parse(const char *s) {
    static const char *const keys[ENEMY_RANKS] = {
        "easy", "normal", "hard", "hardest", "extra1", "extra2"
    };
    if (!s) return -2;
    if (!strcmp(s, "cabinet") || !strcmp(s, "-1")) return -1;
    if (s[0] >= '0' && s[0] < '0' + ENEMY_RANKS && !s[1]) return s[0] - '0';
    for (int i = 0; i < ENEMY_RANKS; i++)
        if (!strcmp(s, keys[i])) return i;
    return -2;
}

/* Up while a netplay session owns the board (netplay_active, set by the run
 * loop each slice): host-side play settings that no room carries stand down. */
static volatile int g_hle_netplay_board = 0;

/* The flag byte's game-type bits for Type A..D (0..3). */
static inline uint8_t game_type_flag_bits(int type) {
    static const uint8_t bits[4] = { 0x00, 0x40, 0x08, 0x48 };
    return bits[type & 3];
}

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
/* The PS3 room's Secret character rule (room byte 0x0C): hidden fighters can
 * be picked in this match (sfight_console.h). */
static volatile int      g_xplay_secret        = 0;
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
        cpu->globals.fp = cpu->frame_fp[cpu->frame_depth];
    } else {
        LOG_WARN("hle_ret: empty frame stack at IP=0x%08X", cpu->sfr.ip);
        cpu->halted = 1;
        emu_attn_bump();   /* the run loop tests a halt only on its slow path */
    }
}

/* Inject a call to target: push current frame with rip = ret_ip, redirect IP.
 * When target executes `ret`, it returns to ret_ip and the saved frame is restored. */
static inline void hle_call(i960_cpu_t *cpu, uint32_t target, uint32_t ret_ip) {
    if (cpu->frame_depth < FRAME_STACK_DEPTH) {
        cpu->frame_stack[cpu->frame_depth] = cpu->locals;
        cpu->frame_irq[cpu->frame_depth] = 0;
        cpu->frame_fp[cpu->frame_depth] = cpu->globals.fp;
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

/*
 * The handler the i960 itself would vector interrupt pin 0..3 to, as MAME's
 * i960 does (execute_set_input / take_interrupt): the pin's vector is byte
 * `pin` of the interrupt control register, which the program writes with a
 * synmov to 0xFF000004 (it lands in the IAC block), and the handler is word
 * 9 + (vector - 8) of the interrupt table the PRCB names. 0 when the program
 * has not set the pin up (vector 0 is MAME's unsupported IAC mode), or when the
 * processor's priority masks it: a vector's priority is vector / 8, and it is
 * taken only above the current priority, or at 31.
 *
 * STF's own table gives exactly the handlers its profile names (vectors 12-15:
 * 0xC40, 0xD10, 0xD30, 0xDF0); a program that is not STF finds its own here.
 */
static inline uint32_t hle_irq_vector_handler(const i960_cpu_t *cpu, memory_bus_t *bus, int pin,
                                              uint32_t *vector_out) {
    if (pin < 0 || pin > 3 || !cpu->prcb) return 0;
    uint32_t vector = (mem_read32(bus, IAC_BASE + 4) >> (8 * pin)) & 0xFFu;
    if (vector < 8) return 0;
    uint32_t pri = vector >> 3, cur = (cpu->sfr.pc >> 16) & 0x1Fu;
    if (pri != 31 && pri <= cur) return 0;
    uint32_t table = mem_read32(bus, cpu->prcb + PRCB_INTR_TABLE);
    if (vector_out) *vector_out = vector;
    return mem_read32(bus, table + 36 + (vector - 8) * 4);
}

/*
 * Deliver an interrupt the way the processor does (MAME take_interrupt): on
 * the interrupt stack the PRCB names, unless the processor is already in the
 * interrupted state (PC bit 13) and so on it, with the frame the call builds
 * there, and the process priority raised to the vector's for the handler. `ret`
 * puts PC, and so the priority, back (hle_interrupt).
 *
 * hle_interrupt alone runs the handler on the interrupted code's stack, from
 * its SP up. STF's handlers are written for that; a gcc960 program's are not.
 * Its SP is only where its frame ends, and an m2-sdk handler stores the global
 * registers from there (`stq g0, (sp)` ...), over what the interrupted code
 * keeps above it: m2-pacman's picture broke into garbage once the vblank came
 * in during its sound queue's pump.
 */
static inline void hle_interrupt_on_stack(i960_cpu_t *cpu, memory_bus_t *bus, uint32_t handler,
                                          uint32_t vector) {
    uint32_t sp = (cpu->sfr.pc & 0x2000u) ? cpu->locals.sp
                                          : mem_read32(bus, cpu->prcb + PRCB_INTR_STACK);
    int d = cpu->frame_depth;
    hle_interrupt(cpu, handler);
    if (cpu->frame_depth != d + 1) return;
    sp = ((sp + FRAME_ALIGN_MASK) & ~FRAME_ALIGN_MASK) + 64;   /* MAME's padding frame */
    cpu->globals.fp = sp;
    cpu->locals.sp  = sp + 64;
    cpu->sfr.pc = (cpu->sfr.pc & ~0x001F0401u) | ((vector >> 3) << 16) | 0x2002u;
}

/* One bit per (ip >> 2) & 0xFFFF, set for every hook address of the profile it
 * was built for (none without a profile): a clear bit means no hook can match,
 * so the table walk is skipped. 16 bits of instruction index, 8 KB: an address
 * shares a bit with a hook only once in 64 KB of code, where 12 bits sent one
 * instruction in a few dozen through the walk. */
static const game_profile_t *s_hle_filter_profile = NULL;
static uint8_t               s_hle_filter[65536 / 8];

/* Hooks that do not belong to a profile: --gems-i960 (gems.h) puts the Sonic
 * Gems Collection's C at its trap sites this way. g_hle_extra_sites lists the
 * addresses for the filter; g_hle_extra_hook is asked first at any of them and
 * declines (1) like a profile hook. Bump g_hle_filter_gen after a change. */
static const uint32_t *g_hle_extra_sites = NULL;
static size_t          g_hle_extra_count = 0;
static int (*g_hle_extra_hook)(i960_cpu_t *cpu, memory_bus_t *bus) = NULL;
static unsigned        g_hle_filter_gen  = 0;
static unsigned        s_hle_filter_gen  = 0;
/* Idle loops found in the program rather than named by a profile: homebrew on
 * a game's board runs the any_program profile, which has no addresses of its
 * own, so m2_spin_find (m2_spin.h) scans the program at install for the loops
 * m2_spin_skip can skip and lists them here. They count only while the profile
 * they were found for is the active one. Bump g_hle_filter_gen after a change. */
#define HLE_SPIN_SITES_MAX 64
static uint32_t              g_hle_spin_sites[HLE_SPIN_SITES_MAX];
static size_t                g_hle_spin_count   = 0;
static const game_profile_t *g_hle_spin_profile = NULL;
static int (*g_hle_spin_hook)(i960_cpu_t *cpu, memory_bus_t *bus) = NULL;
/* Turns the above off for a netplay session (gems_off_for_session). */
static void (*g_hle_extra_session_off)(void) = NULL;

/* Rebuild the filter if the active profile changed. The run loop does this
 * once per slice (the profile cannot change inside one) and then calls
 * hle_check_synced per instruction; hle_check does both. */
static inline void hle_filter_sync(void) {
    const game_profile_t *p = g_active_profile;
    if (s_hle_filter_profile == p && s_hle_filter_gen == g_hle_filter_gen) return;
    memset(s_hle_filter, 0, sizeof(s_hle_filter));
    for (size_t i = 0; p && i < p->hook_count; i++) {
        uint32_t k = (p->hooks[i].addr >> 2) & 0xFFFFu;
        s_hle_filter[k >> 3] |= (uint8_t)(1u << (k & 7u));
    }
    for (size_t i = 0; g_hle_extra_hook && i < g_hle_extra_count; i++) {
        uint32_t k = (g_hle_extra_sites[i] >> 2) & 0xFFFFu;
        s_hle_filter[k >> 3] |= (uint8_t)(1u << (k & 7u));
    }
    for (size_t i = 0; p && p == g_hle_spin_profile && i < g_hle_spin_count; i++) {
        uint32_t k = (g_hle_spin_sites[i] >> 2) & 0xFFFFu;
        s_hle_filter[k >> 3] |= (uint8_t)(1u << (k & 7u));
    }
    s_hle_filter_profile = p;
    s_hle_filter_gen     = g_hle_filter_gen;
}

/* A hook may stand in for a run of instructions rather than the one it sits
 * on (m2_texload.h's rows), and then the board must not be able to tell: the
 * run loop has to count every one of them, against the slice and in the
 * frame's step total, and the live timers have to see their cycles.
 *
 * g_hle_room is how many instructions the slice has left, the hooked one
 * included, set just before a hook runs. A hook that would need more declines
 * (returns 1) and the i960 runs the code, so a slice still ends on the same
 * instruction. Anything that steps one instruction at a time -- a debugger, a
 * test -- passes 1, and sees the real instructions.
 *
 * A hook that did stand in for more sets g_hle_extra to the instructions
 * beyond the first and bumps the attention word; the run loop's slow path adds
 * them to its count. Cycles it adds to cpu->cycles itself (i960_step_core
 * charges none for a hooked instruction). */
static uint32_t g_hle_room = 1;
static uint32_t g_hle_extra = 0;

/* Dispatch: walk the active profile's hook table and call the first match.
 * The filter must be in sync with g_active_profile (hle_filter_sync). `room`
 * is the slice's instructions left (g_hle_room). */
static inline int hle_check_synced(i960_cpu_t *cpu, memory_bus_t *bus, uint32_t room) {
    const game_profile_t *p = g_active_profile;
    uint32_t ip = cpu->sfr.ip;
    uint32_t k = (ip >> 2) & 0xFFFFu;
    if (!(s_hle_filter[k >> 3] & (1u << (k & 7u))))
        return 1;
    g_hle_room = room;
    if (g_hle_extra_hook && g_hle_extra_hook(cpu, bus) == 0) return 0;
    if (!p) return 1;
    if (p == g_hle_spin_profile)
        for (size_t i = 0; i < g_hle_spin_count; i++)
            if (g_hle_spin_sites[i] == ip) return g_hle_spin_hook(cpu, bus);
    const hle_hook_entry_t *h = p->hooks;
    size_t n = p->hook_count;
    for (size_t i = 0; i < n; i++) {
        if (h[i].addr == ip)
            return h[i].fn(cpu, bus);
    }
    return 1;
}

static inline int hle_check(i960_cpu_t *cpu, memory_bus_t *bus) {
    hle_filter_sync();
    return hle_check_synced(cpu, bus, 1);
}

#endif /* HLE_HOOKS_H */
