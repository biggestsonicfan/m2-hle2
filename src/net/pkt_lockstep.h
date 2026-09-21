/*
 * pkt_lockstep.h -- lockstep.h over a host's RELIABLE packet channel, for the
 * libretro core's "Online play: RetroArch" (RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE).
 *
 * RetroArch does the part netplay.h does with RPCN: it hosts (with a password,
 * if the player set one), lists the session in its lobby, connects the players
 * and carries the bytes. What it cannot do is its own netplay, which rolls back
 * through savestates this emulator does not have. The netpacket interface hands
 * the simulation back to the core, so the core runs the same model netplay.h
 * does -- a cold board reset on both machines at a barrier, then delay-based
 * lockstep from frame 0 -- over RetroArch's connection instead of a UDP socket.
 *
 * The channel is TCP: reliable and ordered. So none of netplay.h's loss
 * handling is needed here -- no resend while stalled, no paced re-announce, no
 * stray-datagram filter -- and one announce each way releases the barrier. The
 * redundancy in each record stays because it is part of lockstep_record_t.
 *
 * Like lockstep.h this knows nothing about sockets or the board: the core gives
 * it a send function, hands it what arrives, and asks it each frame whether the
 * board may run. Single-threaded: every call is from retro_run or a netpacket
 * callback, which RetroArch makes on the same thread.
 *
 * Handshake, host = player 0, the one RetroArch calls client 0:
 *   host   start(0)                 waits for a client
 *   host   connected(id)            HELLO {delay, generation}, then ANNOUNCE
 *   client HELLO                    adopts delay + generation, ANNOUNCE back
 *   both   peer's ANNOUNCE          barrier released: RESET, then frame 0
 * A third player is refused at connected(); there are two sides to a cabinet.
 */
#ifndef PKT_LOCKSTEP_H
#define PKT_LOCKSTEP_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lockstep.h"

/* The packet types lockstep.h does not have. */
#define PKT_PACKET_HELLO   16u
#define PKT_HELLO_MAGIC    0x524C324Du   /* "M2LR" */
#define PKT_HELLO_VERSION  1u

/* A peer that has stopped sending for this long has gone, whatever the socket
 * says. The board carries on alone after it; see pkt_lockstep_end(). */
#define PKT_STALL_TIMEOUT_MS 15000u

#pragma pack(push, 1)
typedef struct {
    lockstep_header_t header;
    uint32_t magic;
    uint32_t version;
    uint32_t game_tag;       /* which ROM set; both must be running the same one */
    uint32_t frame_delay;    /* the host's; the client adopts it */
} pkt_hello_t;
#pragma pack(pop)

typedef enum {
    PKT_OFF,          /* no session */
    PKT_WAIT_PEER,    /* host with nobody connected yet, or client before HELLO */
    PKT_SYNCING,      /* announced; waiting for the peer's announce */
    PKT_PLAYING,
    PKT_ENDED,        /* the peer left or stalled out; the board runs alone */
} pkt_state_t;

typedef enum {
    PKT_STEP_OFF,     /* not in a session: run the board on local input */
    PKT_STEP_WAIT,    /* do not run the board this time */
    PKT_STEP_RESET,   /* cold-reset the board now, then ask again */
    PKT_STEP_READY,   /* run one slice with *w0, *w1 */
} pkt_step_t;

