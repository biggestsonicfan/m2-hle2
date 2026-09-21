/*
 * lockstep.h — the netcode core: input rings, packet formats, the round barrier
 * and frame accounting.
 *
 * Deliberately free of sockets, of the i960 and of anything else in this tree, so
 * it can be reasoned about on its own: the session hands it bytes, netplay.h
 * hands it input words. Modelled on the Sonic The Fighters PS3 netcode, the same
 * model yampnet ported:
 *
 *   * DELAY-BASED LOCKSTEP. Inputs are keyed by ABSOLUTE FRAME NUMBER into a
 *     per-player ring. The simulation may advance frame N only once every
 *     player's input for N is known. There is no rollback and no state
 *     snapshotting — the PS3 had none either, and neither does this emulator.
 *   * REDUNDANCY. Every packet re-carries the last LOCKSTEP_REDUNDANCY frames of
 *     that player's input, so a dropped datagram is repaired by the next one
 *     instead of stalling. This is why the transport can be lossy and unordered
 *     and still never desync.
 *   * NEWEST-WINS INSERTION. A ring slot is overwritten only when the incoming
 *     frame is newer than what is there, which makes ingest idempotent and safe
 *     against duplicates and reordering — the property that makes the redundancy
 *     free.
 *   * GENERATION. A 5-bit round counter fences off inputs belonging to a previous
 *     round, so late packets from the session that just ended cannot poison the
 *     new one.
 *
 * WHAT A "ROUND" IS HERE. In yampnet a round is one match inside a running
 * emulator. This emulator has no savestates, so a netplay session instead starts
 * from a COLD BOARD RESET on both machines (netplay.h drives that) and every
 * frame from boot is lockstepped. A generation is therefore one whole session
 * from reset, and bumping it is how a session is restarted without leaving the
 * room.
 */
#ifndef LOCKSTEP_H
#define LOCKSTEP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* 1024 frames of history per player, as on PS3 (~17 s at 60 Hz). Power of two:
 * the ring index is frame & LOCKSTEP_RING_MASK. */
#define LOCKSTEP_RING_SIZE 1024u
#define LOCKSTEP_RING_MASK (LOCKSTEP_RING_SIZE - 1u)

/* Frames of input re-sent in every packet. The PS3 used 10; that is the loss
 * burst we can absorb without stalling. */
#define LOCKSTEP_REDUNDANCY 10u

/* Two players: one cabinet's worth of Model 2 I/O. The rings are per-player, so
 * raising this is mostly the barrier mask and the transport. */
#define LOCKSTEP_MAX_PLAYERS 2u

#define LOCKSTEP_INVALID_FRAME 0xFFFFFFFFu

/* Sent with every input packet: a value that must be identical on both machines
 * for a given frame. LOCKSTEP_NO_CHECK means this peer has not completed a frame
 * yet. It rides along rather than getting its own packet because a datagram is
 * already going out every frame, and a check lost with its packet costs nothing —
 * the next frame carries the next one. */
#define LOCKSTEP_NO_CHECK 0xFFFFFFFFu

typedef enum {
    LOCKSTEP_PACKET_INPUT    = 0,
    LOCKSTEP_PACKET_ANNOUNCE = 1,
    /* The host's session seed, published while merely IN A ROOM. Carries the same
     * payload as an announce and is deliberately NOT the same packet: an announce
     * feeds the barrier, and a seed heartbeat must never be able to release one. */
    LOCKSTEP_PACKET_SEED     = 2,
} lockstep_packet_type_t;

#pragma pack(push, 1)

typedef struct {
    uint8_t  type;
    uint8_t  player;
    uint8_t  generation;
    uint8_t  reserved;
    /* Low 32 bits of the room id. Game traffic is plain P2P between two
     * addresses, so a LEFTOVER PROCESS from a previous test — same machines, same
     * port, same player ids, same generation — is otherwise indistinguishable from
     * the real peer and can join a session it was never in. Stamping the room
     * makes cross-room traffic self-identifying and free to drop. 0 means "room
     * unknown", which only happens before a room is taken. */
    uint32_t session;
} lockstep_header_t;

