/*
 * netplay.h — where the netcode meets the board.
 *
 * Everything below this file is generic (rpcn_session.h knows about rooms,
 * lockstep.h knows about frames); this is the only place that knows there is an
 * i960 at the other end of it.
 *
 * ── HOW A SESSION WORKS HERE, AND WHY IT IS NOT LIKE YAMP's ────────────────
 *
 * yampnet starts a round inside an already-running emulator and re-seeds the
 * game's RNG so both machines agree. That works because its host can put two
 * emulators into the same state on demand. This emulator has no savestates, so
 * the only state two machines can be *certain* to share is the one the board is
 * in a microsecond after power-on.
 *
 * So a netplay session here is: BOTH PEERS COLD-RESET THE BOARD at the barrier,
 * and every frame from boot is lockstepped. That is a stronger guarantee than a
 * shared seed, not a weaker one — there is no window in which the two machines
 * were ever allowed to differ. It costs the boot sequence (a few seconds of
 * SEGA logo) at the start of a session, which is also what an arcade operator
 * flipping the power on both cabinets gets.
 *
 * It is sound because the simulation is already deterministic in exactly the way
 * this needs: `tools/match-replay.mjs` plays attract mode's preprogrammed fight
 * against a MAME reference frame by frame, which only holds if identical inputs
 * from a reset produce identical state. The board timers are ticked by a fixed
 * cycle count per slice and the sound board runs a fixed sample count, so nothing
 * in the emulated path reads a host clock. The pieces that DO read one — frame
 * pacing, the UI, the audio device — are all outside the simulation.
 *
 * ── INPUT ──────────────────────────────────────────────────────────────────
 *
 * The board reads its pads through the I/O ports (input.h): the host keeps a
 * 32-bit active-high `held` mask and the game's own vblank handler reads it back
 * as active-low port bytes. That makes the wire format almost trivial — there is
 * no pad struct to reconstruct and no analog axis to re-derive, which is most of
 * what yampnet's PadCodec exists for.
 *
 * Each peer transmits a 12-bit CANONICAL word (up/down/left/right, B1-B4, start,
 * coin, service, test) describing what ITS player is doing, and both machines
 * compose the same `held` mask from the two words:
 *
 *     held = expand(word[0] -> P1 bits) | expand(word[1] -> P2 bits)
 *          | expand(word[0] -> service/test)
 *
 * THE DETERMINISM RULE still applies and is the reason this is done for BOTH
 * players rather than leaving the local side as the keyboard produced it: the
 * local machine must feed the board exactly what it transmitted, not the richer
 * thing it knows. Here that falls out for free, because the canonical word is
 * losslessly the same information — but the composition is still done from the
 * two words on both machines, so there is no path by which one side's board sees
 * a bit the other's did not.
 *
 * The canonical form also means the guest plays on P2 without rebinding anything:
 * whichever key set you press locally becomes your own side's bits.
 *
 * ── ROOMS OF MORE THAN TWO ─────────────────────────────────────────────────
 *
 * A room holds up to eight (room.h, after the PS3 port's Room Match). Two of
 * them fight; everyone else waits in line and watches. The owner decides who
 * fights by writing the room's state to the server, and every member reacts to
 * that state the same way:
 *
 *   * A new MATCH is a new lockstep generation. BOTH fighters and EVERY watcher
 *     cold-reset the board for it, so a match starts from the one state every
 *     machine is certain to share -- the same reason a two-player session does.
 *     The PS3 port never resets: it forces START in attract and zeroes the frame
 *     counter at character select. It gets away with that on one build of one
 *     emulator; this one has to hold bit for bit against MAME and across native
 *     and wasm builds, and a reset is the only thing that promises it.
 *   * The fighters lockstep with each other exactly as two players always did.
 *     Watchers run the same board from the same two input streams and never hold
 *     anybody up (lockstep.h, "WATCHERS").
 *   * The result is read off the board -- the game profile's versus hook, on the
 *     same frame on every machine -- and the owner moves the line: winner to the
 *     front and staying on their side, loser to the back (room.h).
 *
 * WHICH SIDE A PLAYER DRIVES is the only thing that moves between matches. The
 * wire word is still "what my player is doing"; the room state says which side
 * of the cabinet each fighter's word lands on.
 *
 * ── THREADING ──────────────────────────────────────────────────────────────
 *
 * All netplay state belongs to the EMU THREAD, which pumps it once per slice.
 * The UI thread only posts commands into a small mutex-guarded queue and reads a
 * published snapshot, so nothing here is touched from two threads at once. The
 * pump runs OUTSIDE the emu mutex on purpose: a TLS connect blocks for up to a
 * few seconds, and doing that while holding the mutex freezes the UI (the
 * threading invariant in CLAUDE.md, in a new place).
 */
#ifndef NETPLAY_H
#define NETPLAY_H

/* net_socket.h first, and this header first in main.c: it owns the winsock
 * include order. See the note at the top of net_socket.h. */
#include "net_socket.h"

#include "com_id.h"
#include "lockstep.h"
#include "room.h"
#include "rpcn_session.h"

#include "constants.h"
#include "game_profile.h"
#include "i960.h"
#include "input.h"
#include "log.h"
#include "memory.h"
#include "thread_mutex.h"

#ifdef _WIN32
#  include <shellapi.h>   /* ShellExecuteA, for opening the Twitch activation page */
#  pragma comment(lib, "shell32.lib")
#elif !defined(__EMSCRIPTEN__)
#  include <sys/stat.h>   /* mkdir, for the per-user settings directory */
#  include <unistd.h>     /* getpid */
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/*
 * The peer-to-peer protocol revision, published in the room attribute word and
 * checked on join. Bump for any change to the packet layout, the canonical input
 * word, or a lockstep rule both sides must agree on. Unlike the ComId revision
 * channel (com_id.h) this does not partition the room list — it lets a joiner be
 * TOLD why a room is not joinable instead of finding out at the barrier.
 *
 * 2: rooms of up to eight (room.h) and the 12-byte packet header.
 */
#define NETPLAY_PROTO_REV 2

/* Room attribute word layout. Bits 28-31 are left alone: the server owns
 * SCE_NP_MATCHING2_ROOM_FLAG_ATTR_FULL (0x20000000) in there and rewrites it.
 *
 * The low byte was the protocol revision alone; its top two bits now carry the
 * BUILD FAMILY (see below). Native rooms keep family 0, so their byte is the
 * bare revision exactly as before and every existing build reads them as it
 * always has. A web room's byte is 0x41, which a build from before this reads
 * as "a different netplay protocol" and refuses, with a sentence, before the
 * join. That refusal is the one wanted, and it costs no native release. */
#define NETPLAY_ROOM_REV_SHIFT    0
#define NETPLAY_ROOM_REV_MASK     0x3Fu
#define NETPLAY_ROOM_FAMILY_SHIFT 6
#define NETPLAY_ROOM_FAMILY_MASK  0x3u
#define NETPLAY_ROOM_DELAY_SHIFT  8
#define NETPLAY_ROOM_DELAY_MASK   0xFu
#define NETPLAY_ROOM_GAME_SHIFT   12
#define NETPLAY_ROOM_GAME_MASK    0xFFFFu

/*
 * Which builds compute the same frames. Lockstep sends inputs, not state, so two
 * boards stay together only if they compute bit-identical results, and
 * "bit-identical" is a property of the compiler and the float code it emits as
 * much as of the source. Native x86-64 (MSVC, gcc) and aarch64 built with
 * -ffp-contract=off are one family (tools/ab-builds.mjs, and the ARM parity
 * work). WebAssembly is the other. It left MSVC at frame 2948 of attract, on
 * two things C leaves open: a NaN's sign, and memory read through a pointer of
 * another type. Clang used both and MSVC used neither. With NaNs written the
 * SHARC's way (sharc_float_to_bits) and the i960's (i960_nan_result), and with
 * -fno-strict-aliasing, tests/det_digest.c holds the two identical over 12,000
 * frames of attract and 10,000 of a two-player match (WEB-NETPLAY.md,
 * "Cross-play").
 *
 * The family stays in the room word, so a lobby can still tell the two apart
 * and turn cross-play off again with this one define. An old desktop build
 * still refuses a web room (its revision byte), so a desktop player needs a
 * build with this to JOIN a web room; a web player can join a desktop room
 * hosted by any desktop build that shares this board code.
 */
#define NETPLAY_FAMILY_NATIVE 0u
#define NETPLAY_FAMILY_WASM   1u
#ifdef __EMSCRIPTEN__
#  define NETPLAY_BUILD_FAMILY NETPLAY_FAMILY_WASM
#else
#  define NETPLAY_BUILD_FAMILY NETPLAY_FAMILY_NATIVE
#endif
#ifndef NETPLAY_CROSS_PLAY
#  define NETPLAY_CROSS_PLAY 1
#endif

static inline bool netplay_families_compatible(uint32_t a, uint32_t b) {
    if (a == b) return true;
    return NETPLAY_CROSS_PLAY
        && ((a == NETPLAY_FAMILY_NATIVE && b == NETPLAY_FAMILY_WASM)
         || (a == NETPLAY_FAMILY_WASM   && b == NETPLAY_FAMILY_NATIVE));
}

#define NETPLAY_LOG_LINES 64
#define NETPLAY_LOG_LEN   160
#define NETPLAY_CMD_QUEUE 8

typedef enum {
    NETPLAY_OFF,
    NETPLAY_CONNECTING,
    NETPLAY_ONLINE,       /* logged in, no room */
    NETPLAY_IN_ROOM,      /* room taken; waiting in line, or in the lobby */
    NETPLAY_SYNCING,      /* barrier: both fighters announcing the same generation */
    NETPLAY_PLAYING,      /* fighting: the board is being stepped in lockstep */
    NETPLAY_FAILED,
    NETPLAY_WATCHING,     /* the board is running somebody else's match */
} netplay_state_t;

/* What the emu thread should do with this slice. */
typedef enum {
    NETPLAY_STEP_OFF,     /* netplay inactive — run the slice normally */
    NETPLAY_STEP_READY,   /* inputs for this frame are in, g_input.held is set */
    NETPLAY_STEP_WAIT,    /* stalled on the peer — do NOT advance the board */
    NETPLAY_STEP_RESET,   /* barrier released — reset the board, then continue */
} netplay_step_t;

typedef enum {
    NETPLAY_CMD_NONE = 0,
    NETPLAY_CMD_CONNECT,
    NETPLAY_CMD_DISCONNECT,
    NETPLAY_CMD_HOST,
    NETPLAY_CMD_JOIN,
    NETPLAY_CMD_SEARCH,
    NETPLAY_CMD_START,        /* ready to play: the owner starts once every player is */
    NETPLAY_CMD_STOP,         /* not ready; leave the match being played, stay in the room */
    NETPLAY_CMD_ENTRY,        /* cfg.entry: ask for a side (room.h, "1P/2P Entry") */
    NETPLAY_CMD_WATCH,        /* cfg.watch_only: sit out, or stop sitting out */
    NETPLAY_CMD_FORCE_START,  /* owner only: start the next match now */
    NETPLAY_CMD_LEAVE_ROOM,   /* leave the room, stay signed in */
    NETPLAY_CMD_CREATE_ACCOUNT,
    NETPLAY_CMD_RESEND_TOKEN,
    NETPLAY_CMD_TWITCH_START,   /* begin the OAuth device flow */
    NETPLAY_CMD_TWITCH_CANCEL,
    NETPLAY_CMD_TWITCH_FORGET,  /* drop the stored login token */
} netplay_cmd_kind_t;

/* Everything the UI can set. Copied into the netplay state when a command is
 * posted, so the UI's own buffers are never read from the emu thread. */
typedef struct {
    char     server[128];
    uint16_t port;
    char     fingerprint[80];
    char     npid[20];
    char     password[64];
    char     token[64];
    char     email[128];        /* sign-up only */
    /*
     * The Twitch login token. The device flow runs once and the server hands back
     * one of these; RPCN's Login accepts it IN PLACE OF THE PASSWORD, so from
     * here on a Twitch account is an ordinary login and nothing below netplay.h
     * knows the difference. Non-empty means "signed in with Twitch", and it is
     * what gets sent as the password.
     */
    char     twitch_token[80];
    /*
     * WHOSE token that is. A machine has one settings file and more than one
     * account can pass through it, so a token with no owner gets offered as
     * the password for whatever npid happens to be stored beside it -- which
     * RPCN answers with "wrong password for that account". Empty means a file
     * written before this existed; `netplay_settings_load` adopts the stored
     * npid in that case, which is what such a file always meant.
     */
    char     twitch_npid[20];
    char     room_password[16];
    uint64_t room_id;           /* join target */
    uint32_t frame_delay;
    uint32_t max_players;       /* hosting: 2..8; 0 = 2 */
    uint8_t  entry;             /* room_entry_t, for NETPLAY_CMD_ENTRY */
    bool     watch_only;        /* for NETPLAY_CMD_WATCH */
    bool     browse_yamp;       /* also search YAMP's lobby space, read-only */
    uint16_t local_p2p_port;    /* 0 = RPCN_P2P_PORT */
} netplay_config_t;

typedef struct {
    netplay_cmd_kind_t kind;
    netplay_config_t   cfg;
} netplay_cmd_t;

/* One row of the room, as the UI shows it. */
typedef struct {
    uint16_t member_id;
    char     npid[20];
    bool     is_me;
    bool     is_owner;
    int8_t   line_pos;       /* 0 = front of the line; -1 = not in it yet */
    int8_t   side;           /* 0 = 1P, 1 = 2P in the current match; -1 = not fighting */
    bool     known;          /* their member attribute has arrived */
    room_member_data_t data;
    bool     addr_known;     /* (not for is_me) the server has told us where they are */
    bool     heard;          /* (not for is_me) a datagram has come back from them */
} netplay_member_status_t;

/* The snapshot the UI draws from, published under the netplay mutex. */
typedef struct {
    netplay_state_t state;
    rpcn_stage_t    stage;
    bool            is_host;           /* we own the room (not necessarily created it) */
    int32_t         local_player;      /* 0 = 1P, 1 = 2P, 2 = watching, -1 = neither */

    /* The room. */
    uint16_t                my_member_id;
    uint32_t                max_slot;
    room_state_t            room;
    bool                    room_known;   /* the room's state has been read */
    netplay_member_status_t members[ROOM_MAX_MEMBERS];
    uint32_t                member_count;
    room_member_data_t      me;
    uint32_t                auto_start_s; /* owner's countdown to the next match; 0 = none */
    uint64_t        room_id;
    uint32_t        room_flags;
    char            com_id[COMID_BUFFER_SIZE];
    char            com_id_foreign[COMID_BUFFER_SIZE];
    char            peer_npid[20];
    char            peer_addr[32];
    bool            peer_known;
    bool            peer_heard;
    uint32_t        frame;
    uint32_t        stalls;
    uint32_t        desync_frame;      /* LOCKSTEP_NO_CHECK while in agreement */
    uint32_t        generation;
    uint32_t        seed;
    /* The peer has announced a session we have not begun -- i.e. THEY pressed
     * start and we have not. See `peer_ready_gen` in netplay_t. */
    bool            peer_ready;
    uint32_t        peer_ready_gen;

    rpcn_room_listing_t rooms[RPCN_MAX_ROOMS];
    uint32_t            room_count;
    rpcn_room_listing_t foreign_rooms[RPCN_MAX_ROOMS];
    uint32_t            foreign_room_count;
    bool                search_pending;

    rpcn_account_state_t account_state;
    rpcn_account_job_t   account_job;
    char                 account_error[256];

    rpcn_twitch_state_t  twitch_state;
    char                 twitch_user_code[32];
    char                 twitch_uri[256];
    char                 twitch_npid[20];
    char                 twitch_error[256];
    bool                 twitch_signed_in;   /* a login token is stored */

    char error[256];
    char log[NETPLAY_LOG_LINES][NETPLAY_LOG_LEN];
    uint32_t log_count;     /* total ever written; index = (n % LINES) */
} netplay_status_t;

