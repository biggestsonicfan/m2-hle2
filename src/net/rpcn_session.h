/*
 * rpcn_session.h — an RPCN session: login, discovery, rooms, the members of the
 * room we are in and their addresses, NAT punching, and then direct
 * peer-to-peer datagrams to any of them.
 *
 * Everything above this (room.h, lockstep.h, netplay.h) asks it to send bytes
 * to a MEMBER and hands it back bytes FROM one; nothing up there knows an
 * address.
 *
 * CONNECTION MODEL — SYMMETRIC, AND IT HAS TO BE.
 *   Every member learns every other member's address from the server, and every
 *   one of them starts sending immediately. A joiner gets the addresses of all
 *   the members already there in its JoinRoom reply (signaling_data); those
 *   members get the joiner's in the UserJoinedRoom notification the server
 *   pushes. Both are populated only because rpcn_create_room asks for signaling
 *   (sigOptParam, MESH -- everyone to everyone, which is what a room of eight
 *   needs and also what the PS3 port asked for).
 *
 *   The tempting model — the newcomer transmits first, the others stay silent
 *   until they hear something, on the theory that the first packet "opens the
 *   return path" — does not work. It opens the path through the SENDER's NAT.
 *   The receiver's NAT has no mapping for the sender at all, so it drops that
 *   first packet on the floor, and a silent receiver never creates one: two
 *   members on different networks sit at the barrier forever. Hole punching only
 *   works if both sides transmit, so the punch starts as soon as an address is
 *   known and keeps a small datagram flowing whether or not anything has come
 *   back.
 *
 * WHAT THIS STILL CANNOT DO: if either side is behind a symmetric NAT, the
 * mapping it opens towards a peer differs from the one the server advertised,
 * and no amount of punching helps — RPCN has no relay to fall back on. The recv
 * path absorbs the milder case (a peer whose port differs from the advertised
 * one) by re-pointing at the source it actually hears from; the punch carries
 * the sender's member id so that works with more than one peer on an address.
 *
 * THE ROOM'S STATE LIVES ON THE SERVER. The room internal binary attribute and
 * each member's member attribute (room.h says what is in them) are read from
 * every reply and notification that carries them and kept here, and a change
 * bumps `room_rev` for the layer above to notice. RPCN does not announce a new
 * OWNER -- when one leaves it picks a successor quietly (room_manager.rs
 * `leave_room`) -- so every departure is followed by a GetRoomDataInternal.
 *
 * BROWSING THE OTHER EMULATOR'S ROOMS. Rooms are created in the m2-hle2 lobby
 * space, but the browser can also search YAMP's space for the same arcade game
 * (com_id.h explains why they must stay apart). Those rows are display-only and
 * the session refuses to join one: it is there so "nobody is online" and "two
 * people are online in an emulator you cannot play against" are different
 * answers. That second search needs its own discovery chain, because the server
 * and world ids are per-ComId.
 *
 * Ported from yampnet's RpcnTransport.{h,cpp} and Account.{h,cpp}.
 */
#ifndef RPCN_SESSION_H
#define RPCN_SESSION_H

#include "com_id.h"
#include "rpcn_client.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* The server drops unauthenticated clients after 10 s and forgets a peer's
 * signaling info if it stops hearing from them, so the keepalive runs for the
 * whole session, not just at login. */
#define RPCN_KEEPALIVE_MS     2000
/* Punch hard while we have not heard back, then a slower heartbeat purely to
 * hold the mapping open: players sit in a room for minutes before starting, and
 * a NAT drops an idle UDP mapping in well under that. */
#define RPCN_PUNCH_MS         250
#define RPCN_PUNCH_IDLE_MS    1000
#define RPCN_SIGNALING_RETRY_MS 2000

/* Keepalive round trips kept for the median, and how often, and by how much it
 * must have moved, before an owner publishes it again. */
#define RPCN_RELAY_SAMPLES      8u
#define RPCN_RELAY_PUBLISH_MS   5000u
#define RPCN_RELAY_PUBLISH_STEP 10u
#define RPCN_ACCOUNT_TIMEOUT_MS 15000

#define RPCN_MAX_ROOMS 32

/* The PS3 port's lobby space and the version tag its rooms carry in searchable
 * int 0x53 (np_session_create_join_room; 20120910, which looks like a date). */
#define RPCN_PS3_COM_ID       "NPWR03869_00"
#define RPCN_PS3_VERSION_TAG  0x0133054Eu

/* Everybody in the room but us. */
#define RPCN_MAX_PEERS (RPCN_ROOM_MAX_MEMBERS - 1u)

/* Not a game packet: shorter than any header the lockstep layer accepts, so it
 * is discarded up there even if one leaks through. Its job is to make our NAT
 * create a mapping towards the peer -- and, by carrying the sender's member id
 * after the tag, to let the peer put a name to an address it was never told. */
static const uint8_t g_rpcn_punch_tag[4] = { 'M', '2', 'N', '!' };
#define RPCN_PUNCH_SIZE 6u

/*
 * An INTRODUCTION: "here is where I hear the others from". Tag, then up to
 * RPCN_MAX_PEERS entries of { u16 member, u32 ip (network order), u16 port }.
 *
 * RPCN alone cannot connect two members who share a public address with a
 * third. It tells each of them the other's LOCAL address with port 3658
 * HARDCODED (room_manager.rs, cmd_misc.rs), so everybody on one address is told
 * everybody else is on 3658 -- which is at most one of them. With two players
 * that never mattered: the one on 3658 hears the other's first datagram and
 * learns the real port from it. With three, the two who are not on 3658 each
 * punch 3658 and never hear each other. Every member already knows where it
 * hears the others from, so each passes that on to the rest: the rendezvous
 * that a room of more than two needs, and one that also lets two members behind
 * one NAT use the mapping a third member sees.
 */
static const uint8_t g_rpcn_intro_tag[4] = { 'M', '2', 'N', 'I' };
#define RPCN_INTRO_ENTRY 8u
#define RPCN_INTRO_MS    1000

typedef enum {
    RPCN_STAGE_IDLE,
    RPCN_STAGE_LOGGING_IN,
    RPCN_STAGE_ONLINE,      /* logged in, discovery done */
    RPCN_STAGE_HOSTING,     /* in a room we created */
    RPCN_STAGE_JOINING,     /* in a room we joined */
    RPCN_STAGE_LINKED,      /* in a room, and at least one member has been heard from */
    RPCN_STAGE_FAILED,
} rpcn_stage_t;

typedef struct {
    const char *server;
    uint16_t    port;
    const char *fingerprint_hex;   /* null/empty = validate chain + host name */
    const char *npid;
    const char *password;
    /* RPCN's e-mail verification token, when the player has one. Null/empty is
     * the normal case and the right one for a server that does not validate
     * accounts by e-mail — see rpcn_login for why the client cannot tell which
     * sort of server it has reached. */
    const char *token;
    const char *com_id;            /* our lobby space, e.g. "M2HSNCFTR_00" */
    const char *com_id_foreign;    /* YAMP's space for the same game, or null */
    uint16_t    local_p2p_port;    /* override for tests only */
    /* Cross-play with the PS3 port (ps3_link.h): its lobby space, its room
     * shape, and no m2hle datagrams (punches, introductions) sent to members,
     * since they are RPCS3 clients and speak RPCS3's P2P framing. */
    bool        ps3;

    /* Optional progress log. Connection problems here are almost always somebody's
     * NAT or firewall rather than a bug, and none of that is diagnosable from
     * "waiting for peer" alone — so the session reports what it learned and from
     * where. */
    void *log_ctx;
    void (*log)(void *ctx, const char *msg);
} rpcn_session_config_t;

/* Another member of the room, and how to reach them. */
typedef struct {
    bool     used;
    uint16_t member_id;
    char     npid[20];
    uint32_t flag_attr;
    uint8_t  team_id;        /* the PS3 game's place in the waiting line (ps3_link.h) */
    uint8_t  bin[RPCN_MEMBER_BIN_MAX];   /* their member attribute (room.h) */
    uint32_t bin_len;

    uint32_t ip;             /* network byte order; 0 = not known yet */
    uint16_t port;
    /* Where another member hears them from (rpcn_session_pump_intro). Punched
     * beside ip:port until one of the two answers. */
    uint32_t alt_ip;
    uint16_t alt_port;
    bool     heard;          /* a datagram has actually arrived from them */
    uint64_t last_punch_ms;
    uint64_t signaling_retry_ms;
    uint64_t pending_signaling;
} rpcn_peer_t;

