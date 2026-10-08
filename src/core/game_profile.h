/*
 * game_profile.h — per-ROMset descriptor.
 *
 * The board layer (CPU, memory, COP, tile/3D renderers) is shared across the
 * Model 2 catalogue. Per-game variability lives here:
 *
 *   - load_fn        : reads files from one or two zips, fills a romset_t.
 *                      Each game's ROM set has its own MAME-style transforms
 *                      (interleave / byte-swap / mirror), so this is a
 *                      function pointer rather than a static manifest.
 *   - install_fn     : copies the loaded romset into the active memory bus,
 *                      handles per-game region mirroring (e.g. STF's
 *                      XTRA_DATA→ROM[0x01000000] mapping), and sets the
 *                      CPU's initial IP from the PRCB.
 *   - hooks          : address → HLE hook function table
 *   - input_map      : abstract action → I/O port bit
 *   - quirks         : mesh-pointer offsets, model table, camera, etc.
 *
 * The active profile is resolved at ROM-load time from the set's name, the zip
 * basename (profile_for_rom_set); --profile names one outright. The CRC32s
 * validate each file the profile loads.
 */
#ifndef GAME_PROFILE_H
#define GAME_PROFILE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct i960_cpu;
struct memory_bus;
struct romset;

/* ---- Board variants ------------------------------------------------------ */

typedef enum {
    BOARD_MODEL2,        /* original — Daytona, VF2 */
    BOARD_MODEL2A_CRX,   /* Virtua Cop, VF2.1 */
    BOARD_MODEL2B_CRX,   /* Sonic The Fighters, Fighting Vipers, VC2, Last Bronx */
    BOARD_MODEL2C_CRX,   /* Sega Rally, Dynamite Cop, Top Skater */
} board_variant_t;

/* ---- HLE hook table ------------------------------------------------------ */

typedef int (*hle_hook_fn)(struct i960_cpu *cpu, struct memory_bus *bus);

typedef struct {
    uint32_t    addr;         /* ROM address; hook fires before the instruction at this PC */
    hle_hook_fn fn;
    const char *name;
} hle_hook_entry_t;

#define HLE_HOOK_TABLE_MAX 256

/* ---- Input map ----------------------------------------------------------- */

typedef enum {
    GAME_INPUT_P1_UP, GAME_INPUT_P1_DOWN, GAME_INPUT_P1_LEFT, GAME_INPUT_P1_RIGHT,
    GAME_INPUT_P1_B1, GAME_INPUT_P1_B2, GAME_INPUT_P1_B3, GAME_INPUT_P1_B4,
    GAME_INPUT_P1_START, GAME_INPUT_P1_COIN,
    GAME_INPUT_P2_UP, GAME_INPUT_P2_DOWN, GAME_INPUT_P2_LEFT, GAME_INPUT_P2_RIGHT,
    GAME_INPUT_P2_B1, GAME_INPUT_P2_B2, GAME_INPUT_P2_B3, GAME_INPUT_P2_B4,
    GAME_INPUT_P2_START, GAME_INPUT_P2_COIN,
    GAME_INPUT_SERVICE, GAME_INPUT_TEST,
    GAME_INPUT_COUNT
} game_input_t;

/*
 * Inputs reach the game through the board's I/O ports (input.h), which the
 * game's own read_sw copies into RAM. A profile only says which port bit each
 * abstract action is.
 */
typedef struct {
    uint32_t bits[GAME_INPUT_COUNT];  /* mask per abstract action */
} game_input_map_t;

/* ---- Quirks -------------------------------------------------------------- */

/* See game_quirks_t.attract_replay. */
typedef struct {
    uint32_t step_addr;
    uint8_t  from_step, to_step;
    uint32_t ready_addr;
    uint32_t state_addr;
    uint8_t  state_count;
    uint32_t state[16];
} attract_replay_t;

