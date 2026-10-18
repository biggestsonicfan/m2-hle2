/*
 * comm_role.h — the room's role for a linked comm board (Pinboard #969, step 3
 * of the RPCN plan in VIRTUAL-ON-PS3.md).
 *
 * The room makes its owner the MASTER cabinet and the other player the SLAVE.
 * A Virtual-On ROM learns its role only at boot: InitNetwork reads it from
 * backup RAM in the frame a blank board factory-fills that RAM, so nothing
 * written from outside lands between the two (profiles/vonj.h). What a real
 * cabinet does is what works: the role goes into the backup block the ROM has
 * made (the profile's link_role, with the ROM's own CRC), and the board is
 * reset with its backup RAM kept -- a power cycle with the battery in -- so the
 * next boot finds a valid block and reads the role from it.
 *
 *   comm_role_begin   the role and the other cabinet; the board goes on alone
 *   comm_role_reset   at a frame edge: once the profile has written the role,
 *                     reset the board into it, then plug the peer in
 *   comm_role_end     the board alone again
 *
 * The peer is plugged in only at that reset, so the first boot (alone) sends
 * nothing. After it, the board sends nothing until its ROM starts the comm
 * board (CN 1, CommBoardReset bit 0): m2comm.h makes no pass before that. That
 * is the "transmit only once the ROM has released its comm board" of YAMP's
 * notes, on each side.
 *
 * A link session's board is not the player's, as a netplay session's is not:
 * comm_role_begin detaches the battery (backup_ram.h) before anything is
 * written, so neither the role nor what the session writes reaches the
 * player's backup RAM.
 *
 * Emu thread, emu mutex held, board between slices.
 */
#ifndef NET_COMM_ROLE_H
#define NET_COMM_ROLE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "backup_ram.h"
#include "game_profile.h"
#include "m2comm.h"
#include "memory.h"

typedef enum { COMM_ROLE_OFF, COMM_ROLE_WAITING, COMM_ROLE_LINKED } comm_role_phase_t;

typedef struct {
    comm_role_phase_t    phase;
    game_link_role_t     role;
    const m2comm_peer_t *peer;
    uint32_t             waited;                 /* frame edges until the role was written */
    uint8_t              back[BACKUP_RAM_KEEP];  /* the board's backup RAM across the reset */
} comm_role_t;

static comm_role_t g_comm_role;

static inline const char *comm_role_name(game_link_role_t role) {
    return role == GAME_LINK_MASTER ? "MASTER" : role == GAME_LINK_SLAVE ? "SLAVE" : "alone";
}

/* Link the board to `peer` as `role`, from its next reset on. Anything linked
 * before is unplugged: the board is alone until then. */
static inline void comm_role_begin(game_link_role_t role, const m2comm_peer_t *peer) {
    backup_ram_detach();
    m2comm_set_peer(NULL);
    g_comm_role.phase  = COMM_ROLE_WAITING;
    g_comm_role.role   = role;
    g_comm_role.peer   = peer;
    g_comm_role.waited = 0;
}

static inline bool comm_role_waiting(void) { return g_comm_role.phase == COMM_ROLE_WAITING; }
static inline bool comm_role_linked(void)  { return g_comm_role.phase == COMM_ROLE_LINKED; }

/* A frame edge while waiting: if the profile can write the role now, reset the
 * board with `reset` (a cold boot: the frontend's install, as the netplay
 * barrier's) keeping its backup RAM, and plug the peer in. True if it did. */
static inline bool comm_role_reset(memory_bus_t *bus, const game_profile_t *p,
                                   void (*reset)(void *ctx), void *ctx) {
    if (g_comm_role.phase != COMM_ROLE_WAITING || !p || !p->link_role || !reset) return false;
    if (!p->link_role(bus, g_comm_role.role)) { g_comm_role.waited++; return false; }
    memcpy(g_comm_role.back, bus->back, BACKUP_RAM_KEEP);
    reset(ctx);
    memcpy(bus->back, g_comm_role.back, BACKUP_RAM_KEEP);
    m2comm_set_peer(g_comm_role.peer);
    g_comm_role.phase = COMM_ROLE_LINKED;
    return true;
}

static inline void comm_role_end(void) {
    if (g_comm_role.phase == COMM_ROLE_LINKED) m2comm_set_peer(NULL);
    g_comm_role.phase = COMM_ROLE_OFF;
    g_comm_role.peer  = NULL;
}

#endif /* NET_COMM_ROLE_H */