typedef struct {
    rpcn_client_t client;
    rpcn_stage_t  stage;

    char     com_id[COMID_BUFFER_SIZE];
    char     com_id_foreign[COMID_BUFFER_SIZE];
    char     npid[20];
    /* Whether a token was sent, kept only so a LoginInvalidToken can say whether
     * the fix is correcting the token or supplying one at all. The token itself
     * is used once and deliberately not retained. */
    bool     sent_token;
    /* The server refused the account name or the password themselves, as opposed
     * to failing some other way. netplay.h needs the distinction to tell a dead
     * Twitch login token from a server that merely went away, and the error text
     * is for people, not for strcmp. */
    bool     credential_refused;
    /* What the server said to the login, RPCN_OK until it has refused one. */
    rpcn_error_t login_error;

    uint16_t server_id;
    uint32_t world_id;
    uint64_t room_id;
    uint32_t room_flags;
    bool     is_host;        /* we created this room (the owner may since have moved) */
    bool     ps3;            /* rpcn_session_config_t.ps3 */
    /* We hold a reference on the socket library (net_startup) for the life of
     * the peer-to-peer socket. The TLS layer holds its own only while it is
     * connected, and the UDP socket is opened before it. */
    bool     net_held;

    /* The room we are in. */
    uint16_t    my_member_id;
    uint16_t    owner_id;
    uint32_t    max_slot;
    uint8_t     room_bin[RPCN_ROOM_BIN_MAX];   /* the room's shared state (room.h) */
    uint32_t    room_bin_len;
    uint8_t     my_bin[RPCN_MEMBER_BIN_MAX];   /* our attribute as the server holds it */
    uint32_t    my_bin_len;
    rpcn_peer_t peers[RPCN_MAX_PEERS];
    /* Bumped whenever anything above changes, so netplay.h can tell "the room
     * moved" from "nothing happened" without comparing the lot. */
    uint32_t    room_rev;

    /* The signaling helper has answered a keepalive, so the server has our
     * address on file. Taking a room before that is a mistake: RPCN copies a
     * member's address into the room when it CREATES or JOINS it and never
     * refreshes that copy, so a room taken too soon advertises no address for
     * its owner -- for the life of the room. See netplay.h, "taking a room". */
    bool     signaling_seen;

    /* Outstanding request ids, so replies route without blocking. */
    uint64_t pending_serverlist;
    uint64_t pending_worldlist;
    uint64_t pending_room;
    uint64_t pending_search;
    uint64_t pending_room_data;

    /* The read-only cross-emulator browse: its own discovery, its own search. */
    bool     foreign_ready;
    uint16_t foreign_server_id;
    uint32_t foreign_world_id;
    uint64_t pending_foreign_serverlist;
    uint64_t pending_foreign_worldlist;
    uint64_t pending_foreign_search;

    rpcn_room_listing_t rooms[RPCN_MAX_ROOMS];
    uint32_t            room_count;
    rpcn_room_listing_t foreign_rooms[RPCN_MAX_ROOMS];
    uint32_t            foreign_room_count;

    uint64_t last_keepalive_ms;
    uint64_t last_intro_ms;

    /* Round trips to the signaling helper, one per keepalive answered
     * (rpcn_session_relay_ms), and what we last published of them as a room's
     * owner (RPCN_ROOM_INT_ATTR_RELAY). */
    uint64_t keepalive_sent_us;
    uint32_t relay_rtt_us[RPCN_RELAY_SAMPLES];
    uint32_t relay_count, relay_next;
    uint32_t relay_published_ms;
    uint64_t relay_publish_ms;

    void *log_ctx;
    void (*log)(void *ctx, const char *msg);

    char error[256];
} rpcn_session_t;

/* ---- Small helpers ------------------------------------------------------- */

static inline void rpcn_session_note(rpcn_session_t *s, const char *fmt, ...) {
    if (!s->log) return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s->log(s->log_ctx, buf);
}

static inline void rpcn_session_fail(rpcn_session_t *s, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s->error, sizeof(s->error), fmt, args);
    va_end(args);
    s->stage = RPCN_STAGE_FAILED;
    rpcn_session_note(s, "failed: %s", s->error);
}

static inline const char *rpcn_session_error(const rpcn_session_t *s) {
    return s->error[0] ? s->error : rpcn_last_error(&s->client);
}

static inline bool rpcn_session_in_room(const rpcn_session_t *s) {
    return s->room_id != 0 && (s->stage == RPCN_STAGE_HOSTING || s->stage == RPCN_STAGE_JOINING
                               || s->stage == RPCN_STAGE_LINKED);
}

static inline bool rpcn_session_is_owner(const rpcn_session_t *s) {
    return rpcn_session_in_room(s) && s->my_member_id && s->my_member_id == s->owner_id;
}

/* RPCN account names compare without case (the table is UNIQUE ... NOCASE). */
static inline bool rpcn_same_npid(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return *a == *b;
}

static inline rpcn_peer_t *rpcn_session_peer(rpcn_session_t *s, uint16_t member_id) {
    if (!member_id) return NULL;
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++)
        if (s->peers[i].used && s->peers[i].member_id == member_id) return &s->peers[i];
    return NULL;
}

static inline rpcn_peer_t *rpcn_session_peer_by_npid(rpcn_session_t *s, const char *npid) {
    if (!npid || !npid[0]) return NULL;
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++)
        if (s->peers[i].used && rpcn_same_npid(s->peers[i].npid, npid)) return &s->peers[i];
    return NULL;
}

static inline uint32_t rpcn_session_peer_count(const rpcn_session_t *s) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) if (s->peers[i].used) n++;
    return n;
}

static inline const char *rpcn_peer_addr_text(const rpcn_peer_t *p) {
    static char text[32];
    if (!p || !p->ip || !p->port) return "unknown";
    return net_addr_text(text, sizeof(text), p->ip, p->port);
}

static inline const char *rpcn_peer_name(const rpcn_peer_t *p) {
    return (p && p->npid[0]) ? p->npid : "a member";
}

/* What a rejected login means for the PLAYER. RPCN says which of the three
 * credentials it refused, and that distinction is the difference between a fix
 * that takes ten seconds and a settings page full of boxes to guess at.
 * `sent_token` splits the token case in two, because "the token is wrong" and
 * "this server wanted a token and got none" are not the same problem. */
static inline const char *rpcn_login_error_text(rpcn_error_t error, bool sent_token) {
    switch (error) {
        case RPCN_ERR_LOGIN_BAD_USERNAME:
            return "there is no account of that name on this server";
        case RPCN_ERR_LOGIN_BAD_PASSWORD:
            return "wrong password for that account";
        case RPCN_ERR_LOGIN_BAD_TOKEN:
            return sent_token
                 ? "the verification token was refused - check it against the e-mail the "
                   "server sent, or have a fresh one sent"
                 : "this server verifies accounts by e-mail and no token was given - paste "
                   "the one from the sign-up e-mail into the token field";
        case RPCN_ERR_LOGIN_ALREADY:
            return "that account is already logged in - RPCN allows one session per account, "
                   "so close the other client and wait for the server to drop it";
        case RPCN_ERR_MALFORMED:
            return "the server could not read the login request";
        case RPCN_ERR_DB_FAIL:
            return "the server's database did not answer";
        default:
            return "the server refused the login";
    }
}

/* Adopt an address for a member, whatever told us about it. `source` names that
 * for the log. */
static inline void rpcn_session_set_peer_addr(rpcn_session_t *s, rpcn_peer_t *p,
                                              uint32_t ip, uint16_t port, const char *source) {
    if (!p || !ip || !port) return;
    bool changed = (ip != p->ip || port != p->port);
    /* An address the member has actually been HEARD from beats anything the
     * server says afterwards. The two can disagree -- a room whose copy of an
     * address was taken before the helper had it, or two players the server sees
     * on one public address handed each other's local one -- and replacing a
     * working address with a told one turns every datagram from them into a
     * stray. */
    if (changed && p->heard) {
        char told[32];
        rpcn_session_note(s, "the server says %s is at %s (via %s); keeping %s, which is "
                             "where they are actually heard from", rpcn_peer_name(p),
                          net_addr_text(told, sizeof(told), ip, port), source, rpcn_peer_addr_text(p));
        return;
    }
    p->ip   = ip;
    p->port = port;
    p->signaling_retry_ms = 0;
    if (changed) {
        /* Punch immediately rather than waiting out the interval — this is the
         * moment the hole has to be opened, and they may already be sending. */
        p->last_punch_ms = 0;
        rpcn_session_note(s, "%s at %s (via %s)", rpcn_peer_name(p), rpcn_peer_addr_text(p), source);
    }
}

/* A datagram came from `ip:port` and says it is from `p`. The same rule a first
 * contact always had -- only the PORT may differ from what we were told (a NAT
 * picked another), never the address -- applied per member. */
static inline bool rpcn_session_hear(rpcn_session_t *s, rpcn_peer_t *p, uint32_t ip, uint16_t port) {
    if (!p) return false;
    if (p->heard) return p->ip == ip && p->port == port;
    if (p->ip && p->ip != ip && !(p->alt_ip && p->alt_ip == ip)) return false;
    bool moved = (port != p->port || ip != p->ip);
    p->heard = true;
    p->ip    = ip;
    p->port  = port;
    if (s->stage == RPCN_STAGE_HOSTING || s->stage == RPCN_STAGE_JOINING) s->stage = RPCN_STAGE_LINKED;
    rpcn_session_note(s, moved ? "%s reached us from %s (not the advertised port); using that"
                               : "link established with %s at %s",
                      rpcn_peer_name(p), rpcn_peer_addr_text(p));
    return true;
}

/* ---- The member list ----------------------------------------------------- */

static inline void rpcn_session_clear_room(rpcn_session_t *s) {
    s->room_id      = 0;
    s->room_flags   = 0;
    s->is_host      = false;
    s->my_member_id = 0;
    s->owner_id     = 0;
    s->max_slot     = 0;
    s->room_bin_len = 0;
    s->my_bin_len   = 0;
    s->pending_room_data = 0;
    s->relay_published_ms = 0;
    memset(s->peers, 0, sizeof(s->peers));
    s->room_rev++;
}

/* Add a member or refresh what we know of one. Its address is kept: the server's
 * member data carries none. */
static inline rpcn_peer_t *rpcn_session_upsert_member(rpcn_session_t *s, const rpcn_member_info_t *m) {
    if (!m->member_id) return NULL;
    if (m->member_id == s->my_member_id || (!s->my_member_id && rpcn_same_npid(m->npid, s->npid))) {
        s->my_member_id = m->member_id;
        memcpy(s->my_bin, m->bin, m->bin_len);
        s->my_bin_len = m->bin_len;
        if (m->flag_attr & RPCN_MEMBER_FLAG_OWNER) s->owner_id = m->member_id;
        s->room_rev++;
        return NULL;
    }
    rpcn_peer_t *p = rpcn_session_peer(s, m->member_id);
    bool fresh = false;
    if (!p) {
        for (uint32_t i = 0; i < RPCN_MAX_PEERS && !p; i++)
            if (!s->peers[i].used) p = &s->peers[i];
        if (!p) return NULL;   /* more members than a room can hold: ignore */
        memset(p, 0, sizeof(*p));
        p->used      = true;
        p->member_id = m->member_id;
        fresh = true;
    }
    if (m->npid[0]) snprintf(p->npid, sizeof(p->npid), "%s", m->npid);
    p->flag_attr = m->flag_attr;
    p->team_id   = m->team_id;
    if (m->bin_len) {
        memcpy(p->bin, m->bin, m->bin_len);
        p->bin_len = m->bin_len;
    }
    if (m->flag_attr & RPCN_MEMBER_FLAG_OWNER) s->owner_id = m->member_id;
    if (fresh && s->log) rpcn_session_note(s, "%s is in the room (member %u)", rpcn_peer_name(p), m->member_id);
    s->room_rev++;
    return p;
}