/* Wire record. Fixed size, little-endian, no padding surprises — every member is
 * 4-byte aligned and the struct is memcpy'd straight into the datagram. */
typedef struct {
    uint32_t frame;                          /* newest frame carried */
    uint32_t packed;                         /* player << 29 | generation << 24 */
    uint32_t inputs[LOCKSTEP_REDUNDANCY];    /* [0] = frame, [1] = frame-1, … */
} lockstep_record_t;

typedef struct {
    lockstep_header_t header;
    lockstep_record_t record;
    uint32_t          check_frame;
    uint32_t          check_value;
} lockstep_input_packet_t;

/*
 * The round announcement, which doubles as seed distribution.
 *
 * m2-hle2 does not NEED a shared RNG seed the way the PS3 port did: both peers
 * cold-reset the board at the barrier and feed it identical inputs, so their
 * generators agree by construction. The field is still here and still
 * host-owned, because it is also the session nonce — it is what a log can quote
 * to prove two machines believe they are in the same session — and because a game
 * profile that ever seeds itself from something outside the simulation (a
 * free-running timer, a host clock) needs a value both sides already agree on,
 * and this is it.
 */
typedef struct {
    lockstep_header_t header;
    uint32_t          seed;
} lockstep_announce_packet_t;

#pragma pack(pop)

_Static_assert(sizeof(lockstep_header_t) == 8, "lockstep header must stay 8 bytes");
_Static_assert(sizeof(lockstep_record_t) == 8 + LOCKSTEP_REDUNDANCY * 4,
               "lockstep record must stay packed");

static inline uint32_t lockstep_pack(uint32_t player, uint32_t generation) {
    return (player << 29) | ((generation & 0x1Fu) << 24);
}
static inline uint32_t lockstep_unpack_player(uint32_t packed) { return packed >> 29; }
static inline uint32_t lockstep_unpack_generation(uint32_t packed) { return (packed >> 24) & 0x1Fu; }

/* ---- One player's input history ------------------------------------------ */

typedef struct {
    struct { uint32_t frame, input; } slots[LOCKSTEP_RING_SIZE];
    bool     valid[LOCKSTEP_RING_SIZE];
    uint32_t newest;
} lockstep_ring_t;

static inline void lockstep_ring_clear(lockstep_ring_t *r) {
    memset(r->slots, 0, sizeof(r->slots));
    memset(r->valid, 0, sizeof(r->valid));
    r->newest = LOCKSTEP_INVALID_FRAME;
}

/* Newest-wins. True if the slot was updated, i.e. this was new information.
 *
 * An occupied slot holding a frame >= ours is either this same frame arriving
 * again (a duplicate, or one of the redundant copies) or a NEWER frame that has
 * already lapped the ring — in both cases the stored value is the one to keep, so
 * the write is simply dropped. That is what makes redundant re-sends free and
 * reordering harmless. */
static inline bool lockstep_ring_insert(lockstep_ring_t *r, uint32_t frame, uint32_t input) {
    uint32_t idx = frame & LOCKSTEP_RING_MASK;
    if (r->valid[idx] && r->slots[idx].frame >= frame) return false;
    r->slots[idx].frame = frame;
    r->slots[idx].input = input;
    r->valid[idx] = true;
    if (r->newest == LOCKSTEP_INVALID_FRAME || frame > r->newest) r->newest = frame;
    return true;
}

/* True if the ring currently holds this exact frame. False once it has been
 * overwritten by a newer one that aliases to the same slot. */
static inline bool lockstep_ring_has(const lockstep_ring_t *r, uint32_t frame) {
    uint32_t idx = frame & LOCKSTEP_RING_MASK;
    return r->valid[idx] && r->slots[idx].frame == frame;
}