typedef struct {
    bool              enabled;        /* a session has been asked for */
    netplay_state_t   state;
    netplay_config_t  cfg;

    rpcn_session_t    session;
    rpcn_account_t    account;
    rpcn_twitch_t     twitch;
    lockstep_t        lockstep;

    /* This machine's part in the match being run: 0 = 1P, 1 = 2P,
     * LOCKSTEP_WATCHER, or -1 when the board is not running one. */
    int32_t           local_player;
    uint32_t          generation;
    uint32_t          seed;
    uint32_t          frame;          /* frames executed since the board reset */
    bool              reset_pending;  /* the barrier released; reset before frame 0 */
    bool              delay_seeded;

    /* ---- The room (room.h) ---- */
    room_state_t        room;          /* as we last read it, or as we (the owner) last wrote it */
    bool                room_known;
    uint32_t            room_rev_seen; /* session.room_rev when `room` was last refreshed */
    bool                was_owner;
    room_member_data_t  me;            /* our own attribute; the server's copy follows this one */
    bool                me_dirty;
    uint64_t            me_sent_ms;
    /* The match this board was last started for, and whether it is still
     * running it. A match is started once: the room state saying MATCH again
     * after we have left it is the same match, not a new one. */
    uint16_t            match_started;
    bool                match_live;
    bool                match_result_seen;
    /* After our board reaches a result a fighter goes on answering for a
     * while: the other fighter may still need our last inputs, and a watcher may
     * still be catching up. */
    uint64_t            linger_until_ms;
    uint32_t            watch_stride;
    uint64_t            last_repair_ms;
    uint64_t            watch_stall_ms;
    /* Owner bookkeeping (see netplay_owner_pump). */
    bool                force_start;
    uint64_t            auto_deadline_ms;
    uint64_t            match_begun_ms;
    uint32_t            fighters_seen;   /* bit per side whose board has started this match */

    /* Desync detection. The check is a hash of the board's state at a frame
     * boundary; the FIRST disagreement is latched and reported, and the session
     * keeps running so the player can see what happened rather than being
     * dumped out of it. */
    uint32_t          check_frame[LOCKSTEP_RING_SIZE];
    uint32_t          check_value[LOCKSTEP_RING_SIZE];
    uint32_t          peer_check_frame[LOCKSTEP_RING_SIZE];
    uint32_t          peer_check_value[LOCKSTEP_RING_SIZE];
    uint32_t          last_check_frame;
    uint32_t          last_check_value;
    uint32_t          desync_frame;

    /* Input ownership, derived from the active profile's input map. */
    uint32_t          p1_mask, p2_mask, sys_mask;
    uint32_t          bit_p1[12], bit_p2[12];   /* canonical bit -> profile mask */

    uint64_t          last_wait_report_ms;
    uint64_t          stall_since_ms;
    uint32_t          stall_timeout_ms;

    /* Pacing for the two things that are sent from a loop that spins every
     * millisecond (see NETPLAY_ANNOUNCE_MS), and when the peer's input last
     * arrived -- which is what tells a peer that has gone quiet from one that is
     * still talking and not advancing. */
    uint64_t          last_announce_ms;
    uint64_t          last_resend_ms;
    uint64_t          last_peer_input_ms;

    /* The frame this board was last cleared to run, and since when. Both
     * players' inputs being in is permission to run the frame, not proof that it
     * ran: see netplay_watch_own_board. */
    uint32_t          ready_frame;
    uint64_t          ready_since_ms;
    bool              ready_reported;
    uint32_t          stopped_noted_frame;

    /*
     * The login in flight, as far as the stored Twitch login is concerned.
     *
     * `sent_twitch_token`: the password that went out WAS the token, so the
     * answer is the server's verdict on the token and on whose it is.
     * `twitch_wanted`: the caller asked to sign in with Twitch rather than as a
     * named account, so a token the server refuses falls through to the device
     * flow instead of ending in "wrong password". One-shot.
     * `twitch_tried_owner`: the token was refused and a second login has been
     * made -- under the name the token is labelled with, or with a newer token
     * another program left in the settings file. Bounds the whole thing at two
     * rejections, because asking again is how an account gets locked.
     * `twitch_synced`: the token as this process last read or wrote the file.
     * A file token that differs from it was put there by someone else.
     */
    bool              sent_twitch_token;
    bool              twitch_wanted;
    bool              twitch_tried_owner;
    char              twitch_synced[sizeof(((netplay_config_t *)0)->twitch_token)];

    /* A Host or Join held back until the server has our address (see
     * netplay_take_room). */
    bool              room_deferred;
    netplay_cmd_t     room_cmd;
    uint64_t          room_deferred_ms;

    /* UI <-> emu thread */
    emu_mutex_t       mutex;
    bool              mutex_ready;
    netplay_cmd_t     queue[NETPLAY_CMD_QUEUE];
    uint32_t          queue_head, queue_count;
    netplay_status_t  status;
    char              log[NETPLAY_LOG_LINES][NETPLAY_LOG_LEN];
    uint32_t          log_count;

    /* The host installs this: a full board reset (re-install the ROM set, reset
     * the CPU, re-attach sound and input). Called on the emu thread with the emu
     * mutex held. Netplay refuses to start without one. */
    void            (*reset_board)(void *ctx);
    void             *reset_ctx;

    /* The session's input log (netplay_inputlog_*): both players' words and the
     * check, one line per frame, so a desync can be replayed through
     * tests/det_digest.c --inputs and the frame it split on found. */
    FILE             *inlog;
    uint32_t          inlog_w0, inlog_w1;   /* this frame's words, from begin_frame */
} netplay_t;

static netplay_t g_netplay;

/* ---- The session input log ------------------------------------------------
 *
 * Lockstep sends inputs, not state, so the inputs ARE the session: replayed
 * from a cold boot they give back every frame either board computed. Each
 * session writes them next to the settings file (netplay_cfg_path's directory:
 * %APPDATA%\m2hle2, ~/.config/m2hle2, or wherever --net-config points -- the
 * libretro core points it at RetroArch's saves), as
 *
 *   netplay-<YYYYmmdd-HHMMSS>-s<session>-p<player>.inputs
 *
 * with '#' header lines (version, game, session, seed, player, delay, peer)
 * and then "frame w0 w1 check" in hex, one per frame. When two players saw
 * different games, both logs replay through the same build; the frame where a
 * log's own check stops matching the replay is where that board went its own
 * way, and the two logs' inputs say whether the boards were ever even fed the
 * same thing. Flushed every 60 frames, so a crash loses a second at most.
 * Not in the web build (no file system). */
#ifndef __EMSCRIPTEN__
static inline const char *netplay_cfg_path(void);
#endif

static inline void netplay_inputlog_close(void) {
    if (!g_netplay.inlog) return;
    fclose(g_netplay.inlog);
    g_netplay.inlog = NULL;
}

static inline void netplay_inputlog_open(void) {
    netplay_inputlog_close();
#ifndef __EMSCRIPTEN__
    char dir[512];
    snprintf(dir, sizeof dir, "%s", netplay_cfg_path());
    char *slash = strrchr(dir, '/'), *bslash = strrchr(dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash) slash[1] = '\0'; else dir[0] = '\0';
    time_t now = time(NULL);
    struct tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    char path[640];
    snprintf(path, sizeof path, "%snetplay-%04d%02d%02d-%02d%02d%02d-s%u-p%d.inputs", dir,
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
             (unsigned)g_netplay.generation, (int)g_netplay.local_player + 1);
    g_netplay.inlog = fopen(path, "w");
    if (!g_netplay.inlog) return;
    fprintf(g_netplay.inlog, "# m2-hle netplay input log\n");
    fprintf(g_netplay.inlog, "# version %s\n", M2HLE_VERSION);
    fprintf(g_netplay.inlog, "# game %s\n", g_active_profile && g_active_profile->id ? g_active_profile->id : "?");
    fprintf(g_netplay.inlog, "# session %u seed 0x%08X\n", (unsigned)g_netplay.generation, (unsigned)g_netplay.seed);
    /* player 1 or 2 is a side; 3 is a watcher (LOCKSTEP_WATCHER + 1). */
    const char *fighter[2];
    for (int side = 0; side < 2; side++) {
        uint16_t id = g_netplay.room.fighter[side];
        rpcn_peer_t *p = rpcn_session_peer(&g_netplay.session, id);
        fighter[side] = id && id == g_netplay.session.my_member_id ? g_netplay.session.npid
                      : p && p->npid[0] ? p->npid : "?";
    }
    fprintf(g_netplay.inlog, "# player %d delay %u match %u 1P %s 2P %s\n", (int)g_netplay.local_player + 1,
            (unsigned)g_netplay.lockstep.frame_delay, (unsigned)g_netplay.room.match,
            fighter[0], fighter[1]);
    fprintf(g_netplay.inlog, "# frame w0 w1 check (hex)\n");
#endif
}

/* ---- Logging ------------------------------------------------------------- */

static inline void netplay_log(const char *fmt, ...) {
    char line[NETPLAY_LOG_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    uint32_t idx = g_netplay.log_count % NETPLAY_LOG_LINES;
    memcpy(g_netplay.log[idx], line, sizeof(line));
    g_netplay.log_count++;
    LOG_INFO("netplay: %s", line);
}

static void netplay_session_log_cb(void *ctx, const char *msg) {
    (void)ctx;
    netplay_log("%s", msg);
}

/* ---- Input mapping ------------------------------------------------------- */

/*
 * Canonical input bits. Player-agnostic on purpose: a word says what a PLAYER is
 * doing, and each machine expands it onto whichever side that player owns. Bits
 * 10 and 11 only mean anything from player 0, which owns the cabinet.
 */
enum {
    NP_BIT_UP = 0, NP_BIT_DOWN, NP_BIT_LEFT, NP_BIT_RIGHT,
    NP_BIT_B1, NP_BIT_B2, NP_BIT_B3, NP_BIT_B4,
    NP_BIT_START, NP_BIT_COIN,
    NP_BIT_SERVICE, NP_BIT_TEST,
    NP_BIT_COUNT
};

static inline void netplay_build_masks(const game_profile_t *p) {
    memset(g_netplay.bit_p1, 0, sizeof(g_netplay.bit_p1));
    memset(g_netplay.bit_p2, 0, sizeof(g_netplay.bit_p2));
    g_netplay.p1_mask = g_netplay.p2_mask = g_netplay.sys_mask = 0;
    if (!p) return;

    static const int p1_actions[10] = {
        GAME_INPUT_P1_UP, GAME_INPUT_P1_DOWN, GAME_INPUT_P1_LEFT, GAME_INPUT_P1_RIGHT,
        GAME_INPUT_P1_B1, GAME_INPUT_P1_B2, GAME_INPUT_P1_B3, GAME_INPUT_P1_B4,
        GAME_INPUT_P1_START, GAME_INPUT_P1_COIN,
    };
    static const int p2_actions[10] = {
        GAME_INPUT_P2_UP, GAME_INPUT_P2_DOWN, GAME_INPUT_P2_LEFT, GAME_INPUT_P2_RIGHT,
        GAME_INPUT_P2_B1, GAME_INPUT_P2_B2, GAME_INPUT_P2_B3, GAME_INPUT_P2_B4,
        GAME_INPUT_P2_START, GAME_INPUT_P2_COIN,
    };

    for (int i = 0; i < 10; i++) {
        g_netplay.bit_p1[i] = p->input.bits[p1_actions[i]];
        g_netplay.bit_p2[i] = p->input.bits[p2_actions[i]];
        g_netplay.p1_mask  |= g_netplay.bit_p1[i];
        g_netplay.p2_mask  |= g_netplay.bit_p2[i];
    }
    g_netplay.bit_p1[NP_BIT_SERVICE] = p->input.bits[GAME_INPUT_SERVICE];
    g_netplay.bit_p1[NP_BIT_TEST]    = p->input.bits[GAME_INPUT_TEST];
    g_netplay.sys_mask = g_netplay.bit_p1[NP_BIT_SERVICE] | g_netplay.bit_p1[NP_BIT_TEST];
}

/*
 * What this machine's player is doing, as a canonical word. Both key sets are
 * read, so the guest plays on P2 without rebinding: press either and it becomes
 * YOUR side's bits.
 */
static inline uint32_t netplay_sample_local(void) {
    uint32_t held = g_input.held;
    uint32_t word = 0;
    for (int i = 0; i < 10; i++) {
        if ((g_netplay.bit_p1[i] && (held & g_netplay.bit_p1[i]))
            || (g_netplay.bit_p2[i] && (held & g_netplay.bit_p2[i])))
            word |= (1u << i);
    }
    /* Service and test belong to the cabinet, so only player 0 may send them —
     * a guest holding F2 must not be able to open the operator menu on the
     * host's board, and more to the point, must not do it on only one of them. */
    if (g_netplay.local_player == 0) {
        if (g_netplay.bit_p1[NP_BIT_SERVICE] && (held & g_netplay.bit_p1[NP_BIT_SERVICE]))
            word |= (1u << NP_BIT_SERVICE);
        if (g_netplay.bit_p1[NP_BIT_TEST] && (held & g_netplay.bit_p1[NP_BIT_TEST]))
            word |= (1u << NP_BIT_TEST);
    }
    return word;
}

/* Compose both players' words into the board's held mask. Identical arithmetic
 * on both machines, from the same two words — that is the whole determinism
 * argument for the input path. */
static inline void netplay_apply_inputs(uint32_t w0, uint32_t w1) {
    uint32_t held = 0;
    for (int i = 0; i < 10; i++) {
        if (w0 & (1u << i)) held |= g_netplay.bit_p1[i];
        if (w1 & (1u << i)) held |= g_netplay.bit_p2[i];
    }
    if (w0 & (1u << NP_BIT_SERVICE)) held |= g_netplay.bit_p1[NP_BIT_SERVICE];
    if (w0 & (1u << NP_BIT_TEST))    held |= g_netplay.bit_p1[NP_BIT_TEST];
    /* Into the override, not over the keyboard's own mask — see input_state_t. */
    g_input.net_held = held;
    g_input.use_net  = 1;
}

/* Hand the board back to the keyboard. */
static inline void netplay_release_inputs(void) {
    g_input.use_net  = 0;
    g_input.net_held = 0;
}

/* ---- The frame check ----------------------------------------------------- */

/*
 * A value both machines must agree on for a given frame. FNV-1a over the i960's
 * architectural state at the frame boundary plus the instruction count since
 * reset — the two peers stop at the same instruction (the per-game frame hook),
 * so both are exact, and the step count alone catches any divergence in control
 * flow within a frame of it happening.
 *
 * Deliberately not a hash of all of work RAM. 1 MB a frame is affordable but the
 * register file plus the step count already fails on the first frame that
 * differs, and a cheap check that runs every frame is worth more than a thorough
 * one somebody turns off.
 */
static inline uint32_t netplay_frame_check(const i960_cpu_t *cpu, uint64_t total_steps) {
    uint32_t h = 2166136261u;
    #define NP_MIX(v) do { uint32_t _v = (uint32_t)(v); \
        for (int _b = 0; _b < 4; _b++) { h ^= (uint8_t)(_v >> (_b * 8)); h *= 16777619u; } } while (0)
    NP_MIX(cpu->sfr.ip);
    NP_MIX(cpu->sfr.ac);
    NP_MIX(cpu->sfr.pc);
    for (int i = 0; i < 16; i++) NP_MIX(cpu->globals.g[i]);
    for (int i = 0; i < 16; i++) NP_MIX(cpu->locals.r[i]);
    NP_MIX((uint32_t)total_steps);
    NP_MIX((uint32_t)(total_steps >> 32));
    #undef NP_MIX
    return h;
}

static inline void netplay_check_clear(void) {
    memset(g_netplay.check_frame, 0xFF, sizeof(g_netplay.check_frame));
    memset(g_netplay.peer_check_frame, 0xFF, sizeof(g_netplay.peer_check_frame));
    g_netplay.last_check_frame = LOCKSTEP_NO_CHECK;
    g_netplay.last_check_value = 0;
    g_netplay.desync_frame     = LOCKSTEP_NO_CHECK;
}

static inline void netplay_check_compare(uint32_t frame) {
    if (g_netplay.desync_frame != LOCKSTEP_NO_CHECK) return;   /* already latched */
    uint32_t idx = frame & LOCKSTEP_RING_MASK;
    if (g_netplay.check_frame[idx] != frame || g_netplay.peer_check_frame[idx] != frame) return;
    if (g_netplay.check_value[idx] == g_netplay.peer_check_value[idx]) return;

    g_netplay.desync_frame = frame;
    netplay_log("DESYNC at frame %u: ours 0x%08X, theirs 0x%08X - the two boards have "
                "diverged and everything after this is guesswork",
                frame, g_netplay.check_value[idx], g_netplay.peer_check_value[idx]);
}

/* ---- Room attribute word ------------------------------------------------- */

static inline uint32_t netplay_game_tag(const game_profile_t *p) {
    if (!p || !p->id) return 0;
    return comid_hash(p->id) & NETPLAY_ROOM_GAME_MASK;
}

static inline uint32_t netplay_room_flags(const game_profile_t *p, uint32_t frame_delay) {
    uint32_t f = 0;
    f |= (NETPLAY_PROTO_REV & NETPLAY_ROOM_REV_MASK) << NETPLAY_ROOM_REV_SHIFT;
    f |= (NETPLAY_BUILD_FAMILY & NETPLAY_ROOM_FAMILY_MASK) << NETPLAY_ROOM_FAMILY_SHIFT;
    f |= (frame_delay & NETPLAY_ROOM_DELAY_MASK)     << NETPLAY_ROOM_DELAY_SHIFT;
    f |= (netplay_game_tag(p) & NETPLAY_ROOM_GAME_MASK) << NETPLAY_ROOM_GAME_SHIFT;
    return f;
}

