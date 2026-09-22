/*
 * room.h — a room of up to eight: who fights, who waits, who watches, and who
 * plays next. Pure logic, like lockstep.h: no sockets, no board, so every rule
 * here can be held by tests/net_test.c.
 *
 * ── WHERE THIS COMES FROM ─────────────────────────────────────────────────
 *
 * The PS3 port (NPUB30927) calls it Room Match: two to eight people in one RPCN
 * room, two of them on the cabinet and the rest watching the match, with the
 * line moving after every result. It is reverse-engineered from the port's
 * NPMatching2Session / TaskSession code (addresses below are in its EBOOT):
 *
 *   * THE OWNER RUNS THE ROOM, AND THE ROOM LIVES ON THE SERVER. The owner
 *     writes the room's shared state into room internal binary attribute 0x57
 *     (`np_session_set_room_data_internal` 0xB9410), and each member writes its
 *     own into member internal attribute 0x59 (0xB92CC). Nobody sends the room
 *     state peer to peer, and because it is on the server a new owner simply
 *     carries on from it when the old one leaves -- the port has no host
 *     migration code at all.
 *   * PICKING THE FIGHTERS (`np_session_build_fight_entries` 0xBEBD0). Members
 *     are taken in line order. A member who pressed "1P Entry" or "2P Entry"
 *     jumps the line for that side ("The top players in 1P Entry and 2P Entry get
 *     priority", UI string 0x72); otherwise whoever is first in line fills the
 *     side that is left.
 *   * AFTER A RESULT (`np_session_rotate_queue_after_match` 0xBE788): the winner
 *     goes to the FRONT of the line and keeps their side (their entry request is
 *     set to it); the loser goes to the BACK and loses theirs. Everyone else keeps
 *     their order. Arcade rules, in other words: winner stays on.
 *   * STATS (`OnMatchResult_evt15` 0xACB6C): both fighters get a game; the winner
 *     a win. Battle points are +4 for a win and +1 for a loss.
 *
 * ── WHERE THIS DIFFERS, AND WHY ───────────────────────────────────────────
 *
 *   * THE LINE IS THE OWNER'S, IN THE ROOM STATE. The port keeps it as each
 *     member's RPCN teamId, and every member re-sorts and republishes its own
 *     after a result. That is a distributed computation over asynchronous
 *     updates: a member who applies the rotation after hearing somebody else's
 *     NEW teamId sorts against a half-rotated line. One writer cannot race
 *     itself, so here the owner rotates the line and publishes it whole; the
 *     rules it rotates by are the port's.
 *   * NO ENTRANT EXCHANGE, NO CLOSED ROOM. The port closes the room while a
 *     match is set up and has the fighters hand the owner a 100-byte settings
 *     block. Here the match is a cold board reset on every machine, so there is
 *     nothing to hand over, and a newcomer can join at any time and is
 *     simply last in line.
 *   * A MEMBER MAY SIT OUT (ROOM_MEMBER_WATCH). The port picks everybody in turn;
 *     a room here can hold people who only came to watch.
 *   * VS MODE CAN SKIP THE RESET. With the owner's VS mode on, a decided match
 *     sends every board back to character select with both players still in.
 *     When those two are the only players, the next match is that rematch, on
 *     the boards already running (room_vs_continues): `match` counts on while
 *     `session` stays on the match that did the cold boot. The results, the
 *     stats and the line are handled exactly as for any other match.
 */
#ifndef ROOM_H
#define ROOM_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ROOM_MAX_MEMBERS 8u
#define ROOM_NO_MEMBER   0u      /* RPCN member ids start at 16 (CREATOR_ROOM_MEMBER_ID) */

/* ---- The room's shared state (room attribute 0x57, written by the owner) ---- */

typedef enum {
    ROOM_PHASE_LOBBY = 0,   /* nobody is fighting; the owner starts the next match */
    ROOM_PHASE_MATCH = 1,   /* `match` is being played by `fighter[0]` and `fighter[1]` */
} room_phase_t;

/* The owner will start the next match on its own once the countdown runs out
 * (set after a result, so a rotation room keeps rolling like a cabinet). */
#define ROOM_FLAG_AUTO 0x01u

