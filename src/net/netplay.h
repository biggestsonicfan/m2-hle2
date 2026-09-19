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
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * The peer-to-peer protocol revision, published in the room attribute word and
 * checked on join. Bump for any change to the packet layout, the canonical input
 * word, or a lockstep rule both sides must agree on. Unlike the ComId revision
 * channel (com_id.h) this does not partition the room list — it lets a joiner be
 * TOLD why a room is not joinable instead of finding out at the barrier.
 */
#define NETPLAY_PROTO_REV 1

/* Room attribute word layout. Bits 28-31 are left alone: the server owns
 * SCE_NP_MATCHING2_ROOM_FLAG_ATTR_FULL (0x20000000) in there and rewrites it. */
#define NETPLAY_ROOM_REV_SHIFT    0
#define NETPLAY_ROOM_REV_MASK     0xFFu
#define NETPLAY_ROOM_DELAY_SHIFT  8
#define NETPLAY_ROOM_DELAY_MASK   0xFu
#define NETPLAY_ROOM_GAME_SHIFT   12
#define NETPLAY_ROOM_GAME_MASK    0xFFFFu

#define NETPLAY_LOG_LINES 64
#define NETPLAY_LOG_LEN   160
#define NETPLAY_CMD_QUEUE 8

typedef enum {
    NETPLAY_OFF,
    NETPLAY_CONNECTING,
    NETPLAY_ONLINE,       /* logged in, no room */
    NETPLAY_IN_ROOM,      /* room taken; waiting for a peer, or idling with one */
    NETPLAY_SYNCING,      /* barrier: both peers announcing the same generation */
    NETPLAY_PLAYING,      /* the board is being stepped in lockstep */
    NETPLAY_FAILED,
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
    NETPLAY_CMD_START,        /* begin (or restart) a lockstepped session */
    NETPLAY_CMD_STOP,         /* leave the match, stay in the room */
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
    char     room_password[16];
    uint64_t room_id;           /* join target */
    uint32_t frame_delay;
    bool     browse_yamp;       /* also search YAMP's lobby space, read-only */
    uint16_t local_p2p_port;    /* 0 = RPCN_P2P_PORT */
} netplay_config_t;

typedef struct {
    netplay_cmd_kind_t kind;
    netplay_config_t   cfg;
} netplay_cmd_t;

/* The snapshot the UI draws from, published under the netplay mutex. */
typedef struct {
    netplay_state_t state;
    rpcn_stage_t    stage;
    bool            is_host;
    int32_t         local_player;      /* -1 until a room is taken */
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

    int32_t           local_player;   /* 0 = host/P1, 1 = guest/P2, -1 = none */
    uint32_t          generation;
    uint32_t          seed;
    uint32_t          frame;          /* frames executed since the board reset */
    bool              reset_pending;  /* the barrier released; reset before frame 0 */
    bool              delay_seeded;

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

    /*
     * The last generation the peer announced, and when. Latched from EVERY
     * announce, including the ones `lockstep_on_peer_announce` throws away --
     * it drops anything that is not the round we are already in, which is
     * exactly the case that matters here: a peer who has pressed start while
     * we are still idling in the room announces a generation we have not begun,
     * so the barrier mask never sees it and nothing else in this file knows the
     * challenge happened.
     *
     * Freshness rather than a sticky flag, because a peer in SYNCING announces
     * once per slice and a peer who gave up stops. A latched bool would say
     * "somebody is waiting for you" for the rest of the room's life.
     */
    uint32_t          peer_ready_gen;
    uint64_t          peer_ready_ms;

    uint64_t          last_seed_ms;
    uint64_t          last_wait_report_ms;
    uint64_t          stall_since_ms;
    uint32_t          stall_timeout_ms;

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
} netplay_t;

static netplay_t g_netplay;

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
    uint32_t tag = (flags >> NETPLAY_ROOM_GAME_SHIFT) & NETPLAY_ROOM_GAME_MASK;
    if (tag != netplay_game_tag(p))
        return "that room is for a different game than the one loaded here";
    return NULL;
}

/* ---- Packets ------------------------------------------------------------- */

static inline uint32_t netplay_session_id(void) {
    return (uint32_t)g_netplay.session.room_id;
}

static inline void netplay_fill_header(lockstep_header_t *h, uint8_t type) {
    h->type       = type;
    h->player     = (uint8_t)(g_netplay.local_player < 0 ? 0 : g_netplay.local_player);
    h->generation = (uint8_t)(g_netplay.generation & 0x1F);
    h->reserved   = 0;
    h->session    = netplay_session_id();
}