static inline uint32_t lockstep_ring_get(const lockstep_ring_t *r, uint32_t frame) {
    uint32_t idx = frame & LOCKSTEP_RING_MASK;
    return (r->valid[idx] && r->slots[idx].frame == frame) ? r->slots[idx].input : 0u;
}

/* ---- The per-session state machine --------------------------------------- */

typedef struct {
    lockstep_ring_t rings[LOCKSTEP_MAX_PLAYERS];
    uint32_t local_player;
    uint32_t player_count;
    uint32_t frame_delay;
    uint32_t generation;
    uint32_t announce_mask;      /* bit per player that has announced `generation` */
    uint32_t last_local_frame;
    uint32_t stalls;
} lockstep_t;

static inline void lockstep_configure(lockstep_t *l, uint32_t local_player,
                                      uint32_t player_count, uint32_t frame_delay) {
    l->local_player = (local_player < LOCKSTEP_MAX_PLAYERS) ? local_player : 0;
    l->player_count = (player_count == 0 || player_count > LOCKSTEP_MAX_PLAYERS)
                    ? LOCKSTEP_MAX_PLAYERS : player_count;
    l->frame_delay      = frame_delay;
    l->generation       = 0;
    l->announce_mask    = 0;
    l->last_local_frame = LOCKSTEP_INVALID_FRAME;
    l->stalls           = 0;
    for (uint32_t i = 0; i < LOCKSTEP_MAX_PLAYERS; i++) lockstep_ring_clear(&l->rings[i]);
}

/* Start announcing `generation`, clearing all per-round state so stale inputs
 * from the previous session cannot survive into this one — the same thing the
 * generation field defends against on the wire. */
static inline void lockstep_begin_round(lockstep_t *l, uint32_t generation) {
    l->generation       = generation & 0x1Fu;
    l->announce_mask    = 1u << l->local_player;   /* we have announced by definition */
    l->last_local_frame = LOCKSTEP_INVALID_FRAME;
    for (uint32_t i = 0; i < LOCKSTEP_MAX_PLAYERS; i++) lockstep_ring_clear(&l->rings[i]);
}

static inline void lockstep_on_peer_announce(lockstep_t *l, uint32_t player, uint32_t generation) {
    if (player >= l->player_count) return;
    if ((generation & 0x1Fu) != l->generation) return;   /* not this round */
    l->announce_mask |= (1u << player);
}

static inline bool lockstep_barrier_released(const lockstep_t *l) {
    uint32_t expected = (l->player_count >= 32) ? 0xFFFFFFFFu : ((1u << l->player_count) - 1u);
    return (l->announce_mask & expected) == expected;
}

/* The record whose newest frame is `frame`, read back out of our own ring: that
 * frame plus the previous LOCKSTEP_REDUNDANCY-1. Sending one is idempotent at the
 * other end (newest-wins), which is what lets a record be built and sent again
 * at any time. */
static inline void lockstep_fill_record(const lockstep_t *l, uint32_t frame, lockstep_record_t *out) {
    const lockstep_ring_t *mine = &l->rings[l->local_player];
    memset(out, 0, sizeof(*out));
    out->frame  = frame;
    out->packed = lockstep_pack(l->local_player, l->generation);

    /* inputs[0] is this frame, inputs[i] is frame-i. Frames we do not have (the
     * start of a session, where frame-i underflows) stay 0, a neutral pad. */
    for (uint32_t i = 0; i < LOCKSTEP_REDUNDANCY; i++) {
        if (i > frame) break;
        uint32_t f = frame - i;
        if (lockstep_ring_has(mine, f)) out->inputs[i] = lockstep_ring_get(mine, f);
    }
}

/* Record this machine's input for `frame` and fill `out` with the record to
 * transmit: this frame plus the previous LOCKSTEP_REDUNDANCY-1 from our ring. */
static inline void lockstep_submit_local(lockstep_t *l, uint32_t frame, uint32_t input,
                                         lockstep_record_t *out) {
    lockstep_ring_t *mine = &l->rings[l->local_player];
    lockstep_ring_insert(mine, frame, input);
    if (l->last_local_frame == LOCKSTEP_INVALID_FRAME || frame > l->last_local_frame)
        l->last_local_frame = frame;

    if (out) lockstep_fill_record(l, frame, out);
}

