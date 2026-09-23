/*
 * ps3_link.h — playing the PS3 port of Sonic the Fighters (NPUB30927, in RPCS3)
 * over RPCN, as the PS3 game itself does it.
 *
 * m2-hle2's own netplay (lockstep.h, room.h) is ours to define; this is not. A
 * PS3 in RPCS3 only accepts a peer that behaves exactly like another PS3, so
 * every rule here was read out of the game (its EBOOT, in the Ghidra database
 * the ROOM-MATCH.md notes come from) and checked against a real PS3-vs-PS3 match
 * captured through RPCS3's packet log. Names in parentheses are the PS3
 * functions a rule was ported from.
 *
 * It is used only against the official RPCN server (netplay.h decides), where
 * the PS3 players are. The community server keeps m2-hle2's own protocol.
 *
 * ── THE STACK ──────────────────────────────────────────────────────────────
 *
 *   room        RPCN, in the PS3's lobby space (NPWR03869_00) with its room and
 *               member attributes (below)
 *   signaling   RPCS3's own (rpcs3_signal.h), which is what tells the PS3 game a
 *               member is reachable
 *   transport   Sony's RUDP (rudp.h): channel 1 reliable, 2 and 3 unreliable
 *   messages    the game's five message types (below)
 *   lockstep    the game's TaskSyncIo, ported state for state (ps3_sio_*)
 *   board       three STF hooks (sfight.h, g_xplay_*): forced START in attract,
 *               the barrier at character select, the end of the match
 *
 * WHAT THIS DOES NOT DO YET: make our board compute what the PS3's does. The PS3
 * build does not emulate a Model 2 the way this emulator does (its coprocessor
 * is single-precision C, its `rand` is an MT19937, it has no sound CPU; see the
 * PS3 internals notes), so the two boards drift apart from the same inputs. The
 * wire protocol and the frame structure here are the PS3's exactly; the board
 * is still ours.
 *
 * ── ROLES ──────────────────────────────────────────────────────────────────
 *
 * Either end of a room. As a MEMBER this end joins a room a PS3 created and
 * follows the phase the owner writes. As the OWNER (ps3_owner_pump) it creates
 * the room the way a PS3 does and runs the owner's half too: the phase machine
 * (np_session_update_room_phase 0xBF258), choosing the fighters from the
 * waiting line (np_session_build_fight_entries 0xBEBD0), keeping each fighter's
 * entrant data, the seed and match flags (np_session_seed_match 0xBD7B8), and
 * relaying messages between members (np_rudp_dispatch_message 0xB7504). The
 * order of room writes was checked against an RPCS3 log of a PS3 owning one.
 *
 * A member not chosen to fight waits in the room; unlike a PS3 it does not
 * watch the match, and takes the new order in line from what the fighters
 * publish after it.
 *
 * ── THE ROOM (np_session_*) ────────────────────────────────────────────────
 *
 * Room internal attribute 0x57, 0xE8 bytes, big-endian, written by the owner:
 *   0x00 u32 seed   0x04 u8 room mode   0x05 u8 match flags   0x08 8 rule bytes
 *   0x10 u32 PHASE  0x14 u32 fighter count   0x18 u16[2] fighters (1P, 2P)
 *   0x1C u32 max round trip (ms)   0x20 / 0x84 fighter slots (u32 flags + 96 B)
 * The match flags change the lockstep (SyncIo_Init_rings 0x6E67C): 0x40 = the
 * room holds more than the two fighters, 0x80 = a fighter has no direct link
 * to the other, whose inputs then go through the owner.
 * Member internal attribute 0x59, 0x20 bytes, each member's own:
 *   0x00 u32 flags: bit 31 ready, 30 in the match, 29 line rotated after it
 *   0x04 u32 battle points (0xFFFFFFFF: none yet) ... 0x1C u8 entry request
 *   0x1D (a pad byte; ours, PS3_ME_NO_RESULT: we set bit 29 without a result)
 * The member's teamId is its place in the waiting line (0xFF when new).
 *
 * The phases, as a member sees them (captured):
 *   0 lobby -> 1 choosing -> 2 preparing: each fighter sets flags bit 31 and
 *   sends its 100-byte entrant data to the owner (message 2) -> 3 match: each
 *   member sets bit 30 and the boards start -> after the result each member
 *   publishes its new place in line and bit 29 -> 4 results: flags back to 0
 *   -> 0 again.
 *
 * ── MESSAGES (np_msg_serialize, CMessage_*) ────────────────────────────────
 *
 *   header: u8 type, u8 dest (0xFF = the owner relays it to everyone), u32 flags
 *   0 SyncIo       u8 slot, u16 len, input packet, u16 member, u16 0   (ch 2)
 *   1 SyncIoTcp    u8 slot, u16 len, 68-byte input packet              (ch 1)
 *   2 UpdSetting   u8 side, 100 bytes of entrant data                  (ch 1)
 *   3 SyncStart    u8 generation, u8 side, u16 0, u16 sender member    (ch 1)
 *   4 RespSyncStart u16 target member                                  (ch 1)
 *
 *   Input packets (SyncIo_SampleLocalAndSend):
 *     small, every frame: u32 frame f, u8 (side << 5 | generation), u8 0,
 *       10 input bytes for frames f, f-1, ..., f-9
 *     big, every 60th frame: u32 f, u8 side/generation, u8 delay (bit 7 = new
 *       delay in bits 6..3, 1P only), u16 0, 60 input bytes for f .. f-59
 *   An input byte: 0x01 B3, 0x02 B1, 0x04 B2, 0x08 START, 0x10 UP, 0x20 DOWN,
 *   0x40 LEFT, 0x80 RIGHT.
 */
#ifndef PS3_LINK_H
#define PS3_LINK_H

#include "hle_hooks.h"
#include "rpcn_session.h"
#include "rpcs3_signal.h"
#include "rudp.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PS3_ROOM_BIN_SIZE    0xE8u
#define PS3_MEMBER_BIN_SIZE  0x20u
#define PS3_ENTRANT_SIZE     100u
#define PS3_RING             1024u
#define PS3_EMPTY_FRAME      (-1)
#define PS3_EMPTY_INPUT      0xFFFFu
#define PS3_TICK_US          16667u
#define PS3_RESULTS_US       8000000u

#define PS3_MFLAG_READY      0x80000000u
#define PS3_MFLAG_IN_MATCH   0x40000000u
#define PS3_MFLAG_ROTATED    0x20000000u
/* Ours, in the member attribute's first pad byte after the entry request,
 * which a PS3 leaves 0: this member marked itself done (bit 29) without
 * seeing a result, so its place and entry say nothing about who won. */
#define PS3_ME_NO_RESULT     0x1Du

/* Room blob byte 5, the match flags (np_session_seed_match). */
#define PS3_MATCH_SPECTATORS 0x40u
#define PS3_MATCH_RELAY      0x80u
/* A fighter slot's flags word (blob +0x20 / +0x84). */
#define PS3_SLOT_FILLED      0x80000000u
#define PS3_SLOT_NO_LINK     0x40000000u

/* Room flags the owner sets and clears (SetRoomDataInternal's flagFilter). */
#define PS3_ROOM_CLOSED_HIDDEN 0x50000000u

/* The owner's clocks. The PS3 counts frames: 1800 (0x708, vtable+0x4C) in the
 * lobby, and 300 once three are in a Room Match (Lobby_ReadyCountdownFrames). */
#define PS3_LOBBY_US         30000000u
#define PS3_LOBBY_CROWD_US    5000000u
#define PS3_PREPARE_US       30000000u   /* ours: a fighter that never gets ready */
#define PS3_LINK_WAIT_US      5000000u   /* ours: before owning up to no link */
#define PS3_REWRITE_US        3000000u   /* ours: a room write the server never echoed */
#define PS3_MATCH_MAX_US    900000000u   /* ours: a match nobody reported the end of */
#define PS3_NO_RESULT_US      3000000u   /* ours: before owning up to no result */

enum { PS3_MSG_SYNCIO = 0, PS3_MSG_SYNCIO_TCP = 1, PS3_MSG_UPDATE_SETTING = 2,
       PS3_MSG_SYNC_START = 3, PS3_MSG_RESPONSE_SYNC_START = 4 };

enum { PS3_PHASE_LOBBY = 0, PS3_PHASE_CHOOSING = 1, PS3_PHASE_PREPARING = 2,
       PS3_PHASE_MATCH = 3, PS3_PHASE_RESULTS = 4 };

/* What the board may do this slice (the same three netplay.h answers with). */
typedef enum { PS3_STEP_OFF, PS3_STEP_READY, PS3_STEP_WAIT } ps3_step_t;

/*
 * TaskSyncIo (vtable 0x3e4220), field for field. The PS3 offsets are given
 * because every rule below refers to them.
 */
typedef struct {
    bool     started;        /* the task is registered for a match (Start_side_nplayers) */
    uint32_t nplayers;       /* +0x50, always 2 */
    uint32_t side;           /* +0x54: 0 = 1P, 1 = 2P, 2+ = watching */
    uint8_t  gen;            /* +0x58 (5 bits): our generation */
    uint8_t  rgen;           /* +0x59: the generation both sides have started */
    bool     gen_ok;         /* +0x4C */
    bool     send_ss;        /* +0x5A: send SyncStart at the next tick */
    bool     resp_done;      /* +0x5B: every fighter has answered our SyncStart */
    bool     passed;         /* +0x5C: the board is past the barrier */
    bool     barrier_hit;    /* +0x5D: released this frame, passed from the next */
    bool     sync_active;    /* +0x5E */
    bool     finished;       /* +0x5F */
    int32_t  ring_frame[2][2][PS3_RING];   /* [side][gen & 1][frame & 1023] */
    uint16_t ring_input[2][2][PS3_RING];
    int32_t  sample;         /* +0x84: next local frame to sample */
    int32_t  newest;         /* +0x88: newest remote frame */
    int32_t  play;           /* +0x8C: next frame the board plays; -1 = not started */
    int32_t  delay;          /* +0x90 */
    int32_t  init_delay;     /* +0x94: the first frame played */
    int32_t  holdoff_reset;  /* +0x98 */
    int32_t  holdoff;        /* +0x9C */
    uint32_t got_mask;       /* +0xA0: sides whose SyncStart we have */
    uint32_t need_mask;      /* +0xA4 */
    uint32_t resp_count;     /* +0xA8 */
    bool     speed;          /* +0xAC: 1.0 or 0.0 */
    bool     stall;          /* +0xB0 */
    int32_t  stall_count;    /* +0xB4 */
    int32_t  small_every;    /* +0xB8 */
    int32_t  big_every;      /* +0xBC */
    int32_t  sync_timeout;   /* +0xC0 */
    int32_t  resend_ss;      /* +0xC4 */
    bool     ss_echoed;      /* ours, sent again on hearing theirs (not the PS3's) */
    bool     spectators;     /* room match flag 0x40 (session 0x400000) */
    bool     relay;          /* room match flag 0x80 (session 0x800000) */
} ps3_sio_t;