/*
 * Why a room cannot be joined, or NULL when it can. Checked BEFORE the join so
 * the answer is a sentence rather than a barrier that never releases — which is
 * the entire reason the attribute word carries anything at all.
 *
 * A room with flags of 0 is not refused: that is what a room made by a build
 * older than the attribute word looks like, and also what the server stores when
 * a client sends none, so it is reported as unknown rather than wrong.
 */
static inline const char *netplay_room_reject_reason(uint32_t flags, const game_profile_t *p) {
    if (flags == 0) return NULL;
    uint32_t rev = (flags >> NETPLAY_ROOM_REV_SHIFT) & NETPLAY_ROOM_REV_MASK;
    if (rev != NETPLAY_PROTO_REV)
        return "that room was made by a build with a different netplay protocol";
    uint32_t family = (flags >> NETPLAY_ROOM_FAMILY_SHIFT) & NETPLAY_ROOM_FAMILY_MASK;
    if (!netplay_families_compatible(family, NETPLAY_BUILD_FAMILY)) {
        return family == NETPLAY_FAMILY_WASM
            ? "that match is on the web version, and the web and desktop versions cannot play each other yet"
            : "that match is on the desktop version, and the web and desktop versions cannot play each other yet";
    }
    uint32_t tag = (flags >> NETPLAY_ROOM_GAME_SHIFT) & NETPLAY_ROOM_GAME_MASK;
    if (tag != netplay_game_tag(p))
        return "that room is for a different game than the one loaded here";
    return NULL;
}

/* ---- Packets ------------------------------------------------------------- */

static inline uint32_t netplay_session_id(void) {
    return (uint32_t)g_netplay.session.room_id;
}

static inline uint16_t netplay_my_id(void) { return g_netplay.session.my_member_id; }

static inline void netplay_fill_header(lockstep_header_t *h, uint8_t type) {
    h->type       = type;
    h->player     = (uint8_t)(g_netplay.local_player < 0 ? LOCKSTEP_WATCHER : g_netplay.local_player);
    h->generation = (uint8_t)(g_netplay.generation & 0x1F);
    h->reserved   = 0;
    h->session    = netplay_session_id();
    h->member     = netplay_my_id();
    h->pad        = 0;
}

/* The owner of the room: the member who runs it (room.h). Not necessarily who
 * created it -- RPCN hands the room on when its owner leaves. */
static inline bool netplay_is_host(void) { return rpcn_session_is_owner(&g_netplay.session); }

static inline bool netplay_is_fighter(void) {
    return g_netplay.local_player == 0 || g_netplay.local_player == 1;
}

/* The member on the other side of the cabinet from us in the match we are
 * running, or 0. */
static inline uint16_t netplay_opponent_id(void) {
    if (!netplay_is_fighter()) return ROOM_NO_MEMBER;
    return g_netplay.room.fighter[g_netplay.local_player ^ 1];
}

/* A member we are running a match with who is not fighting in it. */
static inline bool netplay_is_watcher_of_match(uint16_t id) {
    return id && id != g_netplay.room.fighter[0] && id != g_netplay.room.fighter[1];
}

static inline void netplay_send_to(uint16_t member, const void *pkt, uint32_t len) {
    if (member) rpcn_session_send_to(&g_netplay.session, member, pkt, len);
}

/* Everybody else in the room who is not fighting. */
static inline void netplay_send_watchers(const void *pkt, uint32_t len) {
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
        const rpcn_peer_t *p = &g_netplay.session.peers[i];
        if (p->used && netplay_is_watcher_of_match(p->member_id))
            rpcn_session_send_to(&g_netplay.session, p->member_id, pkt, len);
    }
}

/*
 * How long an announce counts for. A peer sitting at the barrier sends one every
 * NETPLAY_ANNOUNCE_MS, so anything above a few hundred milliseconds is generous.
 */
#define NETPLAY_READY_WINDOW_MS 2000u

/*
 * How often a machine that is WAITING repeats itself: the announce at the
 * barrier, and its newest inputs while stalled.
 *
 * Both are sent from netplay_begin_frame, and a waiting emu thread calls that
 * every millisecond. Unpaced, that is a thousand datagrams a second at one
 * address for as long as somebody sits at the barrier -- a minute, if the other
 * player is reading the screen -- and that is what a consumer gateway's UDP flood
 * detection is looking for. Twenty a second is still forty inside the ready
 * window, and releases the barrier within a frame or three of the old rate.
 */
#define NETPLAY_ANNOUNCE_MS 50u
#define NETPLAY_RESEND_MS   50u

/*
 * A fighter sends a watcher one record every this many frames, not every frame.
 * A record carries LOCKSTEP_REDUNDANCY frames, so this still delivers each frame
 * three times over. What it buys is the web gateway's per-player cap of 240
 * datagrams a second (WEB-NETPLAY.md): at one a frame a fighter in a full room
 * would need 60 x 7, and at one every three it needs 60 + 20 x 6.
 */
#define NETPLAY_WATCH_STRIDE 3u
/* A watcher missing a frame this long asks for it again; a repair is repeated
 * at this pace until it lands. */
#define NETPLAY_REPAIR_AFTER_MS 150u
#define NETPLAY_REPAIR_MS       100u
/* A watcher that has asked this long without the frame arriving gives up on
 * the match -- almost always a member who joined too far into it for the
 * fighters' 1024-frame history to still hold frame 0. */
#define NETPLAY_WATCH_GIVEUP_MS 8000u
/* A fighter whose board has reached the result keeps answering for this long. */
#define NETPLAY_LINGER_MS 5000u

/* A board cleared to run a frame and still on it this long later gets a line in
 * the log. No frame of any game is near it: the longest, a scene load, is a few
 * slices. It leaves the session at stall_timeout_ms, like any other stall. */
#define NETPLAY_OWN_BOARD_REPORT_MS 3000u
/* ...and this long on one frame is already not a frame: start repeating ourselves. */
#define NETPLAY_OWN_BOARD_QUIET_MS  250u

/*
 * Is somebody waiting for us? Another player in the lobby has pressed Start and
 * we have not. That is the room's version of a challenge: once every player is
 * ready the owner starts the match.
 */
static inline bool netplay_peer_ready(void) {
    if (g_netplay.state != NETPLAY_IN_ROOM || !g_netplay.room_known) return false;
    if (g_netplay.room.phase != ROOM_PHASE_LOBBY) return false;
    if (g_netplay.me.flags & (ROOM_MEMBER_READY | ROOM_MEMBER_WATCH)) return false;
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
        const rpcn_peer_t *p = &g_netplay.session.peers[i];
        room_member_data_t d;
        if (p->used && room_member_decode(p->bin, p->bin_len, &d) && (d.flags & ROOM_MEMBER_READY)
            && !(d.flags & ROOM_MEMBER_WATCH))
            return true;
    }
    return false;
}

static inline const char *netplay_state_text(netplay_state_t s) {
    switch (s) {
        case NETPLAY_OFF:        return "off";
        case NETPLAY_CONNECTING: return "connecting";
        case NETPLAY_ONLINE:     return "online";
        case NETPLAY_IN_ROOM:    return "in a room";
        case NETPLAY_SYNCING:    return "waiting at the barrier";
        case NETPLAY_PLAYING:    return "playing";
        case NETPLAY_WATCHING:   return "watching";
        case NETPLAY_FAILED:     return "failed";
        default:                 return "?";
    }
}

/* The board is being driven by the room, whether we fight or watch. */
static inline bool netplay_state_running(netplay_state_t s) {
    return s == NETPLAY_PLAYING || s == NETPLAY_WATCHING;
}
static inline bool netplay_running_match(void) { return netplay_state_running(g_netplay.state); }

static inline void netplay_send_announce(void) {
    lockstep_announce_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    netplay_fill_header(&pkt.header, LOCKSTEP_PACKET_ANNOUNCE);
    pkt.seed = g_netplay.seed;
    netplay_send_to(netplay_opponent_id(), &pkt, sizeof(pkt));
    g_netplay.last_announce_ms = net_now_ms();
}

/* Everything that was per-generation, cleared. `player` is our side or
 * LOCKSTEP_WATCHER. */
static inline void netplay_begin_generation(uint32_t generation, uint32_t player, uint32_t delay) {
    g_netplay.generation   = generation & 0x1F;
    g_netplay.local_player = (int32_t)player;
    lockstep_configure(&g_netplay.lockstep, player, LOCKSTEP_MAX_PLAYERS, delay ? delay : 2u);
    lockstep_begin_round(&g_netplay.lockstep, g_netplay.generation);
    netplay_check_clear();
    g_netplay.frame         = 0;
    g_netplay.delay_seeded  = false;
    g_netplay.reset_pending = false;
    g_netplay.stall_since_ms = 0;
    g_netplay.last_resend_ms      = 0;
    g_netplay.last_peer_input_ms  = 0;
    g_netplay.ready_frame         = LOCKSTEP_INVALID_FRAME;
    g_netplay.ready_since_ms      = 0;
    g_netplay.ready_reported      = false;
    g_netplay.stopped_noted_frame = LOCKSTEP_INVALID_FRAME;
    g_netplay.linger_until_ms     = 0;
    g_netplay.watch_stride        = 0;
    g_netplay.last_repair_ms      = 0;
    g_netplay.watch_stall_ms      = 0;
    g_netplay.match_result_seen   = false;
    g_netplay.last_wait_report_ms = net_now_ms();
    if (lockstep_is_watcher(&g_netplay.lockstep)) {
        /* Nothing to agree on: reset now and follow the fighters' frames. */
        g_netplay.state         = NETPLAY_WATCHING;
        g_netplay.reset_pending = true;
    } else {
        g_netplay.state = NETPLAY_SYNCING;
        netplay_send_announce();
    }
}

/* Frames [0, delay) have no local input yet — the delay means input sampled now
 * applies `delay` frames later. Pre-fill them as neutral on both machines so the
 * session can start. */
static inline void netplay_seed_delay_frames(void) {
    uint32_t delay = g_netplay.lockstep.frame_delay;
    for (uint32_t f = 0; f < delay; f++) {
        lockstep_input_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        netplay_fill_header(&pkt.header, LOCKSTEP_PACKET_INPUT);
        pkt.check_frame = LOCKSTEP_NO_CHECK;
        lockstep_submit_local(&g_netplay.lockstep, f, 0, &pkt.record);
        netplay_send_to(netplay_opponent_id(), &pkt, sizeof(pkt));
        netplay_send_watchers(&pkt, sizeof(pkt));
    }
    g_netplay.delay_seeded = true;
}

/* One of our records, whose newest frame is `top`, to one member. */
static inline void netplay_send_record(uint16_t member, uint32_t top) {
    lockstep_input_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    netplay_fill_header(&pkt.header, LOCKSTEP_PACKET_INPUT);
    pkt.check_frame = g_netplay.last_check_frame;
    pkt.check_value = g_netplay.last_check_value;
    lockstep_fill_record(&g_netplay.lockstep, top, &pkt.record);
    netplay_send_to(member, &pkt, sizeof(pkt));
}

/*
 * A watcher is missing our frames from `frame` on: send what we still hold,
 * four records deep (40 frames). A frame our ring has already lapped is not
 * sent at all -- a record fills a frame it does not have with a neutral pad,
 * and a watcher that took that would run a different match from then on.
 */
static inline void netplay_answer_repair(uint16_t member, uint32_t frame) {
    const lockstep_t *l = &g_netplay.lockstep;
    if (lockstep_is_watcher(l) || l->last_local_frame == LOCKSTEP_INVALID_FRAME) return;
    if (frame > l->last_local_frame) return;
    if (!lockstep_ring_has(&l->rings[l->local_player], frame)) return;
    for (int k = 0; k < 4; k++) {
        uint32_t top = frame + LOCKSTEP_REDUNDANCY - 1 + (uint32_t)k * LOCKSTEP_REDUNDANCY;
        if (top > l->last_local_frame) top = l->last_local_frame;
        netplay_send_record(member, top);
        if (top == l->last_local_frame) break;
    }
}

static inline void netplay_end_match(const char *why);   /* below; a BYE ends the match */

static inline void netplay_drain_socket(void) {
    uint8_t buf[sizeof(lockstep_input_packet_t) + 64];
    for (;;) {
        uint16_t from = 0;
        uint32_t ip = 0;
        uint16_t port = 0;
        int got = rpcn_session_recv(&g_netplay.session, buf, (uint32_t)sizeof(buf), &from, &ip, &port);
        if (got <= 0) break;
        if (got < (int)sizeof(lockstep_header_t)) continue;

        const lockstep_header_t *hdr = (const lockstep_header_t *)buf;

        /* Not our room: a stale process from an earlier test, or another session
         * that happens to share this address pair. Dropping it here is what stops
         * it releasing our barrier or feeding our rings. Only filter once we know
         * our own room (0 = not yet). */
        uint32_t mine = netplay_session_id();
        if (mine != 0 && hdr->session != 0 && hdr->session != mine) continue;

        /* An address nobody has claimed yet, which names its sender: the first
         * datagram from a member behind a NAT that picked a port of its own. */
        if (!from) {
            if (!hdr->member || !rpcn_session_claim(&g_netplay.session, hdr->member, ip, port)) continue;
            from = hdr->member;
        }
        if (hdr->member && hdr->member != from) continue;   /* says it is somebody else */

        if (hdr->type == LOCKSTEP_PACKET_INPUT && got >= (int)sizeof(lockstep_input_packet_t)) {
            const lockstep_input_packet_t *pkt = (const lockstep_input_packet_t *)buf;
            if (hdr->player >= LOCKSTEP_MAX_PLAYERS) continue;   /* watchers send no inputs */
            lockstep_on_record(&g_netplay.lockstep, &pkt->record);
            if (from == netplay_opponent_id()
                && hdr->generation == (uint8_t)(g_netplay.generation & 0x1F))
                g_netplay.last_peer_input_ms = net_now_ms();
            if (pkt->check_frame != LOCKSTEP_NO_CHECK
                && hdr->generation == (uint8_t)(g_netplay.generation & 0x1F)) {
                uint32_t idx = pkt->check_frame & LOCKSTEP_RING_MASK;
                g_netplay.peer_check_frame[idx] = pkt->check_frame;
                g_netplay.peer_check_value[idx] = pkt->check_value;
                netplay_check_compare(pkt->check_frame);
            }
        } else if (hdr->type == LOCKSTEP_PACKET_ANNOUNCE
                   && got >= (int)sizeof(lockstep_announce_packet_t)) {
            /* The generation is the room's, not either fighter's: both read it
             * from the same room state, so there is nothing here to adopt. */
            lockstep_on_peer_announce(&g_netplay.lockstep, hdr->player, hdr->generation);
        } else if (hdr->type == LOCKSTEP_PACKET_REPAIR
                   && got >= (int)sizeof(lockstep_repair_packet_t)) {
            const lockstep_repair_packet_t *req = (const lockstep_repair_packet_t *)buf;
            if (hdr->generation == (uint8_t)(g_netplay.generation & 0x1F)
                && (int32_t)req->side == g_netplay.local_player)
                netplay_answer_repair(from, req->frame);
        } else if (hdr->type == LOCKSTEP_PACKET_BYE
                   && got >= (int)sizeof(lockstep_announce_packet_t)) {
            /* The other fighter left THIS match: end it now rather than freeze
             * until the stall timeout. A watcher is told too, since its board
             * cannot go on without both sides. A bye for another match is a late
             * copy; ignored. */
            const lockstep_announce_packet_t *bye = (const lockstep_announce_packet_t *)buf;
            bool from_fighter = from == g_netplay.room.fighter[0] || from == g_netplay.room.fighter[1];
            if (from_fighter && from != netplay_my_id()
                && hdr->generation == (uint8_t)(g_netplay.generation & 0x1F)
                && (netplay_running_match() || g_netplay.state == NETPLAY_SYNCING)
                && !g_netplay.match_result_seen) {
                netplay_log("%s left the match (their frame %u, ours %u)",
                            rpcn_peer_name(rpcn_session_peer(&g_netplay.session, from)),
                            (unsigned)bye->seed, (unsigned)g_netplay.frame);
                netplay_end_match(NULL);
            }
        }
    }
}

/* ---- Opening a browser --------------------------------------------------- */

/*
 * Hands a URL to the desktop. Used by the Twitch flow, which is only usable if
 * the player can get to twitch.tv's activation page.
 *
 * THE https:// CHECK IS NOT DECORATION. The URL comes from the RPCN server, and
 * on Windows ShellExecute("open", …) will happily run a local executable or a
 * registered protocol handler if handed one. A player typing in a server name is
 * not thereby agreeing to let it start programs, so anything that is not a plain
 * https URL is refused and shown as text instead.
 */
/*
 * Off for a headless run. There is no desktop there in any meaningful sense, and
 * a scripted run that makes a browser tab appear on whoever's screen happens to
 * be attached is a surprise, not a convenience — the log line carries the URL.
 */
static bool g_netplay_may_open_browser = true;

static inline void netplay_set_open_browser(bool allow) { g_netplay_may_open_browser = allow; }

static inline bool netplay_open_url(const char *url) {
    if (!g_netplay_may_open_browser) return false;
    if (!url || strncmp(url, "https://", 8) != 0) return false;
    for (const char *p = url; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == '"' || *p == '\'') return false;

#ifdef _WIN32
    return (uintptr_t)ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL) > 32;