/*
 * The oldest frame of OURS that a peer can still be waiting for.
 *
 * A peer whose newest input reached us is for frame N was itself at frame
 * N - delay when it sampled it, and it cannot have needed anything of ours from
 * before that. Nothing heard yet means nothing can be ruled out, so: frame 0.
 *
 * This is the range a STALLED machine has to keep re-sending. Inputs otherwise
 * go out once, when a new local frame is sampled, and the redundancy in each
 * record is carried by the NEXT record -- which a stalled machine never sends.
 * So when both peers are stalled nobody is transmitting at all, and whatever was
 * lost stays lost: with a delay of 2, five consecutive datagrams dropped in one
 * direction (80 ms of bad Wi-Fi) is a session that waits forever. And the newest
 * record alone does not repair it once the delay is large: the frame the peer is
 * missing is 2 * delay + 1 behind our newest, which is past one record's reach
 * for any delay above 4.
 */
static inline uint32_t lockstep_resend_floor(const lockstep_t *l) {
    uint32_t floor = LOCKSTEP_INVALID_FRAME;
    for (uint32_t p = 0; p < l->player_count; p++) {
        if (p == l->local_player) continue;
        uint32_t newest = l->rings[p].newest;
        uint32_t f = (newest == LOCKSTEP_INVALID_FRAME || newest < l->frame_delay)
                   ? 0u : newest - l->frame_delay;
        if (f < floor) floor = f;
    }
    return floor == LOCKSTEP_INVALID_FRAME ? 0u : floor;
}

/* Ingest a received record. Silently drops records from the wrong generation or
 * an out-of-range player — both are normal on a round boundary, not errors.
 *
 * A RECORD FOR THIS ROUND IS ALSO THE PEER'S ANNOUNCE OF IT. A peer sends inputs
 * only once its own barrier has released, which it cannot do without having
 * heard us announce this round -- so it is in this round, whatever became of its
 * announces. And its announces stop the moment its barrier releases. When one
 * side releases on the other's FIRST announce (the host who pressed Start hears
 * the guest accept), every announce it sent before that went to a peer that was
 * not yet in the round and dropped them. Without this the guest waits at the
 * barrier for an announce that is never sent again, and the host stalls at frame
 * 0 until its stall timer ends the session. Over the internet an announce is
 * usually still in flight and the race is rarely lost; over loopback, and through
 * the web build's gateway on one machine, it was lost every time. */
static inline void lockstep_on_record(lockstep_t *l, const lockstep_record_t *rec) {
    uint32_t player = lockstep_unpack_player(rec->packed);
    uint32_t gen    = lockstep_unpack_generation(rec->packed);

    if (player >= l->player_count || gen != l->generation) return;
    /* Never accept a remote record for our own slot — our inputs are
     * authoritative locally, and honouring an echo would let the network rewrite
     * local history. */
    if (player == l->local_player) return;
    l->announce_mask |= (1u << player);

    lockstep_ring_t *ring = &l->rings[player];
    for (uint32_t i = 0; i < LOCKSTEP_REDUNDANCY; i++) {
        if (i > rec->frame) break;
        lockstep_ring_insert(ring, rec->frame - i, rec->inputs[i]);
    }
}

/* True when every player's input for `frame` is known and the sim may advance. */
static inline bool lockstep_ready(const lockstep_t *l, uint32_t frame) {
    for (uint32_t p = 0; p < l->player_count; p++)
        if (!lockstep_ring_has(&l->rings[p], frame)) return false;
    return true;
}

static inline uint32_t lockstep_input_for(const lockstep_t *l, uint32_t player, uint32_t frame) {
    if (player >= l->player_count) return 0;
    return lockstep_ring_get(&l->rings[player], frame);
}

#endif /* LOCKSTEP_H */