static inline bool netplay_is_host(void) { return g_netplay.local_player == 0; }

/*
 * How long an announce counts for. A peer sitting at the barrier sends one per
 * slice -- sixty a second -- so anything above a few hundred milliseconds is
 * generous, and the window is what makes the answer clear itself when they give
 * up and walk away instead of leaving a challenge on screen forever.
 */
#define NETPLAY_READY_WINDOW_MS 2000u

/*
 * Is somebody waiting for us to accept? True only while we are idling in a room
 * we have not started a session from: once WE have started, the barrier owns the
 * question and `lockstep_barrier_released` is the one to ask.
 */
static inline bool netplay_peer_ready(void) {
    if (g_netplay.state != NETPLAY_IN_ROOM) return false;
    /* No peer in the room, no challenge. The npid is cleared by the room's
     * own leave notification, so this is also what retracts a challenge from
     * somebody who announced and then closed their emulator. */
    if (!g_netplay.session.peer_npid[0]) return false;
    if (!g_netplay.peer_ready_ms) return false;
    return net_now_ms() - g_netplay.peer_ready_ms <= NETPLAY_READY_WINDOW_MS;
}

/* Forget any standing challenge. Called wherever the answer has been given,
 * so an announce heard during the barrier cannot re-arm the moment a session
 * ends and start the next one without anybody asking for it. */
static inline void netplay_clear_ready(void) {
    g_netplay.peer_ready_ms  = 0;
    g_netplay.peer_ready_gen = 0;
}

static inline const char *netplay_state_text(netplay_state_t s) {
    switch (s) {
        case NETPLAY_OFF:        return "off";
        case NETPLAY_CONNECTING: return "connecting";
        case NETPLAY_ONLINE:     return "online";
        case NETPLAY_IN_ROOM:    return "in a room";
        case NETPLAY_SYNCING:    return "waiting at the barrier";
        case NETPLAY_PLAYING:    return "playing";
        case NETPLAY_FAILED:     return "failed";
        default:                 return "?";
    }
}

static inline void netplay_send_announce(void) {
    lockstep_announce_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    netplay_fill_header(&pkt.header, LOCKSTEP_PACKET_ANNOUNCE);
    pkt.seed = netplay_is_host() ? g_netplay.seed : 0;   /* only the host's is authoritative */
    rpcn_session_send(&g_netplay.session, &pkt, sizeof(pkt));
}

/*
 * Publish the session seed from the moment the room exists, not from the moment a
 * session starts. Until yampnet did this, the seed only ever travelled on an
 * announce, and announces only begin when a peer presses start — so whoever
 * pressed first decided whether the session could work. Cheap and unconditional
 * rather than clever: 12 bytes a few times a second while idling in a room.
 */
static inline void netplay_send_seed(void) {
    if (!netplay_is_host()) return;
    lockstep_announce_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    netplay_fill_header(&pkt.header, LOCKSTEP_PACKET_SEED);
    pkt.seed = g_netplay.seed;
    rpcn_session_send(&g_netplay.session, &pkt, sizeof(pkt));
}

static inline void netplay_begin_generation(uint32_t generation) {
    g_netplay.generation = generation & 0x1F;
    lockstep_configure(&g_netplay.lockstep, (uint32_t)(g_netplay.local_player < 0 ? 0 : g_netplay.local_player),
                       LOCKSTEP_MAX_PLAYERS,
                       g_netplay.cfg.frame_delay ? g_netplay.cfg.frame_delay : 2u);
    lockstep_begin_round(&g_netplay.lockstep, g_netplay.generation);
    netplay_check_clear();
    g_netplay.frame         = 0;
    g_netplay.delay_seeded  = false;
    g_netplay.reset_pending = false;
    g_netplay.stall_since_ms = 0;
    g_netplay.state = NETPLAY_SYNCING;
    g_netplay.last_wait_report_ms = net_now_ms();
    netplay_send_announce();
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
        rpcn_session_send(&g_netplay.session, &pkt, sizeof(pkt));
    }
    g_netplay.delay_seeded = true;
}