#elif defined(__APPLE__)
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "open '%s' >/dev/null 2>&1 &", url);
    return system(cmd) == 0;
#else
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", url);
    return system(cmd) == 0;
#endif
}

/* ---- Stored settings ----------------------------------------------------- */
/*
 * One file per user, not per copy of the executable. The point of it is the
 * Twitch login token: the device flow is deliberately a once-ever thing, and a
 * client that forgot the token it was given would send the player back to a
 * browser at every launch, which is the exact outcome the token exists to prevent.
 *
 * IT USED TO LIVE IN THE WORKING DIRECTORY, AND RPCN KEEPS ONE TOKEN PER ACCOUNT.
 * Every device flow replaces the account's token (`set_twitch_login`), so with a
 * file per folder, signing in from one copy of the emulator quietly signed every
 * other copy out: their stored token was refused, forgotten as dead, and a
 * headless copy has nobody to approve a new code. That is how the fly lost its
 * login -- its stream kit, the canary download and a build tree each held a
 * settings file, and signing in on the desktop from one of them killed the
 * token in the kit. All copies on a machine now share one file, so one sign-in
 * serves them all. `--net-config` names another file, which is how two accounts
 * run side by side on one machine. A file left in a working directory by an
 * older build is read once, when the per-user one does not exist yet, and
 * written through to it.
 *
 * THE PASSWORD IS STORED TOO, IN CLEAR TEXT, and that was asked for rather
 * than assumed. This file used to hold the Twitch token and refuse the
 * password, on the reasoning that a token is a credential the server issues
 * for storage and can invalidate by reissuing while a password is neither --
 * so writing one beside the executable was not a decision to make on the
 * player's behalf. It is still not; it is now theirs, made deliberately, so
 * that an unattended host (a stream that relaunches its own emulator) can
 * sign a password account back in without anybody at the keyboard. Anyone who
 * would rather it were not kept should leave the field empty and let the
 * Twitch flow issue a token instead -- that path clears the password (see
 * NETPLAY_CMD_TWITCH_POLL) and nothing is written.
 *
 * The file is plain text with no permissions of its own, so it is exactly as
 * private as the directory holding it. The header line below says so, for
 * whoever opens it next.
 */
#define NETPLAY_CFG_PATH "m2hle_netplay.cfg"

#ifndef __EMSCRIPTEN__
/* Outside g_netplay on purpose: netplay_init clears that, and --net-config is
 * parsed before it runs. */
static char g_netplay_cfg_path[512];
static bool g_netplay_cfg_named;    /* --net-config: never adopt a legacy file into it */

/* --net-config: use this file instead of the per-user one. Call before
 * netplay_init. */
static inline void netplay_set_config_path(const char *path) {
    snprintf(g_netplay_cfg_path, sizeof(g_netplay_cfg_path), "%s", path ? path : "");
    g_netplay_cfg_named = g_netplay_cfg_path[0] != '\0';
}

/* %APPDATA%\m2hle2\netplay.cfg, or $XDG_CONFIG_HOME (~/.config)/m2hle2/netplay.cfg.
 * The directory is created here. Falls back to the working directory when there
 * is no per-user location to use at all. */
static inline const char *netplay_cfg_path(void) {
    if (g_netplay_cfg_path[0]) return g_netplay_cfg_path;
    char dir[480] = "";
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && appdata[0]) {
        snprintf(dir, sizeof(dir), "%s\\m2hle2", appdata);
        CreateDirectoryA(dir, NULL);   /* fails harmlessly when it exists */
    }
#else
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0]) {
        snprintf(dir, sizeof(dir), "%s", xdg);
    } else if (home && home[0]) {
        snprintf(dir, sizeof(dir), "%s/.config", home);
    }
    if (dir[0]) {
        mkdir(dir, 0700);
        size_t n = strlen(dir);
        snprintf(dir + n, sizeof(dir) - n, "/m2hle2");
        mkdir(dir, 0700);
    }
#endif
    if (!dir[0]) snprintf(g_netplay_cfg_path, sizeof(g_netplay_cfg_path), "%s", NETPLAY_CFG_PATH);
#ifdef _WIN32
    else snprintf(g_netplay_cfg_path, sizeof(g_netplay_cfg_path), "%s\\netplay.cfg", dir);
#else
    else snprintf(g_netplay_cfg_path, sizeof(g_netplay_cfg_path), "%s/netplay.cfg", dir);
#endif
    return g_netplay_cfg_path;
}
#endif

/* Is the stored Twitch token this account's? A token with no recorded owner
 * predates `twitch_npid` and belonged to whoever was stored beside it, which
 * `netplay_settings_load` has already filled in -- so an empty owner here
 * means there is no token at all. */
static inline bool netplay_twitch_is_for(const netplay_config_t *cfg,
                                         const char *npid) {
    if (!cfg->twitch_token[0] || !npid || !npid[0]) return false;
    if (!cfg->twitch_npid[0]) return true;      /* pre-owner file, already adopted */
    return strcmp(cfg->twitch_npid, npid) == 0;
}

/*
 * The web build has no file system: the same text goes to localStorage under the
 * file's name instead. It is exactly as private as the origin -- any script the
 * page runs can read it -- which is why the page loads no script from anywhere
 * else (web/site/index.html says so, and CI checks).
 */
#ifdef __EMSCRIPTEN__
EM_JS(void, netplay_web_store, (const char *key, const char *text), {
    try { localStorage.setItem(UTF8ToString(key), UTF8ToString(text)); } catch (e) {}
});
EM_JS(int, netplay_web_fetch, (const char *key, char *out, int cap), {
    let v = null;
    try { v = localStorage.getItem(UTF8ToString(key)); } catch (e) {}
    if (v === null) return 0;
    stringToUTF8(v, out, cap);
    return 1;
});
#endif

typedef struct { char *p; size_t left; } netplay_text_t;

static inline void netplay_text_add(netplay_text_t *t, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(t->p, t->left, fmt, args);
    va_end(args);
    if (n < 0) return;
    size_t used = (size_t)n < t->left ? (size_t)n : (t->left ? t->left - 1 : 0);
    t->p += used;
    t->left -= used;
}

static inline void netplay_twitch_merge_disk(void);

static inline void netplay_settings_save(void) {
    netplay_twitch_merge_disk();
    char text[2048];
    netplay_text_t t = { text, sizeof(text) };
    text[0] = '\0';
    netplay_text_add(&t, "# m2-hle2 netplay settings. Delete this file to forget them.\n");
    if (g_netplay.cfg.password[0])
        netplay_text_add(&t, "# This file holds a password in clear text.\n");
    netplay_text_add(&t, "server=%s\n",       g_netplay.cfg.server);
    netplay_text_add(&t, "port=%u\n",         (unsigned)g_netplay.cfg.port);
    netplay_text_add(&t, "npid=%s\n",         g_netplay.cfg.npid);
    netplay_text_add(&t, "fingerprint=%s\n",  g_netplay.cfg.fingerprint);
    netplay_text_add(&t, "frame_delay=%u\n",  g_netplay.cfg.frame_delay);
    netplay_text_add(&t, "max_players=%u\n",  g_netplay.cfg.max_players);
    netplay_text_add(&t, "browse_yamp=%d\n",  g_netplay.cfg.browse_yamp ? 1 : 0);
    /* Only when there is one: an empty `password=` in the file would claim a
     * stored credential that does not exist, and a Twitch login clears the
     * field precisely so that nothing is kept. The same for the e-mail token,
     * which is the netplay window's "Token" box -- RPCN checks it at Login
     * for a password account, so a host that forgot it could not sign back in
     * unattended however well it remembered the password. */
    if (g_netplay.cfg.password[0])
        netplay_text_add(&t, "password=%s\n", g_netplay.cfg.password);
    if (g_netplay.cfg.token[0])
        netplay_text_add(&t, "token=%s\n",    g_netplay.cfg.token);
    netplay_text_add(&t, "twitch_token=%s\n", g_netplay.cfg.twitch_token);
    /* Saved beside the token and never without it: a token whose owner was
     * forgotten is the thing this field exists to prevent. */
    if (g_netplay.cfg.twitch_token[0])
        netplay_text_add(&t, "twitch_npid=%s\n", g_netplay.cfg.twitch_npid);

    memcpy(g_netplay.twitch_synced, g_netplay.cfg.twitch_token, sizeof(g_netplay.twitch_synced));
#ifdef __EMSCRIPTEN__
    netplay_web_store(NETPLAY_CFG_PATH, text);
#else
    /* Written whole and renamed into place: every copy of the emulator on the
     * machine shares this file, and a stream and its training runs can sign in
     * at the same moment. A reader must never see half of one. */
    const char *path = netplay_cfg_path();
    char tmp[sizeof(g_netplay_cfg_path) + 24];
#ifdef _WIN32
    snprintf(tmp, sizeof(tmp), "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
#else
    snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", path, (long)getpid());
#endif
    FILE *f = fopen(tmp, "w");
    if (!f) {
        netplay_log("could not save the netplay settings to %s", path);
        return;
    }
    bool ok = fputs(text, f) >= 0;
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    /* Windows will not replace a file another process has open, and every copy
     * of the emulator (and YAMP) reads this one. A read is over in a moment. */
    bool moved = false;
    for (int tries = 0; ok && !moved && tries < 20; tries++) {
        moved = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
        if (!moved) Sleep(10);
    }
    ok = ok && moved;
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) {
        remove(tmp);
        netplay_log("could not save the netplay settings to %s", path);
    }
#endif
}

static inline void netplay_settings_parse_line(netplay_config_t *cfg, char *line) {
    if (line[0] == '#') return;
    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq = '\0';
    char *key = line, *val = eq + 1;
    size_t n = strlen(val);
    while (n && (val[n - 1] == '\n' || val[n - 1] == '\r')) val[--n] = '\0';

    if      (!strcmp(key, "server"))       snprintf(cfg->server, sizeof(cfg->server), "%s", val);
    else if (!strcmp(key, "port"))         cfg->port = (uint16_t)atoi(val);
    else if (!strcmp(key, "npid"))         snprintf(cfg->npid, sizeof(cfg->npid), "%s", val);
    else if (!strcmp(key, "fingerprint"))  snprintf(cfg->fingerprint, sizeof(cfg->fingerprint), "%s", val);
    else if (!strcmp(key, "frame_delay"))  cfg->frame_delay = (uint32_t)atoi(val);
    else if (!strcmp(key, "max_players"))  cfg->max_players = (uint32_t)atoi(val);
    else if (!strcmp(key, "browse_yamp"))  cfg->browse_yamp = atoi(val) != 0;
    else if (!strcmp(key, "password"))     snprintf(cfg->password, sizeof(cfg->password), "%s", val);
    else if (!strcmp(key, "token"))        snprintf(cfg->token, sizeof(cfg->token), "%s", val);
    else if (!strcmp(key, "twitch_token")) snprintf(cfg->twitch_token, sizeof(cfg->twitch_token), "%s", val);
    else if (!strcmp(key, "twitch_npid"))  snprintf(cfg->twitch_npid, sizeof(cfg->twitch_npid), "%s", val);
}

/* Returns true when the settings came from an older build's file in the working
 * directory, which the caller then writes through to the per-user one. */
static inline bool netplay_settings_load(netplay_config_t *cfg) {
    bool legacy = false;
#ifdef __EMSCRIPTEN__
    char text[2048];
    if (!netplay_web_fetch(NETPLAY_CFG_PATH, text, (int)sizeof(text))) return false;
    for (char *line = text; line && *line; ) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        netplay_settings_parse_line(cfg, line);
        line = end ? end + 1 : NULL;
    }
#else
    const char *path = netplay_cfg_path();
    FILE *f = fopen(path, "r");
    if (!f && !g_netplay_cfg_named && strcmp(path, NETPLAY_CFG_PATH) != 0) {
        f = fopen(NETPLAY_CFG_PATH, "r");
        legacy = f != NULL;
    }
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) netplay_settings_parse_line(cfg, line);
    fclose(f);
#endif

    /* A file written before tokens had owners: the token was whoever's name
     * was stored with it, because there was only ever one account in it. */
    if (cfg->twitch_token[0] && !cfg->twitch_npid[0])
        snprintf(cfg->twitch_npid, sizeof(cfg->twitch_npid), "%s", cfg->npid);
    return legacy;
}

/* The per-user file as it is on disk right now, with no legacy fallback. */
static inline bool netplay_settings_read_current(netplay_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
#ifdef __EMSCRIPTEN__
    netplay_settings_load(cfg);
    return cfg->server[0] || cfg->npid[0] || cfg->twitch_token[0];
#else
    FILE *f = fopen(netplay_cfg_path(), "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) netplay_settings_parse_line(cfg, line);
    fclose(f);
    if (cfg->twitch_token[0] && !cfg->twitch_npid[0])
        snprintf(cfg->twitch_npid, sizeof(cfg->twitch_npid), "%s", cfg->npid);
    return true;
#endif
}

/* RPCN account names compare without case. */
static inline bool netplay_same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return *a == *b;
}

static inline bool netplay_same_server(const netplay_config_t *a, const netplay_config_t *b) {
    uint16_t pa = a->port ? a->port : RPCN_DEFAULT_PORT;
    uint16_t pb = b->port ? b->port : RPCN_DEFAULT_PORT;
    return netplay_same_name(a->server, b->server) && pa == pb;
}

/*
 * ANOTHER PROGRAM MAY HAVE SIGNED IN SINCE WE READ THE FILE. Every copy of the
 * emulator shares it, and so does YAMP (its SharedLogin.cpp), and RPCN keeps
 * one Twitch token per account: a device flow anywhere on the machine writes
 * the only token that still works. A process that saved its own copy over it --
 * which every login used to do -- would put the dead one back and sign
 * everybody out again. So a token this process has not changed since it last
 * synced gives way to whatever the file now holds for the same server.
 */
static inline void netplay_twitch_merge_disk(void) {
    if (strcmp(g_netplay.cfg.twitch_token, g_netplay.twitch_synced) != 0) return;  /* ours is newer */
    netplay_config_t disk;
    if (!netplay_settings_read_current(&disk)) return;
    if (strcmp(disk.twitch_token, g_netplay.twitch_synced) == 0) return;          /* nobody else */
    if (disk.twitch_token[0] && !netplay_same_server(&disk, &g_netplay.cfg)) return;
    memcpy(g_netplay.cfg.twitch_token, disk.twitch_token, sizeof(disk.twitch_token));
    memcpy(g_netplay.cfg.twitch_npid,  disk.twitch_npid,  sizeof(disk.twitch_npid));
    memcpy(g_netplay.twitch_synced,    disk.twitch_token, sizeof(disk.twitch_token));
}

/* ---- Command queue (UI thread -> emu thread) ----------------------------- */

static inline void netplay_init(void) {
    memset(&g_netplay, 0, sizeof(g_netplay));
    g_netplay.local_player     = -1;
    g_netplay.me.result        = ROOM_RESULT_NONE;
    g_netplay.room.last_result = ROOM_RESULT_NONE;
    g_netplay.desync_frame     = LOCKSTEP_NO_CHECK;
    g_netplay.stall_timeout_ms = 15000;
    /* Both sockets start INVALID rather than 0: a zeroed handle is not the
     * "unset" value on Windows (INVALID_SOCKET is ~0), so the first teardown
     * would call closesocket(0). */
    g_netplay.session.client.udp     = NET_SOCK_INVALID;
    g_netplay.session.client.tls.sock = NET_SOCK_INVALID;
    g_netplay.account.client.udp     = NET_SOCK_INVALID;
    g_netplay.account.client.tls.sock = NET_SOCK_INVALID;
    g_netplay.twitch.client.udp      = NET_SOCK_INVALID;
    g_netplay.twitch.client.tls.sock = NET_SOCK_INVALID;
    emu_mutex_init(&g_netplay.mutex);
    g_netplay.mutex_ready = true;
    netplay_check_clear();

    /* Whatever was stored last time, which is mostly the Twitch login token. */
    bool legacy = netplay_settings_load(&g_netplay.cfg);
    memcpy(g_netplay.twitch_synced, g_netplay.cfg.twitch_token, sizeof(g_netplay.twitch_synced));
    if (legacy) {
        /* Now, not at the next login: until the per-user file exists, every
         * copy launched would adopt whatever its own folder happened to hold. */
        netplay_settings_save();
        netplay_log("copied %s into the per-user settings file", NETPLAY_CFG_PATH);
    }
#ifndef __EMSCRIPTEN__
    LOG_INFO("netplay: settings file %s", netplay_cfg_path());
#endif
    if (g_netplay.cfg.twitch_token[0])
        netplay_log("signed in with Twitch as %s (stored login)", g_netplay.cfg.npid);
}

/* The settings as loaded, so the UI can start from them instead of its own
 * defaults. Returns false when nothing was stored. */