static inline void rpcn_session_remove_member(rpcn_session_t *s, uint16_t member_id) {
    rpcn_peer_t *p = rpcn_session_peer(s, member_id);
    if (!p) return;
    rpcn_session_note(s, "%s left the room", rpcn_peer_name(p));
    memset(p, 0, sizeof(*p));
    s->room_rev++;
}

/* Take the whole room as the server holds it: a create or join reply, or a
 * GetRoomDataInternal. Members who are gone are dropped; the ones who stay keep
 * the addresses we have for them. */
static inline void rpcn_session_apply_room(rpcn_session_t *s, const rpcn_room_info_t *room) {
    if (room->room_id && s->room_id && room->room_id != s->room_id) return;   /* some other room */
    if (room->max_slot) s->max_slot = room->max_slot;
    if (room->owner_id) s->owner_id = room->owner_id;
    if (room->bin_len) {
        memcpy(s->room_bin, room->bin, room->bin_len);
        s->room_bin_len = room->bin_len;
    }
    if (room->member_count) {
        for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
            if (!s->peers[i].used) continue;
            bool present = false;
            for (uint32_t k = 0; k < room->member_count; k++)
                if (room->members[k].member_id == s->peers[i].member_id) present = true;
            if (!present) rpcn_session_remove_member(s, s->peers[i].member_id);
        }
        for (uint32_t k = 0; k < room->member_count; k++) rpcn_session_upsert_member(s, &room->members[k]);
    }
    s->room_rev++;
}

/* ---- Lifetime ------------------------------------------------------------ */

static inline void rpcn_session_stop(rpcn_session_t *s) {
    rpcn_disconnect(&s->client);
    if (s->net_held) { net_shutdown_lib(); s->net_held = false; }
    s->stage      = RPCN_STAGE_IDLE;
    rpcn_session_clear_room(s);
    s->signaling_seen = false;
    s->sent_token = false;
    s->credential_refused = false;
    s->login_error = RPCN_OK;
    s->pending_serverlist = s->pending_worldlist = s->pending_room = 0;
    s->pending_search = 0;
    s->pending_foreign_serverlist = s->pending_foreign_worldlist = s->pending_foreign_search = 0;
    s->foreign_ready = false;
    s->room_count = 0;
    s->foreign_room_count = 0;
    s->error[0] = '\0';
}

static inline bool rpcn_session_start(rpcn_session_t *s, const rpcn_session_config_t *cfg) {
    rpcn_session_stop(s);

    /* Before the first failure can happen. Stop() deliberately leaves these alone
     * so a reconnect keeps logging. */
    s->log     = cfg->log;
    s->log_ctx = cfg->log_ctx;

    if (!cfg->server || !cfg->npid || !cfg->password || !cfg->com_id) {
        rpcn_session_fail(s, "a server, account name, password and communication id are all required");
        return false;
    }

    /* Checked here rather than left to the server: a malformed id surfaces as an
     * InvalidInput three requests into discovery, which reads like the server
     * refusing the account. */
    if (!comid_is_well_formed(cfg->com_id)) {
        rpcn_session_fail(s, "'%s' is not a usable communication id: RPCN needs 9 to 12 "
                             "characters whose first 9 are uppercase letters or digits",
                          cfg->com_id);
        return false;
    }

    snprintf(s->com_id, sizeof(s->com_id), "%s", cfg->com_id);
    snprintf(s->npid, sizeof(s->npid), "%s", cfg->npid);
    s->ps3 = cfg->ps3;
    s->com_id_foreign[0] = '\0';
    if (!s->ps3 && cfg->com_id_foreign && comid_is_well_formed(cfg->com_id_foreign))
        snprintf(s->com_id_foreign, sizeof(s->com_id_foreign), "%s", cfg->com_id_foreign);

    /* The token as the player supplied it, which normally means pasted out of an
     * e-mail. Tidied rather than validated: a value that does not look like one
     * is still sent, because only the server knows what its tokens look like, and
     * refusing here would turn a server change into "netplay stopped working"
     * with nothing to try. */
    char token[128];
    if (!rpcn_normalize_token(cfg->token, token, sizeof(token))) {
        rpcn_session_fail(s, "that verification token is too long to be one - RPCN mails %u characters",
                          (unsigned)RPCN_TOKEN_LENGTH);
        return false;
    }
    s->sent_token = token[0] != '\0';
    if (s->sent_token && !rpcn_looks_like_token(token)) {
        rpcn_session_note(s, "the verification token does not look like one (RPCN mails %u "
                             "hexadecimal characters); sending it anyway", (unsigned)RPCN_TOKEN_LENGTH);
    }

    cert_fingerprint_t pin;
    memset(&pin, 0, sizeof(pin));
    if (cfg->fingerprint_hex && *cfg->fingerprint_hex
        && !cert_fp_from_hex(&pin, cfg->fingerprint_hex)) {
        rpcn_session_fail(s, "that certificate fingerprint is not 64 hexadecimal characters");
        return false;
    }

#ifndef __EMSCRIPTEN__
    /* The peer-to-peer port first: it is the one thing here that fails in a
     * second on a machine already running an emulator, and after the TLS
     * handshake it failed seconds later, with the state still reading "off"
     * the whole time. rpcn_connect clears the client, so the socket waits in
     * a local until it has. (The web build's datagram channel is named after
     * the server the connect picks, so it keeps the old order.) */
    uint16_t p2p_port = cfg->local_p2p_port ? cfg->local_p2p_port : RPCN_P2P_PORT;
    net_sock_t p2p = NET_SOCK_INVALID;
    /* Before any socket: on Windows nothing else may have started Winsock yet
     * (the TLS connect does, but only after this), and socket() then fails
     * with WSANOTINITIALISED (10093). Held until rpcn_session_stop, so a
     * reconnect after the TLS layer let go of its own finds it up too. */
    if (!s->net_held) {
        if (!net_startup()) { rpcn_session_fail(s, "could not start the socket library"); return false; }
        s->net_held = true;
    }
    if (!net_udp_open(&p2p, p2p_port)) {
        int err = net_errno();
#ifdef _WIN32
        bool taken = err == WSAEADDRINUSE;
#else
        bool taken = err == EADDRINUSE;
#endif
        rpcn_session_fail(s, "could not open the peer-to-peer socket: could not bind UDP %u (%d)%s",
                          (unsigned)p2p_port, err,
                          taken ? " - another program (a second emulator?) has it" : "");
        return false;
    }
#endif

    if (!rpcn_connect(&s->client, cfg->server, cfg->port, &pin)) {
#ifndef __EMSCRIPTEN__
        net_close(&p2p);
#endif
        rpcn_session_fail(s, "%s", rpcn_last_error(&s->client));
        return false;
    }
#ifndef __EMSCRIPTEN__
    s->client.udp        = p2p;
    s->client.local_port = p2p_port;
#endif

    /* The signaling socket must exist before login completes, so the keepalive
     * can start the moment we have a user id. */
    if (!rpcn_open_signaling(&s->client, cfg->local_p2p_port)) {
        rpcn_session_fail(s, "could not open the peer-to-peer socket: %s", rpcn_last_error(&s->client));
        return false;
    }

    if (!rpcn_login(&s->client, cfg->npid, cfg->password, token)) {
        rpcn_session_fail(s, "login could not be sent: %s", rpcn_last_error(&s->client));
        return false;
    }

    s->stage = RPCN_STAGE_LOGGING_IN;
    return true;
}

/* ---- Pumps --------------------------------------------------------------- */

static inline void rpcn_session_pump_keepalive(rpcn_session_t *s) {
    if (rpcn_user_id(&s->client) == 0) return;
    uint64_t now = net_now_ms();
    if (now - s->last_keepalive_ms < RPCN_KEEPALIVE_MS) return;
    s->last_keepalive_ms = now;
    if (rpcn_send_signaling_ping(&s->client, 0)) s->keepalive_sent_us = net_now_us();
}

/*
 * Our round trip to RPCN's signaling helper: the median of the last few
 * keepalives, in ms (at least 1), or 0 before the first answer.
 *
 * It stands in for "how far this player is from the relay". A web player's
 * keepalive goes through the gateway, and the gateway runs beside RPCN, so for
 * a browser this IS the trip to the gateway that every one of its datagrams
 * makes. Two browsers talk through that gateway, so the sum of their two trips
 * is the trip between them; a desktop player talks straight to a browser's
 * gateway, which again is where the helper is. Only two desktop players are
 * estimated poorly, since they talk directly and may both be far from RPCN.
 *
 * The answer is read when the socket is next drained, so it carries up to a
 * slice of this end's polling, as the peer-to-peer ping does.
 */
