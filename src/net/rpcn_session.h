/*
 * rpcn_session.h — an RPCN session: login, discovery, rooms, peer address
 * exchange, NAT punching, and then direct peer-to-peer datagrams.
 *
 * Everything above this (lockstep.h, netplay.h) only ever asks it to send and
 * receive bytes to "the peer"; nothing up there knows what a room is.
 *
 * CONNECTION MODEL — SYMMETRIC, AND IT HAS TO BE.
 *   BOTH ends learn the other's address from the server, and BOTH start sending
 *   immediately. The guest gets the host's address in its JoinRoom reply
 *   (signaling_data); the host gets the guest's in the UserJoinedRoom
 *   notification the server pushes when someone joins. Both are populated only
 *   because rpcn_create_room asks for signaling (sigOptParam).
 *
 *   The tempting model — guest transmits first, host stays silent until it hears
 *   something, on the theory that the guest's packet "opens the return path" —
 *   does not work. It opens the path through the GUEST's NAT. The host's NAT has
 *   no mapping for the guest at all, so it drops that first packet on the floor,
 *   and a silent host never creates one: two peers on different networks sit at
 *   the barrier forever. Hole punching only works if both sides transmit, so the
 *   punch starts as soon as an address is known and keeps a small datagram
 *   flowing whether or not anything has come back.
 *
 * WHAT THIS STILL CANNOT DO: if either side is behind a symmetric NAT, the
 * mapping it opens towards the peer differs from the one the server advertised,
 * and no amount of punching helps — RPCN has no relay to fall back on. The recv
 * path absorbs the milder case (a peer whose port differs from the advertised
 * one) by re-pointing at the source it actually hears from.
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
#define RPCN_ACCOUNT_TIMEOUT_MS 15000

#define RPCN_MAX_ROOMS 32

/* Not a game packet: shorter than any header the lockstep layer accepts, so it
 * is discarded up there even if one leaks through. Its only job is to make our
 * NAT create a mapping towards the peer. */
static const uint8_t g_rpcn_punch[4] = { 'M', '2', 'N', '!' };

typedef enum {
    RPCN_STAGE_IDLE,
    RPCN_STAGE_LOGGING_IN,
    RPCN_STAGE_ONLINE,      /* logged in, discovery done */
    RPCN_STAGE_HOSTING,     /* room created, nobody has joined yet */
    RPCN_STAGE_JOINING,     /* room joined, resolving the host */
    RPCN_STAGE_LINKED,      /* peer address known — datagrams can flow */
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

    /* Optional progress log. Connection problems here are almost always somebody's
     * NAT or firewall rather than a bug, and none of that is diagnosable from
     * "waiting for peer" alone — so the session reports what it learned and from
     * where. */
    void *log_ctx;
    void (*log)(void *ctx, const char *msg);
} rpcn_session_config_t;

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
    char     peer_npid[20];

    uint16_t server_id;
    uint32_t world_id;
    uint64_t room_id;
    uint32_t room_flags;
    bool     is_host;

    uint32_t peer_ip;        /* network byte order */
    uint16_t peer_port;
    bool     peer_heard;     /* a datagram has actually arrived from the peer */

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
    uint64_t pending_signaling;

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
    uint64_t last_punch_ms;
    /* A signaling lookup is retried rather than fatal: a guest can easily join
     * before the host has been registered by the UDP helper, and that used to
     * kill the whole session. */
    uint64_t signaling_retry_ms;

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

static inline bool rpcn_session_peer_known(const rpcn_session_t *s) {
    return s->peer_ip != 0 && s->peer_port != 0;
}