static inline bool netplay_stored_settings(netplay_config_t *out) {
    if (!g_netplay.cfg.server[0] && !g_netplay.cfg.npid[0]) return false;
    *out = g_netplay.cfg;
    return true;
}

static inline void netplay_set_reset_hook(void (*fn)(void *), void *ctx) {
    g_netplay.reset_board = fn;
    g_netplay.reset_ctx   = ctx;
}

static inline void netplay_post(netplay_cmd_kind_t kind, const netplay_config_t *cfg) {
    if (!g_netplay.mutex_ready) return;
    emu_mutex_lock(&g_netplay.mutex);
    if (g_netplay.queue_count < NETPLAY_CMD_QUEUE) {
        uint32_t idx = (g_netplay.queue_head + g_netplay.queue_count) % NETPLAY_CMD_QUEUE;
        g_netplay.queue[idx].kind = kind;
        if (cfg) g_netplay.queue[idx].cfg = *cfg;
        else     g_netplay.queue[idx].cfg = g_netplay.cfg;
        g_netplay.queue_count++;
    }
    emu_mutex_unlock(&g_netplay.mutex);
}

static inline bool netplay_take_cmd(netplay_cmd_t *out) {
    bool got = false;
    emu_mutex_lock(&g_netplay.mutex);
    if (g_netplay.queue_count) {
        *out = g_netplay.queue[g_netplay.queue_head];
        g_netplay.queue_head = (g_netplay.queue_head + 1) % NETPLAY_CMD_QUEUE;
        g_netplay.queue_count--;
        got = true;
    }
    emu_mutex_unlock(&g_netplay.mutex);
    return got;
}

/* The UI reads this; it is refreshed once per pump. */
static inline void netplay_publish_status(void) {
    emu_mutex_lock(&g_netplay.mutex);
    netplay_status_t *st = &g_netplay.status;
    const rpcn_session_t *s = &g_netplay.session;
    st->state        = g_netplay.state;
    st->stage        = s->stage;
    st->is_host      = netplay_is_host();
    st->local_player = g_netplay.local_player < 0 ? -1
                     : g_netplay.local_player >= (int32_t)LOCKSTEP_WATCHER ? 2 : g_netplay.local_player;
    st->room_id      = s->room_id;
    st->room_flags   = s->room_flags;
    snprintf(st->com_id, sizeof(st->com_id), "%s", s->com_id);
    snprintf(st->com_id_foreign, sizeof(st->com_id_foreign), "%s", s->com_id_foreign);

    /* The room, one row per member, in line order where there is one. */
    st->my_member_id = s->my_member_id;
    st->max_slot     = s->max_slot;
    st->room         = g_netplay.room;
    st->room_known   = g_netplay.room_known;
    st->me           = g_netplay.me;
    st->member_count = 0;
    if (rpcn_session_in_room(s)) {
        uint16_t ids[ROOM_MAX_MEMBERS];
        uint32_t n = 0;
        for (uint32_t i = 0; i < g_netplay.room.line_count && n < ROOM_MAX_MEMBERS; i++)
            ids[n++] = g_netplay.room.line[i];
        if (s->my_member_id) {
            bool listed = false;
            for (uint32_t i = 0; i < n; i++) if (ids[i] == s->my_member_id) listed = true;
            if (!listed && n < ROOM_MAX_MEMBERS) ids[n++] = s->my_member_id;
        }
        for (uint32_t i = 0; i < RPCN_MAX_PEERS && n < ROOM_MAX_MEMBERS; i++) {
            if (!s->peers[i].used) continue;
            bool listed = false;
            for (uint32_t k = 0; k < n; k++) if (ids[k] == s->peers[i].member_id) listed = true;
            if (!listed) ids[n++] = s->peers[i].member_id;
        }
        for (uint32_t i = 0; i < n; i++) {
            netplay_member_status_t *row = &st->members[st->member_count];
            memset(row, 0, sizeof(*row));
            row->member_id = ids[i];
            row->is_me     = ids[i] == s->my_member_id;
            row->is_owner  = ids[i] == s->owner_id;
            row->line_pos  = (int8_t)room_line_index(&g_netplay.room, ids[i]);
            row->side      = (int8_t)room_side_of(&g_netplay.room, ids[i]);
            if (row->is_me) {
                snprintf(row->npid, sizeof(row->npid), "%s", s->npid);
                row->known = true;
                row->data  = g_netplay.me;
            } else {
                const rpcn_peer_t *p = rpcn_session_peer((rpcn_session_t *)s, ids[i]);
                if (!p) continue;   /* in the line, but gone from the room */
                snprintf(row->npid, sizeof(row->npid), "%s", p->npid);
                row->known      = room_member_decode(p->bin, p->bin_len, &row->data);
                row->addr_known = p->ip && p->port;
                row->heard      = p->heard;
            }
            st->member_count++;
        }
    }
    st->auto_start_s = 0;
    if (netplay_is_host() && g_netplay.room.phase == ROOM_PHASE_LOBBY
        && (g_netplay.room.flags & ROOM_FLAG_AUTO) && g_netplay.auto_deadline_ms) {
        uint64_t now = net_now_ms();
        st->auto_start_s = g_netplay.auto_deadline_ms > now
                         ? (uint32_t)((g_netplay.auto_deadline_ms - now + 999) / 1000) : 0;
    }

    /* "The peer": the opponent while there is one, otherwise the first other
     * member. What a two-player front end (and the MCP status) has always shown. */
    const rpcn_peer_t *peer = rpcn_session_peer((rpcn_session_t *)s, netplay_opponent_id());
    for (uint32_t i = 0; !peer && i < RPCN_MAX_PEERS; i++) if (s->peers[i].used) peer = &s->peers[i];
    snprintf(st->peer_npid, sizeof(st->peer_npid), "%s", peer ? peer->npid : "");
    snprintf(st->peer_addr, sizeof(st->peer_addr), "%s", rpcn_peer_addr_text(peer));
    st->peer_known   = peer && peer->ip && peer->port;
    st->peer_heard   = peer && peer->heard;
    st->frame        = g_netplay.frame;
    st->stalls       = g_netplay.lockstep.stalls;
    st->desync_frame = g_netplay.desync_frame;
    st->generation   = g_netplay.generation;
    st->seed         = g_netplay.seed;
    st->peer_ready     = netplay_peer_ready();
    st->peer_ready_gen = g_netplay.room.match;

    st->room_count = g_netplay.session.room_count;
    memcpy(st->rooms, g_netplay.session.rooms, sizeof(st->rooms));
    st->foreign_room_count = g_netplay.session.foreign_room_count;
    memcpy(st->foreign_rooms, g_netplay.session.foreign_rooms, sizeof(st->foreign_rooms));
    st->search_pending = rpcn_session_search_pending(&g_netplay.session);

    st->account_state = g_netplay.account.state;
    st->account_job   = g_netplay.account.job;
    snprintf(st->account_error, sizeof(st->account_error), "%s", g_netplay.account.error);

    st->twitch_state     = g_netplay.twitch.state;
    st->twitch_signed_in = g_netplay.cfg.twitch_token[0] != '\0';
    snprintf(st->twitch_user_code, sizeof(st->twitch_user_code), "%s", g_netplay.twitch.user_code);
    snprintf(st->twitch_uri, sizeof(st->twitch_uri), "%s", g_netplay.twitch.verification_uri);
    /* The token's owner, not whoever is in the account box: the window prints
     * this as "Signed in with Twitch as ...", and with two accounts on one
     * machine those are no longer the same name. */
    snprintf(st->twitch_npid, sizeof(st->twitch_npid), "%s",
             g_netplay.cfg.twitch_npid[0] ? g_netplay.cfg.twitch_npid : g_netplay.cfg.npid);
    snprintf(st->twitch_error, sizeof(st->twitch_error), "%s", g_netplay.twitch.error);

    snprintf(st->error, sizeof(st->error), "%s",
             g_netplay.state == NETPLAY_FAILED ? rpcn_session_error(&g_netplay.session) : "");
    memcpy(st->log, g_netplay.log, sizeof(st->log));
    st->log_count = g_netplay.log_count;
    emu_mutex_unlock(&g_netplay.mutex);
}

/* Copy of the status for the UI thread. */
static inline void netplay_get_status(netplay_status_t *out) {
    if (!g_netplay.mutex_ready) { memset(out, 0, sizeof(*out)); return; }
    emu_mutex_lock(&g_netplay.mutex);
    *out = g_netplay.status;
    emu_mutex_unlock(&g_netplay.mutex);
}

/* ---- Commands ------------------------------------------------------------ */

static inline void netplay_do_connect(const netplay_config_t *cfg) {
    if (!g_active_profile) { netplay_log("load a ROM set before connecting"); return; }
    if (!g_netplay.reset_board) {
        netplay_log("no board-reset hook installed - netplay cannot start a session");
        return;
    }
    /* RPCN reads a login with no name as Malformed and says only that, which
     * reads as a broken client. It is what Connect sends after a Twitch sign-in
     * that did not complete: the account boxes are still empty. */
    if (!cfg->npid[0]) {
        netplay_log("no account name - type one, or sign in with Twitch first");
        return;
    }

    /* THE STORED TWITCH LOGIN IS NOT THE CALLER'S TO OVERWRITE. Every other field
     * here is something a caller typed or passed; the token and its owner are
     * not, there is no box for them, and the config a caller posts is a COPY
     * that goes stale in both directions. The netplay window adopts the stored
     * settings once, at its first draw: a token the device flow lands after that
     * is not in its copy, so Connect wiped it and sent the player back to
     * twitch.tv; and a token that "Sign out" forgot was still in its copy, so
     * the next Connect signed them back in. */
    char twitch_token[sizeof(g_netplay.cfg.twitch_token)];
    char twitch_npid[sizeof(g_netplay.cfg.twitch_npid)];
    memcpy(twitch_token, g_netplay.cfg.twitch_token, sizeof(twitch_token));
    memcpy(twitch_npid,  g_netplay.cfg.twitch_npid,  sizeof(twitch_npid));
    g_netplay.cfg = *cfg;
    memcpy(g_netplay.cfg.twitch_token, twitch_token, sizeof(twitch_token));
    memcpy(g_netplay.cfg.twitch_npid,  twitch_npid,  sizeof(twitch_npid));
    netplay_build_masks(g_active_profile);

    char com_id[COMID_BUFFER_SIZE];
    const char *note = "";
    if (!comid_resolve(g_active_profile->id, com_id, &note)) {
        netplay_log("no lobby space for '%s': %s", g_active_profile->id, note);
        return;
    }
    netplay_log("lobby space %s (%s)", com_id, note);

    char com_id_foreign[COMID_BUFFER_SIZE];
    bool have_foreign = cfg->browse_yamp && comid_yamp_for_game(g_active_profile->id, com_id_foreign);

    rpcn_session_config_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.server          = g_netplay.cfg.server;
    sc.port            = g_netplay.cfg.port;
    sc.fingerprint_hex = g_netplay.cfg.fingerprint;
    sc.npid            = g_netplay.cfg.npid;
    /* A Twitch login token IS the password as far as RPCN is concerned, and it
     * wins when present: someone who signed in with Twitch has no password to
     * type, and their account's real one is random and never disclosed.
     *
     * BUT ONLY FOR THE ACCOUNT IT BELONGS TO. One machine, one settings file,
     * and a household or a bot can easily have two accounts; offering account
     * A's token as account B's password is a guaranteed rejection, and the
     * message it comes back with -- "wrong password for that account" -- sends
     * you looking at the password, which is fine.
     *
     * Unless there is no password at all. An empty one is a guaranteed rejection
     * too, so a token is offered in its place whatever its label says: the label
     * can be wrong (the owner adopted at load is a guess -- the npid stored
     * beside a token is the last account that logged in, not necessarily the one
     * that signed in with Twitch), and the server is the only authority on whose
     * token it is. `netplay_mirror_stage` writes down what it answers. */
    g_netplay.sent_twitch_token =
        netplay_twitch_is_for(&g_netplay.cfg, g_netplay.cfg.npid)
        || (g_netplay.cfg.twitch_token[0] && !g_netplay.cfg.password[0]);
    sc.password        = g_netplay.sent_twitch_token ? g_netplay.cfg.twitch_token
                                                     : g_netplay.cfg.password;
    sc.token           = g_netplay.cfg.token;
    sc.com_id          = com_id;
    sc.com_id_foreign  = have_foreign ? com_id_foreign : NULL;
    sc.local_p2p_port  = g_netplay.cfg.local_p2p_port;
    sc.log             = netplay_session_log_cb;
    sc.log_ctx         = NULL;

    g_netplay.enabled      = true;
    g_netplay.local_player = -1;
    g_netplay.state        = NETPLAY_CONNECTING;
    netplay_log("connecting to %s:%u as %s", g_netplay.cfg.server,
                g_netplay.cfg.port ? g_netplay.cfg.port : RPCN_DEFAULT_PORT, g_netplay.cfg.npid);

    if (!rpcn_session_start(&g_netplay.session, &sc)) {
        g_netplay.state = NETPLAY_FAILED;
        netplay_log("%s", rpcn_session_error(&g_netplay.session));
    }
}

/*
 * "Sign in with Twitch" on a machine that already has.
 *
 * SIGNING IN WITH TWITCH IS NOT THE SAME THING AS RUNNING THE DEVICE FLOW. The
 * flow exists to GET a login token; once there is one, signing in is an
 * ordinary Login with the token for a password, and nobody needs a browser.
 * NETPLAY_CMD_TWITCH_START used to begin the flow unconditionally, so every
 * caller that says "with Twitch" -- {"cmd":"netplay_connect","twitch":1},
 * --net-twitch, the window's button -- sent somebody to twitch.tv to approve a
 * code on every single launch, with a perfectly good token sitting in the
 * settings file the whole time.
 *
 * Returns false when there is nothing to reuse and the flow really is needed.
 */
static inline bool netplay_twitch_reuse(const netplay_config_t *asked) {
    if (!g_netplay.cfg.twitch_token[0]) return false;

    /* A token is only good on the server that issued it. */
    uint16_t asked_port  = asked->port ? asked->port : RPCN_DEFAULT_PORT;
    uint16_t stored_port = g_netplay.cfg.port ? g_netplay.cfg.port : RPCN_DEFAULT_PORT;
    if (strcmp(asked->server, g_netplay.cfg.server) != 0 || asked_port != stored_port)
        return false;

    netplay_config_t c = *asked;
    c.password[0] = '\0';   /* the token is the credential: see netplay_do_connect */
    c.token[0]    = '\0';   /* Twitch vouched; no e-mail token is checked */
    if (!c.npid[0]) snprintf(c.npid, sizeof(c.npid), "%s", g_netplay.cfg.twitch_npid);
    if (!c.npid[0]) return false;

    g_netplay.twitch_wanted      = true;
    g_netplay.twitch_tried_owner = false;
    netplay_log("signing in with the stored Twitch login");
    netplay_do_connect(&c);
    return true;
}

/*
 * The server refused a login that offered the stored Twitch token. Called once,
 * on the way into NETPLAY_FAILED.
 *
 * Two different things produce that answer and they want opposite handling. The
 * token may be fine and the NAME wrong, because it was offered for an account it
 * does not belong to; or the token may be dead, because the server issued a
 * newer one to some other client (YAMP signs in with Twitch as well) or the
 * account was reset. A token refused under its own owner's name is dead and is
 * forgotten, since keeping it costs every later sign-in a rejection before it
 * can do anything useful.
 *
 * AT MOST TWO REJECTIONS, EVER, and then a browser or a stop. Retrying a refused
 * credential is how an account gets locked.
 */