typedef struct {
    pkt_state_t state;
    lockstep_t  ls;
    bool        is_host;
    bool        have_peer;
    uint16_t    peer_id;
    uint32_t    local_player;
    uint32_t    frame_delay;
    uint32_t    generation;
    uint32_t    game_tag;
    uint32_t    frame;          /* netplay frame: game frames since the reset */
    bool        reset_pending;

    /* The frame check: the same value, computed by the caller, as netplay.h's. */
    uint32_t    check_frame[LOCKSTEP_RING_SIZE], check_value[LOCKSTEP_RING_SIZE];
    uint32_t    peer_check_frame[LOCKSTEP_RING_SIZE], peer_check_value[LOCKSTEP_RING_SIZE];
    uint32_t    last_check_frame, last_check_value;
    uint32_t    desync_frame;   /* LOCKSTEP_NO_CHECK while in agreement */

    uint64_t    stall_since_ms;
    uint64_t    last_peer_ms;
    uint32_t    stalls;

    void      (*send)(const void *buf, size_t len, void *ud);
    void       *send_ud;

    /* The latest thing worth telling the player; `note_seq` moves when it changes. */
    char        note[160];
    uint32_t    note_seq;
} pkt_lockstep_t;

static inline void pkt_note(pkt_lockstep_t *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->note, sizeof p->note, fmt, ap);
    va_end(ap);
    p->note_seq++;
}

static inline void pkt_check_clear(pkt_lockstep_t *p) {
    memset(p->check_frame, 0xFF, sizeof p->check_frame);
    memset(p->peer_check_frame, 0xFF, sizeof p->peer_check_frame);
    p->last_check_frame = LOCKSTEP_NO_CHECK;
    p->last_check_value = 0;
    p->desync_frame     = LOCKSTEP_NO_CHECK;
}

static inline void pkt_header(const pkt_lockstep_t *p, lockstep_header_t *h, uint8_t type) {
    memset(h, 0, sizeof *h);
    h->type       = type;
    h->player     = (uint8_t)p->local_player;
    h->generation = (uint8_t)(p->generation & 0x1Fu);
    h->session    = p->game_tag;
}

static inline void pkt_send_announce(pkt_lockstep_t *p) {
    lockstep_announce_packet_t pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt_header(p, &pkt.header, LOCKSTEP_PACKET_ANNOUNCE);
    p->send(&pkt, sizeof pkt, p->send_ud);
}

/* Both boards start this round from a cold reset at the barrier. */
static inline void pkt_begin_round(pkt_lockstep_t *p) {
    lockstep_configure(&p->ls, p->local_player, 2, p->frame_delay);
    lockstep_begin_round(&p->ls, p->generation);
    pkt_check_clear(p);
    p->frame          = 0;
    p->stall_since_ms = 0;
    p->state          = PKT_SYNCING;
    pkt_send_announce(p);
}

/* The frontend opened a session. client_id 0 is the host. */
static inline void pkt_lockstep_start(pkt_lockstep_t *p, uint16_t client_id, uint32_t frame_delay,
                                      uint32_t game_tag,
                                      void (*send)(const void *, size_t, void *), void *ud) {
    uint32_t gen = p->generation;   /* survives a session: late packets of the last one stay fenced off */
    memset(p, 0, sizeof *p);
    p->generation   = (gen + 1) & 0x1Fu;
    p->is_host      = client_id == 0;
    p->local_player = p->is_host ? 0 : 1;
    p->frame_delay  = frame_delay ? frame_delay : 2;
    p->game_tag     = game_tag;
    p->send         = send;
    p->send_ud      = ud;
    p->state        = PKT_WAIT_PEER;
    pkt_check_clear(p);
    if (p->is_host) pkt_note(p, "Netplay: hosting - waiting for a player (input delay %u)", p->frame_delay);
    else            pkt_note(p, "Netplay: connected - waiting for the host");
}

/* Host only: somebody connected. False turns them away. */
static inline bool pkt_lockstep_connected(pkt_lockstep_t *p, uint16_t client_id) {
    if (!p->is_host || p->have_peer || p->state != PKT_WAIT_PEER) return false;
    p->have_peer  = true;
    p->peer_id    = client_id;
    p->generation = (p->generation + 1) & 0x1Fu;   /* a new round: fence off the last player's packets */

    pkt_hello_t h;
    memset(&h, 0, sizeof h);
    pkt_header(p, &h.header, PKT_PACKET_HELLO);
    h.magic       = PKT_HELLO_MAGIC;
    h.version     = PKT_HELLO_VERSION;
    h.game_tag    = p->game_tag;
    h.frame_delay = p->frame_delay;
    p->send(&h, sizeof h, p->send_ud);
    pkt_begin_round(p);
    pkt_note(p, "Netplay: player 2 joined - both boards restart now");
    return true;
}