struct ps3_link_s;

typedef struct {
    struct ps3_link_s *link;
    bool     used;
    uint16_t member_id;
    char     npid[20];
    bool     rudp_up;        /* rudp initialised (signaling had linked) */
    rudp_peer_t rudp;
} ps3_peer_t;

typedef void (*ps3_log_fn)(void *ctx, const char *msg);

typedef struct ps3_link_s {
    bool            active;          /* a PS3-mode session exists */
    rpcn_session_t *session;
    rpcs3_sig_t     sig;
    ps3_peer_t      peers[RPCN_MAX_PEERS];

    /* The room, as the owner last wrote it. */
    uint32_t room_rev_seen;
    bool     room_known;
    uint32_t phase;
    uint32_t seed;
    uint32_t fighter_count;
    uint16_t fighters[2];
    uint32_t room_rtt_ms;
    uint8_t  match_flags;      /* blob +0x05 */

    /* Us. */
    uint8_t  me[PS3_MEMBER_BIN_SIZE];
    uint8_t  team;
    bool     me_dirty;
    int32_t  my_side;          /* index in the fighters, -1 = not fighting */
    bool     entrant_sent;
    uint64_t results_since_us;
    bool     results_cleared;
    bool     rotated;          /* our place in line is taken for this match */
    uint64_t seen_since_us;    /* when the room's phase last changed, as we read it */

    /* The owner's half (ps3_owner_pump), when this end owns the room. `blob` is
     * the room as we last wrote it; the phase machine steps only once the
     * server has echoed that write back, as a PS3 owner's does. */
    bool     hosting;
    uint8_t  blob[PS3_ROOM_BIN_SIZE];
    uint32_t max_slot;
    uint64_t phase_since_us;
    uint64_t lobby_until_us;
    uint64_t last_write_us;
    uint32_t write_filter, write_attr;   /* the flags that went with it */
    uint32_t rewrites;         /* times the last write went again unechoed */
    bool     crowd;            /* the lobby countdown was cut short for three */
    bool     own_entrant;      /* our own fighter slot is filled */
    bool     match_seen;       /* we fought, and our board's match has begun */

    /* The match on our board. */
    bool     match;            /* between phase 3 and the end of the match */
    bool     need_reset;       /* the board was not in attract: reboot it first */
    bool     owner_gone;       /* the room's owner left a room we joined: leave too */
    uint8_t  rules_blob[4];    /* room blob +0x09..+0x0C, what the rules came from */
    uint16_t last_result;      /* 1 = 1P won, 2 = 2P won, 0 = none yet */
    bool     frame_live;       /* inputs for `sio.play` were applied this frame */
    uint8_t  cur_in[2];        /* this frame's two input bytes */
    uint64_t next_tick_us;
    uint32_t stalled_frames;

    ps3_sio_t sio;

    ps3_log_fn log;
    void      *log_ctx;
} ps3_link_t;

/*
 * The wire log (--net-ps3-wire): every datagram this end sends or receives in
 * PS3 mode, and every line the link logs, with a timestamp. It is written in
 * the shape of RPCS3's own sys_net_dump log (the game's payload without RPCS3's
 * 6-byte P2P header, "ip:port:vport"), so tools/ps3-audit.py can match what one
 * machine sent against what the other received and lay both logs on one clock.
 *
 *   # m2hle ps3 wire log; start <local time, as RPCS3's "Current Time:">
 *   <seconds since start> TX|RX <ip>:<port>:<vport> <hex bytes>
 *   <seconds since start> EV <text>
 *
 * Vport-0 datagrams (RPCS3 signaling, RPCN's UDP pong) are logged whole.
 */
static FILE    *g_ps3_wire;
static uint64_t g_ps3_wire_t0;

static inline void ps3_wire_close(void) {
    if (g_ps3_wire) fclose(g_ps3_wire);
    g_ps3_wire = NULL;
}

static inline bool ps3_wire_open(const char *path) {
    ps3_wire_close();
    if (!path || !path[0]) return false;
    g_ps3_wire = fopen(path, "w");
    if (!g_ps3_wire) return false;
    g_ps3_wire_t0 = net_now_us();
    time_t now = time(NULL);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    fprintf(g_ps3_wire, "# m2hle ps3 wire log; start %04d-%02d-%02dT%02d:%02d:%02d\n",
            lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    return true;
}

static inline void ps3_wire_packet(const char *dir, uint32_t ip, uint16_t port, const uint8_t *buf, uint32_t len) {
    if (!g_ps3_wire || !buf) return;
    char addr[32];
    net_addr_text(addr, sizeof(addr), ip, port);
    uint16_t vport = len >= 2 ? (uint16_t)(buf[0] | (buf[1] << 8)) : 0;
    uint32_t skip = (vport != 0 && len >= 6) ? 6u : 0u;
    fprintf(g_ps3_wire, "%.6f %s %s:%u", (double)(net_now_us() - g_ps3_wire_t0) / 1e6, dir, addr, vport);
    for (uint32_t i = skip; i < len; i++) fprintf(g_ps3_wire, " %02X", buf[i]);
    fputc('\n', g_ps3_wire);
}

static inline void ps3_wire_event(const char *text) {
    if (!g_ps3_wire) return;
    fprintf(g_ps3_wire, "%.6f EV %s\n", (double)(net_now_us() - g_ps3_wire_t0) / 1e6, text);
    fflush(g_ps3_wire);
}

static inline void ps3_note(ps3_link_t *L, const char *fmt, ...) {
    char buf[240];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ps3_wire_event(buf);
    if (L->log) L->log(L->log_ctx, buf);
}

/* Signaling's own lines go the same way. */
static inline void ps3_sig_log_cb(void *ctx, const char *msg) {
    ps3_link_t *L = (ps3_link_t *)ctx;
    ps3_wire_event(msg);
    if (L->log) L->log(L->log_ctx, msg);
}

static inline uint32_t ps3_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint16_t ps3_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline void ps3_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline void ps3_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* ---- Input bytes --------------------------------------------------------- */

/* netplay.h's canonical word (up, down, left, right, B1..B4, start, coin, ...)
 * to the PS3's wire byte, and back (SyncIo_SampleLocalAndSend / InjectFrameInputs). */
static inline uint8_t ps3_wire_from_canonical(uint32_t w) {
    uint8_t b = 0;
    if (w & (1u << 6)) b |= 0x01;   /* B3 */
    if (w & (1u << 4)) b |= 0x02;   /* B1 */
    if (w & (1u << 5)) b |= 0x04;   /* B2 */
    if (w & (1u << 8)) b |= 0x08;   /* START */
    if (w & (1u << 0)) b |= 0x10;   /* UP */
    if (w & (1u << 1)) b |= 0x20;   /* DOWN */
    if (w & (1u << 2)) b |= 0x40;   /* LEFT */
    if (w & (1u << 3)) b |= 0x80;   /* RIGHT */
    return b;
}

static inline uint32_t ps3_canonical_from_wire(uint8_t b) {
    uint32_t w = 0;
    if (b & 0x01) w |= 1u << 6;
    if (b & 0x02) w |= 1u << 4;
    if (b & 0x04) w |= 1u << 5;
    if (b & 0x08) w |= 1u << 8;
    if (b & 0x10) w |= 1u << 0;
    if (b & 0x20) w |= 1u << 1;
    if (b & 0x40) w |= 1u << 2;
    if (b & 0x80) w |= 1u << 3;
    return w;
}

/* Delay_FromPingMs (0x644f8), in frames. */
static inline int32_t ps3_delay_from_ping_ms(uint32_t ms) {
    if (ms < 80) return (int32_t)((float)ms * 0.03f + 2.2f);
    return (int32_t)((float)(ms - 80) * 0.0025f + (float)ms * 0.03f + 2.6f);
}

static inline int32_t ps3_clamp_delay(int32_t d) { return d < 2 ? 2 : d > 10 ? 10 : d; }

/* ---- Peers --------------------------------------------------------------- */

static inline ps3_peer_t *ps3_peer_by_id(ps3_link_t *L, uint16_t id) {
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++)
        if (L->peers[i].used && L->peers[i].member_id == id) return &L->peers[i];
    return NULL;
}

static inline bool ps3_link_send_udp(void *ctx, uint32_t ip, uint16_t port, const void *buf, uint32_t len) {
    rpcn_session_t *s = (rpcn_session_t *)ctx;
    ps3_wire_packet("TX", ip, port, (const uint8_t *)buf, len);
    return rpcn_send_to(&s->client, ip, port, buf, len);
}

static inline bool ps3_is_owner(const ps3_link_t *L) {
    return L->session->my_member_id && L->session->my_member_id == L->session->owner_id;
}

/* ---- TaskSyncIo ---------------------------------------------------------- */

static inline void ps3_sio_clear_ring(ps3_sio_t *S, uint32_t side, uint32_t parity) {
    for (uint32_t i = 0; i < PS3_RING; i++) {
        S->ring_frame[side][parity][i] = PS3_EMPTY_FRAME;
        S->ring_input[side][parity][i] = PS3_EMPTY_INPUT;
    }
}

/* The reset every path shares: +0x8C = -1, speed 0, counters to zero. */
static inline void ps3_sio_reset_counters(ps3_sio_t *S) {
    S->play = -1;
    S->speed = false;
    S->stall = false;
    S->stall_count = 0;
    S->sample = 0;
    S->newest = -1;
}

/* SyncIo_Start_side_nplayers + SyncIo_Init_rings, at match setup. The first
 * frame played is the room's delay, the same on every machine because it comes
 * from the round trip the owner wrote into the room. */