static inline void netplay_twitch_refused(void) {
    if (!g_netplay.sent_twitch_token || !g_netplay.session.credential_refused) return;

    /* A token that went dead because someone else on this machine signed in
     * with Twitch (another copy of the emulator, or YAMP) left its replacement
     * in the settings file. Offer that before forgetting anything -- forgetting
     * would save an empty token over the one that works. */
    if (!g_netplay.twitch_tried_owner) {
        netplay_config_t disk;
        if (netplay_settings_read_current(&disk) && disk.twitch_token[0]
            && strcmp(disk.twitch_token, g_netplay.cfg.twitch_token) != 0
            && netplay_same_server(&disk, &g_netplay.cfg)
            && (g_netplay.twitch_wanted || netplay_same_name(disk.twitch_npid, g_netplay.cfg.npid))) {
            g_netplay.twitch_tried_owner = true;
            memcpy(g_netplay.cfg.twitch_token, disk.twitch_token, sizeof(disk.twitch_token));
            memcpy(g_netplay.cfg.twitch_npid,  disk.twitch_npid,  sizeof(disk.twitch_npid));
            memcpy(g_netplay.twitch_synced,    disk.twitch_token, sizeof(disk.twitch_token));
            snprintf(g_netplay.cfg.npid, sizeof(g_netplay.cfg.npid), "%s", disk.twitch_npid);
            netplay_log("the stored Twitch login was replaced on this machine - signing in with "
                        "the new one as %s", g_netplay.cfg.npid);
            netplay_do_connect(&g_netplay.cfg);
            return;
        }
    }

    const char *owner = g_netplay.cfg.twitch_npid;
    bool under_own_name = !owner[0] || strcmp(owner, g_netplay.cfg.npid) == 0;

    /* Only for a caller who asked for "the Twitch login", whoever that is. One
     * who named an account and got refused wanted THAT account, and quietly
     * putting them online as somebody else is worse than failing. */
    if (!under_own_name && g_netplay.twitch_wanted && !g_netplay.twitch_tried_owner) {
        g_netplay.twitch_tried_owner = true;
        netplay_log("the stored Twitch login is not %s's - trying it as %s",
                    g_netplay.cfg.npid, owner);
        snprintf(g_netplay.cfg.npid, sizeof(g_netplay.cfg.npid), "%s", owner);
        netplay_do_connect(&g_netplay.cfg);
        return;
    }

    if (under_own_name) {
        g_netplay.cfg.twitch_token[0] = '\0';
        g_netplay.cfg.twitch_npid[0]  = '\0';
        netplay_settings_save();
        netplay_log("the server no longer accepts the stored Twitch login - forgotten");
    }

    if (g_netplay.twitch_wanted) {
        g_netplay.twitch_wanted = false;
        if (rpcn_twitch_begin(&g_netplay.twitch, g_netplay.cfg.server, g_netplay.cfg.port,
                              g_netplay.cfg.fingerprint))
            netplay_log("Twitch: asking %s for a device code", g_netplay.cfg.server);
        else
            netplay_log("Twitch: %s", g_netplay.twitch.error);
    }
}

/*
 * Tell the room this fighter has walked out of the match (LOCKSTEP_PACKET_BYE):
 * the opponent, who cannot go on alone, and every watcher, whose board cannot
 * either. Three copies each: it is one datagram with nothing after it to repair
 * a loss, and a lost one costs the receiver the whole stall timeout.
 *
 * Only for walking OUT. A match that reached its result sends nothing: the
 * opponent may be a few frames short of the result frame and still has to get
 * there, and the room already knows (netplay_linger_pump keeps feeding it).
 */
static inline void netplay_send_bye(void) {
    if (!netplay_is_fighter() || g_netplay.match_result_seen) return;
    if (g_netplay.state != NETPLAY_PLAYING && g_netplay.state != NETPLAY_SYNCING) return;
    lockstep_announce_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    netplay_fill_header(&pkt.header, LOCKSTEP_PACKET_BYE);
    pkt.seed = g_netplay.frame;
    for (int i = 0; i < 3; i++) {
        netplay_send_to(netplay_opponent_id(), &pkt, sizeof(pkt));
        netplay_send_watchers(&pkt, sizeof(pkt));
    }
}

/* Stop running a match on this board, whatever the reason, and hand it back to
 * the keyboard. The rings are kept: a fighter goes on answering repairs from
 * them until the next match clears them. */
static inline void netplay_end_match(const char *why) {
    if (netplay_running_match() || g_netplay.state == NETPLAY_SYNCING) {
        if (why) netplay_log("match %u: %s", (unsigned)g_netplay.match_started, why);
        g_netplay.state = rpcn_session_in_room(&g_netplay.session) ? NETPLAY_IN_ROOM : NETPLAY_ONLINE;
    }
    g_netplay.match_live     = false;
    g_netplay.reset_pending  = false;
    g_netplay.stall_since_ms = 0;
    if (g_netplay.me.playing) { g_netplay.me.playing = 0; g_netplay.me_dirty = true; }
    netplay_release_inputs();
}

/* Everything we knew about a room, forgotten. */
static inline void netplay_forget_room(void) {
    memset(&g_netplay.room, 0, sizeof(g_netplay.room));
    g_netplay.room.last_result = ROOM_RESULT_NONE;
    g_netplay.room_known    = false;
    g_netplay.room_rev_seen = 0;
    g_netplay.was_owner     = false;
    g_netplay.match_started = 0;
    g_netplay.match_live    = false;
    g_netplay.me_dirty      = false;
    g_netplay.force_start   = false;
    g_netplay.auto_deadline_ms = 0;
    g_netplay.fighters_seen = 0;
    g_netplay.local_player  = -1;
    /* Stats and flags start again with the room; the entry request and the
     * choice to sit out are the player's and outlive it. */
    uint8_t entry = g_netplay.me.entry;
    bool    watch = (g_netplay.me.flags & ROOM_MEMBER_WATCH) != 0;
    memset(&g_netplay.me, 0, sizeof(g_netplay.me));
    g_netplay.me.entry  = entry;
    g_netplay.me.flags  = watch ? ROOM_MEMBER_WATCH : 0;
    g_netplay.me.result = ROOM_RESULT_NONE;
}

static inline void netplay_do_disconnect(void) {
    netplay_send_bye();
    rpcn_session_stop(&g_netplay.session);
    netplay_end_match(NULL);
    netplay_forget_room();
    g_netplay.enabled      = false;
    g_netplay.state        = NETPLAY_OFF;
    g_netplay.frame        = 0;
    netplay_check_clear();
    netplay_log("disconnected");
}

static inline void netplay_do_host(const netplay_config_t *cfg) {
    g_netplay.cfg.frame_delay   = cfg->frame_delay;
    g_netplay.cfg.max_players   = cfg->max_players;
    g_netplay.cfg.room_password[0] = '\0';
    snprintf(g_netplay.cfg.room_password, sizeof(g_netplay.cfg.room_password), "%s",
             cfg->room_password);
    uint32_t delay = cfg->frame_delay ? cfg->frame_delay : 2u;
    uint32_t slots = cfg->max_players < 2 ? 2u
                   : cfg->max_players > ROOM_MAX_MEMBERS ? ROOM_MAX_MEMBERS : cfg->max_players;
    uint32_t flags = netplay_room_flags(g_active_profile, delay);

    /* The room is born with its state, so nobody ever reads it empty: in the
     * lobby, nobody in line yet (the owner puts itself there the moment the
     * server says who it is), and the delay every match in it will use. */
    netplay_forget_room();
    g_netplay.room.phase       = ROOM_PHASE_LOBBY;
    g_netplay.room.frame_delay = (uint8_t)delay;
    uint8_t room_bin[ROOM_STATE_SIZE], me_bin[ROOM_MEMBER_SIZE];
    uint32_t room_len = room_state_encode(&g_netplay.room, room_bin);
    uint32_t me_len   = room_member_encode(&g_netplay.me, me_bin);
    if (!rpcn_session_host(&g_netplay.session, slots,
                           cfg->room_password[0] ? cfg->room_password : NULL, flags,
                           room_bin, room_len, me_bin, me_len)) {
        g_netplay.state = NETPLAY_FAILED;
        return;
    }
    /* One nonce per room, from the clock. It is not load-bearing for determinism
     * (see lockstep.h) but it is what proves in a log that two machines think
     * they are in the same session. Each match gets a fresh one. */
    g_netplay.seed = (uint32_t)(net_now_ms() * 2654435761u) | 1u;
    netplay_log("hosting a room for up to %u; waiting for players", slots);
}

static inline void netplay_do_join(const netplay_config_t *cfg) {
    /* Refuse a room we can already tell will not work, and say why. */
    for (uint32_t i = 0; i < g_netplay.session.room_count; i++) {
        if (g_netplay.session.rooms[i].room_id != cfg->room_id) continue;
        const char *why = netplay_room_reject_reason(g_netplay.session.rooms[i].flag_attr,
                                                     g_active_profile);
        if (why) { netplay_log("not joining: %s", why); return; }
        break;
    }
    netplay_forget_room();
    uint8_t me_bin[ROOM_MEMBER_SIZE];
    uint32_t me_len = room_member_encode(&g_netplay.me, me_bin);
    if (!rpcn_session_join(&g_netplay.session, cfg->room_id,
                           cfg->room_password[0] ? cfg->room_password : NULL, me_bin, me_len)) {
        g_netplay.state = NETPLAY_FAILED;
        return;
    }
    netplay_log("joining room %llu", (unsigned long long)cfg->room_id);
}

/* Start: "I am ready to play". The owner starts the first match once every
 * player has said so; a player who was sitting out is back in. */
static inline void netplay_do_start(void) {
    if (!rpcn_session_in_room(&g_netplay.session)) {
        netplay_log("take a room before starting a session");
        return;
    }
    uint8_t before = g_netplay.me.flags;
    g_netplay.me.flags = (uint8_t)((g_netplay.me.flags | ROOM_MEMBER_READY) & ~ROOM_MEMBER_WATCH);
    if (g_netplay.me.flags != before) {
        g_netplay.me_dirty = true;
        netplay_log("ready to play");
    }
}

/* Stop: not ready, and out of the match this board is running if there is one.
 * A fighter who stops ends the match for both (the owner calls it off, since
 * the other side cannot play on alone); a watcher only stops watching. */
static inline void netplay_do_stop(void) {
    bool fighting = netplay_is_fighter() && (g_netplay.match_live || g_netplay.state == NETPLAY_SYNCING);
    netplay_send_bye();
    if (g_netplay.me.flags & ROOM_MEMBER_READY) {
        g_netplay.me.flags = (uint8_t)(g_netplay.me.flags & ~ROOM_MEMBER_READY);
        g_netplay.me_dirty = true;
    }
    netplay_end_match(fighting ? "left by this player" : (g_netplay.match_live ? "stopped watching" : NULL));
    netplay_log("not ready");
}

static inline void netplay_do_entry(uint8_t entry) {
    if (entry > ROOM_ENTRY_2P) entry = ROOM_ENTRY_NONE;
    if (g_netplay.me.entry == entry) return;
    g_netplay.me.entry = entry;
    g_netplay.me_dirty = true;
    netplay_log(entry == ROOM_ENTRY_1P ? "asking for the 1P side"
              : entry == ROOM_ENTRY_2P ? "asking for the 2P side" : "either side will do");
}

static inline void netplay_do_watch(bool watch) {
    uint8_t flags = watch ? (uint8_t)((g_netplay.me.flags | ROOM_MEMBER_WATCH) & ~ROOM_MEMBER_READY)
                          : (uint8_t)(g_netplay.me.flags & ~ROOM_MEMBER_WATCH);
    if (flags == g_netplay.me.flags) return;
    g_netplay.me.flags = flags;
    g_netplay.me_dirty = true;
    netplay_log(watch ? "sitting out: watching only" : "back in the line to play");
}

static inline void netplay_do_leave_room(void) {
    netplay_send_bye();
    netplay_end_match(NULL);
    rpcn_session_leave(&g_netplay.session);
    netplay_forget_room();
    if (g_netplay.state != NETPLAY_OFF && g_netplay.state != NETPLAY_FAILED) g_netplay.state = NETPLAY_ONLINE;
}

/*
 * TAKING A ROOM waits until the signaling helper has answered us.
 *
 * RPCN copies each member's address into the room when the room is created or
 * joined, and never refreshes the copy. The address itself only reaches the
 * server with the first UDP keepalive after login, so a Host or Join sent in the
 * first moments of a session can snapshot nothing at all. Then the room tells a
 * joiner there is no address for its owner (the joiner falls back to asking), and,
 * worse, it tells two players who share a public address -- which every web player
 * does, through the gateway -- DIFFERENT kinds of address for each other: one gets
 * the other's public address from the room, the other the local one from a
 * lookup, and each then discards the other's datagrams as strays. Found by
 * tools/web-netplay.mjs, which hosts 0.2 s after signing in; a person clicking
 * rarely beats the keepalive, a script always does.
 *
 * So Host and Join wait for the helper's reply, which proves the server has the
 * address, for at most NETPLAY_ROOM_WAIT_MS. After that they go ahead anyway:
 * a server with no UDP helper at all must still be usable for everything else.
 */
#define NETPLAY_ROOM_WAIT_MS 4000u

static inline void netplay_take_room(const netplay_cmd_t *cmd) {
    if (cmd->kind == NETPLAY_CMD_HOST) netplay_do_host(&cmd->cfg);
    else                               netplay_do_join(&cmd->cfg);
}

static inline void netplay_room_or_defer(const netplay_cmd_t *cmd) {
    if (g_netplay.session.signaling_seen) { netplay_take_room(cmd); return; }
    if (!g_netplay.room_deferred)
        netplay_log("waiting for the server to learn this machine's address before taking a room");
    g_netplay.room_cmd         = *cmd;
    g_netplay.room_deferred    = true;
    g_netplay.room_deferred_ms = net_now_ms();
}

static inline void netplay_pump_deferred_room(void) {
    if (!g_netplay.room_deferred) return;
    if (g_netplay.state != NETPLAY_ONLINE) { g_netplay.room_deferred = false; return; }
    bool timed_out = net_now_ms() - g_netplay.room_deferred_ms > NETPLAY_ROOM_WAIT_MS;
    if (!g_netplay.session.signaling_seen && !timed_out) return;
    if (!g_netplay.session.signaling_seen)
        netplay_log("the server's UDP helper has not answered; taking the room anyway");
    g_netplay.room_deferred = false;
    netplay_take_room(&g_netplay.room_cmd);
}

static inline void netplay_pump_commands(void) {
    netplay_cmd_t cmd;
    while (netplay_take_cmd(&cmd)) {
        switch (cmd.kind) {
            case NETPLAY_CMD_CONNECT:
                /* A named account: a refusal is an answer, not a cue to open a
                 * browser. */
                g_netplay.twitch_wanted      = false;
                g_netplay.twitch_tried_owner = false;
                netplay_do_connect(&cmd.cfg);
                break;
            case NETPLAY_CMD_DISCONNECT: netplay_do_disconnect(); break;
            case NETPLAY_CMD_HOST:
            case NETPLAY_CMD_JOIN:       netplay_room_or_defer(&cmd); break;
            case NETPLAY_CMD_SEARCH:
                rpcn_session_search(&g_netplay.session, cmd.cfg.browse_yamp);
                break;
            case NETPLAY_CMD_START:      netplay_do_start(); break;
            case NETPLAY_CMD_STOP:       netplay_do_stop(); break;
            case NETPLAY_CMD_ENTRY:      netplay_do_entry(cmd.cfg.entry); break;
            case NETPLAY_CMD_WATCH:      netplay_do_watch(cmd.cfg.watch_only); break;
            case NETPLAY_CMD_FORCE_START:
                if (netplay_is_host()) g_netplay.force_start = true;
                else netplay_log("only the room's owner can start a match early");
                break;
            case NETPLAY_CMD_LEAVE_ROOM: netplay_do_leave_room(); break;
            case NETPLAY_CMD_CREATE_ACCOUNT:
                rpcn_account_create(&g_netplay.account, cmd.cfg.server, cmd.cfg.port,
                                    cmd.cfg.fingerprint, cmd.cfg.npid, cmd.cfg.password,
                                    cmd.cfg.email);
                break;
            case NETPLAY_CMD_RESEND_TOKEN:
                rpcn_account_resend(&g_netplay.account, cmd.cfg.server, cmd.cfg.port,
                                    cmd.cfg.fingerprint, cmd.cfg.npid, cmd.cfg.password);
                break;
            case NETPLAY_CMD_TWITCH_START:
                /* Already signed in? Then this is a login, not a trip to
                 * twitch.tv. Before the server below is overwritten: the stored
                 * one is what the token is compared against. */
                if (netplay_twitch_reuse(&cmd.cfg)) break;
                /* Remember the server the flow is being run against: the token it
                 * yields is only good on that one. */
                snprintf(g_netplay.cfg.server, sizeof(g_netplay.cfg.server), "%s", cmd.cfg.server);
                g_netplay.cfg.port = cmd.cfg.port;
                snprintf(g_netplay.cfg.fingerprint, sizeof(g_netplay.cfg.fingerprint), "%s",
                         cmd.cfg.fingerprint);
                if (rpcn_twitch_begin(&g_netplay.twitch, cmd.cfg.server, cmd.cfg.port,
                                      cmd.cfg.fingerprint))
                    netplay_log("Twitch: asking %s for a device code", cmd.cfg.server);
                else
                    netplay_log("Twitch: %s", g_netplay.twitch.error);
                break;
            case NETPLAY_CMD_TWITCH_CANCEL:
                rpcn_twitch_reset(&g_netplay.twitch);
                netplay_log("Twitch sign-in cancelled");
                break;
            case NETPLAY_CMD_TWITCH_FORGET:
                g_netplay.cfg.twitch_token[0] = '\0';
                g_netplay.cfg.twitch_npid[0]  = '\0';
                rpcn_twitch_reset(&g_netplay.twitch);
                netplay_settings_save();
                netplay_log("forgot the stored Twitch login");
                break;
            default: break;
        }
    }
}

/* ---- The pump, and the frame gate ---------------------------------------- */