typedef struct {
    uint32_t mesh_ptr_subtract;     /* 0 = board default 0x02000010 */
    uint32_t mesh_ptr_add;          /* 0 = board default 0x10 */
    uint32_t model_table_offset;    /* byte offset of model table within main_data */
    uint32_t model_table_count;     /* number of entries in the model table */
    uint32_t camera_struct_addr;    /* RAM address of camera eye (x,y,z f32; 0 = no game camera) */
    /* Absolute RAM address of the camera angle word (low16 = pitch, high16 = yaw).
     * The struct layout differs per game: STF packs it at eye+0xC (0x519EA4);
     * FV keeps it separate at 0x515584. 0 = default to camera_struct_addr + 0xC. */
    uint32_t camera_angle_addr;
    bool     enable_68k_sound;      /* false = skip M68K/SCSP init (use when sound driver not yet ported) */

    /* Real interrupt delivery (board IRQ controller in irq_timer.h).
     * irq_handler[pin] = i960 address of the dispatch handler for IRQ pin 0..3
     * (pin0=VsyncScr, pin1=VsyncObj, pin2=board Timer, pin3=Other/sound UART).
     * 0 = pin unused / not yet delivered. */
    uint32_t irq_handler[4];
    /* Sound output queue (drained by the pin3/Other handler = send_sound_code).
     * Its count feeds queue_hi; a profile with one also gets the sound IRQ
     * (intreq bit 10, UART TxRDY), raised while the line is enabled. */
    uint32_t sound_queue_count_addr;   /* e.g. STF byte_504001 */

    /* Convenience: RAM flag poked to 1 each slice to auto-skip the boot warning
     * screen (0 = disabled).  STF: 0x500410. */
    uint32_t warning_skip_addr;

    /* The profile carries a hook that honours g_vs_mode (hle_hooks.h): a
     * decided versus match goes back to character select with both players
     * in. Netplay continues a session across that rematch only for a profile
     * that says so; any other board would keep the winner on against the CPU. */
    bool     vs_rematch;

    /* The profile carries a hook that honours g_enemy_rank (hle_hooks.h): the
     * CPU opponent's AI table, including STF's two unused ones. The desktop's
     * Profile menu offers the choice only for a profile that says so. */
    bool     enemy_ranks;

    /* match_replay (--match-replay / the bridge command): take attract mode
     * straight to its preprogrammed replay fight instead of playing the intro
     * movie first. At the first frame edge where the attract step byte holds
     * `from_step` and `ready_addr` is non-zero (the movie has been set up), the
     * `state_count` words in `state` are written from `state_addr` -- the movie
     * as a natural boot leaves it when the replay begins -- and the step becomes
     * `to_step`. The fight that follows has to be the one a natural boot plays,
     * bit for bit; tools/match-replay.mjs holds it against MAME doing the same.
     * step_addr 0 = not supported. */
    attract_replay_t attract_replay;

    /* No frame hook: the vblank marks the geo capture ring's frame boundary
     * (cop_geo_frame_edge), which STF's and FV's frame hooks mark where the
     * game's frame ends. The vblank itself comes for every profile, on the
     * i960's clock (irq_timer.h). */
    bool     board_vblank;

    /* GEO display-list rendering: when true, render 3D by decoding the GEO
     * display list the i960 builds in bufferram (object_data draws), instead of
     * reconstructing it from the STF COP bone stream. This is the authentic
     * hardware path — required for non-STF games and homebrew that drive the GEO
     * directly. STF/FV leave this false (their 3D is HLE'd from the COP stream). */
    bool     geo_displaylist;

    /* Take an interrupt whose pin has no irq_handler through the program's own
     * interrupt table, as the processor does (hle_irq_vector_handler): the
     * vector from the interrupt control register, the handler from the table
     * the PRCB names. For a program whose handlers no profile knows. */
    bool     irq_vectors;
} game_quirks_t;

/* ---- Loader / installer function-pointer types --------------------------- */

/* Returns 0 on success; non-zero on failure. May fall back from the primary
 * zip to the parent zip when files are shared (MAME clone). */
typedef int (*game_load_fn)(struct romset *rs,
                            const char *primary_zip,
                            const char *parent_zip);

/* Hand the loaded romset into the board: re-init the bus, copy region data,
 * apply per-game region mirrors, set initial IP from the PRCB. */
typedef void (*game_install_fn)(const struct romset *rs,
                                struct i960_cpu *cpu,
                                struct memory_bus *bus);

/* Who fights whom, and where, as a replay labels it (core/replay.h). Read off
 * the board by the profile; -1 / NULL where it does not know. */
typedef struct {
    bool        fighting;        /* a two-player versus fight is on the board */
    int         chara[2];        /* each side's character */
    const char *chara_name[2];
    int         stage;
    const char *stage_name;
    int         rounds[2];       /* rounds each side has won */
    int         rounds_to_win;
} game_match_info_t;

typedef void (*game_match_info_fn)(struct memory_bus *bus, game_match_info_t *out);

/* ---- Profile ------------------------------------------------------------- */

typedef struct game_profile {
    const char       *id;            /* "sfight" */
    const char       *display_name;  /* "Sonic the Fighters - Arcade" */
    /* The ROM set this profile runs: the zip basename it is picked by. NULL =
     * the id. Several profiles can run one set (STF's Console and Arcade); the
     * first registered is the default, see profile_for_rom_set(). */
    const char       *rom_set;
    const char       *parent_zip_name; /* MAME parent set zip name, e.g. "schamp.zip" */
    board_variant_t   board;

    game_load_fn      load_fn;
    game_install_fn   install_fn;

    hle_hook_entry_t  hooks[HLE_HOOK_TABLE_MAX];
    size_t            hook_count;

    game_input_map_t  input;

    game_quirks_t     quirks;

    /* Runs whatever program is in the set (sfight_homebrew.h). Never the
     * set's default: profile_for_program picks it for a program that is not
     * the set's game. */
    bool              any_program;

    /* The versus match on the board, for a replay's label. NULL: unknown. */
    game_match_info_fn match_info;
} game_profile_t;