static inline uint32_t rpcn_session_relay_ms(const rpcn_session_t *s) {
    uint32_t n = s->relay_count, v[RPCN_RELAY_SAMPLES];
    if (!n) return 0;
    memcpy(v, s->relay_rtt_us, sizeof(v));
    for (uint32_t a = 1; a < n; a++)
        for (uint32_t b = a; b > 0 && v[b - 1] > v[b]; b--) { uint32_t t = v[b]; v[b] = v[b - 1]; v[b - 1] = t; }
    uint32_t med = (n & 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    uint32_t ms = (med + 500) / 1000;
    return ms ? ms : 1;
}

static inline void rpcn_session_on_keepalive_answer(rpcn_session_t *s) {
    if (!s->keepalive_sent_us) return;   /* one answer per keepalive */
    uint64_t rtt = net_now_us() - s->keepalive_sent_us;
    s->keepalive_sent_us = 0;
    if (rtt > 5000000u) return;
    s->relay_rtt_us[s->relay_next] = (uint32_t)rtt;
    s->relay_next = (s->relay_next + 1) % RPCN_RELAY_SAMPLES;
    if (s->relay_count < RPCN_RELAY_SAMPLES) s->relay_count++;
}

/* The owner keeps the room's RPCN_ROOM_INT_ATTR_RELAY current: its own trip,
 * so a lobby can estimate its distance to this room. RPCN hands a room on when
 * its owner leaves, and the new owner then publishes its own. */
static inline void rpcn_session_pump_relay(rpcn_session_t *s) {
    if (!s->room_id || !rpcn_session_is_owner(s)) return;
    uint32_t ms = rpcn_session_relay_ms(s);
    if (!ms) return;
    uint32_t was = s->relay_published_ms;
    if (was && (ms > was ? ms - was : was - ms) < RPCN_RELAY_PUBLISH_STEP) return;
    uint64_t now = net_now_ms();
    if (was && now - s->relay_publish_ms < RPCN_RELAY_PUBLISH_MS) return;
    if (!rpcn_set_room_relay(&s->client, s->com_id, s->room_id, ms)) return;
    s->relay_published_ms = ms;
    s->relay_publish_ms   = now;
}

static inline void rpcn_session_pump_punch(rpcn_session_t *s) {
    uint8_t punch[RPCN_PUNCH_SIZE];
    memcpy(punch, g_rpcn_punch_tag, sizeof(g_rpcn_punch_tag));
    rpcn_put_u16(punch + 4, s->my_member_id);
    uint64_t now = net_now_ms();
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
        rpcn_peer_t *p = &s->peers[i];
        if (!p->used || !p->ip || !p->port) continue;
        uint64_t interval = p->heard ? RPCN_PUNCH_IDLE_MS : RPCN_PUNCH_MS;
        if (p->last_punch_ms != 0 && now - p->last_punch_ms < interval) continue;
        p->last_punch_ms = now;
        rpcn_send_to(&s->client, p->ip, p->port, punch, sizeof(punch));
        if (!p->heard && p->alt_ip && p->alt_port && (p->alt_ip != p->ip || p->alt_port != p->port))
            rpcn_send_to(&s->client, p->alt_ip, p->alt_port, punch, sizeof(punch));
    }
}

/* Tell every member we hear where we hear the rest (g_rpcn_intro_tag), once a
 * second. We cannot know which pairs of the others have not found each other,
 * and the whole thing is a few dozen bytes. */
static inline void rpcn_session_pump_intro(rpcn_session_t *s, uint64_t *last_ms) {
    uint64_t now = net_now_ms();
    if (*last_ms && now - *last_ms < RPCN_INTRO_MS) return;
    *last_ms = now;
    uint8_t pkt[4 + RPCN_MAX_PEERS * RPCN_INTRO_ENTRY];
    for (uint32_t to = 0; to < RPCN_MAX_PEERS; to++) {
        const rpcn_peer_t *dst = &s->peers[to];
        if (!dst->used || !dst->heard) continue;
        uint32_t n = 0;
        memcpy(pkt, g_rpcn_intro_tag, 4);
        for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
            const rpcn_peer_t *p = &s->peers[i];
            if (i == to || !p->used || !p->heard) continue;
            uint8_t *e = pkt + 4 + n * RPCN_INTRO_ENTRY;
            rpcn_put_u16(e, p->member_id);
            memcpy(e + 2, &p->ip, 4);
            rpcn_put_u16(e + 6, p->port);
            n++;
        }
        if (n) rpcn_send_to(&s->client, dst->ip, dst->port, pkt, 4 + n * RPCN_INTRO_ENTRY);
    }
}

static inline void rpcn_session_on_intro(rpcn_session_t *s, const uint8_t *buf, uint32_t len,
                                         const rpcn_peer_t *from) {
    for (uint32_t at = 4; at + RPCN_INTRO_ENTRY <= len; at += RPCN_INTRO_ENTRY) {
        rpcn_peer_t *p = rpcn_session_peer(s, rpcn_get_u16(buf + at));
        if (!p || p->heard) continue;
        uint32_t ip;
        memcpy(&ip, buf + at + 2, 4);
        uint16_t port = rpcn_get_u16(buf + at + 6);
        if (!ip || !port || (ip == p->alt_ip && port == p->alt_port)) continue;
        if (!p->ip) { rpcn_session_set_peer_addr(s, p, ip, port, "introduced"); continue; }
        p->alt_ip   = ip;
        p->alt_port = port;
        p->last_punch_ms = 0;
        char text[32];
        rpcn_session_note(s, "%s introduces %s at %s", rpcn_peer_name(from), rpcn_peer_name(p),
                          net_addr_text(text, sizeof(text), ip, port));
    }
}

/* Members we know of but have no address for. The usual cause is one the
 * server's UDP helper has not seen yet, which resolves itself within a keepalive
 * or two — so this retries quietly instead of failing the session. Asking also
 * makes the server tell THEM about us (the SignalingHelper notification). */
static inline void rpcn_session_pump_signaling(rpcn_session_t *s) {
    uint64_t now = net_now_ms();
    for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
        rpcn_peer_t *p = &s->peers[i];
        if (!p->used || p->ip || p->pending_signaling || !p->npid[0]) continue;
        if (p->signaling_retry_ms && now < p->signaling_retry_ms) continue;
        p->pending_signaling  = rpcn_request_signaling_infos(&s->client, p->npid);
        p->signaling_retry_ms = now + RPCN_SIGNALING_RETRY_MS;
    }
}

static inline void rpcn_session_refresh_room(rpcn_session_t *s) {
    if (!s->room_id || s->pending_room_data) return;
    s->pending_room_data = rpcn_get_room_data_internal(&s->client, s->com_id, s->room_id);
}

static inline void rpcn_session_on_notification(rpcn_session_t *s, const rpcn_packet_t *pkt) {
    switch ((rpcn_notification_t)pkt->command) {
        case RPCN_NOTIF_USER_JOINED_ROOM: {
            /* Everything we need to start punching a newcomer is in here, provided
             * the room asked for signaling. */
            rpcn_member_info_t m;
            char npid[20];
            uint32_t ip = 0;
            uint16_t port = 0;
            bool has_addr = false;
            memset(npid, 0, sizeof(npid));
            if (!rpcn_session_in_room(s)) break;
            if (!rpcn_parse_joined_notification(pkt->payload, pkt->payload_size, npid, sizeof(npid),
                                                &ip, &port, &has_addr)) break;
            if (!rpcn_parse_joined_member(pkt->payload, pkt->payload_size, &m)) break;
            if (rpcn_same_npid(m.npid, s->npid)) break;   /* our own join echoed back */
            rpcn_peer_t *p = rpcn_session_upsert_member(s, &m);
            if (p && has_addr && ip && port) rpcn_session_set_peer_addr(s, p, ip, port, "join notification");
            /* No address: an older room, or one the server decided needed no
             * signaling. rpcn_session_pump_signaling asks. */
            break;
        }

        case RPCN_NOTIF_USER_LEFT_ROOM: {
            rpcn_member_info_t m;
            uint64_t room_id = 0;
            if (!rpcn_parse_member_notification(pkt->payload, pkt->payload_size, &room_id, &m)) break;
            if (room_id != s->room_id) break;
            rpcn_session_remove_member(s, m.member_id);
            /* The owner may be who left, and nobody will say who took over. */
            rpcn_session_refresh_room(s);
            if (s->stage == RPCN_STAGE_LINKED) {
                bool any = false;
                for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) if (s->peers[i].used && s->peers[i].heard) any = true;
                if (!any) s->stage = s->is_host ? RPCN_STAGE_HOSTING : RPCN_STAGE_JOINING;
            }
            break;
        }

        case RPCN_NOTIF_ROOM_DESTROYED: {
            if (pkt->payload_size < 8 || rpcn_get_u64(pkt->payload) != s->room_id) break;
            rpcn_session_note(s, "the room was closed");
            rpcn_session_clear_room(s);
            s->stage = RPCN_STAGE_ONLINE;
            break;
        }

        case RPCN_NOTIF_UPDATED_ROOM_DATA_INTERNAL: {
            rpcn_room_info_t room;
            if (!rpcn_parse_room_update(pkt->payload, pkt->payload_size, &room)) break;
            if (room.room_id != s->room_id) break;
            rpcn_session_apply_room(s, &room);
            break;
        }

        case RPCN_NOTIF_UPDATED_ROOM_MEMBER_DATA_INTERNAL: {
            rpcn_member_info_t m;
            uint64_t room_id = 0;
            if (!rpcn_parse_member_notification(pkt->payload, pkt->payload_size, &room_id, &m)) break;
            if (room_id != s->room_id) break;
            rpcn_session_upsert_member(s, &m);
            break;
        }

        case RPCN_NOTIF_SIGNALING_HELPER: {
            /* Pushed to the TARGET of a RequestSignalingInfos, carrying the
             * caller's address. The server sends it for exactly one reason: so the
             * side that was asked about also starts transmitting. */
            char npid[20];
            uint32_t ip = 0;
            uint16_t port = 0;
            memset(npid, 0, sizeof(npid));
            if (!rpcn_parse_signaling_helper(pkt->payload, pkt->payload_size, npid, sizeof(npid),
                                             &ip, &port)) break;
            if (npid[0] && rpcn_same_npid(npid, s->npid)) break;
            /* Somebody not in our room is none of our business. */
            rpcn_peer_t *p = rpcn_session_peer_by_npid(s, npid);
            if (p) rpcn_session_set_peer_addr(s, p, ip, port, "signaling helper");
            break;
        }

        default:
            break;
    }
}