static inline void ps3_sio_start(ps3_link_t *L, uint32_t side) {
    ps3_sio_t *S = &L->sio;
    memset(S, 0, sizeof(*S));
    S->started   = true;
    S->nplayers  = 2;
    S->side      = side;
    for (uint32_t k = 0; k < 2; k++) { ps3_sio_clear_ring(S, k, 0); ps3_sio_clear_ring(S, k, 1); }
    for (uint32_t k = 0; k < 2 && k < S->nplayers; k++)
        if (k != side) S->need_mask |= 1u << k;
    S->small_every   = 1;
    S->holdoff_reset = 10;
    S->big_every     = 60;
    S->relay      = (L->match_flags & PS3_MATCH_RELAY) != 0;
    S->spectators = (L->match_flags & PS3_MATCH_SPECTATORS) != 0;
    if (S->relay) {
        /* Inputs go round through the owner: a packet every 4th frame, and
         * the delay stays at its initial 10. */
        S->small_every = 4;
        S->delay = 10;
    } else {
        S->delay = ps3_clamp_delay(ps3_delay_from_ping_ms(L->room_rtt_ms));
        /* Somebody is watching: every 2nd frame to the other fighter, every
         * 12th straight to the watchers, and one frame more of delay. */
        if (S->spectators) {
            S->small_every = 2;
            S->big_every   = 12;
            S->delay       = S->delay < 9 ? S->delay + 1 : 10;
        }
    }
    S->init_delay = S->delay;
    ps3_sio_reset_counters(S);
}

/* SyncIo_BeginNewGeneration: the board has hit the forced START. */
static inline void ps3_sio_begin_generation(ps3_link_t *L) {
    ps3_sio_t *S = &L->sio;
    if (!S->started) return;
    ps3_sio_reset_counters(S);
    S->gen_ok       = false;
    S->holdoff      = S->holdoff_reset;
    S->sync_timeout = 600;
    S->barrier_hit  = false;
    S->resend_ss    = 300;
    S->sync_active  = true;
    S->resp_done    = false;
    S->passed       = false;
    for (uint32_t k = 0; k < 2; k++) ps3_sio_clear_ring(S, k, S->gen & 1u);
    S->send_ss = true;
    S->ss_echoed = false;
    S->gen = (uint8_t)((S->gen + 1u) & 0x1Fu);
    ps3_note(L, "PS3: generation %u (side %u)", (unsigned)S->gen, (unsigned)S->side);
}

/* SyncIo_BeginNextMatchOrFinish: the match is over, tear the lockstep down. */
static inline void ps3_sio_teardown(ps3_sio_t *S) {
    ps3_sio_reset_counters(S);
    S->gen_ok = false;
    for (uint32_t k = 0; k < 2; k++) { ps3_sio_clear_ring(S, k, 0); ps3_sio_clear_ring(S, k, 1); }
    S->barrier_hit = S->send_ss = S->resp_done = S->passed = false;
    S->gen = S->rgen = 0;
    S->sync_active = false;
    S->finished = true;
}

static inline void ps3_ring_put(ps3_sio_t *S, uint32_t side, uint32_t parity, int32_t frame, uint8_t input) {
    if (frame < 0) return;
    uint32_t i = (uint32_t)frame & (PS3_RING - 1);
    if (S->ring_frame[side][parity][i] < frame) {
        S->ring_frame[side][parity][i] = frame;
        S->ring_input[side][parity][i] = input;
    }
}

static inline bool ps3_ring_has(const ps3_sio_t *S, uint32_t side, int32_t frame) {
    uint32_t i = (uint32_t)frame & (PS3_RING - 1);
    return S->ring_frame[side][S->gen & 1u][i] == frame && S->ring_input[side][S->gen & 1u][i] != PS3_EMPTY_INPUT;
}

/* ---- Sending messages ---------------------------------------------------- */

/* Channel-1 messages from a member go to the owner, who relays a broadcast
 * (dest 0xFF) to everyone else. Only an owner writes to every member itself. */
static inline void ps3_send_ch1(ps3_link_t *L, const uint8_t *msg, uint32_t len, uint16_t to_member) {
    uint64_t now = net_now_ms();
    if (to_member) {
        ps3_peer_t *p = ps3_peer_by_id(L, to_member);
        if (p && p->rudp_up) rudp_write(&p->rudp, 1, msg, len, false, now);
        return;
    }
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
        ps3_peer_t *p = &L->peers[i];
        if (!p->used || !p->rudp_up) continue;
        if (!ps3_is_owner(L) && p->member_id != L->session->owner_id) continue;
        rudp_write(&p->rudp, 1, msg, len, false, now);
    }
}

static inline void ps3_send_sync_start(ps3_link_t *L) {
    uint8_t m[12];
    memset(m, 0, sizeof(m));
    m[0] = PS3_MSG_SYNC_START;
    m[1] = 0xFF;
    m[6] = L->sio.gen;
    m[7] = (uint8_t)L->sio.side;
    ps3_put16(m + 10, L->session->my_member_id);
    ps3_send_ch1(L, m, sizeof(m), 0);
    ps3_note(L, "PS3: SyncStart generation %u", (unsigned)L->sio.gen);
}

static inline void ps3_send_response(ps3_link_t *L, uint16_t via, uint16_t target) {
    uint8_t m[8];
    memset(m, 0, sizeof(m));
    m[0] = PS3_MSG_RESPONSE_SYNC_START;
    ps3_put16(m + 6, target);
    ps3_send_ch1(L, m, sizeof(m), via);
}

/* np_session_send_entrant_data: our 100 bytes, to the owner. A PS3 fighter sends
 * its own settings block here; ours is all zero, which is what the capture
 * shows a fresh PS3 profile sending too. */
static inline void ps3_send_entrant(ps3_link_t *L, bool no_link) {
    uint8_t m[7 + PS3_ENTRANT_SIZE];
    memset(m, 0, sizeof(m));
    m[0] = PS3_MSG_UPDATE_SETTING;
    m[6] = (uint8_t)L->my_side;
    if (no_link) m[7] |= 0x40;   /* becomes the slot's PS3_SLOT_NO_LINK */
    ps3_send_ch1(L, m, sizeof(m), L->session->owner_id);
}

/* The other fighter can be reached directly (FUN_000b6450 tests each queued
 * peer's RUDP contexts). Otherwise the entrant data says so, and the owner
 * puts the match in relay mode. */
static inline bool ps3_linked_to_fighters(const ps3_link_t *L) {
    for (uint32_t k = 0; k < L->fighter_count && k < 2; k++) {
        if (L->fighters[k] == L->session->my_member_id) continue;
        const ps3_peer_t *p = NULL;
        for (uint32_t i = 0; i < RPCN_MAX_PEERS && !p; i++)
            if (L->peers[i].used && L->peers[i].member_id == L->fighters[k]) p = &L->peers[i];
        if (!p || !p->rudp_up || !rudp_all_open(&p->rudp)) return false;
    }
    return true;
}

/* np_session_entry_position (0xB8D58): 1 for 1P, 2 for 2P, 0 for anyone else. */
static inline uint32_t ps3_entry_position(const ps3_link_t *L, uint16_t member_id) {
    for (uint32_t k = 0; k < L->fighter_count && k < 2; k++)
        if (L->fighters[k] == member_id) return k + 1u;
    return 0;
}

/* Session_SendSyncIo_or_Spectator -> FUN_000b871c: the per-frame packet,
 * straight to the other fighter on channel 2, latency-critical (write flag 8).
 * Only a plain two-player room names the sender after the packet. In relay
 * mode a member sends it to the owner instead, flagged 0x100 for passing on. */
static inline void ps3_send_small(ps3_link_t *L, const uint8_t pkt[16]) {
    const ps3_sio_t *S = &L->sio;
    uint8_t m[6 + 3 + 16 + 4];
    memset(m, 0, sizeof(m));
    m[0] = PS3_MSG_SYNCIO;
    m[6] = (uint8_t)(S->side + 1u);
    ps3_put16(m + 7, 16);
    memcpy(m + 9, pkt, 16);
    ps3_put16(m + 25, (S->spectators || S->relay) ? 0 : L->session->my_member_id);
    ps3_put16(m + 27, 0);
    uint64_t now = net_now_ms();
    if (S->relay && !ps3_is_owner(L)) {
        ps3_put32(m + 2, 0x100u);
        ps3_peer_t *owner = ps3_peer_by_id(L, L->session->owner_id);
        if (owner && owner->rudp_up) rudp_write(&owner->rudp, 2, m, sizeof(m), false, now);
        return;
    }
    for (uint32_t k = 0; k < L->fighter_count && k < 2; k++) {
        if (L->fighters[k] == L->session->my_member_id) continue;
        ps3_peer_t *p = ps3_peer_by_id(L, L->fighters[k]);
        if (p && p->rudp_up) rudp_write(&p->rudp, 2, m, sizeof(m), true, now);
    }
}

/* Session_SendSyncIo_or_Spectator(.., 1) -> FUN_000b7274: with somebody
 * watching, every 12th frame's 60-frame packet also goes straight to each
 * member who is not fighting, on channel 2. Not in relay mode. */
static inline void ps3_send_watchers(ps3_link_t *L, const uint8_t pkt[68]) {
    if (L->sio.relay) return;
    uint8_t m[6 + 3 + 68 + 4];
    memset(m, 0, sizeof(m));
    m[0] = PS3_MSG_SYNCIO;
    m[6] = (uint8_t)(L->sio.side + 1u);
    ps3_put16(m + 7, 68);
    memcpy(m + 9, pkt, 68);
    uint64_t now = net_now_ms();
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
        ps3_peer_t *p = &L->peers[i];
        if (!p->used || !p->rudp_up || ps3_entry_position(L, p->member_id)) continue;
        rudp_write(&p->rudp, 2, m, sizeof(m), false, now);
    }
}

/* Session_SendSyncIoTcp: the 60-frame packet, broadcast through the owner. */
static inline void ps3_send_big(ps3_link_t *L, const uint8_t pkt[68]) {
    uint8_t m[6 + 3 + 68];
    memset(m, 0, sizeof(m));
    m[0] = PS3_MSG_SYNCIO_TCP;
    m[1] = 0xFF;
    m[6] = (uint8_t)(L->sio.side + 1u);
    ps3_put16(m + 7, 68);
    memcpy(m + 9, pkt, 68);
    ps3_send_ch1(L, m, sizeof(m), 0);
}

/* ---- The lockstep, one tick at a time ------------------------------------ */

/* SyncIo_SampleLocalAndSend, once per 60 Hz tick. `local` is this machine's
 * input as a wire byte. */