#define ROOM_RESULT_NONE 0xFFu   /* aborted, or not played yet */

typedef struct {
    uint8_t  phase;
    uint8_t  flags;
    uint8_t  frame_delay;
    uint8_t  last_result;              /* side that won `match`: 0 = 1P, 1 = 2P */
    uint16_t match;                    /* 1, 2, ...; 0 = none started yet */
    uint16_t fighter[2];               /* member ids on 1P and 2P for `match` */
    uint32_t seed;
    uint8_t  line_count;
    uint16_t line[ROOM_MAX_MEMBERS];   /* the waiting line, front first */
    /* The region every board in `match` cold-boots as (hle_hooks.h g_region,
     * a backup-RAM setting). The owner's: two boards that disagree on it are
     * running two different games from frame 0. */
    uint8_t  region;
    /* VS mode for `match` (hle_hooks.h g_vs_mode), the owner's like `region`:
     * a decided match sends the board back to character select with both
     * players still in. */
    uint8_t  vs_mode;
    /* The match whose cold boot the boards are running `match` on. Equal to
     * `match` for every match that began with a board reset. In VS mode, when
     * the same two players play on, the next match is a rematch on the running
     * boards: `match` moves on and `session` stays where it was. */
    uint16_t session;
} room_state_t;

#define ROOM_STATE_MAGIC   0x4D52324Du   /* "M2RM" */
#define ROOM_STATE_VERSION 1u
#define ROOM_STATE_SIZE    (24u + 2u * ROOM_MAX_MEMBERS)

static inline void room_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void room_put32(uint8_t *p, uint32_t v) { room_put16(p, (uint16_t)v); room_put16(p + 2, (uint16_t)(v >> 16)); }
static inline uint16_t room_get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t room_get32(const uint8_t *p) { return room_get16(p) | ((uint32_t)room_get16(p + 2) << 16); }

/* Little-endian and fixed-size, so every build reads every other's. */
static inline uint32_t room_state_encode(const room_state_t *s, uint8_t *out) {
    memset(out, 0, ROOM_STATE_SIZE);
    room_put32(out + 0, ROOM_STATE_MAGIC);
    out[4] = ROOM_STATE_VERSION;
    out[5] = s->phase;
    out[6] = s->flags;
    out[7] = s->frame_delay;
    room_put16(out + 8,  s->match);
    room_put16(out + 10, s->fighter[0]);
    room_put16(out + 12, s->fighter[1]);
    out[14] = s->last_result;
    out[15] = s->line_count > ROOM_MAX_MEMBERS ? ROOM_MAX_MEMBERS : s->line_count;
    room_put32(out + 16, s->seed);
    for (uint32_t i = 0; i < out[15]; i++) room_put16(out + 20 + 2 * i, s->line[i]);
    out[20 + 2 * ROOM_MAX_MEMBERS] = s->region;
    out[21 + 2 * ROOM_MAX_MEMBERS] = s->vs_mode;
    room_put16(out + 22 + 2 * ROOM_MAX_MEMBERS, s->session);
    return ROOM_STATE_SIZE;
}

/* False for anything that is not ours: a room nobody has written yet, or one
 * made by a build that stores something else there. */
static inline bool room_state_decode(const uint8_t *in, uint32_t len, room_state_t *s) {
    memset(s, 0, sizeof(*s));
    if (len < ROOM_STATE_SIZE || room_get32(in) != ROOM_STATE_MAGIC || in[4] != ROOM_STATE_VERSION)
        return false;
    s->phase       = in[5];
    s->flags       = in[6];
    s->frame_delay = in[7];
    s->match       = room_get16(in + 8);
    s->fighter[0]  = room_get16(in + 10);
    s->fighter[1]  = room_get16(in + 12);
    s->last_result = in[14];
    s->line_count  = in[15] > ROOM_MAX_MEMBERS ? ROOM_MAX_MEMBERS : in[15];
    s->seed        = room_get32(in + 16);
    for (uint32_t i = 0; i < s->line_count; i++) s->line[i] = room_get16(in + 20 + 2 * i);
    s->region      = in[20 + 2 * ROOM_MAX_MEMBERS];
    s->vs_mode     = in[21 + 2 * ROOM_MAX_MEMBERS];
    s->session     = room_get16(in + 22 + 2 * ROOM_MAX_MEMBERS);
    return true;
}

