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
#include <stdio.h>
#include <string.h>

#include "model_table.h"

struct i960_cpu;
struct memory_bus;
struct romset;

/* ---- Board variants ------------------------------------------------------ */

typedef enum {
    /* Which game is on which board is MAME's machine config (model2.cpp). */
    BOARD_MODEL2,        /* original (model2o) — Daytona, Virtua Cop */
    BOARD_MODEL2A_CRX,   /* VF2, Sega Rally, Virtua Cop 2, Dynamite Cop */
    BOARD_MODEL2B_CRX,   /* Sonic The Fighters, Fighting Vipers, Last Bronx */
    BOARD_MODEL2C_CRX,   /* House of the Dead, Top Skater */
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

/*
 * What a profile does with the region (game_quirks_t.region, Pinboard #942/#948).
 *
 * REGION_HOOKED takes two hooks, both named in the profile's hook table, at
 * two different addresses:
 *   factory_default  puts g_region in the factory default of the country
 *                    setting (a cold boot, blank or failed backup RAM / EEPROM);
 *   over_saved       runs once the game has accepted a saved settings block,
 *                    and with g_region_over_saved up puts g_region in the
 *                    working copy's country, which country() returns.
 * A game keeps its country in backup RAM or an EEPROM, so a factory-default
 * hook alone would let a kept save ignore the web page's ?lang=. region_test
 * runs every over_saved hook on a bare bus: a saved country stays with the
 * flag down and becomes g_region with it up, for every region.
 *
 * The others say why there is no hook; `why` is that reason, in words. Only
 * the profiles region_test lists may take them, each as its listed kind, so a
 * new game can skip the hooks only by editing that test.
 */
typedef enum {
    REGION_UNDECLARED = 0,  /* region_test fails it */
    REGION_HOOKED,
    REGION_PROGRAM_ROM,     /* the country is the program ROM's (another set is another country) */
    REGION_HOMEBREW,        /* no country setting at all */
    REGION_TODO,
} game_region_kind_t;

typedef struct {
    game_region_kind_t kind;
    const char *factory_default;                     /* hook name (REGION_HOOKED) */
    const char *over_saved;                          /* hook name (REGION_HOOKED) */
    uint32_t  (*country)(struct memory_bus *bus);    /* the working copy's country byte */
    const char *why;                                 /* the other kinds */
} game_region_use_t;

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
    model_layout_t model_layout;    /* the entry's shape (model_table.h); zero = 1995's */
    uint32_t camera_struct_addr;    /* RAM address of camera eye (x,y,z f32; 0 = no game camera) */
    /* Absolute RAM address of the camera angle word (low16 = pitch, high16 = yaw).
     * The struct layout differs per game: STF packs it at eye+0xC (0x519EA4);
     * FV keeps it separate at 0x515584. 0 = default to camera_struct_addr + 0xC. */
    uint32_t camera_angle_addr;
    bool     enable_68k_sound;      /* false = skip M68K/SCSP init (use when sound driver not yet ported) */
    bool     model1_sound;          /* the sound board is a Model 1 one: MultiPCMs, not the SCSP (model1_sound.h) */

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

    /* The region (hle_hooks.h g_region; ?lang= on the web, Pinboard #942).
     * Every game the emulator runs honours it, or says why it cannot: see
     * game_region_use_t. tests/region_test.c fails a profile that does neither. */
    game_region_use_t region;

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

    /* The I/O board is a 315-5649 with a wheel, pedals and a gear lever
     * (io_5649.h): input_attach serves its analog mux and gearbox, driven by
     * the held bits named in `wheel`. */
    bool     io_5649;
    struct { uint32_t left, right, accel, brake, shift_up, shift_down; } wheel;

    /* The I/O board is a Model 1 one behind a dual-port RAM (model1io.h: its
     * Z80 runs the program in the set's iocpu region). The wheel, pedals and
     * lever are io_5649.h's model of them, named in `wheel`; `vr4` is the held
     * bit of the button IN1 bit 0 reads. */
    bool     model1_io;
    uint32_t model1_io_vr4;

    /* A four-player cabinet on the 315-5649 (MAME model2a_airwlkrs_state):
     * port F bit 7 picks the side IN1 / IN2 and IN0's start bits answer for,
     * players 1 and 2 or 3 and 4 (input.h, input_attach_sides). */
    bool     four_player_sides;

    /* Two light guns on the 315-5649's second serial channel (lightgun.h):
     * input_attach serves their positions, off-screen bits and triggers. Each
     * gun's held bits (its pad walks it; reload is the shot off the screen)
     * and its A/D range, MAME's PORT_MINMAX and default. */
    bool     lightgun;

    /* Power Sled's 315-5649 (input.h input_attach_sled): each rider's two
     * pedals on A/D channels 1 and 3 (P1 right, left) and 5 and 7 (P2), and
     * port E's Cancel Network Check switch (bit 1). The held bits that press
     * them, and what a right and a left pedal read up and down. */
    bool     sled;
    struct {
        uint32_t p1_left, p1_right, p2_left, p2_right, cancel_net;
        uint8_t  right_up, right_down, left_up, left_down;
    } pedal;
    struct {
        uint32_t up, down, left, right, trigger, reload;
        int      x_min, x_max, x_rest, y_min, y_max, y_rest;
    } gun[2];

    /* Charge the i960 what the board's caches and wait states cost on top of
     * MAME's per-opcode table (i960_timing.h). The timers and the vblank count
     * these cycles, so a game that budgets its work by a timer sees the
     * board's budget only at the board's speed. Sega Rally sets it
     * (srallyc.h); STF and FV keep the table, which MAME, their oracle, runs. */
    bool     i960_board_timing;
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
 * the board by the profile; -1 / NULL where it does not know. A one-player
 * game (players 1, Sega Rally's race) has side 0 only: its car in chara[0],
 * its course in stage. */
typedef struct {
    bool        fighting;        /* a fight is on the board: versus, or a player against the CPU; a race when players is 1 */
    int         players;         /* 1 a one-player game; 0 or 2 a versus one */
    int         chara[2];        /* each side's character */
    const char *chara_name[2];
    bool        cpu[2];          /* the side the CPU plays */
    int         difficulty;      /* the CPU's, as the board plays it */
    const char *difficulty_name;
    int         stage;
    const char *stage_name;
    int         rounds[2];       /* rounds each side has won */
    int         rounds_to_win;
} game_match_info_t;

typedef void (*game_match_info_fn)(struct memory_bus *bus, game_match_info_t *out);

/* A round in the game's own replay formats (core/replay.h keeps them in the
 * .m2replay; sfight_native.h has STF's). Each part is a block of the game's RAM
 * as the game itself wrote or would write it, named so the profile knows where
 * it goes back. */
#define GAME_NATIVE_PARTS_MAX 12
typedef struct {
    const char    *name;         /* "key_bank.bin": the file in the replay */
    uint32_t       addr;         /* where the game keeps it */
    const uint8_t *data;
    uint32_t       len;
} game_native_part_t;

typedef struct {
    int                nparts;
    game_native_part_t part[GAME_NATIVE_PARTS_MAX];
    char               json[256];   /* what the round was: one JSON object */
} game_native_round_t;

/* A round the game has ended since the last call: true, with *out pointing
 * into the profile's buffer until the next call. */
typedef bool (*game_native_take_fn)(game_native_round_t *out);
/* Put a round's parts back where the game reads them (out->json unused). NULL
 * or why not. */
typedef const char *(*game_native_put_fn)(struct memory_bus *bus, const game_native_round_t *in);
/* Play rounds back with the game's own replay (Pinboard #617): from the next
 * attract replay on, the board plays rounds[0..n-1] one after another, as
 * g_native_play (hle_hooks.h) reports. The rounds stay the caller's until it
 * stops the play (n = 0). NULL or why not. */
typedef const char *(*game_native_play_fn)(const game_native_round_t *rounds, int n);
/* Is a player's game on the board (not attract, not the test menu)? A local
 * session is recorded while it is. */
typedef bool (*game_in_play_fn)(struct memory_bus *bus);

/* A linked cabinet's role (Pinboard #969): what the room makes this machine's
 * board on a comm-board link (VIRTUAL-ON-PS3.md, "RPCN"). */
typedef enum { GAME_LINK_ALONE, GAME_LINK_MASTER, GAME_LINK_SLAVE } game_link_role_t;
/* Write the role where the ROM reads it when it next boots. False while the
 * board cannot hold one yet: Virtual-On's lives in a backup block the ROM
 * factory-fills at its first boot, so a role written before that is lost. */
typedef bool (*game_link_role_fn)(struct memory_bus *bus, game_link_role_t role);

/* ---- Profile ------------------------------------------------------------- */

typedef struct game_profile {
    const char       *id;            /* "sfight" */
    const char       *display_name;  /* "Sonic the Fighters - Arcade" */
    /* The ROM set this profile runs: the zip basename it is picked by. NULL =
     * the id. Several profiles can run one set (STF's Console and Arcade); the
     * first registered is the default, see profile_for_rom_set(). */
    const char       *rom_set;
    /* The set's name in older MAME (dynmcopb for dynamcopb), picked too. */
    const char       *rom_set_old;
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

    /* Rounds in the game's own replay formats, and the way back in. NULL: none. */
    game_native_take_fn native_take;
    game_native_put_fn  native_put;
    game_native_play_fn native_play;
    game_in_play_fn     in_play;

    /* The cabinet role for a comm-board link (net/comm_role.h). NULL: the game
     * has no link. */
    game_link_role_fn   link_role;

    /* A GGPO match plays the link: both cabinets on each machine, one player
     * on each (core/emu_twin.h). A role outside backup RAM (Power Sled's unit,
     * the program's bytes) must be a savestate extra, as it swaps with the
     * board. */
    bool                link_twin;

    /* A prepared profile (registry.h, g_prepared_profiles): the set's ROMs and
     * what is known of its data, with no board to run it yet. Says what the
     * board lacks; NULL for a runnable profile, or what a runnable one still
     * lacks (Daytona's sound board). */
    const char         *preliminary;
} game_profile_t;

/* ---- Registry ------------------------------------------------------------ */

extern const game_profile_t *const g_profiles[];
extern const size_t              g_profile_count;

extern const game_profile_t *g_active_profile;

/* Profiles kept ready for a board that cannot run them yet (Pinboard #660):
 * each loads its set and names what is missing in `preliminary`. Never picked
 * by profile_for_rom_set; a front end only says the set is known. */
extern const game_profile_t *const g_prepared_profiles[];
extern const size_t              g_prepared_count;

static inline const char *profile_rom_set(const game_profile_t *p) {
    return p->rom_set ? p->rom_set : p->id;
}

/* Whether `set` (a zip basename) is this profile's set, by its name or its
 * older MAME name. */
static inline bool profile_runs_set(const game_profile_t *p, const char *set) {
    return strcmp(profile_rom_set(p), set) == 0 || (p->rom_set_old && strcmp(p->rom_set_old, set) == 0);
}

/* The quirks' model table as the board reads it (model_table.h). No quirks:
 * an empty table. */
static inline model_table_t profile_model_table(const game_quirks_t *q) {
    return q ? model_table_make(q->model_table_offset, q->model_table_count, q->model_layout)
             : model_table_make(0, 0, (model_layout_t)MODEL_LAYOUT_1995);
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
    if (preferred && profile_runs_set(preferred, set)) return preferred;
    for (size_t i = 0; i < g_profile_count; i++)
        if (profile_runs_set(g_profiles[i], set)) return g_profiles[i];
    return NULL;
}

/* The prepared profile for a ROM set, or NULL. */
static inline const game_profile_t *profile_prepared_for_rom_set(const char *set) {
    for (size_t i = 0; set && i < g_prepared_count; i++)
        if (profile_runs_set(g_prepared_profiles[i], set)) return g_prepared_profiles[i];
    return NULL;
}

/* For a front end that found no profile for `set`: when the set is prepared,
 * a sentence saying so into buf, and true. */
static inline bool profile_prepared_message(const char *set, char *buf, size_t cap) {
    const game_profile_t *p = profile_prepared_for_rom_set(set);
    if (!p || !cap) return false;
    snprintf(buf, cap, "%s ('%s') is known but cannot run yet; the board lacks: %s",
             p->display_name, set, p->preliminary ? p->preliminary : "its hardware");
    return true;
}

/* Why a front end has nothing to run `set` with, into buf: the prepared
 * profile's sentence, or the sets that do run. */
static inline void profile_missing_message(const char *set, char *buf, size_t cap) {
    if (!cap || profile_prepared_message(set, buf, cap)) return;
    int k = snprintf(buf, cap, "no profile for ROM set '%s'. Supported:", set);
    for (size_t i = 0; i < g_profile_count && k > 0 && (size_t)k < cap; i++)
        k += snprintf(buf + k, cap - (size_t)k, " %s", profile_rom_set(g_profiles[i]));
}

/* The ROM set a path names: its basename without the extension. */
static inline const char *profile_set_from_path(const char *path, char *id, size_t cap) {
    const char *sep = strrchr(path, '/'), *bsl = strrchr(path, '\\');
    if (bsl && (!sep || bsl > sep)) sep = bsl;
    const char *base = sep ? sep + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= cap) n = cap - 1;
    memcpy(id, base, n);
    id[n] = 0;
    return base;
}

/* MAME clone fall-through: p's parent zip, beside `zip`, into buf. NULL for a
 * set with no parent (or a path too long). */
static inline const char *profile_parent_path(const game_profile_t *p, const char *zip,
                                              char *buf, size_t cap) {
    if (!p->parent_zip_name) return NULL;
    char id[8];
    size_t dir_len = (size_t)(profile_set_from_path(zip, id, sizeof id) - zip);
    if (dir_len + strlen(p->parent_zip_name) >= cap) return NULL;
    memcpy(buf, zip, dir_len);
    strcpy(buf + dir_len, p->parent_zip_name);
    return buf;
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