static inline void ps3_sio_sample(ps3_link_t *L, uint8_t local) {
    ps3_sio_t *S = &L->sio;
    if (!S->started || S->side >= 2) return;
    if (S->send_ss) {
        ps3_send_sync_start(L);
        S->send_ss = false;
        return;
    }
    if (S->holdoff < 1) {
        S->holdoff = 0;
    } else if (--S->holdoff != 0) {
        if (S->gen_ok) { S->stall_count = 0; S->stall = true; }
        return;
    }
    if (!S->gen_ok || !S->resp_done) return;

    if (!S->stall) {
        if (S->play < 1 || S->sample - S->play <= S->delay) S->stall_count = 0;
        else if (++S->stall_count > 60) S->stall = true;
    } else {
        S->stall_count = 0;
        if (S->speed) {
            if (S->play > 0 && S->delay < S->sample - S->play) return;
            S->stall = false;
        }
    }

    uint32_t par = S->gen & 1u;
    int32_t f = S->sample;
    ps3_ring_put(S, S->side, par, f, local);
    uint8_t head = (uint8_t)(((S->side & 7u) << 5) | (S->gen & 0x1Fu));

    if (f % S->small_every == 0) {
        uint8_t pkt[16];
        memset(pkt, 0, sizeof(pkt));
        ps3_put32(pkt, (uint32_t)f);
        pkt[4] = head;
        for (int32_t k = 0; k < 10; k++) {
            uint32_t i = (uint32_t)(f - k) & (PS3_RING - 1);
            pkt[6 + k] = (uint8_t)S->ring_input[S->side][par][i];
        }
        ps3_send_small(L, pkt);
    }
    if (f > 0 && f % S->big_every == 0) {
        uint8_t pkt[68];
        memset(pkt, 0, sizeof(pkt));
        ps3_put32(pkt, (uint32_t)f);
        pkt[4] = head;
        for (int32_t k = 0; k < 60; k++) {
            uint32_t i = (uint32_t)(f - k) & (PS3_RING - 1);
            pkt[8 + k] = (uint8_t)S->ring_input[S->side][par][i];
        }
        /* Only 1P sets the delay, from its round trip to 2P, and not when the
         * inputs go round through the owner. */
        if (S->side == 0 && !S->relay) {
            ps3_peer_t *other = NULL;
            for (uint32_t k = 0; k < 2 && !other; k++)
                if (L->fighters[k] != L->session->my_member_id) other = ps3_peer_by_id(L, L->fighters[k]);
            rpcs3_sig_peer_t *sp = other ? rpcs3_sig_find(&L->sig, other->npid) : NULL;
            uint32_t rtt_us = rpcs3_sig_rtt_us(sp);
            if (rtt_us) {
                int32_t d = ps3_delay_from_ping_ms(rtt_us / 1000u) + (S->spectators ? 1 : 0);
                d = ps3_clamp_delay(d);
                if (d != S->delay && d > 0) {
                    S->delay = d;
                    pkt[5] = (uint8_t)(0x80u | ((uint32_t)(d & 0xF) << 3));
                }
            }
        }
        if (f % 60 == 0) ps3_send_big(L, pkt);
        if (S->big_every < 60) ps3_send_watchers(L, pkt);
    }
    S->sample = f + 1;
}

/* SyncIo_Update_gate_speed, once per tick. False if the match had to stop. */
static inline bool ps3_sio_gate(ps3_link_t *L) {
    ps3_sio_t *S = &L->sio;
    if (!S->started) return true;
    if (S->sync_timeout) S->sync_timeout--;
    if (S->resend_ss) {
        if (S->resend_ss == 1) {
            if (!S->gen_ok || S->play < 0) { S->send_ss = true; S->resend_ss = 180; }
            else S->resend_ss = 0;
        } else S->resend_ss--;
    }
    if (S->sync_active && !S->finished) {
        if (S->sync_timeout == 0) {
            ps3_note(L, "PS3: the lockstep timed out (generation %u/%u, play %d, sample %d, newest %d)",
                     (unsigned)S->gen, (unsigned)S->rgen, S->play, S->sample, S->newest);
            return false;
        }
        if (S->gen_ok) {
            if (S->gen != S->rgen) { ps3_sio_reset_counters(S); S->gen_ok = false; }
        } else if (S->gen == S->rgen) {
            ps3_sio_reset_counters(S);
            S->gen_ok = true;
            ps3_note(L, "PS3: both boards on generation %u", (unsigned)S->gen);
        }
    }
    if (!S->gen_ok) {
        S->speed = !(S->sync_active && !S->finished);
        return true;
    }
    S->speed = true;
    if (S->play < 0) {
        int32_t start = S->init_delay;
        bool ok = true;
        if (start < 11) {
            for (uint32_t k = 0; k < S->nplayers && ok; k++)
                for (int32_t f = start; f <= 10 && ok; f++)
                    if (!ps3_ring_has(S, k, f)) ok = false;
        }
        if (ok) {
            S->play = start;
            ps3_note(L, "PS3: inputs flowing; the first frame played is %d (delay %d)", start, S->delay);
        }
    }
    if (S->passed) {
        if (S->side < 2 && S->sample - S->play < S->delay) S->speed = false;
        else
            for (uint32_t k = 0; k < S->nplayers; k++)
                if (!ps3_ring_has(S, k, S->play)) S->speed = false;
    }
    return true;
}

/* SyncIo_OnSyncIoMsg_evt10 / _OnSyncIoTcpMsg_evt11: an input packet. */
static inline void ps3_sio_on_input(ps3_link_t *L, const uint8_t *pkt, uint32_t len) {
    ps3_sio_t *S = &L->sio;
    if (!S->started || (len != 16 && len != 68)) return;
    int32_t  f    = (int32_t)ps3_be32(pkt);
    uint32_t side = pkt[4] >> 5, g = pkt[4] & 0x1Fu;
    if (side == S->side || side >= 2 || g < S->gen || g > (uint32_t)S->gen + 1u) return;
    uint32_t par = g & 1u;
    uint32_t n = (len == 16) ? 10u : 60u;
    const uint8_t *in = pkt + ((len == 16) ? 6 : 8);
    for (uint32_t k = 0; k < n && f - (int32_t)k >= 0; k++) ps3_ring_put(S, side, par, f - (int32_t)k, in[k]);
    if (len == 68 && S->side != 0 && (pkt[5] & 0x80u)) {
        int32_t d = (pkt[5] >> 3) & 0xF;
        if (d > 1) {
            S->delay = ps3_clamp_delay(d);
            ps3_note(L, "PS3: 1P set the input delay to %d frames", S->delay);
        }
    }
    if (S->newest < f) S->newest = f;
}

/* ---- Receiving messages -------------------------------------------------- */

static inline void ps3_on_message(ps3_link_t *L, ps3_peer_t *from, uint16_t vport,
                                  const uint8_t *m, uint32_t len) {
    (void)vport;
    if (len < 6) return;
    ps3_sio_t *S = &L->sio;
    bool owner = ps3_is_owner(L);
    uint64_t now = net_now_ms();
    /* np_rudp_dispatch_message: the owner passes a broadcast on, as it came,
     * to every other member on channel 1, then reads it itself. */
    if (owner && m[1] == 0xFF) {
        for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
            ps3_peer_t *p = &L->peers[i];
            if (p->used && p->rudp_up && p != from) rudp_write(&p->rudp, 1, m, len, false, now);
        }
    }
    switch (m[0]) {
        case PS3_MSG_SYNCIO:
        case PS3_MSG_SYNCIO_TCP: {
            if (len < 9) return;
            uint32_t n = ps3_be16(m + 7);
            if (9 + n > len) return;
            ps3_sio_on_input(L, m + 9, n);
            /* Relay mode: a fighter's inputs reach the other fighter through
             * the owner (case 0, header flag 0x100). */
            if (owner && m[0] == PS3_MSG_SYNCIO && (ps3_be32(m + 2) & 0x100u)) {
                for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
                    ps3_peer_t *p = &L->peers[i];
                    if (!p->used || !p->rudp_up || p == from) continue;
                    uint32_t pos = ps3_entry_position(L, p->member_id);
                    if (pos && pos != m[6]) rudp_write(&p->rudp, 2, m, len, true, now);
                }
            }
            break;
        }
        case PS3_MSG_SYNC_START: {
            if (len < 12) return;
            uint8_t g = m[6], side = m[7];
            uint16_t sender = ps3_be16(m + 10);
            ps3_note(L, "PS3: SyncStart from %s: generation %u, side %u", from->npid, (unsigned)g, (unsigned)side);
            /* SyncIo_OnSyncStart_evt8 */
            if (S->started && S->rgen < g) {
                S->got_mask |= 1u << (side & 31u);
                if (S->got_mask == S->need_mask) {
                    ps3_sio_reset_counters(S);
                    S->got_mask = 0;
                    S->gen_ok = false;
                    S->rgen = g;
                }
            }
            /* Every fighter answers (np_rudp_dispatch_message, case 3). */
            if (L->my_side >= 0) ps3_send_response(L, from->member_id, sender);
            /* NOT the PS3's: our own SyncStart again, once, if it went out before
             * this one came in. Our board reaches its forced START sooner than a
             * PS3's, and a PS3 drops a SyncStart that arrives before its lockstep
             * task is running -- then waits for our 5 s resend (every live run
             * lost 3.5-5 s there). Its network layer still answers the one it
             * dropped, so an answer proves nothing; theirs arriving proves the
             * task is running. A duplicate is ignored on both sides. */
            if (S->started && S->gen == g && !S->ss_echoed && !S->send_ss) {
                S->ss_echoed = true;
                ps3_send_sync_start(L);
            }
            break;
        }
        case PS3_MSG_RESPONSE_SYNC_START: {
            if (len < 8) return;
            if (ps3_be16(m + 6) != L->session->my_member_id) {
                /* An answer to somebody else's SyncStart: the owner hands it on. */
                ps3_peer_t *to = owner ? ps3_peer_by_id(L, ps3_be16(m + 6)) : NULL;
                if (to && to->rudp_up) rudp_write(&to->rudp, 1, m, len, false, now);
                return;
            }
            /* SyncIo_OnResponseSyncStart_evt9 */
            if (S->resp_done) return;
            if (++S->resp_count >= S->nplayers - 1u) {
                S->resp_count = 0;
                S->resp_done = true;
                ps3_note(L, "PS3: %s answered our SyncStart", from->npid);
            }
            break;
        }
        case PS3_MSG_UPDATE_SETTING: {
            /* A fighter's entrant data, kept by the owner in that side's slot
             * of the room, whose first word becomes the slot's flags. */
            if (!owner || !L->hosting || len < 7 + PS3_ENTRANT_SIZE || m[6] > 1) return;
            /* Only from the fighter chosen for that side, while fighters are
             * being prepared: anything else would fill a slot for somebody. */
            if (ps3_be32(L->blob + 0x10) != PS3_PHASE_PREPARING
                || ps3_be16(L->blob + 0x18 + 2u * m[6]) != from->member_id) return;
            uint8_t *slot = L->blob + 0x20 + (uint32_t)m[6] * PS3_ENTRANT_SIZE;
            memcpy(slot, m + 7, PS3_ENTRANT_SIZE);
            ps3_put32(slot, ps3_be32(slot) | PS3_SLOT_FILLED);
            ps3_note(L, "PS3: %s's entrant data for %s", from->npid, m[6] ? "2P" : "1P");
            break;
        }
        default:
            break;
    }
}