static inline void netplay_mirror_stage(void) {
    /* The session's own progress becomes our state, except while a match is
     * running: SYNCING/PLAYING/WATCHING are owned by the room, not by the
     * transport -- unless the room itself went away under the match. */
    if (g_netplay.state == NETPLAY_SYNCING || netplay_running_match()) {
        if (rpcn_session_in_room(&g_netplay.session)) return;
        netplay_end_match("the room is gone");
    }
    switch (g_netplay.session.stage) {
        case RPCN_STAGE_LOGGING_IN: g_netplay.state = NETPLAY_CONNECTING; break;
        case RPCN_STAGE_ONLINE:
            if (g_netplay.state == NETPLAY_IN_ROOM) netplay_forget_room();   /* closed, or left */
            /* Remember the server and account only once they are known to WORK —
             * storing what was typed would just as happily store a typo. */
            if (g_netplay.state != NETPLAY_ONLINE) {
                /* And the same goes for WHOSE the Twitch token is. The owner
                 * adopted at load is a guess; a login that went out with the
                 * token and came back accepted is the server saying so. */
                if (g_netplay.sent_twitch_token)
                    snprintf(g_netplay.cfg.twitch_npid, sizeof(g_netplay.cfg.twitch_npid),
                             "%s", g_netplay.cfg.npid);
                g_netplay.twitch_wanted      = false;
                g_netplay.twitch_tried_owner = false;
                netplay_settings_save();
            }
            g_netplay.state = NETPLAY_ONLINE;
            break;
        case RPCN_STAGE_HOSTING:
        case RPCN_STAGE_JOINING:
        case RPCN_STAGE_LINKED:
            if (g_netplay.state != NETPLAY_IN_ROOM) {
                netplay_log("room %llu ready (%s)",
                            (unsigned long long)g_netplay.session.room_id,
                            g_netplay.session.is_host ? "hosting" : "joined");
                if (!g_netplay.session.is_host) {
                    /* The attribute word the server actually holds, read back on
                     * join. Two things come out of it, and both are the reason it
                     * is published at all rather than assumed. */
                    uint32_t flags = g_netplay.session.room_flags;
                    const char *why = netplay_room_reject_reason(flags, g_active_profile);
                    if (why)
                        netplay_log("WARNING: %s - this session will not stay in sync", why);

                    /* THE HOST OWNS THE FRAME DELAY. A mismatch is not a desync
                     * (both machines still apply the same two words to the same
                     * frame) but it is unfair: whoever set the lower number plays
                     * with less input lag than the other. Adopting the host's is
                     * what makes the room's advertised delay mean something. */
                    uint32_t d = (flags >> NETPLAY_ROOM_DELAY_SHIFT) & NETPLAY_ROOM_DELAY_MASK;
                    if (flags && d != g_netplay.cfg.frame_delay) {
                        netplay_log("using the host's frame delay of %u (yours was %u)",
                                    d, g_netplay.cfg.frame_delay);
                        g_netplay.cfg.frame_delay = d;
                    }
                }
            }
            g_netplay.state = NETPLAY_IN_ROOM;
            break;
        case RPCN_STAGE_FAILED:
            /* On the edge only: the stage stays FAILED on every pump after it,
             * and the handler below may start a fresh login. */
            if (g_netplay.state != NETPLAY_FAILED) {
                g_netplay.state = NETPLAY_FAILED;
                netplay_twitch_refused();
            }
            break;
        default: break;
    }
}

static inline void netplay_report_wait(void) {
    /* A barrier that never releases is almost always a connectivity problem, and
     * "waiting for the peer" says nothing about which half failed. Report which
     * of the two steps we are stuck on: not knowing where the peer is (the server
     * never told us) or knowing and hearing nothing back (a NAT or firewall is
     * eating the punch). */
    uint64_t now = net_now_ms();
    if (now - g_netplay.last_wait_report_ms <= 5000) return;
    g_netplay.last_wait_report_ms = now;

    const rpcn_peer_t *p = rpcn_session_peer(&g_netplay.session, netplay_opponent_id());
    if (!p)
        netplay_log("still waiting: the opponent is not in the room");
    else if (!p->ip || !p->port)
        netplay_log("still waiting: the server has not given us %s's address yet", rpcn_peer_name(p));
    else if (!p->heard)
        netplay_log("still waiting: punching %s at %s but nothing has come back - check that UDP %u "
                    "is not blocked by a firewall on either machine",
                    rpcn_peer_name(p), rpcn_peer_addr_text(p), (unsigned)RPCN_P2P_PORT);
    else
        netplay_log("still waiting: %s at %s is reachable but has not started this match",
                    rpcn_peer_name(p), rpcn_peer_addr_text(p));
}

/*
 * Drives the Twitch device flow and, when it lands, turns it into an ordinary
 * login: the npid and the login token become the account and the password, they
 * are written to the settings file so the browser dance happens once and not
 * once per launch, and the session connects itself — signing in is the whole of
 * what the player asked for, and stopping to make them press Connect afterwards
 * would be a step with no decision in it.
 */
static inline void netplay_pump_twitch(void) {
    rpcn_twitch_state_t before = g_netplay.twitch.state;
    rpcn_twitch_update(&g_netplay.twitch);
    rpcn_twitch_state_t now = g_netplay.twitch.state;
    if (now == before) return;

    if (now == RPCN_TWITCH_WAITING) {
        netplay_log("Twitch: enter code %s at %s", g_netplay.twitch.user_code,
                    g_netplay.twitch.verification_uri);
        if (!g_netplay_may_open_browser)
            netplay_log("Twitch: open that address on any machine and approve the code");
        else if (!netplay_open_url(g_netplay.twitch.verification_uri))
            netplay_log("Twitch: could not open a browser - go to that address yourself");
        return;
    }

    if (now == RPCN_TWITCH_FAILED) {
        netplay_log("Twitch: %s", g_netplay.twitch.error);
        return;
    }

    if (now == RPCN_TWITCH_DONE) {
        snprintf(g_netplay.cfg.npid, sizeof(g_netplay.cfg.npid), "%s", g_netplay.twitch.npid);
        snprintf(g_netplay.cfg.twitch_token, sizeof(g_netplay.cfg.twitch_token), "%s",
                 g_netplay.twitch.login_token);
        snprintf(g_netplay.cfg.twitch_npid, sizeof(g_netplay.cfg.twitch_npid), "%s",
                 g_netplay.twitch.npid);
        g_netplay.cfg.password[0] = '\0';   /* the token stands in for it */
        g_netplay.cfg.token[0]    = '\0';   /* Twitch vouched; no e-mail token is checked */
        netplay_settings_save();
        netplay_log("Twitch: signed in as %s", g_netplay.cfg.npid);

        if (!g_netplay.enabled || g_netplay.state == NETPLAY_OFF
            || g_netplay.state == NETPLAY_FAILED) {
            netplay_do_connect(&g_netplay.cfg);
        }
    }
}

/*
 * A stalled machine says again what it last said.
 *
 * Inputs go out once, when a new local frame is sampled, and a machine waiting
 * for the peer samples nothing. With both peers waiting nobody transmits, so a
 * burst of loss longer than the frames in flight is never repaired and the two
 * sit there until the stall timer ends the session. See lockstep_resend_floor
 * for the range, and why the newest record alone is not enough.
 */
static inline void netplay_resend_inputs(void) {
    const lockstep_t *l = &g_netplay.lockstep;
    if (lockstep_is_watcher(l) || l->last_local_frame == LOCKSTEP_INVALID_FRAME) return;

    uint64_t now = net_now_ms();
    if (now - g_netplay.last_resend_ms < NETPLAY_RESEND_MS) return;
    g_netplay.last_resend_ms = now;

    uint32_t floor = lockstep_resend_floor(l);
    uint32_t top   = l->last_local_frame;
    /* Newest first: it is the one most likely to be the only thing missing.
     * Four records is 40 frames, well past what the largest delay can need. */
    for (int sent = 0; sent < 4; sent++) {
        netplay_send_record(netplay_opponent_id(), top);
        if (top < floor + LOCKSTEP_REDUNDANCY) break;   /* this record reached the floor */
        top -= LOCKSTEP_REDUNDANCY;
    }
}

/*
 * Watches THIS board, which nothing used to.
 *
 * Both players' inputs being in is permission to run a frame, not proof that it
 * ran. An emulator that is paused, halted, or running without ever reaching the
 * end of the frame is cleared for the same frame on every slice, so it never
 * counts as stalled: it shows "playing" with a frame number that does not move,
 * sends nothing because it samples nothing, and the only evidence anywhere is the
 * OTHER machine's stall timer, fifteen seconds later, blaming the network.
 *   How it surfaced: the first session with a player on another network. Their
 *   board stopped finishing frames 45 frames after the reset, twice, with a
 *   corrupted screen; the host logged a stall at frame 48 and nothing on either
 *   side said which machine had stopped.
 * Returns true when it has ended the session.
 */
static inline bool netplay_watch_own_board(uint32_t frame) {
    uint64_t now = net_now_ms();
    if (g_netplay.ready_frame != frame) {
        g_netplay.ready_frame    = frame;
        g_netplay.ready_since_ms = now;
        g_netplay.ready_reported = false;
        return false;
    }
    uint64_t held = now - g_netplay.ready_since_ms;
    /* Keep talking while we are the one holding things up. We sample nothing, so
     * we would otherwise go silent, and the peer could not tell a board that
     * stopped from a cable that was pulled: its stall message reports how long
     * ago our last input arrived. */
    if (held > NETPLAY_OWN_BOARD_QUIET_MS) netplay_resend_inputs();
    if (!g_netplay.ready_reported && held > NETPLAY_OWN_BOARD_REPORT_MS) {
        g_netplay.ready_reported = true;
        /* Under NETPLAY_LOG_LEN with both numbers at their widest. */
        netplay_log("both inputs for frame %u have been in for %u ms and this board has not "
                    "finished it - paused, halted, or stuck inside the frame",
                    frame, (unsigned)held);
    }
    if (g_netplay.stall_timeout_ms && held > g_netplay.stall_timeout_ms) {
        netplay_log("this board did not finish frame %u in %u ms - leaving the session",
                    frame, g_netplay.stall_timeout_ms);
        netplay_end_match(NULL);
        return true;
    }
    return false;
}

/* The run loop's half of the same report: it knows WHY the board is not running.
 * Called from the emu thread whenever it finds itself stopped; once per frame. */
static inline void netplay_board_stopped(uint32_t ip, bool halted) {
    if (!g_netplay.enabled || !netplay_running_match() || g_netplay.reset_pending) return;
    if (g_netplay.stopped_noted_frame == g_netplay.frame) return;
    g_netplay.stopped_noted_frame = g_netplay.frame;
    netplay_log(halted ? "the emulated CPU HALTED at IP=0x%08X on frame %u - this session cannot continue"
                       : "the emulator is paused at IP=0x%08X on frame %u - the room is waiting (F9 resumes)",
                (unsigned)ip, g_netplay.frame);
}

/* ---- The room ------------------------------------------------------------ */

/*
 * How long the owner waits after a result before the next match starts on its
 * own: long enough to see who won and change an entry or sit out, short enough
 * that a room keeps rolling like a cabinet with a line in front of it. The port
 * uses 10 s for its result screen and a lobby countdown after that.
 */
#define NETPLAY_NEXT_MATCH_MS    10000u
/* Both fighters must have started their boards within this long of the owner
 * calling a match, or it is called off and the room tries again. */
#define NETPLAY_MATCH_START_MS   30000u
/* Our own attribute is republished at most this often. */
#define NETPLAY_MEMBER_PUBLISH_MS  200u

/* The room as room.h sees it: us, from our own copy, and everybody else from
 * the server's. */
static inline uint32_t netplay_room_members(room_member_t *out) {
    uint32_t n = 0;
    const rpcn_session_t *s = &g_netplay.session;
    if (s->my_member_id) {
        out[n].id    = s->my_member_id;
        out[n].known = true;
        out[n].data  = g_netplay.me;
        n++;
    }
    for (uint32_t i = 0; i < RPCN_MAX_PEERS && n < ROOM_MAX_MEMBERS; i++) {
        if (!s->peers[i].used) continue;
        out[n].id    = s->peers[i].member_id;
        out[n].known = room_member_decode(s->peers[i].bin, s->peers[i].bin_len, &out[n].data);
        n++;
    }
    return n;
}

static inline const char *netplay_member_name(uint16_t id) {
    if (!id) return "nobody";
    if (id == g_netplay.session.my_member_id) return g_netplay.session.npid;
    return rpcn_peer_name(rpcn_session_peer(&g_netplay.session, id));
}

static inline void netplay_publish_me(void) {
    if (!g_netplay.me_dirty || !rpcn_session_in_room(&g_netplay.session)) return;
    uint64_t now = net_now_ms();
    if (now - g_netplay.me_sent_ms < NETPLAY_MEMBER_PUBLISH_MS) return;
    uint8_t bin[ROOM_MEMBER_SIZE];
    uint32_t len = room_member_encode(&g_netplay.me, bin);
    if (rpcn_session_set_member_state(&g_netplay.session, bin, len)) {
        g_netplay.me_dirty   = false;
        g_netplay.me_sent_ms = now;
    }
}

/*
 * Read the room's state off the session, once per change. The OWNER does not:
 * its own copy is the newest there is, and the server's is at best the echo of
 * an earlier write -- taking that would undo whatever it decided in between.
 * The one time the owner does read it is on becoming the owner, which is how a
 * room survives its owner leaving: the successor carries on from the state on
 * the server.
 */
static inline void netplay_refresh_room(void) {
    rpcn_session_t *s = &g_netplay.session;
    if (s->room_rev == g_netplay.room_rev_seen) return;
    g_netplay.room_rev_seen = s->room_rev;

    bool owner = netplay_is_host();
    if (!owner || !g_netplay.was_owner) {
        room_state_t fresh;
        if (room_state_decode(s->room_bin, s->room_bin_len, &fresh)) {
            g_netplay.room       = fresh;
            g_netplay.room_known = true;
        }
        if (owner && s->my_member_id) {
            netplay_log(s->is_host ? "you run this room" : "the room's owner left - this machine runs it now");
            g_netplay.auto_deadline_ms = net_now_ms() + NETPLAY_NEXT_MATCH_MS;
            g_netplay.match_begun_ms   = net_now_ms();
            g_netplay.fighters_seen    = 0;
        }
    }
    if (s->my_member_id) g_netplay.was_owner = owner;
}

static inline void netplay_publish_room(void) {
    uint8_t bin[ROOM_STATE_SIZE];
    uint32_t len = room_state_encode(&g_netplay.room, bin);
    rpcn_session_set_room_state(&g_netplay.session, bin, len);
}

/*
 * The owner's half: keep the line in step with who is in the room, start a
 * match when it is time, and finish it when a board reports the result -- or
 * call it off when it cannot finish. Every decision is one write of the room
 * state, which every member (the owner included) then acts on.
 */
static inline void netplay_owner_pump(void) {
    if (!netplay_is_host() || !g_netplay.session.my_member_id) return;

    room_member_t m[ROOM_MAX_MEMBERS];
    uint32_t n = netplay_room_members(m);
    room_state_t s = g_netplay.room;
    bool changed = false;
    uint64_t now = net_now_ms();

    if (!g_netplay.room_known) {
        memset(&s, 0, sizeof(s));
        s.phase       = ROOM_PHASE_LOBBY;
        s.frame_delay = (uint8_t)(g_netplay.cfg.frame_delay ? g_netplay.cfg.frame_delay : 2u);
        s.last_result = ROOM_RESULT_NONE;
        g_netplay.room_known = true;
        changed = true;
    }
    if (room_line_sync(&s, m, n)) changed = true;

    if (s.phase == ROOM_PHASE_LOBBY) {
        uint32_t players = room_player_count(&s, m, n);
        bool due = (s.flags & ROOM_FLAG_AUTO) && g_netplay.auto_deadline_ms && now >= g_netplay.auto_deadline_ms;
        uint16_t f[2];
        if (players >= 2 && (g_netplay.force_start || room_all_ready(&s, m, n) || due)
            && room_pick_fighters(&s, m, n, f)) {
            s.phase       = ROOM_PHASE_MATCH;
            s.match       = (uint16_t)(s.match + 1u);
            if (!s.match) s.match = 1;   /* 0 means "none yet" */
            s.fighter[0]  = f[0];
            s.fighter[1]  = f[1];
            s.seed        = (uint32_t)(now * 2654435761u) ^ ((uint32_t)s.match << 16) ^ 0x5A5Au;
            s.last_result = ROOM_RESULT_NONE;
            s.flags       = (uint8_t)(s.flags & ~ROOM_FLAG_AUTO);
            g_netplay.match_begun_ms = now;
            g_netplay.fighters_seen  = 0;
            changed = true;
            netplay_log("match %u: %s (1P) vs %s (2P)", (unsigned)s.match,
                        netplay_member_name(f[0]), netplay_member_name(f[1]));
        } else if (g_netplay.force_start) {
            netplay_log("cannot start: a match needs two players who are not sitting out");
        }
        g_netplay.force_start = false;
    } else {
        int res = (g_netplay.me.result_match == s.match && g_netplay.me.result <= 1)
                ? g_netplay.me.result : room_reported_result(m, n, s.match);
        if (res >= 0) {
            uint16_t loser = room_rotate_line(&s, (uint32_t)res);
            s.last_result = (uint8_t)res;
            s.phase       = ROOM_PHASE_LOBBY;
            s.flags       = (uint8_t)(s.flags | ROOM_FLAG_AUTO);
            g_netplay.auto_deadline_ms = now + NETPLAY_NEXT_MATCH_MS;
            changed = true;
            netplay_log("match %u won by %s (%s); %s goes to the back of the line", (unsigned)s.match,
                        netplay_member_name(s.fighter[res]), res == 0 ? "1P" : "2P",
                        netplay_member_name(loser));
        } else {
            const char *why = NULL;
            for (uint32_t side = 0; side < 2; side++) {
                const room_member_t *mm = room_find(m, n, s.fighter[side]);
                if (!mm)                                         why = "a fighter left the room";
                else if (mm->known && mm->data.playing == s.match) g_netplay.fighters_seen |= 1u << side;
                else if (g_netplay.fighters_seen & (1u << side))   why = "a fighter stopped playing";
            }
            if (!why && g_netplay.fighters_seen != 3u && now - g_netplay.match_begun_ms > NETPLAY_MATCH_START_MS)
                why = "the fighters' boards did not both start";
            if (why) {
                s.phase       = ROOM_PHASE_LOBBY;
                s.last_result = ROOM_RESULT_NONE;
                changed = true;
                netplay_log("match %u called off: %s", (unsigned)s.match, why);
            }
        }
    }

    if (changed) {
        g_netplay.room = s;
        netplay_publish_room();
    }
}