/* The session is over (the frontend stopped it, or the peer left): the board is
 * the local player's again. */
static inline void pkt_lockstep_end(pkt_lockstep_t *p, const char *why) {
    if (p->state == PKT_OFF || p->state == PKT_ENDED) return;
    /* A host is still hosting: the next player to connect starts a new round. */
    p->state = p->is_host ? PKT_WAIT_PEER : PKT_ENDED;
    p->have_peer = false;
    pkt_note(p, "Netplay: %s", why);
}

static inline void pkt_lockstep_disconnected(pkt_lockstep_t *p, uint16_t client_id) {
    if (p->have_peer && client_id == p->peer_id) pkt_lockstep_end(p, "the other player left");
}

static inline void pkt_lockstep_stop(pkt_lockstep_t *p) {
    pkt_lockstep_end(p, "session closed");
    p->state = PKT_OFF;
}

static inline void pkt_lockstep_receive(pkt_lockstep_t *p, const void *buf, size_t len, uint64_t now_ms) {
    if (len < sizeof(lockstep_header_t) || p->state == PKT_OFF || p->state == PKT_ENDED) return;
    lockstep_header_t h;
    memcpy(&h, buf, sizeof h);
    p->last_peer_ms = now_ms;

    if (h.type == PKT_PACKET_HELLO) {
        if (p->is_host || len < sizeof(pkt_hello_t)) return;
        pkt_hello_t hello;
        memcpy(&hello, buf, sizeof hello);
        if (hello.magic != PKT_HELLO_MAGIC || hello.version != PKT_HELLO_VERSION) {
            pkt_lockstep_end(p, "the host runs an incompatible core version");
            return;
        }
        if (hello.game_tag != p->game_tag) {
            pkt_lockstep_end(p, "the host is running a different game");
            return;
        }
        p->have_peer   = true;
        p->peer_id     = 0;
        p->frame_delay = hello.frame_delay;
        p->generation  = h.generation & 0x1Fu;
        pkt_begin_round(p);
        pkt_note(p, "Netplay: joined as player 2 (input delay %u) - both boards restart now", p->frame_delay);
        return;
    }
    if (h.session != p->game_tag) return;

    if (h.type == LOCKSTEP_PACKET_ANNOUNCE && len >= sizeof(lockstep_announce_packet_t)) {
        lockstep_on_peer_announce(&p->ls, h.player, h.generation);
    } else if (h.type == LOCKSTEP_PACKET_INPUT && len >= sizeof(lockstep_input_packet_t)) {
        lockstep_input_packet_t pkt;
        memcpy(&pkt, buf, sizeof pkt);
        lockstep_on_record(&p->ls, &pkt.record);
        if (pkt.check_frame != LOCKSTEP_NO_CHECK && (h.generation & 0x1Fu) == p->ls.generation) {
            uint32_t idx = pkt.check_frame & LOCKSTEP_RING_MASK;
            p->peer_check_frame[idx] = pkt.check_frame;
            p->peer_check_value[idx] = pkt.check_value;
        }
    }
}

/* May the board run? `local` is this player's input word (netplay.h's layout,
 * NP_BIT_*); on READY, *w0 / *w1 are the two players' words for this frame. */