static inline void ps3_deliver_cb(void *ctx, uint16_t vport, const uint8_t *payload, uint32_t len) {
    ps3_peer_t *p = (ps3_peer_t *)ctx;
    if (p->link) ps3_on_message(p->link, p, vport, payload, len);
}

/* ---- The room ------------------------------------------------------------ */

static inline void ps3_publish_me(ps3_link_t *L) {
    if (!L->me_dirty) return;
    if (rpcn_session_set_member_team(L->session, L->team, L->me, sizeof(L->me))) L->me_dirty = false;
}

static inline void ps3_set_flags(ps3_link_t *L, uint32_t flags) {
    if (ps3_be32(L->me) == flags) return;
    ps3_put32(L->me, flags);
    L->me_dirty = true;
}

/*
 * The match's settings (NetMatch_StateMachine state 3 -> NetGameMode_Set(2) ->
 * Settings_ApplyRoomRules 0x11dff0). The game's rule bytes are built from the
 * room blob as {0, b[0xC], 0, b[0xA], b[0x9], 1, 4, b[0xB]}: b[9] rounds, b[0xA]
 * time, b[0xB] game type (A-D), b[0xC] secret characters; energy and barrier
 * are fixed. Each goes through the EBOOT's table at 0x377AB0 into the game's
 * settings block (hle_hooks.h, g_xplay_rules).
 */
static const uint8_t ps3_rules_table[0x32] = {
    0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x01, 0x03,   /* (damage bit, energy) by cfg[5] */
    0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00,               /* (barrier reset, !hyper) by game type */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x03, 0x04, 0x05, 0x00, 0x00, 0x00, 0x00,               /* rounds to win */
    0x0A, 0x1E, 0x3C, 0x63, 0x00, 0x00, 0x00, 0x00,               /* round time */
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,   /* barrier */
};

static inline void ps3_build_rules(const uint8_t *blob, uint8_t out[5]) {
    const uint8_t *T = ps3_rules_table;
    uint8_t cfg[8] = { 0, blob[0x0C], 0, blob[0x0A], blob[0x09], 1, 4, blob[0x0B] };
    uint32_t e = cfg[5] < 5 ? cfg[5] * 2u : 0u;
    uint32_t g = cfg[7] < 4 ? 10u + cfg[7] * 2u : 10u;
    out[0] = T[0x18 + (cfg[4] < 4 ? cfg[4] : 0)];                                        /* +0x01 */
    out[1] = T[e + 1];                                                                   /* +0x04 */
    out[2] = T[0x20 + (cfg[3] < 4 ? cfg[3] : 0)];                                        /* +0x11 */
    out[3] = (uint8_t)(((T[g + 1] ^ 1u) << 6) | (T[g] << 3) | 0x10u | (T[e] << 7));      /* +0x13 */
    out[4] = T[0x28 + (cfg[6] < 10 ? cfg[6] : 0)];                                       /* +0x18 */
}

/* The board is taken for a match (NetMatch_StateMachine, NetGameMode_Set(2)). */
static inline void ps3_match_begin(ps3_link_t *L) {
    L->match = true;
    L->match_seen = true;
    L->last_result = 0;
    L->frame_live = false;
    L->stalled_frames = 0;
    ps3_sio_start(L, (uint32_t)L->my_side);
    const uint8_t *blob = L->session->room_bin;
    memcpy(L->rules_blob, blob + 9, 4);
    ps3_build_rules(blob, g_xplay_rules);
    g_xplay_seed          = L->seed;
    g_xplay_spectators    = L->sio.spectators ? 1 : 0;
    g_xplay_ready         = 0;
    g_xplay_rules_pending = 1;
    /* FUN_000ac554: a board that is not in attract (ADV_INT..INFO_DSP) is
     * rebooted, so that it reaches the forced START. After a match ours is on
     * the victory screen or playing on against the CPU. */
    int also = g_xplay_also_mode, mode = g_xplay_mode;
    L->need_reset = !(also >= 2 && also <= 5) && mode != 2;
    g_xplay_barrier = 0;
    g_xplay_events  = 0;
    g_xplay_match   = 1;
    ps3_note(L, "PS3: rules: rounds %u, time %u, type %c, secret %u -> settings %02X %02X %02X %02X %02X; "
             "stage %u%s", blob[9], blob[10], 'A' + (blob[11] & 3), blob[12],
             g_xplay_rules[0], g_xplay_rules[1], g_xplay_rules[2], g_xplay_rules[3], g_xplay_rules[4],
             (unsigned)(L->seed % 9u), L->need_reset ? "; rebooting the board (not in attract)" : "");
    ps3_note(L, "PS3: match on; we are %s, first frame delay %d", L->my_side == 0 ? "1P" : "2P",
             L->sio.init_delay);
}

static inline void ps3_match_end(ps3_link_t *L, const char *why) {
    if (!L->match) return;
    L->match = false;
    L->need_reset = false;
    g_xplay_match   = 0;
    g_xplay_barrier = 0;
    g_xplay_ready   = 0;
    g_xplay_spectators = 0;
    g_xplay_rules_pending = 0;
    ps3_sio_teardown(&L->sio);
    L->sio.started = false;
    ps3_note(L, "PS3: match over (%s)", why);
}

/* A member's flags word, ours or as another member last published it. */
static inline uint32_t ps3_member_flags(const ps3_link_t *L, uint16_t member_id) {
    if (member_id == L->session->my_member_id) return ps3_be32(L->me);
    const rpcn_peer_t *p = rpcn_session_peer(L->session, member_id);
    return (p && p->bin_len >= 4) ? ps3_be32(p->bin) : 0;
}

/* One member's place in a sort: the PS3 sorts u32 keys built from teamId and
 * memberId, ascending (FUN_0025e878). */
typedef struct { uint32_t key; uint16_t id; uint8_t entry; } ps3_rank_t;

static inline void ps3_rank_sort(ps3_rank_t *r, uint32_t n) {
    for (uint32_t a = 1; a < n; a++)
        for (uint32_t b = a; b > 0 && r[b - 1].key > r[b].key; b--) {
            ps3_rank_t t = r[b]; r[b] = r[b - 1]; r[b - 1] = t;
        }
}

/* Everyone in the room, keyed (teamId << 16) | memberId: the waiting line. */
static inline uint32_t ps3_rank_members(const ps3_link_t *L, ps3_rank_t *r) {
    const rpcn_session_t *s = L->session;
    uint32_t n = 0;
    r[n].id = s->my_member_id;
    r[n].key = ((uint32_t)L->team << 16) | s->my_member_id;
    r[n].entry = L->me[0x1C];
    n++;
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
        const rpcn_peer_t *p = &s->peers[i];
        if (!p->used) continue;
        r[n].id = p->member_id;
        r[n].key = ((uint32_t)p->team_id << 16) | p->member_id;
        r[n].entry = p->bin_len > 0x1C ? p->bin[0x1C] : 0;
        n++;
    }
    return n;
}

/*
 * np_session_rotate_queue_after_match (0xBE788), run by every member of a Room
 * Match: `win` is the queue position (1 or 2) of the side that won. The winner
 * goes to the front and asks for the same side again; the loser goes to the
 * back and asks for nothing; everyone else keeps their order. Each member then
 * publishes only its own place (teamId) and bit 29.
 */
static inline void ps3_rotate(ps3_link_t *L, uint32_t win) {
    if (L->rotated) return;
    L->rotated = true;
    L->me[PS3_ME_NO_RESULT] = win ? 0 : 1;
    const rpcn_session_t *s = L->session;
    if (s->room_bin_len > 4 && s->room_bin[4] == 1 && win) {
        ps3_rank_t r[RPCN_MAX_PEERS + 1];
        uint32_t n = ps3_rank_members(L, r);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t pos = ps3_entry_position(L, r[i].id);
            if (!pos) continue;
            bool mine = r[i].id == s->my_member_id;
            if (pos == win) {
                r[i].key = r[i].id;
                if (mine) L->me[0x1C] = (uint8_t)pos;
            } else {
                r[i].key |= 0x80000000u;
                if (mine) L->me[0x1C] = 0;
            }
        }
        ps3_rank_sort(r, n);
        for (uint32_t i = 0; i < n; i++)
            if (r[i].id == s->my_member_id) L->team = (uint8_t)(i + 1u);
        ps3_note(L, "PS3: %s won; our place in line is %u", win == 1 ? "1P" : "2P", (unsigned)L->team);
    }
    ps3_set_flags(L, ps3_be32(L->me) | PS3_MFLAG_ROTATED);
}

/* Which side won, from what the fighters published after it: the winner is
 * at the front of the line (teamId 1) asking for its own side again, the
 * loser behind it asking for nothing. For a member whose board did not see
 * the result (it is not fighting, or its lockstep gave out). A fighter of
 * ours that saw no result marks it (PS3_ME_NO_RESULT) and is skipped: a
 * winner that stays on still has teamId 1 and its side from the last match,
 * the very shape of a winner. A PS3 fighter never sets the mark, so a PS3
 * winner that stays on and then times out is still read that way; nothing
 * on our side can tell. 0 = not known. */
static inline uint32_t ps3_published_winner(const ps3_link_t *L) {
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t k = 0; k < L->fighter_count && k < 2; k++) {
            const rpcn_peer_t *p = rpcn_session_peer(L->session, L->fighters[k]);
            if (!p || p->bin_len <= PS3_ME_NO_RESULT || !(ps3_be32(p->bin) & PS3_MFLAG_ROTATED)) continue;
            if (p->bin[PS3_ME_NO_RESULT]) continue;
            if (pass == 0 && p->team_id == 1 && p->bin[0x1C] == k + 1u) return k + 1u;
            if (pass == 1 && p->team_id != 1 && p->bin[0x1C] == 0) return 2u - k;
        }
    return 0;
}

/* Our board's match is over: take our place in line, and the results screen
 * runs from here, whichever came first: the PS3's result can move the room to
 * phase 4 before our board gets here. */
static inline void ps3_after_result(ps3_link_t *L) {
    L->results_since_us = net_now_us();
    uint32_t win = L->last_result ? L->last_result : ps3_published_winner(L);
    if (win) ps3_rotate(L, win);
}

