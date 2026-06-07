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
 *   - hooks          : address → HLE hook function table (later phase)
 *   - input_map      : abstract action → I/O port bit (later phase)
 *   - quirks         : polygon-decoder mask, mesh-pointer offsets, etc.
 *
 * The active profile is resolved at ROM-load time. For now main.c just picks
 * by name from g_profiles; later this becomes CRC32 detection of the picked
 * zip against each profile's identifier files.
 */
#ifndef GAME_PROFILE_H
#define GAME_PROFILE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

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
 * Most Model 2 games write the pad state into RAM (held flags + momentary
 * flags), with the i960 reading those RAM words rather than the I/O ports
 * directly.  We mirror that: the host UI sets a 32-bit `held` and
 * `pending` bitfield (built from per-action masks below) and writes them
 * to the RAM addresses on each frame.
 */
typedef struct {
    uint32_t held_addr;        /* held bits live here    (e.g. STF: 0x500700) */
    uint32_t momentary_addr;   /* momentary bits go here (e.g. STF: 0x500704) */
    uint32_t p1_credits_addr;  /* per-player credit byte (0 = not used)        */
    uint32_t p2_credits_addr;
    uint32_t bits[GAME_INPUT_COUNT];  /* mask per abstract action */
} game_input_map_t;

/* ---- Quirks -------------------------------------------------------------- */

typedef struct {
    uint16_t poly_connect_mask;     /* 0 = board default 0x45B4 (STF-tuned) */
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
     * The sound IRQ (intreq bit 10, UART TxRDY) is asserted while the queue has
     * data: count byte > 0, or state byte != 0xFF (command in progress). */
    uint32_t sound_queue_count_addr;   /* e.g. STF byte_504001 */
    uint32_t sound_queue_state_addr;   /* e.g. STF byte_504014 */

    /* Convenience: RAM flag poked to 1 each slice to auto-skip the boot warning
     * screen (0 = disabled).  STF: 0x500410. */
    uint32_t warning_skip_addr;
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

/* ---- Profile ------------------------------------------------------------- */

typedef struct game_profile {
    const char       *id;            /* "sfight" */
    const char       *display_name;  /* "Sonic The Fighters (Export, US)" */
    const char       *parent_zip_name; /* MAME parent set zip name, e.g. "schamp.zip" */
    board_variant_t   board;

    game_load_fn      load_fn;
    game_install_fn   install_fn;

    hle_hook_entry_t  hooks[HLE_HOOK_TABLE_MAX];
    size_t            hook_count;

    game_input_map_t  input;

    game_quirks_t     quirks;
} game_profile_t;

/* ---- Registry ------------------------------------------------------------ */

extern const game_profile_t *const g_profiles[];
extern const size_t              g_profile_count;

extern const game_profile_t *g_active_profile;

#endif /* GAME_PROFILE_H */