/* Begins the second discovery chain, for the read-only foreign room list. With
 * CreateMissing on, asking about a ComId registers it — that is fine here: the id
 * is YAMP's own, and its own clients register it the same way. */
static inline void rpcn_session_begin_foreign_discovery(rpcn_session_t *s) {
    if (!s->com_id_foreign[0] || s->foreign_ready) return;
    if (s->pending_foreign_serverlist || s->pending_foreign_worldlist) return;
    s->pending_foreign_serverlist = rpcn_get_server_list(&s->client, s->com_id_foreign);
}

static inline bool rpcn_session_pump_replies(rpcn_session_t *s) {
    rpcn_packet_t pkt;
    while (rpcn_poll(&s->client, &pkt)) {
        if (pkt.type == 2) { rpcn_session_on_notification(s, &pkt); continue; }
        if (pkt.type != 1) continue;   /* ServerInfo greeting — nothing to do with it */

        if ((rpcn_command_t)pkt.command == RPCN_CMD_LOGIN) {
            if (pkt.error != RPCN_OK) {
                s->credential_refused = pkt.error == RPCN_ERR_LOGIN_BAD_USERNAME
                                     || pkt.error == RPCN_ERR_LOGIN_BAD_PASSWORD;
                s->login_error = (rpcn_error_t)pkt.error;
                rpcn_session_fail(s, "login rejected: %s (ErrorType=%u)",
                                  rpcn_login_error_text(pkt.error, s->sent_token),
                                  (unsigned)pkt.error);
                return false;
            }
            /* Discovery next. With CreateMissing on, this registers the title. */
            s->pending_serverlist = rpcn_get_server_list(&s->client, s->com_id);
            continue;
        }

        /* The room-state writes answer nothing worth keeping: the server echoes
         * the change back as a notification, which is where it is applied. A
         * refusal is worth a line, since it means the room did not move. */
        if ((rpcn_command_t)pkt.command == RPCN_CMD_SET_ROOM_DATA_INTERNAL
            || (rpcn_command_t)pkt.command == RPCN_CMD_SET_ROOM_MEMBER_DATA
            || (rpcn_command_t)pkt.command == RPCN_CMD_SET_ROOM_DATA_EXTERNAL) {
            if (pkt.error != RPCN_OK)
                rpcn_session_note(s, "the server refused a room update (ErrorType=%u)", (unsigned)pkt.error);
            continue;
        }
        if ((rpcn_command_t)pkt.command == RPCN_CMD_LEAVE_ROOM) continue;

        if (pkt.packet_id == s->pending_serverlist) {
            s->pending_serverlist = 0;
            uint16_t servers[8];
            uint32_t n = rpcn_parse_server_list(pkt.payload, pkt.payload_size, servers, 8);
            if (pkt.error != RPCN_OK || !n) {
                rpcn_session_fail(s, "the server list request failed (ErrorType=%u)", (unsigned)pkt.error);
                return false;
            }
            s->server_id = servers[0];
            s->pending_worldlist = rpcn_get_world_list(&s->client, s->com_id, s->server_id);
            continue;
        }

        if (pkt.packet_id == s->pending_worldlist) {
            s->pending_worldlist = 0;
            uint32_t worlds[8];
            uint32_t n = rpcn_parse_world_list(pkt.payload, pkt.payload_size, worlds, 8);
            if (pkt.error != RPCN_OK || !n) {
                rpcn_session_fail(s, "the world list request failed (ErrorType=%u)", (unsigned)pkt.error);
                return false;
            }
            s->world_id = worlds[0];
            s->stage    = RPCN_STAGE_ONLINE;
            continue;
        }

        /* --- the foreign (read-only) chain. Never fatal: it is a courtesy. --- */
        if (pkt.packet_id == s->pending_foreign_serverlist) {
            s->pending_foreign_serverlist = 0;
            uint16_t servers[8];
            uint32_t n = rpcn_parse_server_list(pkt.payload, pkt.payload_size, servers, 8);
            if (pkt.error != RPCN_OK || !n) {
                rpcn_session_note(s, "no server list for the %s lobby space; not browsing it",
                                  s->com_id_foreign);
                s->com_id_foreign[0] = '\0';
                continue;
            }
            s->foreign_server_id = servers[0];
            s->pending_foreign_worldlist =
                rpcn_get_world_list(&s->client, s->com_id_foreign, s->foreign_server_id);
            continue;
        }

        if (pkt.packet_id == s->pending_foreign_worldlist) {
            s->pending_foreign_worldlist = 0;
            uint32_t worlds[8];
            uint32_t n = rpcn_parse_world_list(pkt.payload, pkt.payload_size, worlds, 8);
            if (pkt.error != RPCN_OK || !n) {
                rpcn_session_note(s, "no world list for the %s lobby space; not browsing it",
                                  s->com_id_foreign);
                s->com_id_foreign[0] = '\0';
                continue;
            }
            s->foreign_world_id = worlds[0];
            s->foreign_ready    = true;
            s->pending_foreign_search =
                rpcn_search_room(&s->client, s->com_id_foreign, s->foreign_world_id);
            continue;
        }

        if (pkt.packet_id == s->pending_foreign_search) {
            s->pending_foreign_search = 0;
            s->foreign_room_count = 0;
            if (pkt.error == RPCN_OK) {
                s->foreign_room_count = rpcn_parse_room_list(pkt.payload, pkt.payload_size,
                                                             s->foreign_rooms, RPCN_MAX_ROOMS);
            }
            continue;
        }

        if (pkt.packet_id == s->pending_search) {
            s->pending_search = 0;
            s->room_count = 0;
            if (pkt.error == RPCN_OK)
                s->room_count = rpcn_parse_room_list(pkt.payload, pkt.payload_size,
                                                     s->rooms, RPCN_MAX_ROOMS);
            /* A failed or empty search is not a session error — an empty server is
             * the normal state — so the list simply comes back with nothing in it. */
            continue;
        }

        if (pkt.packet_id == s->pending_room_data) {
            s->pending_room_data = 0;
            rpcn_room_info_t room;
            if (pkt.error == RPCN_OK && rpcn_parse_room_data_internal(pkt.payload, pkt.payload_size, &room))
                rpcn_session_apply_room(s, &room);
            continue;
        }

        if (pkt.packet_id == s->pending_room) {
            s->pending_room = 0;
            if (pkt.error != RPCN_OK) {
                const char *why = "the room command failed";
                if (pkt.error == RPCN_ERR_ROOM_MISSING)          why = "that room no longer exists";
                else if (pkt.error == RPCN_ERR_ROOM_FULL)        why = "that room is full";
                else if (pkt.error == RPCN_ERR_ROOM_PASSWORD_MISMATCH) why = "wrong room password";
                else if (pkt.error == RPCN_ERR_ROOM_PASSWORD_MISSING)  why = "that room needs a password";
                if (s->ps3 && !s->is_host) {
                    /* PS3 rooms open and close between matches, and the list is
                     * only as fresh as the last search: say so and stay online. */
                    rpcn_session_note(s, "could not join: %s (ErrorType=%u)", why, (unsigned)pkt.error);
                    rpcn_session_clear_room(s);
                    s->stage = RPCN_STAGE_ONLINE;
                    continue;
                }
                rpcn_session_fail(s, "%s (ErrorType=%u)", why, (unsigned)pkt.error);
                return false;
            }

            rpcn_room_info_t room;
            if (!rpcn_parse_room_reply(pkt.payload, pkt.payload_size, &room)) {
                rpcn_session_fail(s, "the room reply carried no room id");
                return false;
            }
            s->room_id = room.room_id;
            /* Read back rather than assumed, on BOTH sides. For a joiner this is
             * the only place the host's settings arrive; for a host it is the
             * server confirming what it actually stored (it clears the FULL bit
             * it owns), so every member reads the same word from the same source. */
            s->room_flags = room.flag_attr;
            s->stage = s->is_host ? RPCN_STAGE_HOSTING : RPCN_STAGE_JOINING;
            rpcn_session_apply_room(s, &room);
            if (!s->my_member_id) {
                rpcn_session_fail(s, "the room reply did not list us as a member");
                return false;
            }

            /* Every member already there, and where: in the join reply when the
             * room has signaling on, so the common case needs no extra round
             * trip. Anyone without one is asked about by the signaling pump. */
            if (!s->is_host) {
                uint16_t ids[RPCN_ROOM_MAX_MEMBERS];
                uint32_t ips[RPCN_ROOM_MAX_MEMBERS];
                uint16_t ports[RPCN_ROOM_MAX_MEMBERS];
                uint32_t n = rpcn_parse_join_signaling_list(pkt.payload, pkt.payload_size,
                                                            ids, ips, ports, RPCN_ROOM_MAX_MEMBERS);
                for (uint32_t i = 0; i < n; i++)
                    rpcn_session_set_peer_addr(s, rpcn_session_peer(s, ids[i]), ips[i], ports[i], "join reply");
                if (!rpcn_session_peer_count(s)) {
                    rpcn_session_fail(s, "joined a room with no other member in it");
                    return false;
                }
            }
            continue;
        }

        /* A signaling lookup answers whichever member it was asked about. */
        rpcn_peer_t *asked = NULL;
        for (uint32_t i = 0; i < RPCN_MAX_PEERS && !asked; i++)
            if (s->peers[i].used && s->peers[i].pending_signaling == pkt.packet_id) asked = &s->peers[i];
        if (asked) {
            asked->pending_signaling = 0;
            uint32_t ip = 0;
            uint16_t port = 0;
            if (pkt.error != RPCN_OK
                || !rpcn_parse_signaling_addr(pkt.payload, pkt.payload_size, &ip, &port)
                || !ip || !port) {
                /* NOT fatal. A member the UDP helper has not seen yet answers
                 * NotFound, which is a timing accident rather than a broken
                 * session — it fixes itself within a keepalive or two. */
                rpcn_session_note(s, "no address for '%s' yet (ErrorType=%u); retrying",
                                  asked->npid, (unsigned)pkt.error);
                asked->signaling_retry_ms = net_now_ms() + RPCN_SIGNALING_RETRY_MS;
                continue;
            }
            rpcn_session_set_peer_addr(s, asked, ip, port, "signaling lookup");
            continue;
        }
    }
    return true;
}