/* ---- A member's own state (member attribute 0x59, written by that member) ---- */

#define ROOM_MEMBER_READY    0x01u   /* pressed Start: wants the first match to begin */
#define ROOM_MEMBER_WATCH    0x02u   /* sitting out: never picked to fight */

typedef enum {
    ROOM_ENTRY_NONE = 0,   /* play when it is my turn, on whichever side is free */
    ROOM_ENTRY_1P   = 1,   /* "1P Entry": jump the line for the 1P side */
    ROOM_ENTRY_2P   = 2,   /* "2P Entry" */
} room_entry_t;

typedef struct {
    uint8_t  flags;
    uint8_t  entry;
    /* The match this member is running a board for right now (0 = none). Lets
     * the owner tell a fighter who has not got going yet from one who has gone. */
    uint16_t playing;
    /* The last result this member's board saw: `result_match` was won by side
     * `result`. A result is a fact about a board, and every board in the match
     * reaches it on the same frame; the owner may not have been running one. */
    uint16_t result_match;
    uint8_t  result;
    uint8_t  pad;
    uint16_t games, wins, points;
} room_member_data_t;

#define ROOM_MEMBER_VERSION 1u
#define ROOM_MEMBER_SIZE    16u

static inline uint32_t room_member_encode(const room_member_data_t *m, uint8_t *out) {
    memset(out, 0, ROOM_MEMBER_SIZE);
    out[0] = ROOM_MEMBER_VERSION;
    out[1] = m->flags;
    out[2] = m->entry;
    out[3] = m->result;
    room_put16(out + 4,  m->playing);
    room_put16(out + 6,  m->result_match);
    room_put16(out + 8,  m->games);
    room_put16(out + 10, m->wins);
    room_put16(out + 12, m->points);
    return ROOM_MEMBER_SIZE;
}

static inline bool room_member_decode(const uint8_t *in, uint32_t len, room_member_data_t *m) {
    memset(m, 0, sizeof(*m));
    m->result = ROOM_RESULT_NONE;
    if (len < ROOM_MEMBER_SIZE || in[0] != ROOM_MEMBER_VERSION) return false;
    m->flags        = in[1];
    m->entry        = in[2] <= ROOM_ENTRY_2P ? in[2] : ROOM_ENTRY_NONE;
    m->result       = in[3];
    m->playing      = room_get16(in + 4);
    m->result_match = room_get16(in + 6);
    m->games        = room_get16(in + 8);
    m->wins         = room_get16(in + 10);
    m->points       = room_get16(in + 12);
    return true;
}

/* A member as the room logic sees one. `known` = its attribute has been read. */
typedef struct {
    uint16_t           id;
    bool               known;
    room_member_data_t data;
} room_member_t;

static inline const room_member_t *room_find(const room_member_t *m, uint32_t n, uint16_t id) {
    for (uint32_t i = 0; i < n; i++) if (m[i].id == id) return &m[i];
    return NULL;
}

/* ---- The line ------------------------------------------------------------- */

static inline int room_line_index(const room_state_t *s, uint16_t id) {
    for (uint32_t i = 0; i < s->line_count; i++) if (s->line[i] == id) return (int)i;
    return -1;
}

/*
 * Make the line match the room: members who left come out, members who joined
 * go on the end in member-id order (RPCN hands out ids in join order within a
 * slot). True if anything changed. The owner runs this whenever the member list
 * moves, including when it has just inherited the room.
 */
static inline bool room_line_sync(room_state_t *s, const room_member_t *m, uint32_t n) {
    bool changed = false;
    uint32_t k = 0;
    for (uint32_t i = 0; i < s->line_count; i++) {
        if (room_find(m, n, s->line[i])) s->line[k++] = s->line[i];
        else changed = true;
    }
    s->line_count = (uint8_t)k;

    /* Newcomers, lowest id first. */
    for (;;) {
        uint16_t next = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (room_line_index(s, m[i].id) >= 0) continue;
            if (!next || m[i].id < next) next = m[i].id;
        }
        if (!next || s->line_count >= ROOM_MAX_MEMBERS) break;
        s->line[s->line_count++] = next;
        changed = true;
    }
    return changed;
}