static inline void ps3_read_room(ps3_link_t *L) {
    rpcn_session_t *s = L->session;
    if (s->room_rev == L->room_rev_seen) return;
    L->room_rev_seen = s->room_rev;
    if (s->room_bin_len < 0x20) return;
    const uint8_t *b = s->room_bin;
    uint32_t phase = ps3_be32(b + 0x10);
    L->seed          = ps3_be32(b + 0x00);
    L->match_flags   = b[0x05];
    L->fighter_count = ps3_be32(b + 0x14);
    L->fighters[0]   = ps3_be16(b + 0x18);
    L->fighters[1]   = ps3_be16(b + 0x1A);
    L->room_rtt_ms   = ps3_be32(b + 0x1C);
    if (!L->room_known || phase != L->phase) {
        ps3_note(L, "PS3: room phase %u (fighters %u: %u, %u; seed %08X; rtt %u ms; flags %02X)", (unsigned)phase,
                 (unsigned)L->fighter_count, L->fighters[0], L->fighters[1], L->seed, L->room_rtt_ms,
                 L->match_flags);
    }
    uint32_t was = L->room_known ? L->phase : 0xFFFFFFFFu;
    L->room_known = true;
    L->phase = phase;

    L->my_side = -1;
    for (uint32_t k = 0; k < 2 && k < L->fighter_count; k++)
        if (L->fighters[k] == s->my_member_id) L->my_side = (int32_t)k;

    if (phase == was) return;
    L->seen_since_us = net_now_us();
    /* The owner moved on without us finishing: the PS3 gave up on the match
     * (its lockstep timed out, or somebody left). */
    if (L->match && phase != PS3_PHASE_MATCH) {
        ps3_match_end(L, "the room left the match phase");
        /* The two boards do not play the same fight yet, so the PS3's can reach
         * its result first and move the room on before ours reaches VIC_INT.
         * Our place in the line still follows our board's result. */
        ps3_after_result(L);
    }
    switch (phase) {
        case PS3_PHASE_LOBBY:
            L->entrant_sent = false;
            L->rotated = false;
            ps3_set_flags(L, 0);
            break;
        case PS3_PHASE_CHOOSING:
        case PS3_PHASE_PREPARING:
            L->entrant_sent = false;
            L->rotated = false;
            L->match_seen = false;
            break;
        case PS3_PHASE_MATCH:
            if (L->my_side >= 0 && !L->match) ps3_match_begin(L);
            /* Published whether or not the flags below change: they may
             * already read IN_MATCH | READY | ROTATED if the last match's
             * were never cleared. */
            if (L->me[PS3_ME_NO_RESULT]) { L->me[PS3_ME_NO_RESULT] = 0; L->me_dirty = true; }
            ps3_set_flags(L, ps3_be32(L->me) | PS3_MFLAG_IN_MATCH | PS3_MFLAG_READY);
            break;
        case PS3_PHASE_RESULTS:
            L->results_since_us = net_now_us();
            L->results_cleared = false;
            break;
        default:
            break;
    }
}

/* ---- The owner ----------------------------------------------------------- */

static inline void ps3_owner_write(ps3_link_t *L, uint32_t flag_filter, uint32_t flag_attr, uint64_t now_us) {
    rpcn_session_set_room_state_flags(L->session, flag_filter, flag_attr, L->blob, PS3_ROOM_BIN_SIZE);
    L->last_write_us = now_us;
    L->write_filter  = flag_filter;
    L->write_attr    = flag_attr;
}

/* A new write: the resend clock starts again. */
static inline void ps3_owner_write_new(ps3_link_t *L, uint32_t flag_filter, uint32_t flag_attr, uint64_t now_us) {
    L->rewrites = 0;
    ps3_owner_write(L, flag_filter, flag_attr, now_us);
}

static inline void ps3_owner_set_phase(ps3_link_t *L, uint32_t phase, uint32_t flag_filter, uint32_t flag_attr,
                                       uint64_t now_us) {
    ps3_put32(L->blob + 0x10, phase);
    L->phase_since_us = now_us;
    ps3_owner_write_new(L, flag_filter, flag_attr, now_us);
}

/* Back to the lobby with nobody chosen, and the room open again (phase 1 or 2
 * failing in a Room Match). */
static inline void ps3_owner_to_lobby(ps3_link_t *L, const char *why, uint64_t now_us) {
    ps3_note(L, "PS3: back to the lobby (%s)", why);
    ps3_put32(L->blob + 0x14, 0);
    memset(L->blob + 0x20, 0, 2 * PS3_ENTRANT_SIZE);
    L->lobby_until_us = now_us + PS3_LOBBY_US;
    ps3_owner_set_phase(L, PS3_PHASE_LOBBY, PS3_ROOM_CLOSED_HIDDEN, 0, now_us);
}

/*
 * np_session_build_fight_entries (0xBEBD0): the two fighters, from the waiting
 * line. The first member in line who asked for 1P gets it and the first who
 * asked for 2P gets that; the rest are taken in line order to fill whichever
 * side is empty. Fighters go to blob +0x18 in side order and the two entrant
 * slots are cleared. False if the room cannot field two.
 */
static inline bool ps3_owner_choose(ps3_link_t *L) {
    ps3_rank_t r[RPCN_MAX_PEERS + 1];
    uint32_t n = ps3_rank_members(L, r);
    ps3_rank_sort(r, n);
    uint32_t n1 = 0, n2 = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cls;
        if      (r[i].entry == 1 && n1 < 1) { n1++; cls = 0; }
        else if (r[i].entry == 2 && n2 < 1) { n2++; cls = 0x04000000u; }
        else if (r[i].entry == 1 || r[i].entry == 2) cls = 0x40000000u;
        else                                          cls = 0x80000000u;
        r[i].key = cls | (r[i].key & 0xFFFFFFu);
    }
    ps3_rank_sort(r, n);
    uint32_t c = n < 2 ? n : 2;
    for (uint32_t i = 0; i < c; i++)
        if (r[i].key & 0xF0000000u) r[i].key = (r[i].key & 0xFFFFFFu) | 0x02000000u;
    ps3_rank_sort(r, c);
    ps3_put32(L->blob + 0x14, c);
    ps3_put16(L->blob + 0x18, c > 0 ? r[0].id : 0);
    ps3_put16(L->blob + 0x1A, c > 1 ? r[1].id : 0);
    memset(L->blob + 0x20, 0, 2 * PS3_ENTRANT_SIZE);
    return c >= 2;
}

/* The round trip to a member, in ms, as signaling measures it (0 = unknown). */
static inline uint32_t ps3_rtt_ms(ps3_link_t *L, uint16_t member_id) {
    ps3_peer_t *p = ps3_peer_by_id(L, member_id);
    rpcs3_sig_peer_t *sp = p ? rpcs3_sig_find(&L->sig, p->npid) : NULL;
    return rpcs3_sig_rtt_us(sp) / 1000u;
}

/*
 * The owner's half of np_session_update_room_phase (0xBF258), once a pump. As
 * on the PS3, a step is taken only once the server has echoed the last write
 * (the owner compares its phase with the room's), so there is one write in
 * flight at a time. The sequence, as a PS3 owner writes it:
 *   0 lobby     until two are in (a room of two) or the countdown runs out
 *   1 choosing  room closed and hidden; pick the fighters
 *   2 preparing the fighters send their entrant data and set ready
 *   3 match     seed, match flags and round trip written; everyone plays
 *   4 results   room reopened; back to 0 once nobody is marked in the match
 */