/* Pump TLS, keepalives and pending requests. Call every frame. */
static inline void rpcn_session_update(rpcn_session_t *s) {
    if (s->stage == RPCN_STAGE_IDLE || s->stage == RPCN_STAGE_FAILED) return;

    if (!rpcn_is_connected(&s->client)) {
        rpcn_session_fail(s, "disconnected: %s", rpcn_last_error(&s->client));
        return;
    }

    rpcn_session_pump_replies(s);
    rpcn_session_pump_keepalive(s);
    if (rpcn_session_in_room(s)) {
        rpcn_session_pump_signaling(s);
        if (!s->ps3) {
            rpcn_session_pump_punch(s);
            rpcn_session_pump_intro(s, &s->last_intro_ms);
            rpcn_session_pump_relay(s);
        }
    }
}

/* ---- Rooms --------------------------------------------------------------- */

/* `room_bin` and `member_bin` are the room's first shared state and our own
 * attribute (room.h), so nobody ever sees the room without them. */
static inline bool rpcn_session_host(rpcn_session_t *s, uint32_t max_slot, const char *password,
                                     uint32_t flag_attr,
                                     const uint8_t *room_bin, uint32_t room_len,
                                     const uint8_t *member_bin, uint32_t member_len) {
    if (s->stage != RPCN_STAGE_ONLINE) {
        rpcn_session_fail(s, "cannot host before discovery has finished");
        return false;
    }
    rpcn_session_clear_room(s);
    s->is_host = true;
    if (max_slot < 2) max_slot = 2;
    if (max_slot > RPCN_ROOM_MAX_MEMBERS) max_slot = RPCN_ROOM_MAX_MEMBERS;
    s->pending_room = rpcn_create_room(&s->client, s->com_id, s->world_id, max_slot, password,
                                       flag_attr, room_bin, room_len, member_bin, member_len,
                                       rpcn_session_relay_ms(s));
    if (!s->pending_room) { rpcn_session_fail(s, "%s", rpcn_last_error(&s->client)); return false; }
    return true;
}

static inline bool rpcn_session_join(rpcn_session_t *s, uint64_t room_id, const char *password,
                                     const uint8_t *member_bin, uint32_t member_len) {
    if (s->stage != RPCN_STAGE_ONLINE) {
        rpcn_session_fail(s, "cannot join before discovery has finished");
        return false;
    }
    rpcn_session_clear_room(s);
    s->is_host = false;
    s->pending_room = s->ps3 ? rpcn_ps3_join_room(&s->client, s->com_id, room_id, member_bin, member_len)
                             : rpcn_join_room(&s->client, s->com_id, room_id, password, member_bin, member_len);
    if (!s->pending_room) { rpcn_session_fail(s, "%s", rpcn_last_error(&s->client)); return false; }
    return true;
}

/* A room in the PS3 port's own shape (ps3_link.h, rpcn_ps3_create_room): its
 * rules in the eight searchable ints, and a teamId of 0xFF. */
static inline bool rpcn_session_host_ps3(rpcn_session_t *s, uint32_t max_slot, const uint32_t int_attr[8],
                                         const uint8_t *room_bin, uint32_t room_len,
                                         const uint8_t *member_bin, uint32_t member_len) {
    if (s->stage != RPCN_STAGE_ONLINE) {
        rpcn_session_fail(s, "cannot host before discovery has finished");
        return false;
    }
    rpcn_session_clear_room(s);
    s->is_host = true;
    if (max_slot < 2) max_slot = 2;
    if (max_slot > RPCN_ROOM_MAX_MEMBERS) max_slot = RPCN_ROOM_MAX_MEMBERS;
    s->pending_room = rpcn_ps3_create_room(&s->client, s->com_id, s->world_id, max_slot, int_attr,
                                           room_bin, room_len, member_bin, member_len);
    if (!s->pending_room) { rpcn_session_fail(s, "%s", rpcn_last_error(&s->client)); return false; }
    return true;
}

/* Leave the room and stay signed in. */
static inline void rpcn_session_leave(rpcn_session_t *s) {
    if (!s->room_id) return;
    rpcn_leave_room(&s->client, s->com_id, s->room_id);
    rpcn_session_note(s, "left room %llu", (unsigned long long)s->room_id);
    rpcn_session_clear_room(s);
    if (s->stage == RPCN_STAGE_HOSTING || s->stage == RPCN_STAGE_JOINING || s->stage == RPCN_STAGE_LINKED)
        s->stage = RPCN_STAGE_ONLINE;
}

/* Publish the room's shared state. room.h's rule is that only the owner does. */
static inline bool rpcn_session_set_room_state(rpcn_session_t *s, const uint8_t *bin, uint32_t len) {
    if (!rpcn_session_in_room(s)) return false;
    return rpcn_set_room_data_internal(&s->client, s->com_id, s->room_id, bin, len) != 0;
}

/* The same, with the room's flags (rpcn_set_room_data_flags). */
static inline bool rpcn_session_set_room_state_flags(rpcn_session_t *s, uint32_t flag_filter, uint32_t flag_attr,
                                                     const uint8_t *bin, uint32_t len) {
    if (!rpcn_session_in_room(s)) return false;
    return rpcn_set_room_data_flags(&s->client, s->com_id, s->room_id, flag_filter, flag_attr, bin, len) != 0;
}

/* Publish our own member attribute. */
static inline bool rpcn_session_set_member_state(rpcn_session_t *s, const uint8_t *bin, uint32_t len) {
    if (!rpcn_session_in_room(s)) return false;
    return rpcn_set_member_data_internal(&s->client, s->com_id, s->room_id, bin, len) != 0;
}

/* The same with our teamId, which the PS3 game uses as the waiting line. */
static inline bool rpcn_session_set_member_team(rpcn_session_t *s, uint8_t team,
                                                const uint8_t *bin, uint32_t len) {
    if (!rpcn_session_in_room(s)) return false;
    return rpcn_set_member_data_team(&s->client, s->com_id, s->room_id, team, bin, len) != 0;
}

/* Ask the server for the rooms in our world, and optionally for YAMP's too. The
 * reply is asynchronous: the room lists report the result of the LAST completed
 * search. Only valid once online. */
static inline bool rpcn_session_search(rpcn_session_t *s, bool include_foreign) {
    if (s->stage != RPCN_STAGE_ONLINE && s->stage != RPCN_STAGE_HOSTING
        && s->stage != RPCN_STAGE_JOINING && s->stage != RPCN_STAGE_LINKED) return false;

    if (include_foreign && s->com_id_foreign[0]) {
        if (!s->foreign_ready) rpcn_session_begin_foreign_discovery(s);
        else if (!s->pending_foreign_search)
            s->pending_foreign_search = rpcn_search_room(&s->client, s->com_id_foreign,
                                                         s->foreign_world_id);
    }

    if (s->pending_search != 0) return true;   /* one in flight; its reply refreshes the list */
    s->pending_search = s->ps3 ? rpcn_ps3_search_room(&s->client, s->com_id, s->world_id, RPCN_PS3_VERSION_TAG)
                               : rpcn_search_room(&s->client, s->com_id, s->world_id);
    return s->pending_search != 0;
}

static inline bool rpcn_session_search_pending(const rpcn_session_t *s) {
    return s->pending_search != 0 || s->pending_foreign_search != 0
        || s->pending_foreign_serverlist != 0 || s->pending_foreign_worldlist != 0;
}

/* ---- Datagrams ----------------------------------------------------------- */

static inline bool rpcn_session_send_to(rpcn_session_t *s, uint16_t member_id,
                                        const void *data, uint32_t len) {
    rpcn_peer_t *p = rpcn_session_peer(s, member_id);
    if (!p || !p->ip || !p->port) return false;
    return rpcn_send_to(&s->client, p->ip, p->port, data, len);
}

/*
 * Bytes received from a room member, or 0. `*from` is the member it came from,
 * or 0 when the source is an address nobody has claimed yet -- the caller may
 * then put a name to it with rpcn_session_claim, from what the datagram says
 * about its sender. Signaling replies and punches are absorbed here so nothing
 * above ever sees them.
 */
static inline int rpcn_session_recv(rpcn_session_t *s, void *buf, uint32_t cap,
                                    uint16_t *from, uint32_t *from_ip, uint16_t *from_port) {
    for (;;) {
        uint32_t ip = 0;
        uint16_t port = 0;
        int got = rpcn_recv_from(&s->client, buf, cap, &ip, &port);
        if (got <= 0) return 0;
        *from = 0;
        if (from_ip)   *from_ip   = ip;
        if (from_port) *from_port = port;

        /* Signaling replies share this socket; route them by SOURCE rather than
         * by content, since a signaling reply's leading bytes can look exactly
         * like a game packet header. */
        if (rpcn_is_signaling_source(&s->client, ip, port)) {
            s->signaling_seen = true;
            rpcn_session_on_keepalive_answer(s);
            continue;
        }

        /* A datagram from OURSELVES. This is not paranoia: when two peers share a
         * public IPv4 the server hands each the other's LOCAL address with port
         * 3658 HARDCODED (room_manager.rs and cmd_misc.rs both do it), so two
         * clients on one machine — or one whose peer has not yet been registered
         * — are told to punch at an address that is their own socket. Without
         * this the punch comes straight back, a member latches onto our own port,
         * and every real datagram from them is then discarded as a stray. */
        if (ip == s->client.local_ip && port == s->client.local_port) continue;

        if (!rpcn_session_in_room(s)) continue;

        /* A punch names its sender, so it can introduce an address nobody told us. */
        if (got == (int)RPCN_PUNCH_SIZE && memcmp(buf, g_rpcn_punch_tag, sizeof(g_rpcn_punch_tag)) == 0) {
            rpcn_session_hear(s, rpcn_session_peer(s, rpcn_get_u16((const uint8_t *)buf + 4)), ip, port);
            continue;
        }
        /* An introduction, only from a member we already hear. */
        if (got >= 4 && memcmp(buf, g_rpcn_intro_tag, sizeof(g_rpcn_intro_tag)) == 0) {
            for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
                const rpcn_peer_t *p = &s->peers[i];
                if (p->used && p->heard && p->ip == ip && p->port == port)
                    rpcn_session_on_intro(s, (const uint8_t *)buf, (uint32_t)got, p);
            }
            continue;
        }

        for (uint32_t i = 0; i < RPCN_MAX_PEERS; i++) {
            rpcn_peer_t *p = &s->peers[i];
            if (p->used && p->heard && p->ip == ip && p->port == port) { *from = p->member_id; break; }
        }
        return got;
    }
}