static inline pkt_step_t pkt_lockstep_begin_frame(pkt_lockstep_t *p, uint32_t local, uint64_t now_ms,
                                                  uint32_t *w0, uint32_t *w1) {
    switch (p->state) {
        case PKT_OFF:
        case PKT_ENDED:
        case PKT_WAIT_PEER:   /* nobody to wait for yet: play on until the barrier's reset */
            return PKT_STEP_OFF;
        case PKT_SYNCING:
            if (!lockstep_barrier_released(&p->ls)) return PKT_STEP_WAIT;
            p->state = PKT_PLAYING;
            p->reset_pending = true;
            break;
        case PKT_PLAYING:
            break;
    }
    if (p->reset_pending) {
        p->reset_pending = false;
        return PKT_STEP_RESET;
    }

    /* Service and test belong to the cabinet: only player 0 may send them. */
    if (p->local_player != 0) local &= 0x3FFu;
    uint32_t send_frame = p->frame + p->ls.frame_delay;
    if (p->ls.last_local_frame == LOCKSTEP_INVALID_FRAME || send_frame > p->ls.last_local_frame) {
        /* The first frames of a session have no input from anyone yet: fill them
         * with a neutral pad, as netplay_seed_delay_frames does, or frame 0
         * waits for a sample that was never going to be taken. */
        lockstep_input_packet_t pkt;
        if (p->ls.last_local_frame == LOCKSTEP_INVALID_FRAME) {
            for (uint32_t f = 0; f < p->ls.frame_delay; f++) {
                memset(&pkt, 0, sizeof pkt);
                pkt_header(p, &pkt.header, LOCKSTEP_PACKET_INPUT);
                pkt.check_frame = LOCKSTEP_NO_CHECK;
                lockstep_submit_local(&p->ls, f, 0, &pkt.record);
                p->send(&pkt, sizeof pkt, p->send_ud);
            }
        }
        memset(&pkt, 0, sizeof pkt);
        pkt_header(p, &pkt.header, LOCKSTEP_PACKET_INPUT);
        pkt.check_frame = p->last_check_frame;
        pkt.check_value = p->last_check_value;
        lockstep_submit_local(&p->ls, send_frame, local, &pkt.record);
        p->send(&pkt, sizeof pkt, p->send_ud);
    }

    if (!lockstep_ready(&p->ls, p->frame)) {
        if (p->stall_since_ms == 0) {
            p->stall_since_ms = now_ms;
            p->stalls++;
        } else if (now_ms - p->stall_since_ms > PKT_STALL_TIMEOUT_MS) {
            pkt_lockstep_end(p, "the other player stopped sending - playing on alone");
            return PKT_STEP_OFF;
        }
        return PKT_STEP_WAIT;
    }
    p->stall_since_ms = 0;
    *w0 = lockstep_input_for(&p->ls, 0, p->frame);
    *w1 = lockstep_input_for(&p->ls, 1, p->frame);
    return PKT_STEP_READY;
}

/* A slice ended on a game frame: record this frame's check and move on. */
static inline void pkt_lockstep_end_frame(pkt_lockstep_t *p, uint32_t check) {
    if (p->state != PKT_PLAYING) return;
    uint32_t frame = p->frame, idx = frame & LOCKSTEP_RING_MASK;
    p->check_frame[idx]  = frame;
    p->check_value[idx]  = check;
    p->last_check_frame  = frame;
    p->last_check_value  = check;
    p->frame = frame + 1;

    /* Compare whatever pair is complete: the peer's check for a frame reaches
     * us a frame or more after our own. */
    for (uint32_t back = 0; back < 64 && back <= frame && p->desync_frame == LOCKSTEP_NO_CHECK; back++) {
        uint32_t f = frame - back, i = f & LOCKSTEP_RING_MASK;
        if (p->check_frame[i] == f && p->peer_check_frame[i] == f && p->check_value[i] != p->peer_check_value[i]) {
            p->desync_frame = f;
            pkt_note(p, "Netplay: DESYNC at frame %u - the two boards have diverged", f);
        }
    }
}

static inline bool pkt_lockstep_playing(const pkt_lockstep_t *p) { return p->state == PKT_PLAYING; }

#endif /* PKT_LOCKSTEP_H */