static inline const char *rpcn_session_peer_text(const rpcn_session_t *s) {
    static char text[32];
    if (!rpcn_session_peer_known(s)) return "unknown";
    return net_addr_text(text, sizeof(text), s->peer_ip, s->peer_port);
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

/* Adopt a peer address, whatever told us about it. `source` names that for the log. */
static inline void rpcn_session_set_peer(rpcn_session_t *s, uint32_t ip, uint16_t port,
                                         const char *source) {
    if (!ip || !port) return;
    bool changed = (ip != s->peer_ip || port != s->peer_port);
    /* An address the peer has actually been HEARD from beats anything the server
     * says afterwards. The two can disagree -- a room whose copy of an address was
     * taken before the helper had it, or two players the server sees on one
     * public address handed each other's local one -- and replacing a working
     * address with a told one turns every datagram from the peer into a stray. */
    if (changed && s->peer_heard) {
        char told[32];
        rpcn_session_note(s, "the server says the peer is at %s (via %s); keeping %s, which is "
                             "where it is actually heard from",
                          net_addr_text(told, sizeof(told), ip, port), source, rpcn_session_peer_text(s));
        return;
    }
    s->peer_ip   = ip;
    s->peer_port = port;
    s->signaling_retry_ms = 0;
    if (s->stage == RPCN_STAGE_HOSTING || s->stage == RPCN_STAGE_JOINING)
        s->stage = RPCN_STAGE_LINKED;
    if (changed) {
        /* Punch immediately rather than waiting out the interval — this is the
         * moment the hole has to be opened, and the peer may already be sending. */
        s->last_punch_ms = 0;
        rpcn_session_note(s, "peer %s at %s (via %s)",
                          s->peer_npid[0] ? s->peer_npid : "?", rpcn_session_peer_text(s), source);
    }
}

/* ---- Lifetime ------------------------------------------------------------ */

static inline void rpcn_session_stop(rpcn_session_t *s) {
    rpcn_disconnect(&s->client);
    s->stage      = RPCN_STAGE_IDLE;
    s->room_id    = 0;
    s->room_flags = 0;
    s->is_host    = false;
    s->peer_ip    = 0;
    s->peer_port  = 0;
    s->peer_heard = false;
    s->signaling_seen = false;
    s->peer_npid[0] = '\0';
    s->sent_token = false;
    s->credential_refused = false;
    s->pending_serverlist = s->pending_worldlist = s->pending_room = 0;
    s->pending_signaling  = s->pending_search = 0;
    s->pending_foreign_serverlist = s->pending_foreign_worldlist = s->pending_foreign_search = 0;
    s->foreign_ready = false;
    s->signaling_retry_ms = 0;
    s->last_punch_ms = 0;
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
    s->com_id_foreign[0] = '\0';
    if (cfg->com_id_foreign && comid_is_well_formed(cfg->com_id_foreign))
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

    if (!rpcn_connect(&s->client, cfg->server, cfg->port, &pin)) {
        rpcn_session_fail(s, "%s", rpcn_last_error(&s->client));
        return false;
    }

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
    rpcn_send_signaling_ping(&s->client, 0);
}

static inline void rpcn_session_pump_punch(rpcn_session_t *s) {
    if (!rpcn_session_peer_known(s)) return;
    uint64_t now = net_now_ms();
    uint64_t interval = s->peer_heard ? RPCN_PUNCH_IDLE_MS : RPCN_PUNCH_MS;
    if (s->last_punch_ms != 0 && now - s->last_punch_ms < interval) return;
    s->last_punch_ms = now;
    rpcn_send_to(&s->client, s->peer_ip, s->peer_port, g_rpcn_punch, sizeof(g_rpcn_punch));
}

/* Only when we know WHO the peer is but not WHERE. The usual cause is a peer the
 * server's UDP helper has not seen yet, which resolves itself within a keepalive
 * or two — so this retries quietly instead of failing the session. */
static inline void rpcn_session_pump_signaling_retry(rpcn_session_t *s) {
    if (s->signaling_retry_ms == 0 || rpcn_session_peer_known(s)
        || s->pending_signaling != 0 || !s->peer_npid[0]) return;
    if (net_now_ms() < s->signaling_retry_ms) return;
    s->signaling_retry_ms = 0;
    s->pending_signaling  = rpcn_request_signaling_infos(&s->client, s->peer_npid);
}

static inline void rpcn_session_on_notification(rpcn_session_t *s, const rpcn_packet_t *pkt) {
    switch ((rpcn_notification_t)pkt->command) {
        case RPCN_NOTIF_USER_JOINED_ROOM: {
            /* The host's cue that it has a peer at all. Everything it needs to
             * start punching is in here, provided the room asked for signaling. */
            char npid[20];
            uint32_t ip = 0;
            uint16_t port = 0;
            bool has_addr = false;
            memset(npid, 0, sizeof(npid));
            if (!rpcn_parse_joined_notification(pkt->payload, pkt->payload_size, npid, sizeof(npid),
                                                &ip, &port, &has_addr)) break;
            if (npid[0] && strcmp(npid, s->npid) == 0) break;   /* our own join echoed back */
            if (npid[0]) snprintf(s->peer_npid, sizeof(s->peer_npid), "%s", npid);

            rpcn_session_note(s, "%s joined the room", s->peer_npid[0] ? s->peer_npid : "a peer");

            if (has_addr && ip && port) {
                rpcn_session_set_peer(s, ip, port, "join notification");
            } else if (s->peer_npid[0] && !rpcn_session_peer_known(s) && s->pending_signaling == 0) {
                /* No address in the notification: an older room, or one the server
                 * decided needed no signaling. Ask directly — which also makes the
                 * server tell the PEER about us. */
                s->pending_signaling = rpcn_request_signaling_infos(&s->client, s->peer_npid);
            }
            break;
        }

        case RPCN_NOTIF_USER_LEFT_ROOM:
        case RPCN_NOTIF_ROOM_DESTROYED: {
            if (!rpcn_session_peer_known(s) && !s->peer_npid[0]) break;
            rpcn_session_note(s, "the peer left the room");
            s->peer_ip    = 0;
            s->peer_port  = 0;
            s->peer_heard = false;
            s->peer_npid[0] = '\0';
            s->signaling_retry_ms = 0;
            if (s->stage == RPCN_STAGE_LINKED)
                s->stage = s->is_host ? RPCN_STAGE_HOSTING : RPCN_STAGE_JOINING;
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
            if (npid[0] && strcmp(npid, s->npid) == 0) break;
            if (s->peer_npid[0] && npid[0] && strcmp(npid, s->peer_npid) != 0)
                break;   /* somebody else entirely — not the peer we are in a room with */
            if (npid[0]) snprintf(s->peer_npid, sizeof(s->peer_npid), "%s", npid);
            rpcn_session_set_peer(s, ip, port, "signaling helper");
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
                rpcn_session_fail(s, "login rejected: %s (ErrorType=%u)",
                                  rpcn_login_error_text(pkt.error, s->sent_token),
                                  (unsigned)pkt.error);
                return false;
            }
            /* Discovery next. With CreateMissing on, this registers the title. */
            s->pending_serverlist = rpcn_get_server_list(&s->client, s->com_id);
            continue;
        }

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

        if (pkt.packet_id == s->pending_room) {
            s->pending_room = 0;
            if (pkt.error != RPCN_OK) {
                const char *why = "the room command failed";
                if (pkt.error == RPCN_ERR_ROOM_MISSING)          why = "that room no longer exists";
                else if (pkt.error == RPCN_ERR_ROOM_FULL)        why = "that room is full";
                else if (pkt.error == RPCN_ERR_ROOM_PASSWORD_MISMATCH) why = "wrong room password";
                else if (pkt.error == RPCN_ERR_ROOM_PASSWORD_MISSING)  why = "that room needs a password";
                rpcn_session_fail(s, "%s (ErrorType=%u)", why, (unsigned)pkt.error);
                return false;
            }

            s->room_id = rpcn_parse_room_id(pkt.payload, pkt.payload_size);
            if (!s->room_id) { rpcn_session_fail(s, "the room reply carried no room id"); return false; }

            /* Read back rather than assumed, on BOTH sides. For a guest this is the
             * only place the host's settings arrive; for a host it is the server
             * confirming what it actually stored (it clears the FULL bit it owns),
             * so both peers end up reading the same word from the same source. */
            s->room_flags = rpcn_parse_room_flag_attr(pkt.payload, pkt.payload_size);

            if (s->is_host) {
                /* Wait for a UserJoinedRoom notification, which tells us both that
                 * a guest exists and where it is. */
                s->stage = RPCN_STAGE_HOSTING;
            } else {
                char members[8][20];
                uint32_t n = rpcn_parse_room_members(pkt.payload, pkt.payload_size, members, 8);
                s->peer_npid[0] = '\0';
                for (uint32_t i = 0; i < n; i++) {
                    if (strcmp(members[i], s->npid) != 0) {
                        snprintf(s->peer_npid, sizeof(s->peer_npid), "%s", members[i]);
                        break;
                    }
                }
                if (!s->peer_npid[0]) {
                    rpcn_session_fail(s, "joined a room with no other member in it");
                    return false;
                }

                s->stage = RPCN_STAGE_JOINING;

                /* The host's address is already in this reply when the room has
                 * signaling on, so the common case needs no extra round trip. */
                uint32_t ip = 0;
                uint16_t port = 0;
                if (rpcn_parse_join_signaling_addr(pkt.payload, pkt.payload_size, &ip, &port)
                    && ip && port) {
                    rpcn_session_set_peer(s, ip, port, "join reply");
                } else {
                    s->pending_signaling = rpcn_request_signaling_infos(&s->client, s->peer_npid);
                }
            }
            continue;
        }

        if (pkt.packet_id == s->pending_signaling) {
            s->pending_signaling = 0;
            uint32_t ip = 0;
            uint16_t port = 0;
            if (pkt.error != RPCN_OK
                || !rpcn_parse_signaling_addr(pkt.payload, pkt.payload_size, &ip, &port)
                || !ip || !port) {
                /* NOT fatal. A peer the UDP helper has not seen yet answers
                 * NotFound, which is a timing accident rather than a broken
                 * session — it fixes itself within a keepalive or two. */
                rpcn_session_note(s, "no address for '%s' yet (ErrorType=%u); retrying",
                                  s->peer_npid, (unsigned)pkt.error);
                s->signaling_retry_ms = net_now_ms() + RPCN_SIGNALING_RETRY_MS;
                continue;
            }
            rpcn_session_set_peer(s, ip, port, "signaling lookup");
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
    rpcn_session_pump_signaling_retry(s);
    rpcn_session_pump_punch(s);
}

/* ---- Rooms --------------------------------------------------------------- */

static inline bool rpcn_session_host(rpcn_session_t *s, uint32_t max_slot, const char *password,
                                     uint32_t flag_attr) {
    if (s->stage != RPCN_STAGE_ONLINE) {
        rpcn_session_fail(s, "cannot host before discovery has finished");
        return false;
    }
    s->is_host    = true;
    s->room_flags = 0;   /* adopted from the server's reply, like the room id */
    s->peer_ip    = 0;
    s->peer_port  = 0;
    s->peer_heard = false;
    s->peer_npid[0] = '\0';
    s->pending_room = rpcn_create_room(&s->client, s->com_id, s->world_id,
                                       max_slot ? max_slot : 2, password, flag_attr);
    if (!s->pending_room) { rpcn_session_fail(s, "%s", rpcn_last_error(&s->client)); return false; }
    return true;
}

static inline bool rpcn_session_join(rpcn_session_t *s, uint64_t room_id, const char *password) {
    if (s->stage != RPCN_STAGE_ONLINE) {
        rpcn_session_fail(s, "cannot join before discovery has finished");
        return false;
    }
    s->is_host    = false;
    s->room_flags = 0;
    s->peer_ip    = 0;
    s->peer_port  = 0;
    s->peer_heard = false;
    s->peer_npid[0] = '\0';
    s->pending_room = rpcn_join_room(&s->client, s->com_id, room_id, password);
    if (!s->pending_room) { rpcn_session_fail(s, "%s", rpcn_last_error(&s->client)); return false; }
    return true;
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
    s->pending_search = rpcn_search_room(&s->client, s->com_id, s->world_id);
    return s->pending_search != 0;
}

static inline bool rpcn_session_search_pending(const rpcn_session_t *s) {
    return s->pending_search != 0 || s->pending_foreign_search != 0
        || s->pending_foreign_serverlist != 0 || s->pending_foreign_worldlist != 0;
}

/* ---- Datagrams ----------------------------------------------------------- */

static inline bool rpcn_session_send(rpcn_session_t *s, const void *data, uint32_t len) {
    if (!s->peer_ip || !s->peer_port) return false;
    return rpcn_send_to(&s->client, s->peer_ip, s->peer_port, data, len);
}

/* Bytes received from the peer, or 0. Signaling replies and punches are absorbed
 * here so nothing above ever sees them. */
static inline int rpcn_session_recv(rpcn_session_t *s, void *buf, uint32_t cap) {
    for (;;) {
        uint32_t ip = 0;
        uint16_t port = 0;
        int got = rpcn_recv_from(&s->client, buf, cap, &ip, &port);
        if (got <= 0) return 0;

        /* Signaling replies share this socket; route them by SOURCE rather than
         * by content, since a signaling reply's leading bytes can look exactly
         * like a game packet header. */
        if (rpcn_is_signaling_source(&s->client, ip, port)) { s->signaling_seen = true; continue; }

        /* A datagram from OURSELVES. This is not paranoia: when two peers share a
         * public IPv4 the server hands each the other's LOCAL address with port
         * 3658 HARDCODED (room_manager.rs and cmd_misc.rs both do it), so two
         * clients on one machine — or one whose peer has not yet been registered
         * — are told to punch at an address that is their own socket. Without
         * this the punch comes straight back, `peer_heard` latches onto our own
         * port, and every real datagram from the peer is then discarded as a
         * stray. */
        if (ip == s->client.local_ip && port == s->client.local_port) continue;

        if (!s->peer_heard) {
            /* First contact. Prefer the source we actually hear from over the one
             * we were told about: a peer behind a symmetric NAT reaches us from a
             * different port than the server observed, and that address is the
             * only one that can work. Only the PORT may differ though — a datagram
             * from an unrelated IP is not our peer. */
            bool plausible = (!s->peer_ip || ip == s->peer_ip);
            if (!plausible) continue;

            s->peer_heard = true;
            bool moved = (port != s->peer_port || ip != s->peer_ip);
            s->peer_ip   = ip;
            s->peer_port = port;
            if (s->stage == RPCN_STAGE_HOSTING || s->stage == RPCN_STAGE_JOINING)
                s->stage = RPCN_STAGE_LINKED;
            rpcn_session_note(s, moved
                ? "the peer reached us from %s (not the advertised port); using that"
                : "peer link established with %s", rpcn_session_peer_text(s));
        } else if (ip != s->peer_ip || port != s->peer_port) {
            continue;   /* stray datagram from somewhere else */
        }

        /* A punch is not game traffic; it exists only to open the NAT. */
        if (got == (int)sizeof(g_rpcn_punch) && memcmp(buf, g_rpcn_punch, sizeof(g_rpcn_punch)) == 0)
            continue;

        return got;
    }
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
