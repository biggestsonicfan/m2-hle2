/*
 * rpcn_client.h — the RPCN protocol: framing, commands, replies, notifications,
 * and the UDP signaling helper that lets two peers learn each other's address.
 *
 * Verified against the RPCN server source (RipleyTom/rpcn), not guessed:
 *
 *   Header, 15 bytes, little-endian:
 *     [0]      u8   packet_type   Request=0 Reply=1 Notification=2 ServerInfo=3
 *     [1..3]   u16  command
 *     [3..7]   u32  packet_size   TOTAL, INCLUDING this header
 *     [7..15]  u64  packet_id     echoed back on the reply
 *   A Reply carries u8 ErrorType at [15], then its payload.
 *   Strings in payloads are NUL-terminated raw bytes.
 *   Unauthenticated clients are dropped after 10 s, so log in promptly.
 *
 * Room commands instead carry [12-byte ComId][u32 LE protobuf length][protobuf],
 * and every room REPLY is [u32 LE length][protobuf] (the server's
 * Client::add_data_packet). Strip that prefix before reading, or the length is
 * parsed as a tag and the message silently reads as empty.
 *
 * The UDP signaling helper (server port 3657) records each client's public
 * address so peers can be told where to punch; game traffic itself is direct P2P
 * on port 3658 and never touches the server.
 *
 * Ported from yampnet's RpcnClient.{h,cpp}. The command and error numbers are the
 * declaration order of the server's own enums (src/server/client.rs).
 */
#ifndef RPCN_CLIENT_H
#define RPCN_CLIENT_H

#include "net_socket.h"
#include "protobuf.h"
#include "tls.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- Protocol constants -------------------------------------------------- */

#define RPCN_HEADER_SIZE    15u
#define RPCN_DEFAULT_PORT   31313
#define RPCN_SIGNALING_PORT 3657    /* server-side UDP helper */
#define RPCN_P2P_PORT       3658    /* peer-to-peer game traffic */

typedef enum {
    RPCN_CMD_LOGIN                    = 0,
    RPCN_CMD_TERMINATE                = 1,
    RPCN_CMD_CREATE                   = 2,
    /* Re-mails the token of an account that already exists. Unauthenticated like
     * Create, and like Create the server hangs up after answering it. */
    RPCN_CMD_SEND_TOKEN               = 4,
    RPCN_CMD_GET_SERVER_LIST          = 12,
    RPCN_CMD_GET_WORLD_LIST           = 13,
    RPCN_CMD_CREATE_ROOM              = 14,
    RPCN_CMD_JOIN_ROOM                = 15,
    RPCN_CMD_LEAVE_ROOM               = 16,
    RPCN_CMD_SEARCH_ROOM              = 17,
    RPCN_CMD_SET_ROOM_DATA_EXTERNAL   = 19,
    RPCN_CMD_GET_ROOM_DATA_INTERNAL   = 20,
    RPCN_CMD_SET_ROOM_DATA_INTERNAL   = 21,
    RPCN_CMD_SET_ROOM_MEMBER_DATA     = 23,
    RPCN_CMD_REQUEST_SIGNALING_INFOS  = 27,
    /* OAuth device code flow. Unauthenticated, like Login and Create, and unlike
     * those two the connection SURVIVES every status they return — a client that
     * offers both login methods can fall back to a password on the same socket. */
    RPCN_CMD_TWITCH_DEVICE_START      = 63,
    RPCN_CMD_TWITCH_DEVICE_POLL       = 64,
} rpcn_command_t;

/*
 * The whole ErrorType enum rather than the handful this client can meet: the
 * numbers are what a log carries, a name is the only thing that makes one
 * readable, and a partial copy is how the room-password codes once ended up
 * documented one off from the server's.
 */
typedef enum {
    RPCN_OK                        = 0,
    RPCN_ERR_MALFORMED             = 1,
    RPCN_ERR_INVALID               = 2,
    RPCN_ERR_INVALID_INPUT         = 3,
    RPCN_ERR_TOO_SOON              = 4,
    /* Login. WHICH credential was refused is the entire value of these three:
     * the account name, the password and the e-mail token fail separately and
     * are fixed separately. */
    RPCN_ERR_LOGIN                 = 5,
    RPCN_ERR_LOGIN_ALREADY         = 6,
    RPCN_ERR_LOGIN_BAD_USERNAME    = 7,
    RPCN_ERR_LOGIN_BAD_PASSWORD    = 8,
    RPCN_ERR_LOGIN_BAD_TOKEN       = 9,
    /* Create. */
    RPCN_ERR_CREATION              = 10,
    RPCN_ERR_CREATION_EXISTING_USER= 11,
    RPCN_ERR_CREATION_BANNED_EMAIL = 12,
    RPCN_ERR_CREATION_EXISTING_MAIL= 13,
    RPCN_ERR_ROOM_MISSING          = 14,
    RPCN_ERR_ROOM_ALREADY_JOINED   = 15,
    RPCN_ERR_ROOM_FULL             = 16,
    RPCN_ERR_ROOM_PASSWORD_MISMATCH= 17,
    RPCN_ERR_ROOM_PASSWORD_MISSING = 18,
    RPCN_ERR_ROOM_GROUP_NO_LABEL   = 19,
    RPCN_ERR_ROOM_GROUP_FULL       = 20,
    RPCN_ERR_ROOM_GROUP_NO_JOIN    = 21,
    RPCN_ERR_ROOM_GROUP_SLOT_MISM  = 22,
    RPCN_ERR_UNAUTHORIZED          = 23,
    RPCN_ERR_DB_FAIL               = 24,
    RPCN_ERR_EMAIL_FAIL            = 25,
    RPCN_ERR_NOT_FOUND             = 26,
    RPCN_ERR_BLOCKED               = 27,
    RPCN_ERR_ALREADY_FRIEND        = 28,
    RPCN_ERR_SCORE_NOT_BEST        = 29,
    RPCN_ERR_SCORE_INVALID         = 30,
    RPCN_ERR_SCORE_HAS_DATA        = 31,
    RPCN_ERR_COND_FAIL             = 32,
    RPCN_ERR_UNSUPPORTED           = 33,
    /* Twitch device flow. None of these close the connection. */
    RPCN_ERR_TWITCH_DISABLED       = 34,   /* not configured here — hide the button */
    RPCN_ERR_TWITCH_PENDING        = 35,   /* not approved yet, keep polling */
    RPCN_ERR_TWITCH_SLOW_DOWN      = 36,   /* polling faster than `interval` */
    RPCN_ERR_TWITCH_EXPIRED        = 37,   /* code expired or already used */
    RPCN_ERR_TWITCH_DENIED         = 38,   /* the user refused it */
    RPCN_ERR_TWITCH_ERROR          = 39,   /* the server could not reach Twitch */
} rpcn_error_t;

/*
 * Server-pushed notifications (packet_type 2), in the declaration order of the
 * server's NotificationType enum. These are NOT optional extras: UserJoinedRoom
 * is how a HOST learns a guest exists at all, and the only way it can learn an
 * address to punch towards before the guest's own datagrams start arriving.
 */
typedef enum {
    RPCN_NOTIF_USER_JOINED_ROOM = 0,
    RPCN_NOTIF_USER_LEFT_ROOM   = 1,
    RPCN_NOTIF_ROOM_DESTROYED   = 2,
    /* A room's or a member's internal binary attribute changed. These carry the
     * room's shared state (room.h), and the server sends them to the member who
     * made the change as well (`self_notification` in cmd_room.rs), so every
     * member applies an update at the same point in the stream. */
    RPCN_NOTIF_UPDATED_ROOM_DATA_INTERNAL        = 3,
    RPCN_NOTIF_UPDATED_ROOM_MEMBER_DATA_INTERNAL = 4,
    RPCN_NOTIF_SIGNALING_HELPER = 12,
} rpcn_notification_t;

/*
 * The binary attributes a room carries, by the ids RPCN accepts (room_manager.rs).
 * Room internal attribute 1 is the room's shared state and member internal
 * attribute 1 each member's own, exactly the slots the PS3 port used for the
 * same two things (0x57 and 0x59, np_session_create_join_room). The server
 * holds up to 256 and 128 bytes. room.h uses far less; a PS3 room's (ps3_link.h)
 * is 0xE8 bytes, so the buffer is the server's whole 256.
 */
#define RPCN_ROOM_BIN_ATTR_ID        0x57u
#define RPCN_MEMBER_BIN_ATTR_ID      0x59u
#define RPCN_ROOM_BIN_MAX            256u
#define RPCN_MEMBER_BIN_MAX          32u
#define RPCN_ROOM_MAX_MEMBERS        8u
/*
 * Room searchable int attribute 1 (SCE_NP_MATCHING2_ROOM_SEARCHABLE_INT_ATTR_EXTERNAL_1_ID):
 * the owner's round trip to RPCN's signaling helper, in ms, 0 = not measured.
 * Unlike the flag word it is only in a search result when the search asks for
 * it by id (attrId), and only the owner may change it (set_roomdata_external).
 * A lobby adds its own trip to the owner's to estimate the trip between the two
 * before joining -- see rpcn_session_relay_ms for why that is a fair estimate.
 */
#define RPCN_ROOM_INT_ATTR_RELAY     0x4Cu