/* Players: members in the line who have not chosen to sit out. */
static inline uint32_t room_player_count(const room_state_t *s, const room_member_t *m, uint32_t n) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < s->line_count; i++) {
        const room_member_t *mm = room_find(m, n, s->line[i]);
        if (mm && !(mm->data.flags & ROOM_MEMBER_WATCH)) count++;
    }
    return count;
}

/* Every player has pressed Start (members whose attribute has not arrived yet
 * count as not ready). */
static inline bool room_all_ready(const room_state_t *s, const room_member_t *m, uint32_t n) {
    uint32_t players = 0;
    for (uint32_t i = 0; i < s->line_count; i++) {
        const room_member_t *mm = room_find(m, n, s->line[i]);
        if (!mm || (mm->known && (mm->data.flags & ROOM_MEMBER_WATCH))) continue;
        if (!mm->known || !(mm->data.flags & ROOM_MEMBER_READY)) return false;
        players++;
    }
    return players >= 2;
}

/*
 * Who fights next: `np_session_build_fight_entries`, over the owner's line.
 *
 * Each player gets a sort key: its place in the line, raised by class. The first
 * player asking for 1P is class 0, the first asking for 2P class 0x04000000,
 * anybody else who asked for a side 0x40000000, and everybody who did not ask
 * 0x80000000. The two lowest keys fight. A picked player who did not win a side
 * of their own is a FILLER, and fillers re-sort to 0x02000000 -- between the two
 * classes -- which is what puts a filler on whichever side the other fighter did
 * not ask for: [1P entrant][filler], or [filler][2P entrant], or two fillers in
 * line order.
 *
 * False when there are fewer than two players.
 */
static inline bool room_pick_fighters(const room_state_t *s, const room_member_t *m, uint32_t n,
                                      uint16_t out[2]) {
    uint32_t key[ROOM_MAX_MEMBERS];
    uint16_t id[ROOM_MAX_MEMBERS];
    uint32_t count = 0;
    bool have_1p = false, have_2p = false;

    for (uint32_t i = 0; i < s->line_count; i++) {
        const room_member_t *mm = room_find(m, n, s->line[i]);
        if (!mm || (mm->data.flags & ROOM_MEMBER_WATCH)) continue;
        uint32_t k = i;
        uint8_t  e = mm->known ? mm->data.entry : ROOM_ENTRY_NONE;
        if (e == ROOM_ENTRY_1P && !have_1p)      { have_1p = true; }
        else if (e == ROOM_ENTRY_2P && !have_2p) { have_2p = true; k |= 0x04000000u; }
        else if (e != ROOM_ENTRY_NONE)           { k |= 0x40000000u; }
        else                                     { k |= 0x80000000u; }
        key[count] = k;
        id[count]  = mm->id;
        count++;
    }
    out[0] = out[1] = ROOM_NO_MEMBER;
    if (count < 2) return false;

    /* The two smallest keys. */
    uint32_t a = 0, b = 1;
    if (key[b] < key[a]) { a = 1; b = 0; }
    for (uint32_t i = 2; i < count; i++) {
        if (key[i] < key[a])      { b = a; a = i; }
        else if (key[i] < key[b]) { b = i; }
    }

    /* Fillers between the classes (the port's own test: any of the top four
     * bits, which a 2P entrant's 0x04000000 is not), then side order. */
    uint32_t ka = (key[a] & 0xF0000000u) ? ((key[a] & 0x00FFFFFFu) | 0x02000000u) : key[a];
    uint32_t kb = (key[b] & 0xF0000000u) ? ((key[b] & 0x00FFFFFFu) | 0x02000000u) : key[b];
    if (ka <= kb) { out[0] = id[a]; out[1] = id[b]; }
    else          { out[0] = id[b]; out[1] = id[a]; }
    return true;
}