static inline void netplay_drain_socket(void) {
    uint8_t buf[sizeof(lockstep_input_packet_t) + 64];
    for (;;) {
        int got = rpcn_session_recv(&g_netplay.session, buf, (uint32_t)sizeof(buf));
        if (got <= 0) break;
        if (got < (int)sizeof(lockstep_header_t)) continue;

        const lockstep_header_t *hdr = (const lockstep_header_t *)buf;

        /* Not our room: a stale process from an earlier test, or another session
         * that happens to share this address pair. Dropping it here is what stops
         * it releasing our barrier or feeding our rings. Only filter once we know
         * our own room (0 = not yet). */
        uint32_t mine = netplay_session_id();
        if (mine != 0 && hdr->session != 0 && hdr->session != mine) continue;

        if (hdr->type == LOCKSTEP_PACKET_INPUT && got >= (int)sizeof(lockstep_input_packet_t)) {
            const lockstep_input_packet_t *pkt = (const lockstep_input_packet_t *)buf;
            lockstep_on_record(&g_netplay.lockstep, &pkt->record);
            if (pkt->check_frame != LOCKSTEP_NO_CHECK) {
                uint32_t idx = pkt->check_frame & LOCKSTEP_RING_MASK;
                g_netplay.peer_check_frame[idx] = pkt->check_frame;
                g_netplay.peer_check_value[idx] = pkt->check_value;
                netplay_check_compare(pkt->check_frame);
            }
        } else if (hdr->type == LOCKSTEP_PACKET_ANNOUNCE
                   && got >= (int)sizeof(lockstep_announce_packet_t)) {
            const lockstep_announce_packet_t *ann = (const lockstep_announce_packet_t *)buf;

            /* THE HOST OWNS THE GENERATION, exactly as it owns the seed. Both
             * peers must compute the same one or the barrier never releases, and
             * neither knows how many sessions the other has played — so a guest
             * simply adopts whatever the host announces. */
            if (!netplay_is_host() && hdr->player == 0 && g_netplay.state == NETPLAY_SYNCING
                && hdr->generation != (uint8_t)(g_netplay.generation & 0x1F)) {
                netplay_log("adopting the host's session %u", hdr->generation);
                netplay_begin_generation(hdr->generation);
            }

            /* Before the barrier gets a say: `lockstep_on_peer_announce` keeps
             * only announces for the round we are already in, and a challenge
             * arrives as an announce for a round we are NOT in. Latch it here,
             * where every announce still exists. */
            if ((int32_t)hdr->player != g_netplay.local_player) {
                g_netplay.peer_ready_gen = hdr->generation;
                g_netplay.peer_ready_ms  = net_now_ms();
            }

            lockstep_on_peer_announce(&g_netplay.lockstep, hdr->player, hdr->generation);
            if (!netplay_is_host() && hdr->player == 0) g_netplay.seed = ann->seed;
        } else if (hdr->type == LOCKSTEP_PACKET_SEED
                   && got >= (int)sizeof(lockstep_announce_packet_t)) {
            /* Seed only. Pointedly does NOT feed the barrier: this arrives while
             * the host is merely sitting in the room, and treating it as an
             * announcement would release a barrier for a session the guest has
             * not begun. */
            const lockstep_announce_packet_t *ann = (const lockstep_announce_packet_t *)buf;
            if (!netplay_is_host() && hdr->player == 0 && ann->seed != 0
                && g_netplay.seed != ann->seed) {
                g_netplay.seed = ann->seed;
                netplay_log("adopted the host's session seed 0x%08X", ann->seed);
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
 * Beside the log, in the working directory. The point of it is the Twitch login
 * token: the device flow is deliberately a once-ever thing, and a client that
 * forgot the token it was given would send the player back to a browser at every
 * launch, which is the exact outcome the token exists to prevent.
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

static inline void netplay_settings_save(void) {
    FILE *f = fopen(NETPLAY_CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "# m2-hle2 netplay settings. Delete this file to forget them.\n");
    if (g_netplay.cfg.password[0])
        fprintf(f, "# This file holds a password in clear text.\n");
    fprintf(f, "server=%s\n",       g_netplay.cfg.server);
    fprintf(f, "port=%u\n",         (unsigned)g_netplay.cfg.port);
    fprintf(f, "npid=%s\n",         g_netplay.cfg.npid);
    fprintf(f, "fingerprint=%s\n",  g_netplay.cfg.fingerprint);
    fprintf(f, "frame_delay=%u\n",  g_netplay.cfg.frame_delay);
    fprintf(f, "browse_yamp=%d\n",  g_netplay.cfg.browse_yamp ? 1 : 0);
    /* Only when there is one: an empty `password=` in the file would claim a
     * stored credential that does not exist, and a Twitch login clears the
     * field precisely so that nothing is kept. The same for the e-mail token,
     * which is the netplay window's "Token" box -- RPCN checks it at Login
     * for a password account, so a host that forgot it could not sign back in
     * unattended however well it remembered the password. */
    if (g_netplay.cfg.password[0])
        fprintf(f, "password=%s\n", g_netplay.cfg.password);
    if (g_netplay.cfg.token[0])
        fprintf(f, "token=%s\n",    g_netplay.cfg.token);
    fprintf(f, "twitch_token=%s\n", g_netplay.cfg.twitch_token);
    fclose(f);
}

static inline void netplay_settings_load(netplay_config_t *cfg) {
    FILE *f = fopen(NETPLAY_CFG_PATH, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        size_t n = strlen(val);
        while (n && (val[n - 1] == '\n' || val[n - 1] == '\r')) val[--n] = '\0';

        if      (!strcmp(key, "server"))       snprintf(cfg->server, sizeof(cfg->server), "%s", val);
        else if (!strcmp(key, "port"))         cfg->port = (uint16_t)atoi(val);
        else if (!strcmp(key, "npid"))         snprintf(cfg->npid, sizeof(cfg->npid), "%s", val);
        else if (!strcmp(key, "fingerprint"))  snprintf(cfg->fingerprint, sizeof(cfg->fingerprint), "%s", val);
        else if (!strcmp(key, "frame_delay"))  cfg->frame_delay = (uint32_t)atoi(val);
        else if (!strcmp(key, "browse_yamp"))  cfg->browse_yamp = atoi(val) != 0;
        else if (!strcmp(key, "password"))     snprintf(cfg->password, sizeof(cfg->password), "%s", val);
        else if (!strcmp(key, "token"))        snprintf(cfg->token, sizeof(cfg->token), "%s", val);
        else if (!strcmp(key, "twitch_token")) snprintf(cfg->twitch_token, sizeof(cfg->twitch_token), "%s", val);
    }
    fclose(f);
}

/* ---- Command queue (UI thread -> emu thread) ----------------------------- */

static inline void netplay_init(void) {
    memset(&g_netplay, 0, sizeof(g_netplay));
    g_netplay.local_player     = -1;
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
    netplay_settings_load(&g_netplay.cfg);
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
    st->state        = g_netplay.state;
    st->stage        = g_netplay.session.stage;
    st->is_host      = netplay_is_host();
    st->local_player = g_netplay.local_player;
    st->room_id      = g_netplay.session.room_id;
    st->room_flags   = g_netplay.session.room_flags;
    snprintf(st->com_id, sizeof(st->com_id), "%s", g_netplay.session.com_id);
    snprintf(st->com_id_foreign, sizeof(st->com_id_foreign), "%s", g_netplay.session.com_id_foreign);
    snprintf(st->peer_npid, sizeof(st->peer_npid), "%s", g_netplay.session.peer_npid);
    snprintf(st->peer_addr, sizeof(st->peer_addr), "%s", rpcn_session_peer_text(&g_netplay.session));
    st->peer_known   = rpcn_session_peer_known(&g_netplay.session);
    st->peer_heard   = g_netplay.session.peer_heard;
    st->frame        = g_netplay.frame;
    st->stalls       = g_netplay.lockstep.stalls;
    st->desync_frame = g_netplay.desync_frame;
    st->generation   = g_netplay.generation;
    st->seed         = g_netplay.seed;
    st->peer_ready     = netplay_peer_ready();
    st->peer_ready_gen = g_netplay.peer_ready_gen;

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
    snprintf(st->twitch_npid, sizeof(st->twitch_npid), "%s", g_netplay.cfg.npid);
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

    g_netplay.cfg = *cfg;
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
     * type, and their account's real one is random and never disclosed. */
    sc.password        = g_netplay.cfg.twitch_token[0] ? g_netplay.cfg.twitch_token
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

static inline void netplay_do_disconnect(void) {
    rpcn_session_stop(&g_netplay.session);
    g_netplay.enabled      = false;
    g_netplay.state        = NETPLAY_OFF;
    g_netplay.local_player = -1;
    g_netplay.frame        = 0;
    netplay_check_clear();
    netplay_clear_ready();
    netplay_release_inputs();
    netplay_log("disconnected");
}

static inline void netplay_do_host(const netplay_config_t *cfg) {
    g_netplay.cfg.frame_delay   = cfg->frame_delay;
    g_netplay.cfg.room_password[0] = '\0';
    snprintf(g_netplay.cfg.room_password, sizeof(g_netplay.cfg.room_password), "%s",
             cfg->room_password);
    uint32_t flags = netplay_room_flags(g_active_profile,
                                        cfg->frame_delay ? cfg->frame_delay : 2u);
    if (!rpcn_session_host(&g_netplay.session, 2,
                           cfg->room_password[0] ? cfg->room_password : NULL, flags)) {
        g_netplay.state = NETPLAY_FAILED;
        return;
    }
    /* The host is always player 0. That is not a convention we are free to pick
     * per machine: it decides which side of the cabinet each board drives, and
     * both ends have to answer it the same way with nothing to negotiate it. */
    g_netplay.local_player = 0;
    /* One nonce per room, from the clock. It is not load-bearing for determinism
     * (see lockstep.h) but it is what proves in a log that two machines think
     * they are in the same session. */
    g_netplay.seed = (uint32_t)(net_now_ms() * 2654435761u) | 1u;
    netplay_log("hosting; waiting for a peer");
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
    if (!rpcn_session_join(&g_netplay.session, cfg->room_id,
                           cfg->room_password[0] ? cfg->room_password : NULL)) {
        g_netplay.state = NETPLAY_FAILED;
        return;
    }
    g_netplay.local_player = 1;
    netplay_log("joining room %llu", (unsigned long long)cfg->room_id);
}

static inline void netplay_do_start(void) {
    if (g_netplay.local_player < 0 || !g_netplay.session.room_id) {
        netplay_log("take a room before starting a session");
        return;
    }
    /* The host picks the generation; a guest pressing start announces its own and
     * adopts the host's the moment one arrives (see netplay_drain_socket). */
    uint32_t gen = (g_netplay.generation + 1) & 0x1F;
    netplay_clear_ready();
    netplay_begin_generation(gen);
    netplay_log("session %u: waiting for both boards at the barrier", gen);
}

static inline void netplay_do_stop(void) {
    if (g_netplay.state == NETPLAY_PLAYING || g_netplay.state == NETPLAY_SYNCING)
        g_netplay.state = NETPLAY_IN_ROOM;
    g_netplay.reset_pending = false;
    netplay_clear_ready();
    netplay_release_inputs();
    netplay_log("session stopped");
}

static inline void netplay_pump_commands(void) {
    netplay_cmd_t cmd;
    while (netplay_take_cmd(&cmd)) {
        switch (cmd.kind) {
            case NETPLAY_CMD_CONNECT:    netplay_do_connect(&cmd.cfg); break;
            case NETPLAY_CMD_DISCONNECT: netplay_do_disconnect(); break;
            case NETPLAY_CMD_HOST:       netplay_do_host(&cmd.cfg); break;
            case NETPLAY_CMD_JOIN:       netplay_do_join(&cmd.cfg); break;
            case NETPLAY_CMD_SEARCH:
                rpcn_session_search(&g_netplay.session, cmd.cfg.browse_yamp);
                break;
            case NETPLAY_CMD_START:      netplay_do_start(); break;
            case NETPLAY_CMD_STOP:       netplay_do_stop(); break;
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
    /* The session's own progress becomes our state, except while a session is
     * running: SYNCING/PLAYING are owned by the barrier, not by the transport. */
    if (g_netplay.state == NETPLAY_SYNCING || g_netplay.state == NETPLAY_PLAYING) return;
    switch (g_netplay.session.stage) {
        case RPCN_STAGE_LOGGING_IN: g_netplay.state = NETPLAY_CONNECTING; break;
        case RPCN_STAGE_ONLINE:
            /* Remember the server and account only once they are known to WORK —
             * storing what was typed would just as happily store a typo. */
            if (g_netplay.state != NETPLAY_ONLINE) netplay_settings_save();
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
        case RPCN_STAGE_FAILED:     g_netplay.state = NETPLAY_FAILED; break;
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

    if (!rpcn_session_peer_known(&g_netplay.session))
        netplay_log("still waiting: the server has not given us the peer's address yet");
    else if (!g_netplay.session.peer_heard)
        netplay_log("still waiting: punching %s but nothing has come back - check that UDP %u "
                    "is not blocked by a firewall on either machine",
                    rpcn_session_peer_text(&g_netplay.session), (unsigned)RPCN_P2P_PORT);
    else
        netplay_log("still waiting: peer %s is reachable but has not announced this session",
                    rpcn_session_peer_text(&g_netplay.session));
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
 * Called once per slice from the emu thread, OUTSIDE the emu mutex. Pumps the
 * network, then answers what this slice may do.
 */
static inline netplay_step_t netplay_begin_frame(void) {
    if (!g_netplay.mutex_ready) return NETPLAY_STEP_OFF;

    netplay_pump_commands();
    rpcn_account_update(&g_netplay.account);
    netplay_pump_twitch();

    if (!g_netplay.enabled) { netplay_publish_status(); return NETPLAY_STEP_OFF; }

    rpcn_session_update(&g_netplay.session);
    netplay_mirror_stage();
    netplay_drain_socket();

    /* The host republishes the seed while idling in a room, so a guest holds it
     * long before anyone presses start. */
    if (g_netplay.state == NETPLAY_IN_ROOM && netplay_is_host()) {
        uint64_t now = net_now_ms();
        if (now - g_netplay.last_seed_ms >= 1000) { g_netplay.last_seed_ms = now; netplay_send_seed(); }
    }

    if (g_netplay.state == NETPLAY_SYNCING) {
        /* Keep announcing until the barrier releases: the announce is a plain
         * datagram and may be lost, and it is also how the host's generation and
         * seed reach a guest that pressed start first. */
        netplay_send_announce();
        netplay_report_wait();

        if (lockstep_barrier_released(&g_netplay.lockstep)) {
            netplay_seed_delay_frames();
            g_netplay.reset_pending = true;
            g_netplay.state         = NETPLAY_PLAYING;
            g_netplay.frame         = 0;
            netplay_log("barrier released; session %u seed 0x%08X - resetting the board",
                        g_netplay.generation, g_netplay.seed);
            netplay_publish_status();
            return NETPLAY_STEP_RESET;
        }
        netplay_publish_status();
        return NETPLAY_STEP_WAIT;
    }

    if (g_netplay.state != NETPLAY_PLAYING) {
        netplay_publish_status();
        /* Not in a session: the board runs normally and the keyboard drives it. */
        return NETPLAY_STEP_OFF;
    }

    if (g_netplay.reset_pending) { netplay_publish_status(); return NETPLAY_STEP_RESET; }

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
        rpcn_session_send(&g_netplay.session, &pkt, sizeof(pkt));
    }

    if (!lockstep_ready(&g_netplay.lockstep, frame)) {
        uint64_t now = net_now_ms();
        if (g_netplay.stall_since_ms == 0) {
            g_netplay.stall_since_ms = now;
            g_netplay.lockstep.stalls++;
        } else if (g_netplay.stall_timeout_ms
                   && now - g_netplay.stall_since_ms > g_netplay.stall_timeout_ms) {
            netplay_log("stalled for more than %u ms at frame %u - leaving the session",
                        g_netplay.stall_timeout_ms, frame);
            g_netplay.state = NETPLAY_IN_ROOM;
            g_netplay.stall_since_ms = 0;
            netplay_release_inputs();
        }
        netplay_publish_status();
        return NETPLAY_STEP_WAIT;
    }
    g_netplay.stall_since_ms = 0;

    netplay_apply_inputs(lockstep_input_for(&g_netplay.lockstep, 0, frame),
                         lockstep_input_for(&g_netplay.lockstep, 1, frame));
    netplay_publish_status();
    return NETPLAY_STEP_READY;
}

/*
 * Called from the emu thread with the mutex held, right after a slice that ended
 * on a frame boundary. Records this frame's check for the next outgoing packet
 * and advances the netplay frame counter.
 */
static inline void netplay_end_frame(const i960_cpu_t *cpu, uint64_t total_steps) {
    if (!g_netplay.enabled || g_netplay.state != NETPLAY_PLAYING) return;

    uint32_t frame = g_netplay.frame;
    uint32_t check = netplay_frame_check(cpu, total_steps);
    uint32_t idx   = frame & LOCKSTEP_RING_MASK;
    g_netplay.check_frame[idx] = frame;
    g_netplay.check_value[idx] = check;
    g_netplay.last_check_frame = frame;
    g_netplay.last_check_value = check;
    netplay_check_compare(frame);

    g_netplay.frame = frame + 1;
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

static inline bool netplay_active(void) {
    return g_netplay.enabled && g_netplay.state == NETPLAY_PLAYING;
}

static inline void netplay_shutdown(void) {
    if (!g_netplay.mutex_ready) return;
    rpcn_session_stop(&g_netplay.session);
    rpcn_account_reset(&g_netplay.account);
    netplay_release_inputs();
    g_netplay.enabled = false;
    g_netplay.mutex_ready = false;
    emu_mutex_destroy(&g_netplay.mutex);
}

#endif /* NETPLAY_H */