/* ---- Registry ------------------------------------------------------------ */

extern const game_profile_t *const g_profiles[];
extern const size_t              g_profile_count;

extern const game_profile_t *g_active_profile;

static inline const char *profile_rom_set(const game_profile_t *p) {
    return p->rom_set ? p->rom_set : p->id;
}

/* The profile with this id, or NULL. */
static inline const game_profile_t *profile_by_id(const char *id) {
    for (size_t i = 0; id && i < g_profile_count; i++)
        if (strcmp(g_profiles[i]->id, id) == 0) return g_profiles[i];
    return NULL;
}

/*
 * The profile to run a ROM set with. `preferred` wins when it runs this set --
 * that is how --profile, a core option or the Game menu picks STF's Arcade
 * profile over the Console default, and why the choice survives the zip being
 * loaded. Otherwise the first profile registered for the set, which is the
 * default (registry.h lists it first). NULL when no profile runs the set.
 */
static inline const game_profile_t *profile_for_rom_set(const char *set,
                                                        const game_profile_t *preferred) {
    if (!set) return NULL;
    if (preferred && strcmp(profile_rom_set(preferred), set) == 0) return preferred;
    for (size_t i = 0; i < g_profile_count; i++)
        if (strcmp(profile_rom_set(g_profiles[i]), set) == 0) return g_profiles[i];
    return NULL;
}

/*
 * Whether `rom` is the program of `p`'s game, as far as its interrupt table
 * tells: p's hooks and interrupt handlers are addresses in that program. The
 * table is found the way the processor finds it at reset (word 1 of the ROM is
 * the PRCB, PRCB + 0x14 the table), and its vectors 12-15 (pins 0-3) must be
 * the handlers p names. True when there is nothing to go on: p names no
 * handlers, or the table is not in the ROM.
 */
static inline bool profile_program_matches(const game_profile_t *p, const uint8_t *rom, size_t size) {
    const uint32_t *h = p->quirks.irq_handler;
    if (!(h[0] | h[1] | h[2] | h[3]) || size < 8) return true;
#define PFP_RD32(o) ((uint32_t)rom[(o)] | (uint32_t)rom[(o) + 1] << 8 | \
                     (uint32_t)rom[(o) + 2] << 16 | (uint32_t)rom[(o) + 3] << 24)
    uint32_t prcb = PFP_RD32(4);
    if ((uint64_t)prcb + 0x18 > size) return true;
    uint32_t table = PFP_RD32(prcb + 0x14);
    if ((uint64_t)table + 36 + 8 * 4 > size) return true;
    for (int pin = 0; pin < 4; pin++)
        if (h[pin] && PFP_RD32(table + 36 + (4 + pin) * 4) != h[pin]) return false;
#undef PFP_RD32
    return true;
}

/*
 * The profile to run a loaded program with. Homebrew ships as a stock set with
 * the program EPROMs swapped (m2-pacman is `sfight` with three EPROMs
 * replaced), and the game's profile would plant its hooks in it. So: `p`,
 * unless the program is not p's game, when the set's any_program profile runs
 * it instead; and an any_program profile hands a program that IS the set's
 * game back to the set's default profile. A patched build of the game keeps
 * its interrupt table, and so its profile.
 */
static inline const game_profile_t *profile_for_program(const game_profile_t *p,
                                                        const uint8_t *rom, size_t size) {
    if (!p || !rom) return p;
    const char *set = profile_rom_set(p);
    for (size_t i = 0; i < g_profile_count; i++) {
        const game_profile_t *q = g_profiles[i];
        if (strcmp(profile_rom_set(q), set) != 0 || q->any_program == p->any_program) continue;
        if (p->any_program) {
            /* the set's default game profile, if the program is its game */
            return profile_program_matches(q, rom, size) && (q->quirks.irq_handler[0] |
                   q->quirks.irq_handler[1] | q->quirks.irq_handler[2] | q->quirks.irq_handler[3]) ? q : p;
        }
        return profile_program_matches(p, rom, size) ? p : q;
    }
    return p;
}

/* After a load: make the active profile the one for the program just loaded
 * (profile_for_program). True if it changed. */
static inline bool profile_adopt_program(const uint8_t *rom, size_t size) {
    const game_profile_t *p = profile_for_program(g_active_profile, rom, size);
    if (p == g_active_profile) return false;
    g_active_profile = p;
    return true;
}

#endif /* GAME_PROFILE_H */