/* A datagram from an address nobody has claimed says it is from `member_id`:
 * adopt the address if that is plausible. True if it now belongs to them. */
static inline bool rpcn_session_claim(rpcn_session_t *s, uint16_t member_id, uint32_t ip, uint16_t port) {
    return rpcn_session_hear(s, rpcn_session_peer(s, member_id), ip, port);
}

/* ======================================================================== */
/* Account registration                                                      */
/* ======================================================================== */
/*
 * RPCN's Create and SendToken run on their OWN connection and before any login,
 * so this deliberately does not go through the session above: that is a logged-in
 * thing with discovery, a room, a signaling socket and a peer, none of which
 * exist yet and none of which a sign-up should be able to disturb. This is a
 * connection, one request and one reply.
 *
 * BOTH JOBS LIVE HERE because they are the same shape and two halves of one
 * story: a server with e-mail validation switched on mails a token when Create
 * succeeds, and login refuses the account until that token comes back. A player
 * who never received the message would otherwise hold an account they can never
 * use.
 *
 * It carries its own timeout, because the failure this is most likely to meet in
 * the wild is a server that accepts the TCP connection and then says nothing, and
 * "the button did nothing, for ever" is not an outcome a UI can explain.
 */

typedef enum { RPCN_ACCOUNT_IDLE, RPCN_ACCOUNT_WORKING, RPCN_ACCOUNT_DONE, RPCN_ACCOUNT_FAILED }
    rpcn_account_state_t;
typedef enum { RPCN_ACCOUNT_JOB_CREATE, RPCN_ACCOUNT_JOB_RESEND } rpcn_account_job_t;

typedef struct {
    rpcn_client_t        client;
    rpcn_account_state_t state;
    rpcn_account_job_t   job;
    uint64_t             pending;
    uint64_t             deadline_ms;
    char                 error[256];
} rpcn_account_t;

/* The server insists on a non-empty avatar, and a player has no reason to have
 * one. This says where the account came from rather than inventing a picture. */
#define RPCN_DEFAULT_AVATAR "https://github.com/biggestsonicfan/m2-hle2"

static inline const char *rpcn_create_error_text(rpcn_error_t error) {
    switch (error) {
        case RPCN_ERR_CREATION_EXISTING_USER:
        case RPCN_ERR_INVALID:
            return "that account name is already taken on this server";
        case RPCN_ERR_CREATION_EXISTING_MAIL:
            return "there is already an account on this server with that e-mail address";
        case RPCN_ERR_CREATION_BANNED_EMAIL:
            return "this server does not accept accounts from that e-mail provider";
        case RPCN_ERR_INVALID_INPUT:
            return "the server refused these details: the name must be 3-16 characters of "
                   "letters, digits, '-' or '_', and the e-mail address must be a real one";
        case RPCN_ERR_TOO_SOON:
            return "too many sign-ups from this address recently - wait a while and try again";
        case RPCN_ERR_DB_FAIL:
            return "the server's database did not answer";
        default:
            return "the server refused the sign-up";
    }
}

/* The same for SendToken, where every code means something different again —
 * Invalid is not a name clash here but "this server has no tokens at all". */
static inline const char *rpcn_resend_error_text(rpcn_error_t error) {
    switch (error) {
        case RPCN_ERR_INVALID:
            return "this server does not verify accounts by e-mail, so it has no token to send - "
                   "leave the token box empty and log in";
        case RPCN_ERR_TOO_SOON:
            return "a token was already e-mailed for this account in the last 24 hours";
        case RPCN_ERR_LOGIN:
            return "the server did not accept that account name and password";
        case RPCN_ERR_EMAIL_FAIL:
            return "the server could not send the e-mail";
        case RPCN_ERR_MALFORMED:
            return "the server could not read the request";
        case RPCN_ERR_DB_FAIL:
            return "the server's database did not answer";
        default:
            return "the server refused to send the token";
    }
}

static inline void rpcn_account_finish(rpcn_account_t *a, rpcn_account_state_t state) {
    a->state       = state;
    a->pending     = 0;
    a->deadline_ms = 0;
    /* The connection has done its one job. Holding it open leaves an
     * unauthenticated socket the server drops after ten seconds anyway. */
    rpcn_disconnect(&a->client);
}

static inline void rpcn_account_fail(rpcn_account_t *a, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(a->error, sizeof(a->error), fmt, args);
    va_end(args);
    rpcn_account_finish(a, RPCN_ACCOUNT_FAILED);
}

static inline void rpcn_account_reset(rpcn_account_t *a) {
    rpcn_disconnect(&a->client);
    a->state       = RPCN_ACCOUNT_IDLE;
    a->job         = RPCN_ACCOUNT_JOB_CREATE;
    a->pending     = 0;
    a->deadline_ms = 0;
    a->error[0]    = '\0';
}

static inline bool rpcn_account_begin(rpcn_account_t *a, rpcn_account_job_t job,
                                      const char *server, uint16_t port,
                                      const char *fingerprint_hex) {
    rpcn_account_reset(a);
    a->job = job;

    if (!server || !*server) {
        rpcn_account_fail(a, job == RPCN_ACCOUNT_JOB_CREATE ? "no server to create the account on"
                                                            : "no server to ask for a token");
        return false;
    }

    cert_fingerprint_t pin;
    memset(&pin, 0, sizeof(pin));
    if (fingerprint_hex && *fingerprint_hex && !cert_fp_from_hex(&pin, fingerprint_hex)) {
        rpcn_account_fail(a, "that certificate fingerprint is not 64 hexadecimal characters");
        return false;
    }

    if (!rpcn_connect(&a->client, server, port ? port : RPCN_DEFAULT_PORT, &pin)) {
        rpcn_account_fail(a, "could not reach %s: %s", server, rpcn_last_error(&a->client));
        return false;
    }

    a->state       = RPCN_ACCOUNT_WORKING;
    a->deadline_ms = net_now_ms() + RPCN_ACCOUNT_TIMEOUT_MS;
    return true;
}

static inline bool rpcn_account_create(rpcn_account_t *a, const char *server, uint16_t port,
                                       const char *fingerprint_hex, const char *npid,
                                       const char *password, const char *email) {
    /* Checked before anything is connected, so a blank box costs no round trip
     * and the message can name the box. */
    rpcn_account_reset(a);
    a->job = RPCN_ACCOUNT_JOB_CREATE;
    if (!npid || !*npid || !password || !*password) {
        rpcn_account_fail(a, "an account name and a password are both required");
        return false;
    }
    if (!email || !*email) {
        rpcn_account_fail(a, "an e-mail address is required: the server stores one for every account");
        return false;
    }

    if (!rpcn_account_begin(a, RPCN_ACCOUNT_JOB_CREATE, server, port, fingerprint_hex)) return false;

    /* The online name defaults to the account name and the avatar to this
     * project's page: a second display name is a question with no useful answer
     * for someone with one account, and an avatar is a URL nobody has to hand. */
    a->pending = rpcn_create_account(&a->client, npid, password, npid, RPCN_DEFAULT_AVATAR, email);
    if (a->pending == 0) { rpcn_account_fail(a, "%s", rpcn_last_error(&a->client)); return false; }
    return true;
}

static inline bool rpcn_account_resend(rpcn_account_t *a, const char *server, uint16_t port,
                                       const char *fingerprint_hex, const char *npid,
                                       const char *password) {
    rpcn_account_reset(a);
    a->job = RPCN_ACCOUNT_JOB_RESEND;
    if (!npid || !*npid || !password || !*password) {
        rpcn_account_fail(a, "the account name and password are both required to ask for a token");
        return false;
    }
    if (!rpcn_account_begin(a, RPCN_ACCOUNT_JOB_RESEND, server, port, fingerprint_hex)) return false;

    a->pending = rpcn_resend_token(&a->client, npid, password);
    if (a->pending == 0) { rpcn_account_fail(a, "%s", rpcn_last_error(&a->client)); return false; }
    return true;
}

static inline void rpcn_account_update(rpcn_account_t *a) {
    if (a->state != RPCN_ACCOUNT_WORKING) return;

    rpcn_command_t expect = (a->job == RPCN_ACCOUNT_JOB_CREATE) ? RPCN_CMD_CREATE
                                                                : RPCN_CMD_SEND_TOKEN;
    rpcn_packet_t pkt;
    while (rpcn_poll(&a->client, &pkt)) {
        /* type 1 is a reply; the ServerInfo greeting arrives first and says
         * nothing about this request. */
        if (pkt.type != 1 || (rpcn_command_t)pkt.command != expect) continue;
        if (a->pending != 0 && pkt.packet_id != a->pending) continue;

        if (pkt.error != RPCN_OK) {
            rpcn_account_fail(a, "%s (ErrorType=%u)",
                              (a->job == RPCN_ACCOUNT_JOB_CREATE) ? rpcn_create_error_text(pkt.error)
                                                                  : rpcn_resend_error_text(pkt.error),
                              (unsigned)pkt.error);
            return;
        }
        a->error[0] = '\0';
        rpcn_account_finish(a, RPCN_ACCOUNT_DONE);
        return;
    }

    if (!rpcn_is_connected(&a->client)) {
        rpcn_account_fail(a, "the server closed the connection without answering");
        return;
    }
    if (a->deadline_ms != 0 && net_now_ms() > a->deadline_ms)
        rpcn_account_fail(a, "the server did not answer within %u seconds",
                          (unsigned)(RPCN_ACCOUNT_TIMEOUT_MS / 1000));
}