static inline void ps3_owner_pump(ps3_link_t *L, uint64_t now_us) {
    if (!L->hosting || !ps3_is_owner(L)) return;
    rpcn_session_t *s = L->session;
    uint32_t phase = ps3_be32(L->blob + 0x10);
    uint32_t echo  = s->room_bin_len >= 0x14 ? ps3_be32(s->room_bin + 0x10) : 0xFFFFFFFFu;
    if (echo != phase) {
        /* Again after 3 s, then backing off to a minute: a server that keeps
         * refusing the write is told so less and less often. */
        uint32_t shift = L->rewrites < 5 ? L->rewrites : 5;
        uint64_t wait  = (uint64_t)PS3_REWRITE_US << shift;
        if (wait > 60000000u) wait = 60000000u;
        if (now_us > L->last_write_us + wait) {
            if (L->rewrites < 3 || (L->rewrites & 7) == 0)
                ps3_note(L, "PS3: the room write for phase %u was not echoed; sending it again (%u)",
                         (unsigned)phase, (unsigned)(L->rewrites + 1));
            L->rewrites++;
            ps3_owner_write(L, L->write_filter, L->write_attr, now_us);
        }
        return;
    }
    uint32_t members = 1u + rpcn_session_peer_count(s);
    uint16_t me = s->my_member_id;
    uint32_t count = ps3_be32(L->blob + 0x14);
    uint16_t fighter[2] = { ps3_be16(L->blob + 0x18), ps3_be16(L->blob + 0x1A) };
    uint32_t my_pos = 0;
    bool fighter_gone = false;
    for (uint32_t k = 0; k < count && k < 2; k++) {
        if (fighter[k] == me) my_pos = k + 1u;
        else if (!rpcn_session_peer(s, fighter[k])) fighter_gone = true;
    }

    switch (phase) {
        case PS3_PHASE_LOBBY:
            /* np_session_entries_should_close: a room of two starts the moment
             * it is full; a bigger one when its countdown runs out, which is
             * shorter once three are in. */
            /* Ours: the countdown starts with the second player, not with the
             * room. The PS3's runs from the lobby's start, so a room left
             * empty for 30 s starts the moment a second player walks in and
             * a third never makes it. */
            if (members < 2) L->lobby_until_us = now_us + PS3_LOBBY_US;
            if (members >= 3 && L->lobby_until_us > now_us + PS3_LOBBY_CROWD_US) {
                L->lobby_until_us = now_us + PS3_LOBBY_CROWD_US;
                L->crowd = true;
            } else if (members < 3 && L->crowd) {
                L->crowd = false;                /* down to two: the full wait again */
                L->lobby_until_us = now_us + PS3_LOBBY_US;
            }
            if (members > 1 && (L->max_slot <= 2 || now_us >= L->lobby_until_us)) {
                ps3_note(L, "PS3: choosing the fighters (%u in the room)", (unsigned)members);
                ps3_owner_set_phase(L, PS3_PHASE_CHOOSING, PS3_ROOM_CLOSED_HIDDEN, PS3_ROOM_CLOSED_HIDDEN, now_us);
            }
            break;

        case PS3_PHASE_CHOOSING:
            if (members > 1 && ps3_owner_choose(L)) {
                L->own_entrant = false;
                L->match_seen  = false;
                ps3_note(L, "PS3: fighters %u (1P) and %u (2P)", ps3_be16(L->blob + 0x18), ps3_be16(L->blob + 0x1A));
                ps3_owner_set_phase(L, PS3_PHASE_PREPARING, 0, 0, now_us);
            } else {
                ps3_owner_to_lobby(L, "not enough players", now_us);
            }
            break;

        case PS3_PHASE_PREPARING: {
            if (fighter_gone) { ps3_owner_to_lobby(L, "a fighter left", now_us); break; }
            if (now_us > L->phase_since_us + PS3_PREPARE_US) {
                ps3_owner_to_lobby(L, "a fighter never got ready", now_us);
                break;
            }
            /* np_session_send_entrant_data, for the owner: its own entrant
             * data goes straight into its slot, once it can reach the other
             * fighter or has waited long enough to say it cannot. */
            if (my_pos && !L->own_entrant) {
                bool linked = ps3_linked_to_fighters(L);
                if (linked || now_us > L->phase_since_us + PS3_LINK_WAIT_US) {
                    uint8_t *slot = L->blob + 0x20 + (my_pos - 1u) * PS3_ENTRANT_SIZE;
                    memset(slot, 0, PS3_ENTRANT_SIZE);
                    ps3_put32(slot, PS3_SLOT_FILLED | (linked ? 0 : PS3_SLOT_NO_LINK));
                    ps3_set_flags(L, ps3_be32(L->me) | PS3_MFLAG_READY);
                    L->own_entrant = true;
                    ps3_note(L, "PS3: our entrant data is in; ready as %s%s", my_pos == 1 ? "1P" : "2P",
                             linked ? "" : " (no direct link to the other fighter)");
                }
            }
            bool all = count >= 2;
            for (uint32_t k = 0; k < count && k < 2 && all; k++) {
                const uint8_t *slot = L->blob + 0x20 + k * PS3_ENTRANT_SIZE;
                if (!(ps3_member_flags(L, fighter[k]) & PS3_MFLAG_READY) || !(ps3_be32(slot) & PS3_SLOT_FILLED))
                    all = false;
            }
            if (!all) break;
            /* np_session_seed_match: a fresh seed, and the flags that shape the
             * lockstep. */
            uint64_t x = now_us ^ ((uint64_t)me << 32) ^ 0x9E3779B97F4A7C15ull;
            x ^= x >> 33; x *= 0xFF51AFD7ED558CCDull; x ^= x >> 33;
            ps3_put32(L->blob + 0x00, (uint32_t)x);
            uint8_t flags = 0;
            if (members != count) flags |= PS3_MATCH_SPECTATORS;
            for (uint32_t k = 0; k < count && k < 2; k++)
                if (ps3_be32(L->blob + 0x20 + k * PS3_ENTRANT_SIZE) & PS3_SLOT_NO_LINK) flags |= PS3_MATCH_RELAY;
            L->blob[0x05] = flags;
            uint32_t rtt = 0;
            for (uint32_t k = 0; k < count && k < 2; k++) {
                if (fighter[k] == me) continue;
                uint32_t r = ps3_rtt_ms(L, fighter[k]);
                if (r > rtt) rtt = r;
            }
            ps3_put32(L->blob + 0x1C, rtt);
            ps3_note(L, "PS3: both fighters ready; the match is on (rtt %u ms, flags %02X)", rtt, flags);
            ps3_owner_set_phase(L, PS3_PHASE_MATCH, 0, 0, now_us);
            break;
        }

        case PS3_PHASE_MATCH: {
            /* The PS3 owner moves on when its own match is over, as it sees it;
             * a PS3 not fighting watches the match. We do not watch, so a
             * non-fighting owner waits for both fighters to publish their
             * places in line. */
            bool done = fighter_gone;
            if (my_pos) {
                done = done || (L->match_seen && !L->match);
            } else {
                bool both = true;
                for (uint32_t k = 0; k < count && k < 2; k++)
                    if (!(ps3_member_flags(L, fighter[k]) & PS3_MFLAG_ROTATED)) both = false;
                done = done || both;
            }
            if (!done && now_us > L->phase_since_us + PS3_MATCH_MAX_US) {
                ps3_note(L, "PS3: nobody reported the end of the match");
                done = true;
            }
            if (done) ps3_owner_set_phase(L, PS3_PHASE_RESULTS, PS3_ROOM_CLOSED_HIDDEN, 0, now_us);
            break;
        }

        case PS3_PHASE_RESULTS: {
            bool busy = false;
            if (ps3_be32(L->me) & (PS3_MFLAG_IN_MATCH | PS3_MFLAG_ROTATED)) busy = true;
            for (uint32_t i = 0; i < RPCN_MAX_PEERS && !busy; i++) {
                const rpcn_peer_t *p = &s->peers[i];
                if (p->used && p->bin_len >= 4 && (ps3_be32(p->bin) & (PS3_MFLAG_IN_MATCH | PS3_MFLAG_ROTATED)))
                    busy = true;
            }
            if (!busy) {
                char who[160];
                int w = snprintf(who, sizeof(who), "%08X", ps3_be32(L->me));
                for (uint32_t i = 0; i < RPCN_MAX_PEERS && w > 0 && w < (int)sizeof(who); i++)
                    if (s->peers[i].used)
                        w += snprintf(who + w, sizeof(who) - (size_t)w, " %s=%08X/%u", s->peers[i].npid,
                                      s->peers[i].bin_len >= 4 ? ps3_be32(s->peers[i].bin) : 0u,
                                      (unsigned)s->peers[i].bin_len);
                ps3_note(L, "PS3: results over after %.1f s (flags %s); back to the lobby",
                         (double)(now_us > L->phase_since_us ? now_us - L->phase_since_us : 0) / 1e6, who);
                L->lobby_until_us = now_us + PS3_LOBBY_US;
                ps3_owner_set_phase(L, PS3_PHASE_LOBBY, 0, 0, now_us);
            }
            break;
        }

        default:
            ps3_owner_to_lobby(L, "an unknown phase", now_us);
            break;
    }
}

/* What a member does in the room each pump (np_session_update_room_phase's
 * non-owner half), and, when we own the room, the owner's half after it. */
static inline void ps3_room_pump(ps3_link_t *L, uint64_t now_us) {
    ps3_read_room(L);
    if (!L->room_known) return;
    if (L->phase == PS3_PHASE_PREPARING && L->my_side >= 0 && !L->entrant_sent && !ps3_is_owner(L)) {
        ps3_peer_t *owner = ps3_peer_by_id(L, L->session->owner_id);
        bool linked = ps3_linked_to_fighters(L);
        if (owner && owner->rudp_up && rudp_open(&owner->rudp, 1)
            && (linked || now_us > L->seen_since_us + PS3_LINK_WAIT_US)) {
            ps3_set_flags(L, ps3_be32(L->me) | PS3_MFLAG_READY);
            ps3_send_entrant(L, !linked);
            L->entrant_sent = true;
            ps3_note(L, "PS3: entrant data sent; ready as %s%s", L->my_side == 0 ? "1P" : "2P",
                     linked ? "" : " (no direct link to the other fighter)");
        }
    }
    /* Not fighting, or our board never saw the result: our place in line
     * follows the result the fighters publish. */
    if ((L->phase == PS3_PHASE_MATCH || L->phase == PS3_PHASE_RESULTS) && !L->match && !L->rotated) {
        uint32_t win = ps3_published_winner(L);
        if (win) ps3_rotate(L, win);
        /* A fighter whose match ended with no result (its lockstep gave out)
         * still says it is done, as the PS3 does (rotate_queue with no winner
         * changes nothing but sets bit 29), or a non-fighting owner would sit
         * in phase 3 until PS3_MATCH_MAX_US. It gives the other fighter's
         * result a moment to arrive first. */
        else if (L->my_side >= 0 && L->match_seen && now_us > L->results_since_us + PS3_NO_RESULT_US)
            ps3_rotate(L, 0);
    }
    /* After the result screen the flags go back to 0, and the owner moves on
     * once nobody is still marked in the match. (The stamps are taken with a
     * later clock than `now_us`, inside this pump: compare, never subtract, or
     * the wait underflows and ends at once.) A PS3 member clears them some
     * 10 s after publishing its place in line (the captured match); 8 s after
     * the later of phase 4 and our own result keeps the rotation on show. */
    if (L->phase == PS3_PHASE_RESULTS && !L->results_cleared && !L->match
        && now_us > L->results_since_us + PS3_RESULTS_US) {
        L->results_cleared = true;
        ps3_set_flags(L, 0);
    }
    ps3_publish_me(L);
    ps3_owner_pump(L, now_us);
}

/* ---- Peers: signaling, then RUDP ----------------------------------------- */

static inline void ps3_peers_pump(ps3_link_t *L, uint64_t now_us) {
    rpcn_session_t *s = L->session;
    /* Mirror the room's members. */
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
        ps3_peer_t *p = &L->peers[i];
        if (p->used && !rpcn_session_peer(s, p->member_id)) {
            if (p->rudp_up) rudp_close_all(&p->rudp);
            rpcs3_sig_finish(&L->sig, p->npid);
            ps3_note(L, "PS3: %s left", p->npid);
            /* RPCN hands the room to whoever is left, and we run only a room we
             * created; a room with nobody else in it is no use either. Our own
             * room stays open for the next player. */
            if (!L->hosting && (p->member_id == s->owner_id || rpcn_session_peer_count(s) == 0))
                L->owner_gone = true;
            memset(p, 0, sizeof(*p));
        }
    }
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
        const rpcn_peer_t *rp = &s->peers[i];
        if (!rp->used || !rp->npid[0]) continue;
        ps3_peer_t *p = ps3_peer_by_id(L, rp->member_id);
        if (!p) {
            for (uint32_t k = 0; k < RPCN_MAX_PEERS && !p; k++) if (!L->peers[k].used) p = &L->peers[k];
            if (!p) continue;
            memset(p, 0, sizeof(*p));
            p->used = true;
            p->link = L;
            p->member_id = rp->member_id;
            snprintf(p->npid, sizeof(p->npid), "%s", rp->npid);
        }
        rpcs3_sig_peer_t *sp = rpcs3_sig_find(&L->sig, p->npid);
        if ((!sp || (!sp->connecting && !sp->active && !sp->dead)) && rp->ip && rp->port)
            sp = rpcs3_sig_start(&L->sig, p->npid, rp->ip, rp->port);
        if (!sp) continue;
        if (!p->rudp_up && rpcs3_sig_linked(sp)) {
            rudp_peer_init(&p->rudp, sp->ip, sp->port, now_us ^ ((uint64_t)p->member_id << 40),
                           ps3_link_send_udp, s, ps3_deliver_cb, p);
            p->rudp_up = true;
            /* Channel 1 exists only between the owner and each member; 2 and 3
             * between every pair (the PS3's FUN_000b6884 connect loop). */
            uint64_t ms = now_us / 1000u;
            if (ps3_is_owner(L) || p->member_id == s->owner_id) rudp_connect(&p->rudp, 1, ms);
            rudp_connect(&p->rudp, 2, ms);
            rudp_connect(&p->rudp, 3, ms);
            ps3_note(L, "PS3: opening RUDP to %s", p->npid);
        }
        if (p->rudp_up) {
            p->rudp.ip = sp->ip;       /* signaling follows the peer; so do we */
            p->rudp.port = sp->port;
            bool was_open = rudp_all_open(&p->rudp);
            rudp_pump(&p->rudp, now_us / 1000u);
            (void)was_open;
        }
    }
    rpcs3_sig_pump(&L->sig, now_us);
}