/*
 * Every member's half, the owner's included: do what the room state says.
 * A match we have not started yet is started -- as a fighter if the state names
 * us, as a watcher otherwise -- and a match the room has called off is stopped.
 * A match the room has FINISHED is not cut short: a board a few frames behind the
 * one that reported the result still has to reach it itself, or it would miss
 * the rotation it is about to see.
 */
static inline void netplay_member_pump(void) {
    uint16_t me = g_netplay.session.my_member_id;
    if (!g_netplay.room_known || !me) return;
    const room_state_t *r = &g_netplay.room;

    if (r->phase == ROOM_PHASE_MATCH && r->match && r->match != g_netplay.match_started) {
        int side = room_side_of(r, me);
        netplay_end_match(NULL);
        g_netplay.match_started = r->match;
        g_netplay.seed          = r->seed;
        if (side >= 0) {
            netplay_log("match %u: you are %s against %s", (unsigned)r->match, side == 0 ? "1P" : "2P",
                        netplay_member_name(r->fighter[side ^ 1]));
            netplay_begin_generation(r->match, (uint32_t)side, r->frame_delay);
        } else {
            netplay_log("match %u: watching %s vs %s", (unsigned)r->match,
                        netplay_member_name(r->fighter[0]), netplay_member_name(r->fighter[1]));
            netplay_begin_generation(r->match, LOCKSTEP_WATCHER, r->frame_delay);
        }
        g_netplay.match_live = true;
        g_netplay.me.playing = r->match;
        g_netplay.me_dirty   = true;
    }

    if (g_netplay.match_live && r->match == g_netplay.match_started
        && r->phase == ROOM_PHASE_LOBBY && r->last_result == ROOM_RESULT_NONE)
        netplay_end_match("called off by the room");

    /* A result our board did not see -- we were not running it, or dropped out
     * of it -- is still ours to record, and still moves our entry request. */
    if (r->phase == ROOM_PHASE_LOBBY && r->match && r->last_result <= 1
        && g_netplay.me.result_match != r->match && !g_netplay.match_live) {
        if (room_after_result(&g_netplay.me, me, r, r->match, r->last_result)) g_netplay.me_dirty = true;
    }
}

/* A fighter whose board has reached the result goes on repeating its last
 * inputs for a while: the other fighter may be a few frames behind and missing
 * the last of them, and watchers catch up from them. */
static inline void netplay_linger_pump(void) {
    if (!g_netplay.linger_until_ms || net_now_ms() > g_netplay.linger_until_ms) {
        g_netplay.linger_until_ms = 0;
        return;
    }
    const lockstep_t *l = &g_netplay.lockstep;
    if (lockstep_is_watcher(l) || l->last_local_frame == LOCKSTEP_INVALID_FRAME) return;
    uint64_t before = g_netplay.last_resend_ms;
    netplay_resend_inputs();
    if (g_netplay.last_resend_ms != before) {
        lockstep_input_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        netplay_fill_header(&pkt.header, LOCKSTEP_PACKET_INPUT);
        pkt.check_frame = g_netplay.last_check_frame;
        pkt.check_value = g_netplay.last_check_value;
        lockstep_fill_record(l, l->last_local_frame, &pkt.record);
        netplay_send_watchers(&pkt, sizeof(pkt));
    }
}

/* A watcher that has fallen behind asks the fighter who is missing a frame to
 * send it again -- but only once there is evidence the match got past it,
 * because before the fighters' barrier releases every frame is "missing". */
static inline void netplay_watch_repair(uint32_t frame) {
    const lockstep_t *l = &g_netplay.lockstep;
    uint64_t now = net_now_ms();
    if (now - g_netplay.watch_stall_ms < NETPLAY_REPAIR_AFTER_MS) return;
    if (now - g_netplay.last_repair_ms < NETPLAY_REPAIR_MS) return;
    g_netplay.last_repair_ms = now;
    for (uint32_t side = 0; side < LOCKSTEP_MAX_PLAYERS; side++) {
        if (lockstep_ring_has(&l->rings[side], frame)) continue;
        uint32_t n0 = l->rings[0].newest, n1 = l->rings[1].newest;
        bool past = (n0 != LOCKSTEP_INVALID_FRAME && n0 >= frame) || (n1 != LOCKSTEP_INVALID_FRAME && n1 >= frame);
        if (!past) continue;
        lockstep_repair_packet_t req;
        memset(&req, 0, sizeof(req));
        netplay_fill_header(&req.header, LOCKSTEP_PACKET_REPAIR);
        req.side  = side;
        req.frame = frame;
        netplay_send_to(g_netplay.room.fighter[side], &req, sizeof(req));
    }
}

/* One slice of a watcher: run the frame when both fighters' inputs are in. */
static inline netplay_step_t netplay_watch_step(void) {
    if (g_netplay.reset_pending) return NETPLAY_STEP_RESET;
    uint32_t frame = g_netplay.frame;
    const lockstep_t *l = &g_netplay.lockstep;
    if (!lockstep_ready(l, frame)) {
        uint64_t now = net_now_ms();
        if (!g_netplay.watch_stall_ms) g_netplay.watch_stall_ms = now;
        netplay_watch_repair(frame);
        /* Only once the fighters have started: until then it is the barrier we
         * are waiting on, and the room calls the match off if that never goes. */
        bool started = l->rings[0].newest != LOCKSTEP_INVALID_FRAME || l->rings[1].newest != LOCKSTEP_INVALID_FRAME;
        if (started && now - g_netplay.watch_stall_ms > NETPLAY_WATCH_GIVEUP_MS) {
            netplay_log("could not get frame %u of the match from the fighters - "
                        "you will be in the next one", frame);
            netplay_end_match(NULL);
        }
        g_netplay.ready_frame = LOCKSTEP_INVALID_FRAME;
        return NETPLAY_STEP_WAIT;
    }
    g_netplay.watch_stall_ms = 0;
    if (netplay_watch_own_board(frame)) return NETPLAY_STEP_WAIT;
    g_netplay.inlog_w0 = lockstep_input_for(l, 0, frame);
    g_netplay.inlog_w1 = lockstep_input_for(l, 1, frame);
    netplay_apply_inputs(g_netplay.inlog_w0, g_netplay.inlog_w1);
    return NETPLAY_STEP_READY;
}

/* One slice of a fighter: sample, send, and run the frame when both sides are in. */
static inline netplay_step_t netplay_fight_step(void) {
    if (g_netplay.reset_pending) return NETPLAY_STEP_RESET;

    /* Sample local input for frame + delay. This is the delay in "delay-based":
     * what the player does now is consumed `delay` frames from now, which buys
     * that many frames of network latency before anyone has to stall. */
    uint32_t frame      = g_netplay.frame;
    uint32_t send_frame = frame + g_netplay.lockstep.frame_delay;
    uint32_t last_sent  = g_netplay.lockstep.last_local_frame;
    if (last_sent == LOCKSTEP_INVALID_FRAME || send_frame > last_sent) {
        lockstep_input_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        netplay_fill_header(&pkt.header, LOCKSTEP_PACKET_INPUT);
        pkt.check_frame = g_netplay.last_check_frame;
        pkt.check_value = g_netplay.last_check_value;
        lockstep_submit_local(&g_netplay.lockstep, send_frame, netplay_sample_local(), &pkt.record);
        netplay_send_to(netplay_opponent_id(), &pkt, sizeof(pkt));
        if (++g_netplay.watch_stride >= NETPLAY_WATCH_STRIDE) {
            g_netplay.watch_stride = 0;
            netplay_send_watchers(&pkt, sizeof(pkt));
        }
    }

    if (!lockstep_ready(&g_netplay.lockstep, frame)) {
        uint64_t now = net_now_ms();
        if (g_netplay.stall_since_ms == 0) {
            g_netplay.stall_since_ms = now;
            g_netplay.lockstep.stalls++;
        } else if (g_netplay.stall_timeout_ms
                   && now - g_netplay.stall_since_ms > g_netplay.stall_timeout_ms) {
            /* Which of the two it was. With the resend below, a peer that is
             * alive keeps talking even when it cannot advance, so silence means
             * the network or the process went away, and recent input means the
             * other board is the one that stopped. */
            if (g_netplay.last_peer_input_ms)
                netplay_log("stalled for more than %u ms at frame %u (the opponent's last input "
                            "arrived %u ms ago) - leaving the match",
                            g_netplay.stall_timeout_ms, frame,
                            (unsigned)(now - g_netplay.last_peer_input_ms));
            else
                netplay_log("stalled for more than %u ms at frame %u (no input ever arrived "
                            "from the opponent) - leaving the match",
                            g_netplay.stall_timeout_ms, frame);
            netplay_send_bye();   /* the watchers cannot go on either */
            netplay_end_match(NULL);
            return NETPLAY_STEP_WAIT;
        }
        netplay_resend_inputs();
        /* Not ours to answer for while we wait on the peer. */
        g_netplay.ready_frame = LOCKSTEP_INVALID_FRAME;
        return NETPLAY_STEP_WAIT;
    }
    g_netplay.stall_since_ms = 0;

    if (netplay_watch_own_board(frame)) return NETPLAY_STEP_WAIT;

    g_netplay.inlog_w0 = lockstep_input_for(&g_netplay.lockstep, 0, frame);
    g_netplay.inlog_w1 = lockstep_input_for(&g_netplay.lockstep, 1, frame);
    netplay_apply_inputs(g_netplay.inlog_w0, g_netplay.inlog_w1);
    return NETPLAY_STEP_READY;
}

/*
 * Called once per slice from the emu thread, OUTSIDE the emu mutex. Pumps the
 * network and the room, then answers what this slice may do.
 */
static inline netplay_step_t netplay_begin_frame(void) {
    if (!g_netplay.mutex_ready) return NETPLAY_STEP_OFF;
    if (g_netplay.inlog && !netplay_running_match()) netplay_inputlog_close();

    netplay_pump_commands();
    rpcn_account_update(&g_netplay.account);
    netplay_pump_twitch();

    if (!g_netplay.enabled) { netplay_publish_status(); return NETPLAY_STEP_OFF; }

    rpcn_session_update(&g_netplay.session);
    netplay_mirror_stage();
    netplay_drain_socket();
    netplay_pump_deferred_room();

    if (rpcn_session_in_room(&g_netplay.session)) {
        netplay_refresh_room();
        netplay_owner_pump();
        netplay_member_pump();
        netplay_linger_pump();
        netplay_publish_me();
    }

    netplay_step_t step = NETPLAY_STEP_OFF;
    if (g_netplay.state == NETPLAY_SYNCING) {
        /* Keep announcing until the barrier releases: the announce is a plain
         * datagram and may be lost. Paced, because this runs every millisecond
         * while we wait: see NETPLAY_ANNOUNCE_MS. */
        if (net_now_ms() - g_netplay.last_announce_ms >= NETPLAY_ANNOUNCE_MS)
            netplay_send_announce();
        netplay_report_wait();

        if (lockstep_barrier_released(&g_netplay.lockstep)) {
            netplay_seed_delay_frames();
            g_netplay.reset_pending = true;
            g_netplay.state         = NETPLAY_PLAYING;
            g_netplay.frame         = 0;
            netplay_log("barrier released; match %u seed 0x%08X - resetting the board",
                        (unsigned)g_netplay.match_started, g_netplay.seed);
            step = NETPLAY_STEP_RESET;
        } else {
            step = NETPLAY_STEP_WAIT;
        }
    } else if (g_netplay.state == NETPLAY_PLAYING) {
        step = netplay_fight_step();
    } else if (g_netplay.state == NETPLAY_WATCHING) {
        step = netplay_watch_step();
    }
    /* Anything else: not in a match. The board runs normally and the keyboard
     * drives it. */
    netplay_publish_status();
    return step;
}

/*
 * Called from the emu thread with the mutex held, right after a slice that ended
 * on a frame boundary. Records this frame's check for the next outgoing packet
 * and advances the netplay frame counter.
 *
 * `versus_result` is the game's own verdict on this frame, from the profile's
 * versus hook (hle_hooks.h): 0 = nothing, 1 = 1P won the match, 2 = 2P won.
 * Every board in the match reaches it on the same frame, which is what makes it
 * safe to act on.
 */
static inline void netplay_end_frame(const i960_cpu_t *cpu, uint64_t total_steps, int versus_result) {
    if (!g_netplay.enabled || !netplay_running_match()) return;

    uint32_t frame = g_netplay.frame;
    uint32_t check = netplay_frame_check(cpu, total_steps);
    uint32_t idx   = frame & LOCKSTEP_RING_MASK;
    g_netplay.check_frame[idx] = frame;
    g_netplay.check_value[idx] = check;
    g_netplay.last_check_frame = frame;
    g_netplay.last_check_value = check;
    netplay_check_compare(frame);

    if (frame == 0) netplay_inputlog_open();
    if (g_netplay.inlog) {
        fprintf(g_netplay.inlog, "%u %03X %03X %08X\n", frame, g_netplay.inlog_w0, g_netplay.inlog_w1, check);
        if (frame % 60 == 59) fflush(g_netplay.inlog);
    }

    g_netplay.frame = frame + 1;

    if (versus_result >= 1 && versus_result <= 2 && !g_netplay.match_result_seen) {
        uint32_t winner = (uint32_t)versus_result - 1u;
        g_netplay.match_result_seen = true;
        netplay_log("match %u over at frame %u: %s (%s) won", (unsigned)g_netplay.match_started, frame,
                    netplay_member_name(g_netplay.room.fighter[winner]), winner == 0 ? "1P" : "2P");
        if (room_after_result(&g_netplay.me, g_netplay.session.my_member_id, &g_netplay.room,
                              g_netplay.match_started, winner))
            g_netplay.me_dirty = true;
        if (netplay_is_fighter()) g_netplay.linger_until_ms = net_now_ms() + NETPLAY_LINGER_MS;
        netplay_end_match(NULL);
    }
}

/* Called from the emu thread with the mutex held when netplay_begin_frame
 * answered RESET. Performs the board reset both peers do at the same instant. */
static inline void netplay_do_reset(void) {
    if (!g_netplay.reset_pending) return;
    g_netplay.reset_pending = false;
    if (g_netplay.reset_board) {
        g_netplay.reset_board(g_netplay.reset_ctx);
        input_reset();
        netplay_log("board reset; frame 0 of session %u", g_netplay.generation);
    } else {
        netplay_log("no board-reset hook - the two boards will not start from the same state");
    }
}

/*
 * The same reset with no session behind it, for whoever wants to know what the
 * barrier's reset does to a board that has been running: the MCP bridge's
 * `board_reset`, which tools/grade-reset.mjs holds against a first boot. Emu
 * thread, emu mutex held, like netplay_do_reset. False without a hook.
 */
static inline bool netplay_reset_board_now(void) {
    if (!g_netplay.reset_board) return false;
    g_netplay.reset_board(g_netplay.reset_ctx);
    input_reset();
    netplay_log("board reset on request (no session)");
    return true;
}

/* A session owns the board: fighting or watching. */
static inline bool netplay_active(void) {
    return g_netplay.enabled && netplay_running_match();
}

static inline void netplay_shutdown(void) {
    if (!g_netplay.mutex_ready) return;
    netplay_inputlog_close();
    rpcn_session_stop(&g_netplay.session);
    rpcn_account_reset(&g_netplay.account);
    netplay_release_inputs();
    g_netplay.enabled = false;
    g_netplay.mutex_ready = false;
    emu_mutex_destroy(&g_netplay.mutex);
}

#endif /* NETPLAY_H */