/* ======================================================================== */
/* Twitch sign-in                                                            */
/* ======================================================================== */
/*
 * The OAuth device code flow, run on its own connection like the sign-up above
 * and for the same reason: it happens before any login, and it must not be able
 * to disturb a live session.
 *
 * WHAT IT ACTUALLY PRODUCES is a password. The flow needs a browser and a human,
 * which is far too slow to do at every launch, so it runs ONCE and the server
 * hands back an npid and a long-lived **login token** that the ordinary Login
 * command accepts in place of the password. Everything after this is a plain
 * login — which is why nothing else in net/ knows Twitch exists.
 *
 * THE POLL IS ON THE SERVER'S SCHEDULE, not ours. Twitch hands out an interval
 * (5 s in practice) and answers TwitchAuthSlowDown if we beat it; that is not an
 * error, it is a rate limit, and the only correct response is to wait longer and
 * carry on. Same for TwitchAuthPending, which is simply "they have not clicked
 * yet" and will be the answer for most of the flow's life.
 *
 * ONE CONNECTION FOR THE WHOLE FLOW. The server relaxes this connection's
 * unauthenticated read timeout from 10 s to 120 s once the start succeeds, so
 * polling on the interval keeps it alive. Reconnecting between polls would lose
 * the relaxed timeout and the flow id is bound to the server's state, not ours.
 */

typedef enum {
    RPCN_TWITCH_IDLE,
    RPCN_TWITCH_STARTING,   /* asked for a device code */
    RPCN_TWITCH_WAITING,    /* code in hand; the user is off approving it */
    RPCN_TWITCH_DONE,
    RPCN_TWITCH_FAILED,
} rpcn_twitch_state_t;

typedef struct {
    rpcn_client_t       client;
    rpcn_twitch_state_t state;

    char     flow_id[128];
    char     user_code[32];
    char     verification_uri[256];
    uint32_t expires_in;
    uint32_t interval_s;

    /* What the flow yields. */
    char     npid[20];
    char     online_name[32];
    char     avatar_url[256];
    char     login_token[80];

    uint64_t pending;
    uint64_t deadline_ms;    /* when the device code dies */
    uint64_t next_poll_ms;
    bool     started_ok;     /* the start reply landed — see the disconnect rule */
    char     error[256];
} rpcn_twitch_t;

static inline const char *rpcn_twitch_error_text(rpcn_error_t error) {
    switch (error) {
        case RPCN_ERR_TWITCH_DISABLED:
            return "this server has no Twitch sign-in configured - use an account name and password";
        case RPCN_ERR_TWITCH_EXPIRED:
            return "the code expired before it was approved - start again";
        case RPCN_ERR_TWITCH_DENIED:
            return "the authorization was refused on twitch.tv";
        case RPCN_ERR_TWITCH_ERROR:
            return "the server could not reach Twitch - try again in a moment";
        case RPCN_ERR_UNAUTHORIZED:
            return "that account is already linked to a different Twitch user";
        case RPCN_ERR_CREATION_EXISTING_USER:
            return "no free account name could be derived from that Twitch login";
        case RPCN_ERR_DB_FAIL:
            return "the server's database did not answer";
        default:
            return "the server refused the Twitch sign-in";
    }
}

static inline void rpcn_twitch_finish(rpcn_twitch_t *t, rpcn_twitch_state_t state) {
    t->state   = state;
    t->pending = 0;
    rpcn_disconnect(&t->client);
}

static inline void rpcn_twitch_fail(rpcn_twitch_t *t, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(t->error, sizeof(t->error), fmt, args);
    va_end(args);
    rpcn_twitch_finish(t, RPCN_TWITCH_FAILED);
}

static inline void rpcn_twitch_reset(rpcn_twitch_t *t) {
    rpcn_disconnect(&t->client);
    memset(t, 0, sizeof(*t));
    t->client.udp      = NET_SOCK_INVALID;
    t->client.tls.sock = NET_SOCK_INVALID;
    t->state           = RPCN_TWITCH_IDLE;
}

static inline bool rpcn_twitch_begin(rpcn_twitch_t *t, const char *server, uint16_t port,
                                     const char *fingerprint_hex) {
    rpcn_twitch_reset(t);

    if (!server || !*server) { rpcn_twitch_fail(t, "no server to sign in to"); return false; }

    cert_fingerprint_t pin;
    memset(&pin, 0, sizeof(pin));
    if (fingerprint_hex && *fingerprint_hex && !cert_fp_from_hex(&pin, fingerprint_hex)) {
        rpcn_twitch_fail(t, "that certificate fingerprint is not 64 hexadecimal characters");
        return false;
    }
    if (!rpcn_connect(&t->client, server, port ? port : RPCN_DEFAULT_PORT, &pin)) {
        rpcn_twitch_fail(t, "could not reach %s: %s", server, rpcn_last_error(&t->client));
        return false;
    }

    t->pending = rpcn_twitch_start(&t->client);
    if (!t->pending) { rpcn_twitch_fail(t, "%s", rpcn_last_error(&t->client)); return false; }

    t->state       = RPCN_TWITCH_STARTING;
    t->deadline_ms = net_now_ms() + 30000;   /* replaced by expires_in on the reply */
    return true;
}

static inline void rpcn_twitch_update(rpcn_twitch_t *t) {
    if (t->state != RPCN_TWITCH_STARTING && t->state != RPCN_TWITCH_WAITING) return;

    rpcn_packet_t pkt;
    while (rpcn_poll(&t->client, &pkt)) {
        if (pkt.type != 1) continue;                     /* the ServerInfo greeting */
        if (t->pending && pkt.packet_id != t->pending) continue;

        if ((rpcn_command_t)pkt.command == RPCN_CMD_TWITCH_DEVICE_START) {
            t->pending = 0;
            if (pkt.error != RPCN_OK) {
                rpcn_twitch_fail(t, "%s", rpcn_twitch_error_text(pkt.error));
                return;
            }
            uint32_t expires = 0, interval = 0;
            if (!rpcn_parse_twitch_start(pkt.payload, pkt.payload_size,
                                         t->flow_id, sizeof(t->flow_id),
                                         t->user_code, sizeof(t->user_code),
                                         t->verification_uri, sizeof(t->verification_uri),
                                         &expires, &interval)) {
                rpcn_twitch_fail(t, "the server's Twitch reply could not be read");
                return;
            }
            t->started_ok   = true;
            t->expires_in   = expires ? expires : 900;
            t->interval_s   = interval ? interval : 5;
            t->deadline_ms  = net_now_ms() + (uint64_t)t->expires_in * 1000ull;
            t->next_poll_ms = net_now_ms() + (uint64_t)t->interval_s * 1000ull;
            t->state        = RPCN_TWITCH_WAITING;
            continue;
        }

        if ((rpcn_command_t)pkt.command == RPCN_CMD_TWITCH_DEVICE_POLL) {
            t->pending = 0;
            switch (pkt.error) {
                case RPCN_ERR_TWITCH_PENDING:
                    /* The overwhelmingly normal answer: they have not clicked yet. */
                    t->next_poll_ms = net_now_ms() + (uint64_t)t->interval_s * 1000ull;
                    continue;
                case RPCN_ERR_TWITCH_SLOW_DOWN:
                    /* A rate limit, not a failure. Back off by a whole extra
                     * interval rather than retrying at the same cadence, or the
                     * next poll earns the same answer. */
                    t->next_poll_ms = net_now_ms() + (uint64_t)(t->interval_s * 2u) * 1000ull;
                    continue;
                case RPCN_OK:
                    break;
                default:
                    rpcn_twitch_fail(t, "%s", rpcn_twitch_error_text(pkt.error));
                    return;
            }

            if (!rpcn_parse_twitch_poll(pkt.payload, pkt.payload_size,
                                        t->npid, sizeof(t->npid),
                                        t->online_name, sizeof(t->online_name),
                                        t->avatar_url, sizeof(t->avatar_url),
                                        t->login_token, sizeof(t->login_token))) {
                rpcn_twitch_fail(t, "the server's Twitch reply could not be read");
                return;
            }
            t->error[0] = '\0';
            rpcn_twitch_finish(t, RPCN_TWITCH_DONE);
            return;
        }
    }

    if (!rpcn_is_connected(&t->client)) {
        /* An RPCN without this feature does not know command 63: it answers
         * Malformed and hangs up. Saying "no Twitch here" is both more likely to
         * be true and more useful than "the connection dropped". */
        rpcn_twitch_fail(t, t->started_ok
            ? "the server closed the connection during the Twitch sign-in"
            : "this server does not support Twitch sign-in (it is an older RPCN, or "
              "Twitch is not configured on it) - use an account name and password");
        return;
    }

    if (net_now_ms() > t->deadline_ms) {
        rpcn_twitch_fail(t, t->started_ok
            ? "the code expired before it was approved - start again"
            : "the server did not answer the Twitch request");
        return;
    }

    if (t->state == RPCN_TWITCH_WAITING && !t->pending && net_now_ms() >= t->next_poll_ms) {
        t->pending = rpcn_twitch_poll_flow(&t->client, t->flow_id);
        if (!t->pending) { rpcn_twitch_fail(t, "%s", rpcn_last_error(&t->client)); return; }
        /* Re-armed by whichever status comes back. */
        t->next_poll_ms = net_now_ms() + (uint64_t)t->interval_s * 1000ull;
    }
}

#endif /* RPCN_SESSION_H */