/*
 * Move the line after `winner_side` won the match `s` describes:
 * `np_session_rotate_queue_after_match`. The winner goes to the front, the loser
 * to the back, and everybody else keeps their order. Returns the loser's id (or
 * ROOM_NO_MEMBER) so the caller can see what happened.
 *
 * Only the line moves here. The winner's entry request -- which the port sets to
 * the side they won on, so they stay there -- is the winner's own attribute and
 * the winner sets it (room_after_result).
 */
static inline uint16_t room_rotate_line(room_state_t *s, uint32_t winner_side) {
    if (winner_side > 1) return ROOM_NO_MEMBER;
    uint16_t winner = s->fighter[winner_side];
    uint16_t loser  = s->fighter[winner_side ^ 1];
    uint16_t line[ROOM_MAX_MEMBERS];
    uint32_t k = 0;
    if (room_line_index(s, winner) >= 0) line[k++] = winner;
    for (uint32_t i = 0; i < s->line_count; i++)
        if (s->line[i] != winner && s->line[i] != loser) line[k++] = s->line[i];
    if (room_line_index(s, loser) >= 0) line[k++] = loser;
    memcpy(s->line, line, sizeof(line[0]) * k);
    s->line_count = (uint8_t)k;
    return loser;
}

/*
 * What a member does to its OWN attribute when its board reaches a result, or
 * when it learns of one it did not see: `OnMatchResult_evt15` for the stats,
 * and the entry request `rotate_queue` sets. `me` is this member's id. Returns
 * true when the attribute changed and should be republished.
 */
static inline bool room_after_result(room_member_data_t *me_data, uint16_t me,
                                     const room_state_t *s, uint16_t match, uint32_t winner_side) {
    if (winner_side > 1 || me_data->result_match == match) return false;
    me_data->result_match = match;
    me_data->result       = (uint8_t)winner_side;
    if (s->match == match && (s->fighter[0] == me || s->fighter[1] == me)) {
        uint32_t my_side = (s->fighter[0] == me) ? 0u : 1u;
        me_data->games++;
        if (my_side == winner_side) {
            me_data->wins++;
            me_data->points = (uint16_t)(me_data->points + 4u);
            me_data->entry  = (uint8_t)(my_side == 0 ? ROOM_ENTRY_1P : ROOM_ENTRY_2P);
        } else {
            me_data->points = (uint16_t)(me_data->points + 1u);
            me_data->entry  = ROOM_ENTRY_NONE;
        }
    }
    return true;
}

/*
 * VS mode: after a result, do the two who just fought play again on the boards
 * already running, with no reset? Only when nobody else is waiting to play.
 * With a line, the rotation brings somebody else on, and a new fighter needs a
 * cold boot like any new match. The sides stay as they are, because the board
 * keeps them.
 *
 * Counting players and not asking room_pick_fighters is deliberate. The owner
 * decides the moment a result arrives, and the fighters' entry requests are
 * their own attributes and may still say what they said before the result. A
 * pick made from those can bring back the loser ahead of somebody waiting.
 */
static inline bool room_vs_continues(const room_state_t *s, const room_member_t *m, uint32_t n) {
    if (!s->vs_mode || s->phase != ROOM_PHASE_MATCH) return false;
    if (room_player_count(s, m, n) != 2) return false;
    for (uint32_t side = 0; side < 2; side++) {
        const room_member_t *mm = room_find(m, n, s->fighter[side]);
        if (!mm || room_line_index(s, s->fighter[side]) < 0) return false;
        if (mm->known && (mm->data.flags & ROOM_MEMBER_WATCH)) return false;
    }
    return true;
}

/* A result for `match` that any member's board has reported, or -1. */
static inline int room_reported_result(const room_member_t *m, uint32_t n, uint16_t match) {
    for (uint32_t i = 0; i < n; i++) {
        if (!m[i].known || m[i].data.result_match != match) continue;
        if (m[i].data.result <= 1) return m[i].data.result;
    }
    return -1;
}

/* This member's role in the room's current match: 0 = 1P, 1 = 2P, -1 = not fighting. */
static inline int room_side_of(const room_state_t *s, uint16_t id) {
    if (s->phase != ROOM_PHASE_MATCH || !id) return -1;
    if (s->fighter[0] == id) return 0;
    if (s->fighter[1] == id) return 1;
    return -1;
}

#endif /* ROOM_H */