/* SCE_NP_MATCHING2_ROOMMEMBER_FLAG_ATTR_OWNER, in a member's flagAttr. */
#define RPCN_MEMBER_FLAG_OWNER       0x80000000u

/* What RPCN mails: 8 random bytes formatted "{:02X}", so 16 uppercase hex. */
#define RPCN_TOKEN_LENGTH 16

/* A decoded inbound packet. `payload` points into the client's buffer and is
 * valid only until the next rpcn_poll(). */
typedef struct {
    uint8_t        type;
    uint16_t       command;
    uint64_t       packet_id;
    rpcn_error_t   error;
    const uint8_t *payload;      /* past the error byte for replies */
    uint32_t       payload_size;
} rpcn_packet_t;

/* One row of a SearchRoom reply. */
typedef struct {
    uint64_t room_id;
    uint16_t cur_members;
    uint16_t max_slots;
    bool     has_password;
    char     owner[20];
    uint32_t flag_attr;          /* as published by CreateRoom */
    uint32_t relay_ms;           /* RPCN_ROOM_INT_ATTR_RELAY; 0 = not published */
    /* Searchable int attributes 0x4C..0x53, the ones the search asked for
     * (bit i of int_mask = 0x4C + i came back). A PS3 room's rules live here. */
    uint32_t int_attr[8];
    uint8_t  int_mask;
} rpcn_room_listing_t;

/* One RoomMemberDataInternal: who, where in the room, and their own attribute. */
typedef struct {
    uint16_t member_id;
    uint8_t  team_id;
    uint32_t flag_attr;          /* RPCN_MEMBER_FLAG_OWNER marks the owner */
    char     npid[20];
    uint8_t  bin[RPCN_MEMBER_BIN_MAX];
    uint32_t bin_len;            /* 0 = the member has published nothing */
} rpcn_member_info_t;

/* One RoomDataInternal: the whole room as the server holds it. */
typedef struct {
    uint64_t           room_id;
    uint32_t           max_slot;
    uint16_t           owner_id;
    uint32_t           flag_attr;
    uint8_t            bin[RPCN_ROOM_BIN_MAX];
    uint32_t           bin_len;  /* 0 = nobody has set it */
    rpcn_member_info_t members[RPCN_ROOM_MAX_MEMBERS];
    uint32_t           member_count;
} rpcn_room_info_t;

typedef struct {
    tls_client_t tls;
    uint64_t     next_packet_id;

    /* Inbound reassembly: TLS gives a byte stream, packets straddle records. */
    uint8_t      in[64 * 1024];
    uint32_t     in_used;
    uint32_t     in_consumed;   /* bytes of `in` the last poll handed out; dropped at the next */

    net_sock_t   udp;
    /* The signaling helper lives on the SAME host as the TLS server but on UDP
     * 3657, so the address is resolved once at connect and reused. */
    uint32_t     signaling_addr;   /* network byte order */
    /* Our own end of the P2P socket. Cached because the keepalive carries the LAN
     * address on every send, and because the session layer has to be able to
     * recognise a datagram that came from ITSELF — see rpcn_session_recv. */
    uint32_t     local_ip;         /* network byte order */
    uint16_t     local_port;
    int64_t      user_id;
    char         error[256];
} rpcn_client_t;

/* ---- Little-endian scalars ----------------------------------------------- */

static inline void rpcn_put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void rpcn_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void rpcn_put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}
static inline uint16_t rpcn_get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rpcn_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rpcn_get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* ---- Lifetime ------------------------------------------------------------ */

static inline void rpcn_fail(rpcn_client_t *c, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(c->error, sizeof(c->error), fmt, args);
    va_end(args);
}

static inline const char *rpcn_last_error(const rpcn_client_t *c) {
    return c->error[0] ? c->error : tls_last_error(&c->tls);
}

static inline bool rpcn_is_connected(const rpcn_client_t *c) { return tls_is_connected(&c->tls); }
static inline int64_t rpcn_user_id(const rpcn_client_t *c) { return c->user_id; }

static inline void rpcn_disconnect(rpcn_client_t *c) {
    tls_close(&c->tls);
    net_close(&c->udp);
    c->in_used = 0;
    c->in_consumed = 0;
}

static inline bool rpcn_connect(rpcn_client_t *c, const char *host, uint16_t port,
                                const cert_fingerprint_t *pinned) {
    memset(c, 0, sizeof(*c));
    c->udp            = NET_SOCK_INVALID;
    c->next_packet_id = 1;

    if (!tls_connect(&c->tls, host, port ? port : RPCN_DEFAULT_PORT, pinned)) return false;

    /* Resolve the host for the UDP signaling helper, which sits on the same
     * machine but its own port. This MUST happen after the TLS connect: the TLS
     * layer owns the winsock startup, so resolving first fails with
     * WSANOTINITIALISED for the first client in a process — which silently left
     * the address at 0 and made every signaling ping a no-op. Not fatal if it
     * fails: only signaling suffers, the TLS session is fine. */
    c->signaling_addr = net_resolve_ipv4(host);
    c->local_ip       = net_local_ipv4_towards(c->signaling_addr);
    return true;
}

/* ---- Requests ------------------------------------------------------------ */

/* Returns the packet id used, or 0 on failure. */
static inline uint64_t rpcn_request(rpcn_client_t *c, rpcn_command_t cmd,
                                    const void *payload, uint32_t payload_size) {
    if (!tls_is_connected(&c->tls)) { rpcn_fail(c, "not connected"); return 0; }

    uint32_t total = RPCN_HEADER_SIZE + payload_size;
    uint8_t  header[RPCN_HEADER_SIZE];
    header[0] = 0;                                  /* PacketType::Request */
    rpcn_put_u16(header + 1, (uint16_t)cmd);
    rpcn_put_u32(header + 3, total);                /* size INCLUDES the header */
    uint64_t id = c->next_packet_id++;
    rpcn_put_u64(header + 7, id);

    if (!tls_send_all(&c->tls, header, RPCN_HEADER_SIZE)) return 0;
    if (payload_size && !tls_send_all(&c->tls, payload, payload_size)) return 0;
    return id;
}

/* Payloads for the non-room commands are NUL-terminated strings back to back. */
typedef struct {
    uint8_t  buf[1024];
    uint32_t n;
    bool     ok;
} rpcn_strpack_t;

static inline void rpcn_strpack_init(rpcn_strpack_t *p) { p->n = 0; p->ok = true; }

static inline void rpcn_strpack_put(rpcn_strpack_t *p, const char *s) {
    uint32_t len = (uint32_t)(s ? strlen(s) : 0);
    if (!p->ok || p->n + len + 1 > sizeof(p->buf)) { p->ok = false; return; }
    if (len) memcpy(p->buf + p->n, s, len);
    p->n += len;
    p->buf[p->n++] = 0;
}

/*
 * Login (npid, password, token).
 *
 * THE TOKEN IS NOT A SECOND PASSWORD: it is the e-mail verification token, 16
 * hexadecimal characters the server mails when the account is created, and the
 * server compares it only when IT has e-mail validation switched on —
 * cmd_account.rs passes is_email_validated() as check_user's check_token
 * argument, which is off by default and off on most community servers. A client
 * cannot ask which kind of server it is talking to, so it sends whatever the
 * player has: empty is correct for a server that does not validate, and one that
 * does answers LoginInvalidToken rather than a generic refusal.
 */
static inline uint64_t rpcn_login(rpcn_client_t *c, const char *npid, const char *password,
                                  const char *token) {
    if (!npid || !password) { rpcn_fail(c, "login requires an npid and password"); return 0; }

    /* The token may be empty — the server reads it with get_string(true), the
     * only one of the three that tolerates that — but its terminator must still
     * be present. */
    rpcn_strpack_t p;
    rpcn_strpack_init(&p);
    rpcn_strpack_put(&p, npid);
    rpcn_strpack_put(&p, password);
    rpcn_strpack_put(&p, token ? token : "");
    if (!p.ok) { rpcn_fail(c, "login credentials too long"); return 0; }
    return rpcn_request(c, RPCN_CMD_LOGIN, p.buf, p.n);
}

/*
 * SendToken: mails the account's token again, for a player who signed up and
 * never got the message. It authenticates with check_user(..., check_token =
 * false), so the token is not needed to ask for the token — which is the entire
 * point of the command. Unauthenticated like Create, and the server hangs up
 * after answering. Refused with Invalid on a server that does no e-mail
 * validation, and TooSoon within 24 hours of the last one.
 */
static inline uint64_t rpcn_resend_token(rpcn_client_t *c, const char *npid, const char *password) {
    /* Both are read with get_string(false), so an empty one is Malformed at the
     * server. Caught here because "the request was malformed" says nothing about
     * which box was left blank. */
    if (!npid || !*npid || !password || !*password) {
        rpcn_fail(c, "SendToken: the account name and password are both required");
        return 0;
    }
    rpcn_strpack_t p;
    rpcn_strpack_init(&p);
    rpcn_strpack_put(&p, npid);
    rpcn_strpack_put(&p, password);
    if (!p.ok) { rpcn_fail(c, "SendToken: credentials too long"); return 0; }
    return rpcn_request(c, RPCN_CMD_SEND_TOKEN, p.buf, p.n);
}