/* ---- Lifetime ------------------------------------------------------------ */

static inline void ps3_link_begin(ps3_link_t *L, rpcn_session_t *s, ps3_log_fn log, void *log_ctx) {
    memset(L, 0, sizeof(*L));
    L->active  = true;
    L->session = s;
    L->log     = log;
    L->log_ctx = log_ctx;
    L->sig.log = ps3_sig_log_cb;
    L->sig.log_ctx = L;
    rpcs3_sig_init(&L->sig, s->npid, ps3_link_send_udp, s);
    /* A fresh member: no flags, no battle points yet, last in line. */
    memset(L->me, 0, sizeof(L->me));
    ps3_put32(L->me + 4, 0xFFFFFFFFu);
    L->team = 0xFF;
    L->my_side = -1;
}

/* The member attribute to join a room with. */
static inline const uint8_t *ps3_link_member_bin(const ps3_link_t *L) { return L->me; }

/* Leaving the room: tell every peer, and give the board back. */
static inline void ps3_link_leave(ps3_link_t *L) {
    if (!L->active) return;
    ps3_match_end(L, "left the room");
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++)
        if (L->peers[i].used && L->peers[i].rudp_up) rudp_close_all(&L->peers[i].rudp);
    rpcs3_sig_finish_all(&L->sig);
    rpcs3_sig_pump(&L->sig, net_now_us());
    memset(L->peers, 0, sizeof(L->peers));
    L->owner_gone = false;
    L->room_known = false;
    L->room_rev_seen = 0;
    L->entrant_sent = false;
    L->rotated = false;
    L->hosting = false;
    memset(L->me, 0, sizeof(L->me));
    ps3_put32(L->me + 4, 0xFFFFFFFFu);
    L->team = 0xFF;
    L->my_side = -1;
}

/*
 * Before creating a room of our own (rpcn_session_host_ps3): the room's first
 * state and its eight searchable ints, as a PS3 creating a Player Match room
 * with the default rules writes them (MatchCond_SetDefaults 0xAF230, checked
 * against a PS3's CreateJoinRoom): room mode 1, rounds 3 and time 30
 * (indices 1 and 1), game type A, no secret characters, worldwide. Byte 8 and
 * int 0x4C are the player count's index, 2 players = 0.
 */
static inline void ps3_link_host(ps3_link_t *L, uint32_t max_slot, uint32_t int_attr[8]) {
    ps3_link_leave(L);
    if (max_slot < 2) max_slot = 2;
    if (max_slot > RPCN_ROOM_MAX_MEMBERS) max_slot = RPCN_ROOM_MAX_MEMBERS;
    uint64_t now = net_now_us();
    L->hosting  = true;
    L->max_slot = max_slot;
    memset(L->blob, 0, sizeof(L->blob));
    L->blob[0x04] = 1;                          /* room mode: Room Match */
    L->blob[0x08] = (uint8_t)(max_slot - 2u);   /* players */
    L->blob[0x09] = 1;                          /* rounds: 3 */
    L->blob[0x0A] = 1;                          /* time: 30 */
    const uint32_t ints[8] = { max_slot - 2u, 1, 1, 0, 0, 0, 1, RPCN_PS3_VERSION_TAG };
    memcpy(int_attr, ints, sizeof(ints));
    L->phase_since_us = now;
    L->lobby_until_us = now + PS3_LOBBY_US;
    L->last_write_us  = now;                    /* the create carries the blob */
    L->write_filter = L->write_attr = 0;
    L->own_entrant = L->match_seen = false;
}

static inline void ps3_link_stop(ps3_link_t *L) {
    ps3_link_leave(L);
    L->active = false;
}

/* A datagram from the room's socket. */
static inline void ps3_link_on_datagram(ps3_link_t *L, uint32_t ip, uint16_t port,
                                        const uint8_t *buf, uint32_t len, uint64_t now_us) {
    if (!L->active) return;
    ps3_wire_packet("RX", ip, port, buf, len);
    if (rpcs3_sig_on_datagram(&L->sig, ip, port, buf, len, now_us)) return;
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
        ps3_peer_t *p = &L->peers[i];
        if (!p->used || !p->rudp_up || p->rudp.ip != ip || p->rudp.port != port) continue;
        rudp_on_datagram(&p->rudp, buf, len, now_us / 1000u);
        return;
    }
}

/*
 * The board should run without pacing:
 *   * a PS3 match has begun and our board has not reached its forced START yet
 *     (a reboot is on its way there), and the PS3 gives up on a SyncStart
 *     after 10 s;
 *   * the board has fallen more than `delay` frames behind our own sampling.
 *     A PS3's board never does, but ours can (a heavy loading frame), and the
 *     PS3's rule then stops sampling -- our inputs stop -- until it has caught
 *     up, which at 60 Hz pacing it never would.
 */
static inline bool ps3_link_hurry(const ps3_link_t *L) {
    if (!L->active || !L->match) return false;
    if (!L->sio.sync_active) return true;
    return L->sio.passed && L->sio.side < 2 && L->sio.sample - L->sio.play > L->sio.delay;
}

/* Once per emu-thread pump. `local_wire` is this machine's input as a wire
 * byte. False when the match had to be abandoned. */
static inline bool ps3_link_pump(ps3_link_t *L, uint8_t local_wire, uint64_t now_us) {
    if (!L->active) return true;
    bool ok = true;
    if (rpcn_session_in_room(L->session)) {
        ps3_peers_pump(L, now_us);
        ps3_room_pump(L, now_us);
    }
    if (!L->match) { L->next_tick_us = 0; return true; }

    /* The PS3 runs its lockstep once per 60 Hz host frame, whatever the board
     * is doing; so do we. */
    /* One tick per 1/60 s of wall clock. A pump that comes late runs the ticks
     * it owes, up to 100 ms of them; past that the clock starts over rather
     * than burst (next_tick_us is usually AHEAD of now: compare, never subtract). */
    if (!L->next_tick_us || now_us > L->next_tick_us + 100000u) L->next_tick_us = now_us;
    while (now_us >= L->next_tick_us) {
        ps3_sio_sample(L, local_wire);
        if (!ps3_sio_gate(L)) { ok = false; break; }
        L->next_tick_us += PS3_TICK_US;
    }
    if (!ok) {
        ps3_match_end(L, "the lockstep timed out");
        ps3_after_result(L);
    }
    return ok;
}

/*
 * The board is about to run a frame (called once per slice; a slice that ends
 * mid-frame is asked again for the same frame). Sets `in[0]`/`in[1]` to the two
 * players' wire bytes for it and says whether it may run.
 *
 * Before the barrier the board runs free with no input at all, as the PS3's
 * does in its network mode, except that it waits between the forced START and
 * both sides reaching the same generation (the PS3's speed 0 there). From the
 * barrier on, frame n plays ring entry n of both sides and does not run until
 * both are in (SyncIo_InjectFrameInputs).
 */
static inline ps3_step_t ps3_link_frame(ps3_link_t *L, uint8_t in[2]) {
    in[0] = in[1] = 0;
    if (!L->active || !L->match) return PS3_STEP_OFF;
    ps3_sio_t *S = &L->sio;
    if (!S->passed) {
        if (S->sync_active && !S->finished && !S->gen_ok) return PS3_STEP_WAIT;
        if (g_xplay_barrier != 2) g_xplay_barrier = (S->gen_ok && S->play >= 0) ? 1 : 0;
        return PS3_STEP_READY;
    }
    bool ready = S->speed;
    for (uint32_t k = 0; k < S->nplayers && ready; k++) if (!ps3_ring_has(S, k, S->play)) ready = false;
    if (!ready) {
        L->stalled_frames++;
        return PS3_STEP_WAIT;
    }
    uint32_t par = S->gen & 1u, i = (uint32_t)S->play & (PS3_RING - 1);
    in[0] = (uint8_t)S->ring_input[0][par][i];
    in[1] = (uint8_t)S->ring_input[1][par][i];
    L->cur_in[0] = in[0];
    L->cur_in[1] = in[1];
    L->frame_live = true;
    return PS3_STEP_READY;
}

/*
 * The board finished a frame. Advances the play frame for a frame that ran on
 * lockstep inputs, and takes what the board hooks saw during it.
 * `versus_result` is the profile's versus hook (1 = 1P won, 2 = 2P won).
 */
static inline void ps3_link_end_frame(ps3_link_t *L, int versus_result) {
    if (!L->active) return;
    int ev = g_xplay_events;
    g_xplay_events = 0;
    if (!L->match) return;
    ps3_sio_t *S = &L->sio;
    if (L->frame_live) {
        L->frame_live = false;
        S->play++;
        S->sync_timeout = 180;
        /* A frame's inputs are spent once played; the next tick's gate looks at
         * the next frame. */
        bool ready = true;
        for (uint32_t k = 0; k < S->nplayers; k++) if (!ps3_ring_has(S, k, S->play)) ready = false;
        if (S->side < 2 && S->sample - S->play < S->delay) ready = false;
        S->speed = ready;
    }
    if (ev & XPLAY_EV_NEW_GENERATION) ps3_sio_begin_generation(L);
    if (ev & XPLAY_EV_BARRIER) {
        S->barrier_hit = false;
        S->passed = true;
        S->resend_ss = 0;      /* FUN_0006c924 clears +0xC4, the SyncStart resend */
        ps3_note(L, "PS3: past the barrier at character select; frame %d plays next", S->play);
    }
    if (versus_result == 1 || versus_result == 2) {
        L->last_result = (uint16_t)versus_result;
        ps3_note(L, "PS3: the match was decided: %s won", versus_result == 1 ? "1P" : "2P");
    }
    if (ev & XPLAY_EV_MATCH_OVER) {
        ps3_match_end(L, "the board reached VIC_INT");
        ps3_after_result(L);
    }
}

#endif /* PS3_LINK_H */