/*
 * Tidies a token as a PLAYER supplies it — which means pasted out of an e-mail,
 * with whatever whitespace came along. Trims both ends, and upper-cases a value
 * that is entirely hexadecimal because the server stores it upper-case and
 * compares byte for byte.
 *
 * Anything that is not hex passes through with its case untouched instead of
 * being rejected: the format belongs to the server, and this must never be the
 * thing that locks a player out of one that changed it. False only if the result
 * does not fit `cap`; `out` is always NUL-terminated when cap > 0.
 */
static inline bool rpcn_normalize_token(const char *in, char *out, uint32_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!in) return true;

    const char *begin = in;
    while (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n'
           || *begin == '\v' || *begin == '\f') begin++;
    const char *end = begin + strlen(begin);
    while (end > begin) {
        char c = end[-1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\v' && c != '\f') break;
        end--;
    }

    uint32_t len = (uint32_t)(end - begin);
    if (len + 1 > cap) return false;

    /* Case is only ours to change where it cannot mean anything: an all-hex value
     * is the server's own "{:02X}" and folds safely, while anything else might be
     * a format that distinguishes case and is copied exactly. */
    bool all_hex = (len != 0);
    for (const char *p = begin; p != end; p++) {
        bool hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F');
        if (!hex) { all_hex = false; break; }
    }

    uint32_t n = 0;
    for (const char *p = begin; p != end; p++) {
        char c = *p;
        if (all_hex && c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
        out[n++] = c;
    }
    out[n] = '\0';
    return true;
}

/* True when `token` is exactly what the server mails — for WARNING that what was
 * pasted does not look like one, never for refusing to send it. */
static inline bool rpcn_looks_like_token(const char *token) {
    if (!token || strlen(token) != RPCN_TOKEN_LENGTH) return false;
    for (const char *p = token; *p; p++) {
        bool hex = (*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f');
        if (!hex) return false;
    }
    return true;
}

static inline bool rpcn_valid_username(const char *s) {
    size_t len = strlen(s);
    if (len < 3 || len > 16) return false;
    for (const char *p = s; *p; p++) {
        bool alnum = (*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z');
        if (!alnum && *p != '-' && *p != '_') return false;
    }
    return true;
}

/*
 * Create an account. Server rules, worth knowing before calling:
 *   * npid and online_name must be 3-16 chars of [A-Za-z0-9_-].
 *   * NONE of the five fields may be empty — the server reads them all with
 *     get_string(false), so an empty avatar_url alone is rejected as Malformed.
 *   * email must PARSE as a real address even when validation is disabled; a
 *     token is only needed if the server has EmailUrl set (empty by default).
 */
static inline uint64_t rpcn_create_account(rpcn_client_t *c, const char *npid,
                                           const char *password, const char *online_name,
                                           const char *avatar_url, const char *email) {
    struct { const char *v; const char *name; } fields[] = {
        { npid, "npid" }, { password, "password" }, { online_name, "online_name" },
        { avatar_url, "avatar_url" }, { email, "email" },
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (!fields[i].v || !*fields[i].v) {
            rpcn_fail(c, "Create: %s must not be empty", fields[i].name);
            return 0;
        }
    }
    if (!rpcn_valid_username(npid)) {
        rpcn_fail(c, "Create: the account name must be 3-16 characters of [A-Za-z0-9_-]");
        return 0;
    }
    if (!rpcn_valid_username(online_name)) {
        rpcn_fail(c, "Create: the online name must be 3-16 characters of [A-Za-z0-9_-]");
        return 0;
    }

    rpcn_strpack_t p;
    rpcn_strpack_init(&p);
    rpcn_strpack_put(&p, npid);
    rpcn_strpack_put(&p, password);
    rpcn_strpack_put(&p, online_name);
    rpcn_strpack_put(&p, avatar_url);
    rpcn_strpack_put(&p, email);
    if (!p.ok) { rpcn_fail(c, "Create: fields too long"); return 0; }
    return rpcn_request(c, RPCN_CMD_CREATE, p.buf, p.n);
}

/* ---- Room command framing ------------------------------------------------ */

/* [12-byte ComId][u32 LE protobuf size][protobuf]. Returns total bytes, or 0 if
 * the ComId is unusable. */
static inline uint32_t rpcn_frame_room_payload(uint8_t *out, uint32_t cap, const char *com_id,
                                               const uint8_t *pb, uint32_t pb_size) {
    uint32_t total = 12 + 4 + pb_size;
    if (!com_id || cap < total) return 0;

    /* Exactly 12 bytes, NUL-padded. The server rejects it unless the first 9 are
     * ASCII uppercase or digits, so a lowercase id fails as Malformed rather than
     * as NotFound. */
    memset(out, 0, 12);
    uint32_t len = (uint32_t)strlen(com_id);
    memcpy(out, com_id, len < 12 ? len : 12);
    for (int i = 0; i < 9; i++) {
        char c = (char)out[i];
        bool valid = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!valid) return 0;
    }

    rpcn_put_u32(out + 12, pb_size);
    if (pb_size) memcpy(out + 16, pb, pb_size);
    return total;
}

/*
 * Discovery, in the order the server expects before a room can be created. With
 * the server's CreateMissing=true these also REGISTER a previously unknown title,
 * so a fresh ComId becomes usable without anybody editing servers.cfg. Neither
 * carries protobuf: GetServerList is the ComId alone, GetWorldList the ComId
 * followed by a u16 server id.
 */
static inline uint64_t rpcn_get_server_list(rpcn_client_t *c, const char *com_id) {
    uint8_t payload[16];
    if (!rpcn_frame_room_payload(payload, sizeof(payload), com_id, NULL, 0)) {
        rpcn_fail(c, "GetServerList: bad ComId '%s'", com_id ? com_id : "(null)");
        return 0;
    }
    return rpcn_request(c, RPCN_CMD_GET_SERVER_LIST, payload, 12);
}

static inline uint64_t rpcn_get_world_list(rpcn_client_t *c, const char *com_id, uint16_t server_id) {
    uint8_t payload[16];
    if (!rpcn_frame_room_payload(payload, sizeof(payload), com_id, NULL, 0)) {
        rpcn_fail(c, "GetWorldList: bad ComId");
        return 0;
    }
    rpcn_put_u16(payload + 12, server_id);
    return rpcn_request(c, RPCN_CMD_GET_WORLD_LIST, payload, 14);
}

static inline uint32_t rpcn_parse_server_list(const uint8_t *p, uint32_t size,
                                              uint16_t *out, uint32_t max) {
    if (size < 2) return 0;
    uint32_t count = rpcn_get_u16(p);
    if (2 + count * 2 > size) count = (size - 2) / 2;
    if (count > max) count = max;
    for (uint32_t i = 0; i < count; i++) out[i] = rpcn_get_u16(p + 2 + i * 2);
    return count;
}

static inline uint32_t rpcn_parse_world_list(const uint8_t *p, uint32_t size,
                                             uint32_t *out, uint32_t max) {
    /* NOTE the asymmetry with the server list: the world count is a u32 while the
     * server count is a u16 (cmd_server.rs). Reading it as u16 shifts every entry
     * by two bytes and yields plausible-but-wrong world ids rather than an obvious
     * failure. */
    if (size < 4) return 0;
    uint32_t count = rpcn_get_u32(p);
    if (4 + count * 4 > size) count = (size - 4) / 4;
    if (count > max) count = max;
    for (uint32_t i = 0; i < count; i++) out[i] = rpcn_get_u32(p + 4 + i * 4);
    return count;
}

/*
 * CreateRoom. `password` may be null/empty for a public room; RPCN answers
 * RoomPasswordMismatch(17) or RoomPasswordMissing(18) on a bad join rather than a
 * generic failure.
 *
 * `flag_attr` is the room's u32 attribute word, which netplay uses to publish
 * what a match will be played under (see NETPLAY_ROOM_* in netplay.h). The server
 * stores it verbatim apart from SCE_NP_MATCHING2_ROOM_FLAG_ATTR_FULL (0x20000000),
 * which it clears here and sets itself once the room is full — so never rely on
 * that bit.
 *
 * The room is created WITH SIGNALING ENABLED (sigOptParam). That single field is
 * what makes the server exchange peer addresses on its own: without it
 * `need_signaling` is false in room_manager.rs, the join reply carries no
 * signaling_data and the host's UserJoinedRoom notification carries no address —
 * which leaves the host with nobody to punch towards.
 */
/* BinAttr { uint16 id = 1; bytes data = 2; } as field `field`. The id is a
 * WRAPPER, read with get_verified(): a bare varint there is Malformed. */
static inline void rpcn_pb_bin_attr(pb_writer_t *w, uint32_t field, uint16_t id,
                                    const void *data, uint32_t len) {
    uint32_t tok = pb_begin_sub(w, field);
    pb_wrapped(w, 1, id);
    pb_bytes(w, 2, data, len);
    pb_end_sub(w, tok);
}

/* IntAttr { uint16 id = 1; uint32 num = 2; } as field `field`. The id is a
 * wrapper, as in BinAttr; the number is a bare varint. */
static inline void rpcn_pb_int_attr(pb_writer_t *w, uint32_t field, uint16_t id, uint32_t num) {
    uint32_t tok = pb_begin_sub(w, field);
    pb_wrapped(w, 1, id);
    pb_varint(w, 2, num);
    pb_end_sub(w, tok);
}

/*
 * `room_bin` / `member_bin` seed the room's shared state and the creator's own
 * attribute (room.h), so the room is never seen without them. Either may be
 * null, which leaves the attribute empty. `relay_ms` is RPCN_ROOM_INT_ATTR_RELAY,
 * left unset at 0.
 */
static inline uint64_t rpcn_create_room(rpcn_client_t *c, const char *com_id, uint32_t world_id,
                                        uint32_t max_slot, const char *password,
                                        uint32_t flag_attr,
                                        const uint8_t *room_bin, uint32_t room_len,
                                        const uint8_t *member_bin, uint32_t member_len,
                                        uint32_t relay_ms) {
    uint8_t pb[512];
    pb_writer_t w;
    pb_writer_init(&w, pb, sizeof(pb));
    pb_varint(&w, 1, world_id);                 /* worldId */
    pb_varint(&w, 3, max_slot);                 /* maxSlot */
    if (flag_attr != 0) {
        /* flagAttr. A bare varint, unlike the uint8/uint16 fields around it — the
         * proto types it uint32, so it is not one of the flatbuffers-era wrappers. */
        pb_varint(&w, 4, flag_attr);
    }
    if (room_bin && room_len)                   /* roomBinAttrInternal */
        rpcn_pb_bin_attr(&w, 5, RPCN_ROOM_BIN_ATTR_ID, room_bin, room_len);
    if (relay_ms)                               /* roomSearchableIntAttrExternal */
        rpcn_pb_int_attr(&w, 6, RPCN_ROOM_INT_ATTR_RELAY, relay_ms);
    if (password && *password) {
        /* TWO RULES, BOTH ENFORCED SILENTLY BY THE SERVER — get either wrong and
         * the room ends up with NO password while still looking protected here.
         *
         * 1. The password is a FIXED 8 BYTES (SceNpMatching2SessionPassword).
         *    room_manager.rs takes it only `if pb.room_password.len() == 8` and
         *    otherwise just logs "Invalid password length" and leaves the room
         *    open. So pad/truncate to 8.
         * 2. passwordSlotMask marks WHICH SLOTS the password guards, and defaults
         *    to 0 — meaning every slot is public. A joiner sending no password
         *    takes req_slot(false) and walks straight into a "private" room.
         *    Marking all maxSlot slots private (the mask is MSB-first: slot i =
         *    bit 63-i) is what actually gates the room, and is also how a browser
         *    tells a locked room from an open one, since the search reply exposes
         *    privateSlotNum but no password flag. */
        char fixed[8];
        memset(fixed, 0, sizeof(fixed));
        size_t given = strlen(password);
        memcpy(fixed, password, given < sizeof(fixed) ? given : sizeof(fixed));
        pb_bytes(&w, 9, fixed, sizeof(fixed));                       /* roomPassword */

        uint64_t slot_mask = 0;
        for (uint32_t i = 0; i < max_slot && i < 64; i++) slot_mask |= (1ull << (63 - i));
        pb_varint(&w, 11, slot_mask);                                /* passwordSlotMask */
    }
    if (member_bin && member_len)               /* roomMemberBinAttrInternal */
        rpcn_pb_bin_attr(&w, 15, RPCN_MEMBER_BIN_ATTR_ID, member_bin, member_len);
    /* teamId is NOT optional despite proto3: room_manager.rs does
     * `pb.team_id.get_verified()?` and Option<Uint8>::get_verified() returns
     * Malformed when the field is absent. It is a uint8 WRAPPER submessage, so it
     * has to be written as { uint32 value = 1 }, not a bare varint. */
    pb_wrapped(&w, 16, 0);

    /* sigOptParam — THE FIELD THAT MAKES PEERS REACHABLE. All three of its fields
     * are uint8/uint16 wrappers read with get_verified(), so omitting any one is
     * Malformed rather than a default. Mesh (1) is "everyone connects to
     * everyone"; for two players that is star without needing a hub id. */
    {
        uint32_t sig = pb_begin_sub(&w, 17);
        pb_wrapped(&w, 1, 1);                   /* type = SignalingMesh */
        pb_wrapped(&w, 2, 0);                   /* flag = 0 — bit 0 would disable it */
        pb_wrapped(&w, 3, 0);                   /* hubMemberId — unused by mesh */
        pb_end_sub(&w, sig);
    }

    if (!w.ok) { rpcn_fail(c, "CreateRoom: protobuf overflow"); return 0; }

    uint8_t payload[640];
    uint32_t n = rpcn_frame_room_payload(payload, sizeof(payload), com_id, pb, w.used);
    if (!n) { rpcn_fail(c, "CreateRoom: bad ComId '%s'", com_id ? com_id : "(null)"); return 0; }
    return rpcn_request(c, RPCN_CMD_CREATE_ROOM, payload, n);
}

static inline uint64_t rpcn_join_room(rpcn_client_t *c, const char *com_id, uint64_t room_id,
                                      const char *password,
                                      const uint8_t *member_bin, uint32_t member_len) {
    uint8_t pb[256];
    pb_writer_t w;
    pb_writer_init(&w, pb, sizeof(pb));
    pb_varint(&w, 1, room_id);                  /* roomId */
    if (password && *password) {
        /* Same fixed 8 bytes as CreateRoom — the server compares the raw arrays,
         * so a password padded differently here would simply never match. */
        char fixed[8];
        memset(fixed, 0, sizeof(fixed));
        size_t given = strlen(password);
        memcpy(fixed, password, given < sizeof(fixed) ? given : sizeof(fixed));
        pb_bytes(&w, 2, fixed, sizeof(fixed));                       /* roomPassword */
    }
    if (member_bin && member_len)               /* roomMemberBinAttrInternal */
        rpcn_pb_bin_attr(&w, 4, RPCN_MEMBER_BIN_ATTR_ID, member_bin, member_len);
    pb_wrapped(&w, 6, 0);                       /* teamId — mandatory here too */
    if (!w.ok) { rpcn_fail(c, "JoinRoom: protobuf overflow"); return 0; }

    uint8_t payload[512];
    uint32_t n = rpcn_frame_room_payload(payload, sizeof(payload), com_id, pb, w.used);
    if (!n) { rpcn_fail(c, "JoinRoom: bad ComId"); return 0; }
    return rpcn_request(c, RPCN_CMD_JOIN_ROOM, payload, n);
}

static inline uint64_t rpcn_leave_room(rpcn_client_t *c, const char *com_id, uint64_t room_id) {
    uint8_t pb[32];
    pb_writer_t w;
    pb_writer_init(&w, pb, sizeof(pb));
    pb_varint(&w, 1, room_id);                  /* roomId */
    if (!w.ok) { rpcn_fail(c, "LeaveRoom: protobuf overflow"); return 0; }
    uint8_t payload[64];
    uint32_t n = rpcn_frame_room_payload(payload, sizeof(payload), com_id, pb, w.used);
    if (!n) { rpcn_fail(c, "LeaveRoom: bad ComId"); return 0; }
    return rpcn_request(c, RPCN_CMD_LEAVE_ROOM, payload, n);
}

/*
 * SetRoomDataInternal { roomId = 1; flagFilter = 2; flagAttr = 3;
 * repeated BinAttr roomBinAttrInternal = 4; ... }. The room's shared state.
 *
 * ANY MEMBER MAY WRITE IT: set_roomdata_internal checks ownership only for the
 * flag and password fields, and stores a bin attr from whoever sent it. So the
 * rule that only the owner writes the room state is ours (room.h), not the
 * server's. A flagFilter of 0 leaves the room's flags as they are.
 */
static inline uint64_t rpcn_set_room_data_internal(rpcn_client_t *c, const char *com_id,
                                                   uint64_t room_id,
                                                   const uint8_t *bin, uint32_t len) {
    uint8_t pb[RPCN_ROOM_BIN_MAX + 64];
    pb_writer_t w;
    pb_writer_init(&w, pb, sizeof(pb));
    pb_varint(&w, 1, room_id);
    rpcn_pb_bin_attr(&w, 4, RPCN_ROOM_BIN_ATTR_ID, bin, len);
    if (!w.ok) { rpcn_fail(c, "SetRoomDataInternal: protobuf overflow"); return 0; }
    uint8_t payload[RPCN_ROOM_BIN_MAX + 128];
    uint32_t n = rpcn_frame_room_payload(payload, sizeof(payload), com_id, pb, w.used);
    if (!n) { rpcn_fail(c, "SetRoomDataInternal: bad ComId"); return 0; }
    return rpcn_request(c, RPCN_CMD_SET_ROOM_DATA_INTERNAL, payload, n);
}

/*
 * SetRoomMemberDataInternal { roomId = 1; uint16 memberId = 2; uint8 teamId = 3;
 * repeated BinAttr roomMemberBinAttrInternal = 4; }. Our own attribute.
 * memberId 0 means "me", and teamId 0 means "leave it": both are wrappers
 * read with get_verified(), so both must be present even at 0.
 */
static inline uint64_t rpcn_set_member_data_internal(rpcn_client_t *c, const char *com_id,
                                                     uint64_t room_id,
                                                     const uint8_t *bin, uint32_t len) {
    uint8_t pb[160];
    pb_writer_t w;
    pb_writer_init(&w, pb, sizeof(pb));
    pb_varint(&w, 1, room_id);
    pb_wrapped(&w, 2, 0);                       /* memberId: self */
    pb_wrapped(&w, 3, 0);                       /* teamId: unchanged */
    rpcn_pb_bin_attr(&w, 4, RPCN_MEMBER_BIN_ATTR_ID, bin, len);
    if (!w.ok) { rpcn_fail(c, "SetRoomMemberDataInternal: protobuf overflow"); return 0; }
    uint8_t payload[224];
    uint32_t n = rpcn_frame_room_payload(payload, sizeof(payload), com_id, pb, w.used);
    if (!n) { rpcn_fail(c, "SetRoomMemberDataInternal: bad ComId"); return 0; }
    return rpcn_request(c, RPCN_CMD_SET_ROOM_MEMBER_DATA, payload, n);
}

/*
 * SetRoomDataExternalRequest { roomId = 1; repeated IntAttr
 * roomSearchableIntAttrExternal = 2; ... }. Owner only: anyone else is
 * Unauthorized. Used for RPCN_ROOM_INT_ATTR_RELAY alone.
 */
static inline uint64_t rpcn_set_room_relay(rpcn_client_t *c, const char *com_id, uint64_t room_id,
                                           uint32_t relay_ms) {
    uint8_t pb[64];
    pb_writer_t w;
    pb_writer_init(&w, pb, sizeof(pb));
    pb_varint(&w, 1, room_id);
    rpcn_pb_int_attr(&w, 2, RPCN_ROOM_INT_ATTR_RELAY, relay_ms);
    if (!w.ok) { rpcn_fail(c, "SetRoomDataExternal: protobuf overflow"); return 0; }
    uint8_t payload[128];
    uint32_t n = rpcn_frame_room_payload(payload, sizeof(payload), com_id, pb, w.used);
    if (!n) { rpcn_fail(c, "SetRoomDataExternal: bad ComId"); return 0; }
    return rpcn_request(c, RPCN_CMD_SET_ROOM_DATA_EXTERNAL, payload, n);
}

/*
 * GetRoomDataInternal { roomId = 1; repeated uint16 attrId = 2; }. The whole room
 * again. Needed because RPCN says nothing when ownership moves: a departing
 * owner's successor is picked in room_manager.rs `leave_room` and the other
 * members get only a UserLeftRoom about the one who went. The server returns
 * every bin attr whatever attrId asks for.
 */
static inline uint64_t rpcn_get_room_data_internal(rpcn_client_t *c, const char *com_id,
                                                   uint64_t room_id) {
    uint8_t pb[32];
    pb_writer_t w;
    pb_writer_init(&w, pb, sizeof(pb));
    pb_varint(&w, 1, room_id);
    if (!w.ok) { rpcn_fail(c, "GetRoomDataInternal: protobuf overflow"); return 0; }
    uint8_t payload[64];
    uint32_t n = rpcn_frame_room_payload(payload, sizeof(payload), com_id, pb, w.used);
    if (!n) { rpcn_fail(c, "GetRoomDataInternal: bad ComId"); return 0; }
    return rpcn_request(c, RPCN_CMD_GET_ROOM_DATA_INTERNAL, payload, n);
}

static inline uint64_t rpcn_search_room(rpcn_client_t *c, const char *com_id, uint32_t world_id) {
    uint8_t pb[256];
    pb_writer_t w;
    pb_writer_init(&w, pb, sizeof(pb));
    /* option bit 0 = include the owner's npId in each result. Without it the
     * server's to_RoomDataExternal leaves `owner` unset (it gates on
     * `search_option & 0x7`) and every room comes back nameless. Bits 1 and 2
     * would add onlineName / avatarUrl; the npid is all we display. */
    pb_varint(&w, 1, 1);                        /* option */
    pb_varint(&w, 2, world_id);                 /* worldId */
    /* The range filter is 1-BASED and capped at 20 (SCE_NP_MATCHING2_RANGE_FILTER_MAX).
     * The server logs "startIndex was 0!" / "max was invalid: 32" and substitutes
     * its own values for anything outside that. */
    pb_varint(&w, 4, 1);                        /* rangeFilter_startIndex */
    pb_varint(&w, 5, 20);                       /* rangeFilter_max */
    /* attrId: which searchable attributes to include. `repeated uint16`, so each
     * is a wrapper. Without it the owner's round trip is not in the reply. */
    pb_wrapped(&w, 10, RPCN_ROOM_INT_ATTR_RELAY);
    if (!w.ok) { rpcn_fail(c, "SearchRoom: protobuf overflow"); return 0; }

    uint8_t payload[512];
    uint32_t n = rpcn_frame_room_payload(payload, sizeof(payload), com_id, pb, w.used);
    if (!n) { rpcn_fail(c, "SearchRoom: bad ComId"); return 0; }
    return rpcn_request(c, RPCN_CMD_SEARCH_ROOM, payload, n);
}

/*
 * The PS3 port's own lobby requests (Sonic the Fighters, NPUB30927), for
 * cross-play with it (ps3_link.h). The values are the game's, taken from an
 * RPCS3 log of it creating, searching and joining rooms, not chosen here.
 */

/* SearchRoom as the PS3 game shapes it: closed, full and hidden rooms left out
 * (flagFilter 0x70000000, flagAttr 0), only rooms carrying the game's version tag
 * in searchable int 0x53, and all eight rule ints returned. The game also
 * filters on its own region and room mode; we show every room it could join. */
static inline uint64_t rpcn_ps3_search_room(rpcn_client_t *c, const char *com_id, uint32_t world_id,
                                            uint32_t version_tag) {
    uint8_t pb[256];
    pb_writer_t w;
    pb_writer_init(&w, pb, sizeof(pb));
    pb_varint(&w, 1, 1);                        /* option: with the owner's npid */
    pb_varint(&w, 2, world_id);
    pb_varint(&w, 4, 1);                        /* rangeFilter_startIndex (1-based) */
    pb_varint(&w, 5, 20);                       /* rangeFilter_max */
    pb_varint(&w, 6, 0x70000000u);              /* flagFilter */
    /* flagAttr 0 is proto3's default and needs no bytes. */
    {
        uint32_t f = pb_begin_sub(&w, 8);       /* intFilter */
        pb_wrapped(&w, 1, 1);                   /* searchOperator: EQ */
        rpcn_pb_int_attr(&w, 2, 0x53, version_tag);
        pb_end_sub(&w, f);
    }
    for (uint16_t id = 0x4C; id <= 0x53; id++) pb_wrapped(&w, 10, id);   /* attrId */
    if (!w.ok) { rpcn_fail(c, "SearchRoom: protobuf overflow"); return 0; }
    uint8_t payload[512];
    uint32_t n = rpcn_frame_room_payload(payload, sizeof(payload), com_id, pb, w.used);
    if (!n) { rpcn_fail(c, "SearchRoom: bad ComId"); return 0; }
    return rpcn_request(c, RPCN_CMD_SEARCH_ROOM, payload, n);
}

/* JoinRoom as the PS3 game sends it: teamId 0xFF (a newcomer's place in the
 * waiting line is last) and its 0x20-byte member attribute. */
static inline uint64_t rpcn_ps3_join_room(rpcn_client_t *c, const char *com_id, uint64_t room_id,
                                          const uint8_t *member_bin, uint32_t member_len) {
    uint8_t pb[256];
    pb_writer_t w;
    pb_writer_init(&w, pb, sizeof(pb));
    pb_varint(&w, 1, room_id);
    if (member_bin && member_len) rpcn_pb_bin_attr(&w, 4, RPCN_MEMBER_BIN_ATTR_ID, member_bin, member_len);
    pb_wrapped(&w, 6, 0xFF);                    /* teamId */
    if (!w.ok) { rpcn_fail(c, "JoinRoom: protobuf overflow"); return 0; }
    uint8_t payload[512];
    uint32_t n = rpcn_frame_room_payload(payload, sizeof(payload), com_id, pb, w.used);
    if (!n) { rpcn_fail(c, "JoinRoom: bad ComId"); return 0; }
    return rpcn_request(c, RPCN_CMD_JOIN_ROOM, payload, n);
}

/* SetRoomMemberDataInternal with a teamId: the PS3 game publishes its place in
 * the waiting line there (0 would leave it unchanged). */
static inline uint64_t rpcn_set_member_data_team(rpcn_client_t *c, const char *com_id, uint64_t room_id,
                                                 uint8_t team_id, const uint8_t *bin, uint32_t len) {
    uint8_t pb[160];
    pb_writer_t w;
    pb_writer_init(&w, pb, sizeof(pb));
    pb_varint(&w, 1, room_id);
    pb_wrapped(&w, 2, 0);                       /* memberId: self */
    pb_wrapped(&w, 3, team_id);
    rpcn_pb_bin_attr(&w, 4, RPCN_MEMBER_BIN_ATTR_ID, bin, len);
    if (!w.ok) { rpcn_fail(c, "SetRoomMemberDataInternal: protobuf overflow"); return 0; }
    uint8_t payload[224];
    uint32_t n = rpcn_frame_room_payload(payload, sizeof(payload), com_id, pb, w.used);
    if (!n) { rpcn_fail(c, "SetRoomMemberDataInternal: bad ComId"); return 0; }
    return rpcn_request(c, RPCN_CMD_SET_ROOM_MEMBER_DATA, payload, n);
}

static inline uint64_t rpcn_request_signaling_infos(rpcn_client_t *c, const char *npid) {
    if (!npid || !*npid) { rpcn_fail(c, "RequestSignalingInfos needs an npid"); return 0; }
    rpcn_strpack_t p;
    rpcn_strpack_init(&p);
    rpcn_strpack_put(&p, npid);
    if (!p.ok) { rpcn_fail(c, "npid too long"); return 0; }
    return rpcn_request(c, RPCN_CMD_REQUEST_SIGNALING_INFOS, p.buf, p.n);
}

/* ---- Reply parsers ------------------------------------------------------- */

/* Every room reply goes through Client::add_data_packet, i.e.
 * [u32 LE length][protobuf]. Strip that before reading. */
static inline bool rpcn_strip_data_packet(const uint8_t **p, uint32_t *size) {
    if (*size < 4) return false;
    uint32_t len = rpcn_get_u32(*p);
    if (len == 0 || 4 + len > *size) return false;
    *p += 4;
    *size = len;
    return true;
}

/* SearchRoomResponse { startIndex = 1, total = 2, repeated RoomDataExternal = 3 }.
 * RoomDataExternal: privateSlotNum = 4, roomId = 6 (bare varint), maxSlot = 8,
 * curMemberNum = 10 (the numbered ones are uint16 WRAPPER submessages — see the
 * proto note in protobuf.h), owner = 12 (UserInfo), flagAttr = 14 (bare varint;
 * the server fills it unconditionally, unlike the searchable attr arrays). */
static inline uint32_t rpcn_parse_room_list(const uint8_t *payload, uint32_t size,
                                            rpcn_room_listing_t *out, uint32_t max_out) {
    if (!out || max_out == 0 || !rpcn_strip_data_packet(&payload, &size)) return 0;

    uint32_t count = 0;
    pb_reader_t top = pb_reader(payload, size);
    while (count < max_out && pb_next(&top)) {
        if (top.field != 3 || top.wire != PB_WIRE_LEN) continue;

        rpcn_room_listing_t row;
        memset(&row, 0, sizeof(row));

        pb_reader_t room = pb_sub(&top);
        while (pb_next(&room)) {
            switch (room.field) {
                case 4:  row.has_password = pb_as_wrapped(&room) != 0; break;
                case 6:  row.room_id      = room.varint; break;
                case 8:  row.max_slots    = (uint16_t)pb_as_wrapped(&room); break;
                case 10: row.cur_members  = (uint16_t)pb_as_wrapped(&room); break;
                case 12: {
                    /* UserInfo { npId = 1, onlineName = 2, avatarUrl = 3 } */
                    pb_reader_t owner = pb_sub(&room);
                    while (pb_next(&owner)) {
                        if (owner.field == 1 && owner.wire == PB_WIRE_LEN) {
                            pb_copy_string(&owner, row.owner, sizeof(row.owner));
                            break;
                        }
                    }
                    break;
                }
                case 14: row.flag_attr = (uint32_t)room.varint; break;
                case 15: {
                    /* IntAttr { uint16 id = 1 (wrapper); uint32 num = 2 } */
                    if (room.wire != PB_WIRE_LEN) break;
                    pb_reader_t attr = pb_sub(&room);
                    uint32_t id = 0, num = 0;
                    while (pb_next(&attr)) {
                        if (attr.field == 1 && attr.wire == PB_WIRE_LEN) id = (uint32_t)pb_as_wrapped(&attr);
                        else if (attr.field == 2 && attr.wire == PB_WIRE_VARINT) num = (uint32_t)attr.varint;
                    }
                    if (id == RPCN_ROOM_INT_ATTR_RELAY) row.relay_ms = num;
                    if (id >= 0x4C && id <= 0x53) {
                        row.int_attr[id - 0x4C] = num;
                        row.int_mask |= (uint8_t)(1u << (id - 0x4C));
                    }
                    break;
                }
                default: break;
            }
        }

        if (row.room_id != 0) out[count++] = row;
    }
    return count;
}

/* SignalingAddr { bytes ip = 1; uint16 port = 2; } — the port is a WRAPPER
 * submessage. Shared by every message that embeds an address. */
static inline bool rpcn_read_signaling_addr(pb_reader_t addr, uint32_t *out_ip_be,
                                            uint16_t *out_port) {
    bool got_ip = false;
    while (pb_next(&addr)) {
        if (addr.field == 1 && addr.wire == PB_WIRE_LEN && addr.bytes_len >= 4) {
            if (out_ip_be) memcpy(out_ip_be, addr.bytes, 4);
            got_ip = true;
        } else if (addr.field == 2 && addr.wire == PB_WIRE_LEN) {
            if (out_port) *out_port = (uint16_t)pb_as_wrapped(&addr);
        }
    }
    return got_ip;
}

/* A RequestSignalingInfos reply: [u32 len][SignalingAddr protobuf]. */
static inline bool rpcn_parse_signaling_addr(const uint8_t *payload, uint32_t size,
                                             uint32_t *out_ip_be, uint16_t *out_port) {
    if (!rpcn_strip_data_packet(&payload, &size)) return false;
    return rpcn_read_signaling_addr(pb_reader(payload, size), out_ip_be, out_port);
}

/*
 * NotificationUserJoinedRoom { uint64 room_id = 1; RoomMemberUpdateInfo
 * update_info = 2; SignalingAddr signaling = 3; }. `out_has_addr` distinguishes
 * "no address in this notification" (the caller should ask for one) from "the
 * address of 0.0.0.0:0".
 */
static inline bool rpcn_parse_joined_notification(const uint8_t *payload, uint32_t size,
                                                  char *out_npid, uint32_t npid_cap,
                                                  uint32_t *out_ip_be, uint16_t *out_port,
                                                  bool *out_has_addr) {
    if (out_npid && npid_cap) out_npid[0] = '\0';
    if (out_has_addr) *out_has_addr = false;
    if (!rpcn_strip_data_packet(&payload, &size)) return false;

    bool got_npid = false;
    pb_reader_t top = pb_reader(payload, size);
    while (pb_next(&top)) {
        if (top.field == 3 && top.wire == PB_WIRE_LEN) {
            pb_reader_t sub = pb_sub(&top);
            if (rpcn_read_signaling_addr(sub, out_ip_be, out_port) && out_has_addr)
                *out_has_addr = true;
            continue;
        }
        if (top.field != 2 || top.wire != PB_WIRE_LEN) continue;

        /* RoomMemberUpdateInfo.roomMemberDataInternal = 1 -> .userInfo = 1 -> .npId = 1 */
        pb_reader_t update = pb_sub(&top);
        while (pb_next(&update)) {
            if (update.field != 1 || update.wire != PB_WIRE_LEN) continue;
            pb_reader_t member = pb_sub(&update);
            while (pb_next(&member)) {
                if (member.field != 1 || member.wire != PB_WIRE_LEN) continue;
                pb_reader_t ui = pb_sub(&member);
                while (pb_next(&ui)) {
                    if (ui.field != 1 || ui.wire != PB_WIRE_LEN) continue;
                    pb_copy_string(&ui, out_npid, npid_cap);
                    got_npid = true;
                    break;
                }
            }
        }
    }
    return got_npid;
}

/* MatchingSignalingInfo { string npid = 1; SignalingAddr addr = 2; } — pushed to
 * the TARGET of a RequestSignalingInfos, carrying the caller's address. The
 * server sends it precisely so both ends punch. */
static inline bool rpcn_parse_signaling_helper(const uint8_t *payload, uint32_t size,
                                               char *out_npid, uint32_t npid_cap,
                                               uint32_t *out_ip_be, uint16_t *out_port) {
    if (out_npid && npid_cap) out_npid[0] = '\0';
    if (!rpcn_strip_data_packet(&payload, &size)) return false;

    bool got_addr = false;
    pb_reader_t top = pb_reader(payload, size);
    while (pb_next(&top)) {
        if (top.field == 1 && top.wire == PB_WIRE_LEN) {
            pb_copy_string(&top, out_npid, npid_cap);
        } else if (top.field == 2 && top.wire == PB_WIRE_LEN) {
            pb_reader_t sub = pb_sub(&top);
            got_addr = rpcn_read_signaling_addr(sub, out_ip_be, out_port);
        }
    }
    return got_addr;
}

/* ---- Whole rooms and members ---------------------------------------------- */

/* BinAttr { uint16 id = 1 (wrapper); bytes data = 2; }. False unless it is `want`. */
static inline bool rpcn_read_bin_attr(pb_reader_t r, uint32_t want,
                                      uint8_t *out, uint32_t cap, uint32_t *out_len) {
    uint32_t id = 0;
    const uint8_t *data = NULL;
    uint32_t len = 0;
    while (pb_next(&r)) {
        if (r.field == 1 && r.wire == PB_WIRE_LEN) id = pb_as_wrapped(&r);
        else if (r.field == 2 && r.wire == PB_WIRE_LEN) { data = r.bytes; len = r.bytes_len; }
    }
    if (id != want) return false;
    if (len > cap) len = cap;
    if (len && data) memcpy(out, data, len);
    *out_len = len;
    return true;
}

/*
 * RoomMemberDataInternal { UserInfo userInfo = 1; uint64 joinDate = 2;
 * uint32 memberId = 3 (BARE); uint8 teamId = 4 (wrapper); RoomGroup = 5;
 * uint8 natType = 6; uint32 flagAttr = 7 (bare);
 * repeated RoomMemberBinAttrInternal { updateDate = 1; BinAttr data = 2; } = 8 }.
 */
static inline void rpcn_read_member_internal(pb_reader_t r, rpcn_member_info_t *m) {
    memset(m, 0, sizeof(*m));
    while (pb_next(&r)) {
        switch (r.field) {
            case 1: {
                pb_reader_t ui = pb_sub(&r);
                while (pb_next(&ui))
                    if (ui.field == 1 && ui.wire == PB_WIRE_LEN) pb_copy_string(&ui, m->npid, sizeof(m->npid));
                break;
            }
            case 3: if (r.wire == PB_WIRE_VARINT) m->member_id = (uint16_t)r.varint; break;
            case 4: m->team_id = (uint8_t)pb_as_wrapped(&r); break;
            case 7: if (r.wire == PB_WIRE_VARINT) m->flag_attr = (uint32_t)r.varint; break;
            case 8: {
                pb_reader_t attr = pb_sub(&r);
                while (pb_next(&attr)) {
                    if (attr.field != 2 || attr.wire != PB_WIRE_LEN) continue;
                    rpcn_read_bin_attr(pb_sub(&attr), RPCN_MEMBER_BIN_ATTR_ID,
                                       m->bin, sizeof(m->bin), &m->bin_len);
                }
                break;
            }
            default: break;
        }
    }
}

/*
 * RoomDataInternal { serverId = 1; worldId = 2; lobbyId = 3; uint64 roomId = 4;
 * passwordSlotMask = 5; uint32 maxSlot = 6 (bare); repeated
 * RoomMemberDataInternal memberList = 7; uint16 ownerId = 8 (wrapper);
 * roomGroup = 9; uint32 flagAttr = 10 (bare); repeated BinAttrInternal
 * { updateDate = 1; updateMemberId = 2; BinAttr data = 3 } roomBinAttrInternal = 11 }.
 */
static inline void rpcn_read_room_internal(pb_reader_t r, rpcn_room_info_t *room) {
    memset(room, 0, sizeof(*room));
    while (pb_next(&r)) {
        switch (r.field) {
            case 4:  if (r.wire == PB_WIRE_VARINT) room->room_id = r.varint; break;
            case 6:  if (r.wire == PB_WIRE_VARINT) room->max_slot = (uint32_t)r.varint; break;
            case 7:
                if (r.wire == PB_WIRE_LEN && room->member_count < RPCN_ROOM_MAX_MEMBERS)
                    rpcn_read_member_internal(pb_sub(&r), &room->members[room->member_count++]);
                break;
            case 8:  room->owner_id = (uint16_t)pb_as_wrapped(&r); break;
            case 10: if (r.wire == PB_WIRE_VARINT) room->flag_attr = (uint32_t)r.varint; break;
            case 11: {
                pb_reader_t attr = pb_sub(&r);
                while (pb_next(&attr)) {
                    if (attr.field != 3 || attr.wire != PB_WIRE_LEN) continue;
                    rpcn_read_bin_attr(pb_sub(&attr), RPCN_ROOM_BIN_ATTR_ID,
                                       room->bin, sizeof(room->bin), &room->bin_len);
                }
                break;
            }
            default: break;
        }
    }
}

/* CreateRoomResponse { internal = 1 } and JoinRoomResponse { room_data = 1 }:
 * [u32 len][protobuf], with the room at field 1 of both. */
static inline bool rpcn_parse_room_reply(const uint8_t *payload, uint32_t size, rpcn_room_info_t *out) {
    if (!rpcn_strip_data_packet(&payload, &size)) return false;
    pb_reader_t top = pb_reader(payload, size);
    while (pb_next(&top)) {
        if (top.field == 1 && top.wire == PB_WIRE_LEN) {
            rpcn_read_room_internal(pb_sub(&top), out);
            return out->room_id != 0;
        }
    }
    return false;
}

/* A GetRoomDataInternal reply: [u32 len][RoomDataInternal], NOT wrapped. */
static inline bool rpcn_parse_room_data_internal(const uint8_t *payload, uint32_t size,
                                                 rpcn_room_info_t *out) {
    if (!rpcn_strip_data_packet(&payload, &size)) return false;
    rpcn_read_room_internal(pb_reader(payload, size), out);
    return out->room_id != 0;
}

/*
 * The JoinRoomResponse's signaling_data: repeated Matching2SignalingInfo
 * { uint16 member_id = 1 (wrapper); SignalingAddr addr = 2; } -- one per member
 * already in the room, so a joiner can start punching every one of them at once.
 */
static inline uint32_t rpcn_parse_join_signaling_list(const uint8_t *payload, uint32_t size,
                                                      uint16_t *ids, uint32_t *ips_be,
                                                      uint16_t *ports, uint32_t max) {
    if (!rpcn_strip_data_packet(&payload, &size)) return 0;
    uint32_t n = 0;
    pb_reader_t top = pb_reader(payload, size);
    while (n < max && pb_next(&top)) {
        if (top.field != 2 || top.wire != PB_WIRE_LEN) continue;
        uint16_t id = 0; uint32_t ip = 0; uint16_t port = 0; bool got = false;
        pb_reader_t info = pb_sub(&top);
        while (pb_next(&info)) {
            if (info.field == 1 && info.wire == PB_WIRE_LEN) id = (uint16_t)pb_as_wrapped(&info);
            else if (info.field == 2 && info.wire == PB_WIRE_LEN)
                got = rpcn_read_signaling_addr(pb_sub(&info), &ip, &port);
        }
        if (id && got) { ids[n] = id; ips_be[n] = ip; ports[n] = port; n++; }
    }
    return n;
}

/*
 * The notifications that name a room first: UpdatedRoomDataInternal,
 * UpdatedRoomMemberDataInternal, UserLeftRoom and RoomDestroyed are all
 * [u64 room_id][u32 len][protobuf] (cmd_room.rs builds each the same way).
 * UserJoinedRoom is the odd one out -- its room id is inside the protobuf.
 */
static inline bool rpcn_strip_room_notification(const uint8_t **p, uint32_t *size, uint64_t *room_id) {
    if (*size < 8) return false;
    if (room_id) *room_id = rpcn_get_u64(*p);
    *p += 8;
    *size -= 8;
    return rpcn_strip_data_packet(p, size);
}

/* RoomDataInternalUpdateInfo { RoomDataInternal newRoomDataInternal = 1; ... } */
static inline bool rpcn_parse_room_update(const uint8_t *payload, uint32_t size, rpcn_room_info_t *out) {
    uint64_t room_id = 0;
    if (!rpcn_strip_room_notification(&payload, &size, &room_id)) return false;
    pb_reader_t top = pb_reader(payload, size);
    while (pb_next(&top)) {
        if (top.field == 1 && top.wire == PB_WIRE_LEN) {
            rpcn_read_room_internal(pb_sub(&top), out);
            if (!out->room_id) out->room_id = room_id;
            return true;
        }
    }
    return false;
}

/* RoomMemberDataInternalUpdateInfo { RoomMemberDataInternal = 1; ... } and
 * RoomMemberUpdateInfo { RoomMemberDataInternal = 1; eventCause = 2; ... }
 * (UserLeftRoom): the member is field 1 of both. */
static inline bool rpcn_parse_member_notification(const uint8_t *payload, uint32_t size,
                                                  uint64_t *room_id, rpcn_member_info_t *out) {
    if (!rpcn_strip_room_notification(&payload, &size, room_id)) return false;
    pb_reader_t top = pb_reader(payload, size);
    while (pb_next(&top)) {
        if (top.field == 1 && top.wire == PB_WIRE_LEN) {
            rpcn_read_member_internal(pb_sub(&top), out);
            return true;
        }
    }
    return false;
}

/* NotificationUserJoinedRoom's member: update_info = 2 -> roomMemberDataInternal = 1. */
static inline bool rpcn_parse_joined_member(const uint8_t *payload, uint32_t size,
                                            rpcn_member_info_t *out) {
    if (!rpcn_strip_data_packet(&payload, &size)) return false;
    pb_reader_t top = pb_reader(payload, size);
    while (pb_next(&top)) {
        if (top.field != 2 || top.wire != PB_WIRE_LEN) continue;
        pb_reader_t update = pb_sub(&top);
        while (pb_next(&update)) {
            if (update.field == 1 && update.wire == PB_WIRE_LEN) {
                rpcn_read_member_internal(pb_sub(&update), out);
                return out->member_id != 0;
            }
        }
    }
    return false;
}

/* ---- Twitch device flow --------------------------------------------------- */
/*
 * The device flow needs a browser and a human, which takes far longer than a
 * game's login should — so it is run ONCE and hands back a long-lived login
 * token that the ordinary Login command accepts IN PLACE OF THE PASSWORD. Every
 * later session is a plain login with no browser involved.
 *
 * Both commands are unauthenticated. An older RPCN does not know command 63 at
 * all: it answers Malformed and CLOSES the connection, so a disconnect right
 * after the start means "this server has no Twitch support", the same as
 * TwitchDisabled.
 */

/* Empty request body. */
static inline uint64_t rpcn_twitch_start(rpcn_client_t *c) {
    return rpcn_request(c, RPCN_CMD_TWITCH_DEVICE_START, NULL, 0);
}

static inline uint64_t rpcn_twitch_poll_flow(rpcn_client_t *c, const char *flow_id) {
    if (!flow_id || !*flow_id) { rpcn_fail(c, "TwitchDevicePoll needs a flow id"); return 0; }
    rpcn_strpack_t p;
    rpcn_strpack_init(&p);
    rpcn_strpack_put(&p, flow_id);
    if (!p.ok) { rpcn_fail(c, "flow id too long"); return 0; }
    return rpcn_request(c, RPCN_CMD_TWITCH_DEVICE_POLL, p.buf, p.n);
}

/* Walks NUL-terminated strings back to back, the shape every non-room payload
 * uses. Returns the count actually copied. */
static inline uint32_t rpcn_split_strings(const uint8_t *p, uint32_t size,
                                          char **out, const uint32_t *caps, uint32_t count) {
    uint32_t at = 0, got = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (at >= size) break;
        uint32_t start = at;
        while (at < size && p[at] != 0) at++;
        if (at >= size) break;                 /* unterminated: refuse the tail */
        uint32_t len = at - start;
        if (len > caps[i] - 1) len = caps[i] - 1;
        memcpy(out[i], p + start, len);
        out[i][len] = '\0';
        at++;                                  /* past the NUL */
        got++;
    }
    return got;
}

/* Reply: flow_id, user_code, verification_uri (strings), then u32 expires_in and
 * u32 interval. */
static inline bool rpcn_parse_twitch_start(const uint8_t *payload, uint32_t size,
                                           char *flow_id, uint32_t flow_cap,
                                           char *user_code, uint32_t code_cap,
                                           char *uri, uint32_t uri_cap,
                                           uint32_t *out_expires, uint32_t *out_interval) {
    char *outs[3] = { flow_id, user_code, uri };
    uint32_t caps[3] = { flow_cap, code_cap, uri_cap };
    if (rpcn_split_strings(payload, size, outs, caps, 3) != 3) return false;

    /* The two u32s sit after the three terminators; find where they start rather
     * than assuming the strings were short enough not to have been truncated. */
    uint32_t at = 0;
    for (int i = 0; i < 3; i++) {
        while (at < size && payload[at] != 0) at++;
        at++;
    }
    if (at + 8 > size) return false;
    if (out_expires)  *out_expires  = rpcn_get_u32(payload + at);
    if (out_interval) *out_interval = rpcn_get_u32(payload + at + 4);
    return true;
}

/* Reply: npid, online_name, avatar_url, login_token — all strings. */
static inline bool rpcn_parse_twitch_poll(const uint8_t *payload, uint32_t size,
                                          char *npid, uint32_t npid_cap,
                                          char *online_name, uint32_t name_cap,
                                          char *avatar, uint32_t avatar_cap,
                                          char *login_token, uint32_t token_cap) {
    char *outs[4] = { npid, online_name, avatar, login_token };
    uint32_t caps[4] = { npid_cap, name_cap, avatar_cap, token_cap };
    return rpcn_split_strings(payload, size, outs, caps, 4) == 4;
}

/* ---- Poll ---------------------------------------------------------------- */

/* One decoded packet at a time; false when nothing more is pending. Never blocks. */
static inline bool rpcn_poll(rpcn_client_t *c, rpcn_packet_t *out) {
    if (!out || !tls_is_connected(&c->tls)) return false;

    /* The packet the last poll returned, now that its caller is done with it. */
    if (c->in_consumed) {
        memmove(c->in, c->in + c->in_consumed, c->in_used - c->in_consumed);
        c->in_used -= c->in_consumed;
        c->in_consumed = 0;
    }

    for (;;) {
        if (c->in_used >= RPCN_HEADER_SIZE) {
            uint32_t size = rpcn_get_u32(c->in + 3);
            if (size < RPCN_HEADER_SIZE || size > sizeof(c->in)) {
                rpcn_fail(c, "malformed packet size %u", size);
                rpcn_disconnect(c);
                return false;
            }
            if (c->in_used >= size) {
                out->type      = c->in[0];
                out->command   = rpcn_get_u16(c->in + 1);
                out->packet_id = rpcn_get_u64(c->in + 7);

                /* Replies carry an error byte before their payload; notifications
                 * do not. */
                if (out->type == 1 && size > RPCN_HEADER_SIZE) {
                    out->error        = (rpcn_error_t)c->in[RPCN_HEADER_SIZE];
                    out->payload      = c->in + RPCN_HEADER_SIZE + 1;
                    out->payload_size = size - RPCN_HEADER_SIZE - 1;
                } else {
                    out->error        = RPCN_OK;
                    out->payload      = c->in + RPCN_HEADER_SIZE;
                    out->payload_size = size - RPCN_HEADER_SIZE;
                }

                /* Snoop our own Login reply for the user_id. Doing it here means
                 * callers never have to remember to parse it, and signaling just
                 * works after login. Layout: online_name\0 avatar_url\0 then i64. */
                if (out->type == 1 && out->command == (uint16_t)RPCN_CMD_LOGIN
                    && out->error == RPCN_OK) {
                    uint32_t at = 0;
                    int skipped = 0;
                    while (at < out->payload_size && skipped < 2) {
                        if (out->payload[at] == 0) skipped++;
                        at++;
                    }
                    if (skipped == 2 && at + 8 <= out->payload_size)
                        c->user_id = (int64_t)rpcn_get_u64(out->payload + at);
                }

                /* The caller's payload points into `in`, so the packet stays
                 * where it is until the NEXT poll slides it out. Sliding it here
                 * moved the following packet over the payload before the caller
                 * had read a byte of it -- invisible while replies arrived one
                 * at a time, and every time once a room joined: the join reply
                 * comes with room notifications right behind it in one read. */
                c->in_consumed = size;
                return true;
            }
        }

        if (c->in_used == sizeof(c->in)) {
            rpcn_fail(c, "inbound buffer full");
            rpcn_disconnect(c);
            return false;
        }

        int got = tls_recv(&c->tls, c->in + c->in_used, (uint32_t)sizeof(c->in) - c->in_used);
        if (got < 0) {
            rpcn_fail(c, "%s", tls_last_error(&c->tls));
            rpcn_disconnect(c);
            return false;
        }
        if (got == 0) return false;   /* nothing more right now */
        c->in_used += (uint32_t)got;
    }
}

/* ---- UDP signaling + P2P ------------------------------------------------- */

/*
 * The local P2P socket, used both for signaling keepalives and for game traffic.
 * Override `local_port` only for testing two peers inside ONE process: a second
 * bind of 3658 fails with EADDRINUSE, and RPCN hardcodes 3658 when it hands out a
 * peer's LOCAL address, so a non-default port is not usable for same-NAT play.
 */
static inline bool rpcn_open_signaling(rpcn_client_t *c, uint16_t local_port) {
    if (net_sock_valid(c->udp)) return true;
    c->local_port = local_port ? local_port : RPCN_P2P_PORT;
    if (!net_udp_open(&c->udp, c->local_port)) {
        rpcn_fail(c, "could not bind UDP %u (%d)", (unsigned)c->local_port, net_errno());
        return false;
    }
    return true;
}

/* The 13-byte keepalive that records/refreshes our public address. Call every
 * couple of seconds while online; pass 0 to use the logged-in user id. */
static inline bool rpcn_send_signaling_ping(rpcn_client_t *c, int64_t user_id) {
    if (!net_sock_valid(c->udp)) return false;
    if (user_id == 0) user_id = c->user_id;
    if (user_id == 0 || c->signaling_addr == 0) return false;

    /* Exactly 13 bytes or the server rejects it:
     * [0]=1, [1..9]=user_id LE, [9..13]=our LAN IPv4 (cached at connect — it has
     * to be the real interface address, not the 0.0.0.0 a bound socket reports). */
    uint8_t pkt[13];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 1;
    for (int i = 0; i < 8; i++) pkt[1 + i] = (uint8_t)((uint64_t)user_id >> (i * 8));

    memcpy(pkt + 9, &c->local_ip, 4);

    return net_udp_send(c->udp, c->signaling_addr, RPCN_SIGNALING_PORT, pkt, sizeof(pkt));
}

/*
 * Game traffic rides the SAME socket as the signaling keepalives, on purpose:
 * that socket is the one the server observed and advertised to peers, so its NAT
 * mapping is the one they can reach. A second socket would get a different
 * mapping and silently fail.
 */
static inline bool rpcn_send_to(rpcn_client_t *c, uint32_t ip_be, uint16_t port,
                                const void *data, uint32_t len) {
    return net_udp_send(c->udp, ip_be, port, data, len);
}

static inline int rpcn_recv_from(rpcn_client_t *c, void *buf, uint32_t cap,
                                 uint32_t *out_ip_be, uint16_t *out_port) {
    return net_udp_recv(c->udp, buf, cap, out_ip_be, out_port);
}

/* True if this datagram came from the RPCN signaling helper rather than a peer. */
static inline bool rpcn_is_signaling_source(const rpcn_client_t *c, uint32_t ip_be, uint16_t port) {
    return ip_be == c->signaling_addr && port == RPCN_SIGNALING_PORT;
}

#endif /* RPCN_CLIENT_H */
