/*
 * net_test.c — the netplay pieces that can be proven without a server.
 *
 * The RPCN half of src/net needs a live server and is exercised against one by
 * hand (README, "Netplay"). Everything below it does not: the lockstep engine,
 * the protobuf writer/reader, the ComId standard and the canonical input word
 * are pure functions of their inputs, and they are also where a quiet mistake
 * costs a desync rather than an error message. So they get a test.
 *
 * Six parts:
 *  (A) The input rings: newest-wins ingest, redundancy repair, ring aliasing.
 *  (B) The barrier and the generation fence.
 *  (C) protobuf round trips, including the uint8/uint16 WRAPPER trap that
 *      np2_structs.proto sets for anyone who reads those fields as bare varints.
 *  (D) ComId derivation: stable, case- and punctuation-insensitive, well formed
 *      by RPCN's own rule, and never colliding between two different games.
 *  (E) Rooms of more than two (room.h): the PS3 port's rules for who fights and
 *      how the line moves after a result.
 *  (F) Watchers: never gating, and still getting every frame.
 *  (G) A member heard from an address the server did not give (issue #108):
 *      a punch may re-point them, a game packet may not. A member heard from
 *      is followed to a new address only after the old one goes quiet (a NAT
 *      that made a new mapping). And a taken peer-to-peer port is stepped
 *      past. This part uses UDP sockets on loopback, and no server.
 */
#define NDEBUG 1
#include <stdio.h>
#include <string.h>

#include "com_id.h"
#include "lockstep.h"
#include "protobuf.h"
#include "room.h"
#include "rpcn_session.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

/* Two engines wired to each other's records, so a test reads like a session. */
typedef struct { lockstep_t a, b; } pair_t;

static void pair_begin(pair_t *p, uint32_t generation, uint32_t delay) {
    lockstep_configure(&p->a, 0, 2, delay);
    lockstep_configure(&p->b, 1, 2, delay);
    lockstep_begin_round(&p->a, generation);
    lockstep_begin_round(&p->b, generation);
    lockstep_on_peer_announce(&p->a, 1, generation);
    lockstep_on_peer_announce(&p->b, 0, generation);
}

/* ---- (G) helpers ------------------------------------------------------- */

/* A port nothing on this machine holds, from `from` up; 0 if none. */
static uint16_t free_udp_port(uint16_t from) {
    for (uint16_t p = from; p < from + 200; p++) {
        net_sock_t k = NET_SOCK_INVALID;
        if (net_udp_open(&k, p)) { net_close(&k); return p; }
    }
    return 0;
}

static char g_notes[1024];
static void note_cb(void *ctx, const char *msg) {
    (void)ctx;
    size_t n = strlen(g_notes);
    snprintf(g_notes + n, sizeof(g_notes) - n, "%s\n", msg);
}

/* Drain the session's socket for up to ~200 ms (loopback is quick, but not
 * synchronous everywhere). Returns the last game datagram's length, or 0. */
static int pump_recv(rpcn_session_t *s, uint16_t *from, uint32_t *ip, uint16_t *port) {
    uint8_t buf[64];
    int last = 0;
    uint64_t end = net_now_ms() + 200;
    while (net_now_ms() < end) {
        int got = rpcn_session_recv(s, buf, sizeof(buf), from, ip, port);
        if (got > 0) { last = got; break; }
    }
    return last;
}

static void punch_from(net_sock_t k, uint16_t to_port, uint16_t member) {
    uint8_t pkt[RPCN_PUNCH_SIZE];
    memcpy(pkt, g_rpcn_punch_tag, 4);
    rpcn_put_u16(pkt + 4, member);
    net_udp_send(k, htonl(0x7F000001u), to_port, pkt, sizeof(pkt));
}

int main(void) {
    /* ---- (A) rings ------------------------------------------------------- */
    {
        lockstep_ring_t r;
        lockstep_ring_clear(&r);

        CHECK(!lockstep_ring_has(&r, 0), "a cleared ring holds nothing");
        CHECK(lockstep_ring_insert(&r, 10, 0x111), "inserting a new frame reports new information");
        CHECK(lockstep_ring_has(&r, 10) && lockstep_ring_get(&r, 10) == 0x111,
              "the inserted frame reads back");

        /* Newest-wins is what makes the redundancy free: a duplicate must be a
         * no-op, not an overwrite, or reordered datagrams rewrite history. */
        CHECK(!lockstep_ring_insert(&r, 10, 0x222), "a duplicate frame is refused");
        CHECK(lockstep_ring_get(&r, 10) == 0x111, "…and leaves the stored input alone");

        /* A slot that has been lapped by a newer frame reports the OLD frame as
         * absent rather than returning the new frame's input under its number. */
        lockstep_ring_insert(&r, 10 + LOCKSTEP_RING_SIZE, 0x333);
        CHECK(!lockstep_ring_has(&r, 10), "a lapped frame is gone, not silently aliased");
        CHECK(lockstep_ring_get(&r, 10 + LOCKSTEP_RING_SIZE) == 0x333, "…and the newer one is there");
    }

    /* ---- (A2) redundancy repairs a dropped datagram ---------------------- */
    {
        pair_t p;
        pair_begin(&p, 3, 2);

        /* B submits 20 frames; only the LAST record reaches A. Every frame back
         * to 20-(redundancy-1) must still arrive, because each packet re-carries
         * them — this is the whole reason the transport is allowed to be lossy. */
        lockstep_record_t rec;
        for (uint32_t f = 0; f < 20; f++)
            lockstep_submit_local(&p.b, f, 0x1000u + f, &rec);
        lockstep_on_record(&p.a, &rec);

        uint32_t newest = 19;
        int repaired = 0;
        for (uint32_t i = 0; i < LOCKSTEP_REDUNDANCY; i++) {
            uint32_t f = newest - i;
            if (lockstep_ring_has(&p.a.rings[1], f)
                && lockstep_input_for(&p.a, 1, f) == 0x1000u + f) repaired++;
        }
        CHECK(repaired == (int)LOCKSTEP_REDUNDANCY,
              "one surviving packet repairs all 10 frames of redundancy");
        CHECK(!lockstep_ring_has(&p.a.rings[1], newest - LOCKSTEP_REDUNDANCY),
              "…and nothing older than the redundancy window");

        /* A peer's record must never be able to rewrite our OWN slot. */
        lockstep_record_t forged;
        memset(&forged, 0, sizeof(forged));
        forged.frame  = 5;
        forged.packed = lockstep_pack(0, 3);      /* claims to be player 0 = us */
        forged.inputs[0] = 0xDEAD;
        lockstep_submit_local(&p.a, 5, 0xBEEF, NULL);
        lockstep_on_record(&p.a, &forged);
        CHECK(lockstep_input_for(&p.a, 0, 5) == 0xBEEF,
              "a record for our own player index is ignored");
    }

    /* ---- (B) barrier and generation fence -------------------------------- */
    {
        lockstep_t l;
        lockstep_configure(&l, 0, 2, 2);
        lockstep_begin_round(&l, 1);
        CHECK(!lockstep_barrier_released(&l), "the barrier holds until the peer announces");
        lockstep_on_peer_announce(&l, 1, 2);
        CHECK(!lockstep_barrier_released(&l), "an announce for another round does not release it");
        lockstep_on_peer_announce(&l, 1, 1);
        CHECK(lockstep_barrier_released(&l), "the matching announce releases it");

        /* The peer released first and stopped announcing; its first input for
         * this round is all that reaches us, and it must release us too. */
        lockstep_t g;
        lockstep_configure(&g, 1, 2, 2);
        lockstep_begin_round(&g, 3);
        lockstep_record_t first;
        memset(&first, 0, sizeof(first));
        first.frame  = 0;
        first.packed = lockstep_pack(0, 3);
        lockstep_on_record(&g, &first);
        CHECK(lockstep_barrier_released(&g), "a record for this round releases the barrier like an announce");
        lockstep_t h;
        lockstep_configure(&h, 1, 2, 2);
        lockstep_begin_round(&h, 3);
        first.packed = lockstep_pack(0, 2);
        lockstep_on_record(&h, &first);
        CHECK(!lockstep_barrier_released(&h), "a record for another round does not");

        /* A late record from the round that just ended must not reach the new
         * round's rings — that is exactly what the generation is for. */
        lockstep_record_t stale;
        memset(&stale, 0, sizeof(stale));
        stale.frame  = 4;
        stale.packed = lockstep_pack(1, 0);       /* previous generation */
        stale.inputs[0] = 0x55;
        lockstep_on_record(&l, &stale);
        CHECK(!lockstep_ring_has(&l.rings[1], 4), "a record from the previous round is dropped");

        /* Beginning a round wipes the rings, so nothing survives into it. */
        lockstep_submit_local(&l, 7, 0x99, NULL);
        lockstep_begin_round(&l, 2);
        CHECK(!lockstep_ring_has(&l.rings[0], 7), "beginning a round clears the rings");
        CHECK(l.last_local_frame == LOCKSTEP_INVALID_FRAME, "…and the local frame cursor");
    }

    /* ---- (B2) ready-ness gates on EVERY player --------------------------- */
    {
        pair_t p;
        pair_begin(&p, 1, 2);
        lockstep_submit_local(&p.a, 0, 0x1, NULL);
        CHECK(!lockstep_ready(&p.a, 0), "our own input alone does not make a frame ready");

        lockstep_record_t rec;
        lockstep_submit_local(&p.b, 0, 0x2, &rec);
        lockstep_on_record(&p.a, &rec);
        CHECK(lockstep_ready(&p.a, 0), "both players' inputs make it ready");
        CHECK(lockstep_input_for(&p.a, 0, 0) == 0x1 && lockstep_input_for(&p.a, 1, 0) == 0x2,
              "…and each player's word is kept under its own index");
    }

    /* ---- (B3) two stalled peers repair a burst of loss ------------------- */
    /*
     * The session as netplay.h runs it: at frame f a machine samples f + delay
     * and sends it, then may run f only if it holds the other player's f. One
     * direction goes dark. The machine that can still hear runs out of the
     * other's inputs and stops sending too, because a stalled machine samples
     * nothing -- and then nobody is transmitting, so the loss is permanent
     * however briefly the link was down. What gets them out is each machine
     * re-sending [lockstep_resend_floor, last_local_frame] while it waits.
     * Run for every delay the room word can carry: above 4 the frame that was
     * lost is further behind the newest than one record reaches.
     */
    for (uint32_t delay = 0; delay <= 10; delay++) {
        pair_t p;
        pair_begin(&p, 1, delay);
        uint32_t fa = 0, fb = 0;
        lockstep_record_t rec;
        for (uint32_t f = 0; f < delay; f++) {           /* netplay_seed_delay_frames */
            lockstep_submit_local(&p.a, f, 0, &rec); lockstep_on_record(&p.b, &rec);
            lockstep_submit_local(&p.b, f, 0, &rec); lockstep_on_record(&p.a, &rec);
        }

        bool a_to_b_up = true;
        for (int tick = 0; tick < 200; tick++) {
            if (tick == 30) a_to_b_up = false;           /* the burst starts ... */
            if (p.a.last_local_frame == LOCKSTEP_INVALID_FRAME || fa + delay > p.a.last_local_frame) {
                lockstep_submit_local(&p.a, fa + delay, 0xA000u | (fa + delay), &rec);
                if (a_to_b_up) lockstep_on_record(&p.b, &rec);
            }
            if (p.b.last_local_frame == LOCKSTEP_INVALID_FRAME || fb + delay > p.b.last_local_frame) {
                lockstep_submit_local(&p.b, fb + delay, 0xB000u | (fb + delay), &rec);
                lockstep_on_record(&p.a, &rec);
            }
            if (lockstep_ready(&p.a, fa)) fa++;
            if (lockstep_ready(&p.b, fb)) fb++;
        }
        /* ... and is over. Both are stalled, so neither has anything new to say. */
        bool deadlocked = !lockstep_ready(&p.a, fa) && !lockstep_ready(&p.b, fb);

        /* One resend from each, exactly as netplay_resend_inputs walks it. */
        for (int side = 0; side < 2; side++) {
            lockstep_t *from = side ? &p.b : &p.a, *to = side ? &p.a : &p.b;
            uint32_t floor = lockstep_resend_floor(from), top = from->last_local_frame;
            for (int sent = 0; sent < 4; sent++) {
                lockstep_fill_record(from, top, &rec);
                lockstep_on_record(to, &rec);
                if (top < floor + LOCKSTEP_REDUNDANCY) break;
                top -= LOCKSTEP_REDUNDANCY;
            }
        }
        bool repaired = lockstep_ready(&p.a, fa) || lockstep_ready(&p.b, fb);
        bool intact = true;
        for (uint32_t f = delay; f < fb + delay; f++)
            if (lockstep_input_for(&p.b, 0, f) != (0xA000u | f)) intact = false;

        char msg[96];
        snprintf(msg, sizeof(msg), "delay %2u: a burst of loss deadlocks both peers, and a resend frees them",
                 (unsigned)delay);
        CHECK(deadlocked && repaired && intact, msg);
    }

    /* ---- (C) protobuf ---------------------------------------------------- */
    {
        uint8_t buf[256];
        pb_writer_t w;
        pb_writer_init(&w, buf, sizeof(buf));
        pb_varint(&w, 1, 7);            /* a bare varint, as flagAttr/roomId are */
        pb_wrapped(&w, 2, 300);         /* a uint16 WRAPPER, as maxSlot is       */
        pb_string(&w, 3, "M2HSNCFTR_00");
        {
            uint32_t tok = pb_begin_sub(&w, 4);
            pb_wrapped(&w, 1, 1);
            pb_end_sub(&w, tok);
        }
        CHECK(w.ok, "the writer stayed within its buffer");

        int seen = 0;
        uint64_t bare = 0;
        uint32_t wrapped = 0, nested = 0;
        char str[32] = {0};
        pb_reader_t r = pb_reader(buf, w.used);
        while (pb_next(&r)) {
            seen++;
            if (r.field == 1) bare = r.varint;
            if (r.field == 2) wrapped = pb_as_wrapped(&r);
            if (r.field == 3) pb_copy_string(&r, str, sizeof(str));
            if (r.field == 4) {
                pb_reader_t sub = pb_sub(&r);
                while (pb_next(&sub)) if (sub.field == 1) nested = pb_as_wrapped(&sub);
            }
        }
        CHECK(seen == 4 && r.ok, "every field read back");
        CHECK(bare == 7, "a bare varint round trips");
        CHECK(wrapped == 300, "a uint16 wrapper round trips through its inner value");
        CHECK(strcmp(str, "M2HSNCFTR_00") == 0, "a string round trips");
        CHECK(nested == 1, "a wrapper nested inside a submessage round trips");

        /* The trap: a wrapper read as a bare varint is not an error, it is a
         * length-delimited field whose .varint is simply never set. */
        pb_reader_t r2 = pb_reader(buf, w.used);
        while (pb_next(&r2)) {
            if (r2.field == 2) {
                CHECK(r2.wire == PB_WIRE_LEN && r2.varint == 0,
                      "a wrapper field is length-delimited, not a varint (the proto trap)");
            }
        }

        /* A submessage over 127 bytes forces the back-patch to widen the length
         * prefix and shift the payload; get that wrong and everything after it
         * is garbage rather than an error. */
        pb_writer_t w2;
        uint8_t big[512];
        char blob[200];
        memset(blob, 'x', sizeof(blob));
        pb_writer_init(&w2, big, sizeof(big));
        uint32_t tok = pb_begin_sub(&w2, 1);
        pb_bytes(&w2, 1, blob, sizeof(blob));
        pb_end_sub(&w2, tok);
        pb_varint(&w2, 2, 0x1234);
        CHECK(w2.ok, "a long submessage encodes");
        uint32_t tail = 0, inner = 0;
        pb_reader_t r3 = pb_reader(big, w2.used);
        while (pb_next(&r3)) {
            if (r3.field == 1) {
                pb_reader_t sub = pb_sub(&r3);
                while (pb_next(&sub)) if (sub.field == 1) inner = sub.bytes_len;
            }
            if (r3.field == 2) tail = (uint32_t)r3.varint;
        }
        CHECK(inner == sizeof(blob), "…its payload survives the length back-patch");
        CHECK(tail == 0x1234, "…and the field after it is still where it should be");
    }

    /* ---- (D) ComId ------------------------------------------------------- */
    {
        char a[COMID_BUFFER_SIZE], b[COMID_BUFFER_SIZE], c[COMID_BUFFER_SIZE];

        CHECK(comid_for_game("sfight", a) && strcmp(a, "M2HSNCFTR_00") == 0,
              "the reference game gets its listed id");
        CHECK(comid_for_game("Sonic The Fighters", b) && strcmp(a, b) == 0,
              "a spelling with spaces and capitals lands in the same lobby");
        CHECK(comid_for_game("sonic_the_fighters", c) && strcmp(a, c) == 0,
              "…and so does one with punctuation");

        CHECK(comid_for_game("fvipers", b) && strcmp(a, b) != 0,
              "a different game gets a different lobby");
        CHECK(comid_for_game("sfight_console", b) && strcmp(b, "M2HSNCFTC_00") == 0,
              "STF's console profile has a lobby of its own, apart from the arcade game's");
        CHECK(comid_yamp_for_game("sfight_console", b) && strcmp(b, "YMPSNCFTR_00") == 0,
              "…and browses YAMP's STF rooms, since YAMP runs the console emulator");

        /* Every id we can produce must satisfy RPCN's own rule, or the server
         * answers Malformed three requests into discovery. */
        CHECK(comid_is_well_formed(a) && comid_looks_like_id(a) && comid_is_ours(a),
              "a listed id is well formed by RPCN's rule and in our namespace");
        CHECK(comid_for_game("a game nobody listed", b) && comid_is_well_formed(b)
              && comid_looks_like_id(b) && comid_is_ours(b),
              "a hashed id is too");
        CHECK(comid_for_game("a game nobody listed", c) && strcmp(b, c) == 0,
              "…and hashing is stable, so two peers derive the same one");

        bool listed = true;
        comid_for_game_ex("a game nobody listed", c, &listed);
        CHECK(!listed, "an unlisted game reports that its id was derived, not looked up");

        /* The cross-emulator browse only claims a YAMP space for games YAMP
         * actually hosts — a hash of our key would name a lobby nobody is in. */
        CHECK(comid_yamp_for_game("sfight", b) && strcmp(b, "YMPSNCFTR_00") == 0,
              "a shared arcade game maps to YAMP's own id");
        CHECK(!comid_yamp_for_game("m2snake", b),
              "a game YAMP does not host has no YAMP lobby, and none is invented");

        /* Resolve's three cases. */
        const char *note = NULL;
        CHECK(comid_resolve("M2HSNCFTR_00", b, &note) && strcmp(b, "M2HSNCFTR_00") == 0,
              "an id already in our namespace is used verbatim");
        CHECK(comid_resolve("YMPSNCFTR_00", b, &note) && comid_is_yamp(b),
              "a YAMP id is passed through (and noted)");
        CHECK(comid_resolve("sfight", b, &note) && strcmp(b, "M2HSNCFTR_00") == 0,
              "a game key is derived");
        CHECK(!comid_resolve("", b, &note) && !comid_resolve("!!!", b, &note),
              "nothing usable is refused rather than guessed at");

        /* "VIRTUALON" is nine uppercase letters, so it passes RPCN's rule — and
         * must still be read as a game KEY, or that game silently gets a lobby
         * space nobody else computes. */
        CHECK(!comid_looks_like_id("VIRTUALON"),
              "a nine-letter game name is not mistaken for a communication id");
    }

    /* ---- (E) rooms of more than two (room.h) ----------------------------- */
    {
        /* Both attributes survive a round trip, and a stranger's bytes are not
         * mistaken for ours. */
        room_state_t s, back;
        memset(&s, 0, sizeof(s));
        s.phase = ROOM_PHASE_MATCH; s.flags = ROOM_FLAG_AUTO; s.frame_delay = 3; s.last_result = 1;
        s.match = 513; s.fighter[0] = 0x21; s.fighter[1] = 0x32; s.seed = 0xCAFEF00Du;
        s.line_count = 3; s.line[0] = 0x32; s.line[1] = 0x41; s.line[2] = 0x21; s.region = 2;
        s.vs_mode = 1; s.session = 511; s.damage_real = 1;
        uint8_t bin[ROOM_STATE_SIZE];
        uint32_t len = room_state_encode(&s, bin);
        CHECK(room_state_decode(bin, len, &back) && back.match == 513 && back.fighter[1] == 0x32
              && back.seed == 0xCAFEF00Du && back.line_count == 3 && back.line[1] == 0x41
              && back.frame_delay == 3 && back.last_result == 1 && back.flags == ROOM_FLAG_AUTO
              && back.region == 2 && back.vs_mode == 1 && back.session == 511
              && back.damage_real == 1,
              "the room state round-trips");
        bin[0] ^= 1;
        CHECK(!room_state_decode(bin, len, &back), "bytes without our magic are not a room state");

        room_member_data_t md, mback;
        memset(&md, 0, sizeof(md));
        md.flags = ROOM_MEMBER_READY; md.entry = ROOM_ENTRY_2P; md.playing = 7;
        md.result_match = 6; md.result = 0; md.games = 9; md.wins = 4; md.points = 21;
        uint8_t mbin[ROOM_MEMBER_SIZE];
        CHECK(room_member_decode(mbin, room_member_encode(&md, mbin), &mback)
              && mback.entry == ROOM_ENTRY_2P && mback.playing == 7 && mback.result_match == 6
              && mback.result == 0 && mback.points == 21 && mback.wins == 4,
              "a member's attribute round-trips");
    }
    {
        /* VS mode: the same two play on, on the running boards, only when nobody
         * else is waiting to play. */
        room_state_t s;
        memset(&s, 0, sizeof(s));
        s.phase = ROOM_PHASE_MATCH; s.match = 9; s.session = 7; s.vs_mode = 1;
        s.fighter[0] = 1; s.fighter[1] = 2;
        s.line_count = 2; s.line[0] = 1; s.line[1] = 2;
        room_member_t m[3];
        memset(m, 0, sizeof(m));
        for (int i = 0; i < 3; i++) { m[i].id = (uint16_t)(i + 1); m[i].known = true; }
        CHECK(room_vs_continues(&s, m, 2), "VS mode, two players: the rematch is on the same boards");

        s.vs_mode = 0;
        CHECK(!room_vs_continues(&s, m, 2), "without VS mode every match gets its own reset");
        s.vs_mode = 1;

        s.line_count = 3; s.line[2] = 3;
        CHECK(!room_vs_continues(&s, m, 3), "somebody waiting in line: the line moves, with a reset");

        m[2].data.flags = ROOM_MEMBER_WATCH;
        CHECK(room_vs_continues(&s, m, 3), "somebody only watching does not stop the rematch");

        m[1].data.flags = ROOM_MEMBER_WATCH;
        CHECK(!room_vs_continues(&s, m, 3), "a fighter who sits out ends it");
        m[1].data.flags = 0;

        CHECK(!room_vs_continues(&s, m, 1), "a fighter who left ends it");

        s.phase = ROOM_PHASE_LOBBY;
        CHECK(!room_vs_continues(&s, m, 3), "a match that is not on has nothing to continue");
    }
    {
        /* The line follows the room: leavers out, newcomers on the end in id order. */
        room_state_t s;
        memset(&s, 0, sizeof(s));
        room_member_t m[4];
        memset(m, 0, sizeof(m));
        m[0].id = 0x10; m[1].id = 0x31; m[2].id = 0x22;
        for (int i = 0; i < 3; i++) m[i].known = true;
        CHECK(room_line_sync(&s, m, 3) && s.line_count == 3 && s.line[0] == 0x10
              && s.line[1] == 0x22 && s.line[2] == 0x31,
              "newcomers join the back of the line, oldest member id first");
        m[0] = m[2];   /* 0x10 leaves; 0x22 and 0x31 stay */
        CHECK(room_line_sync(&s, m, 2) && s.line_count == 2 && s.line[0] == 0x22 && s.line[1] == 0x31,
              "a member who leaves is taken out of the line and the rest close up");
        CHECK(!room_line_sync(&s, m, 2), "an unchanged room leaves the line alone");
    }
    {
        /* build_fight_entries: who fights, and on which side. */
        room_state_t s;
        memset(&s, 0, sizeof(s));
        s.line_count = 4; s.line[0] = 1; s.line[1] = 2; s.line[2] = 3; s.line[3] = 4;
        room_member_t m[4];
        memset(m, 0, sizeof(m));
        for (int i = 0; i < 4; i++) { m[i].id = (uint16_t)(i + 1); m[i].known = true; }
        uint16_t f[2];

        CHECK(room_pick_fighters(&s, m, 4, f) && f[0] == 1 && f[1] == 2,
              "nobody asking for a side: the front two, in line order");

        m[3].data.entry = ROOM_ENTRY_2P;
        CHECK(room_pick_fighters(&s, m, 4, f) && f[0] == 1 && f[1] == 4,
              "a 2P Entry jumps the line for 2P, and the front of the line fills 1P");

        m[3].data.entry = ROOM_ENTRY_1P;
        CHECK(room_pick_fighters(&s, m, 4, f) && f[0] == 4 && f[1] == 1,
              "a 1P Entry jumps the line for 1P, and the front of the line fills 2P");

        m[2].data.entry = ROOM_ENTRY_1P;
        CHECK(room_pick_fighters(&s, m, 4, f) && f[0] == 3 && f[1] == 4,
              "two asking for 1P: the one further forward gets it, the other beats non-entrants to 2P");

        m[2].data.entry = ROOM_ENTRY_NONE; m[3].data.entry = ROOM_ENTRY_NONE;
        m[0].data.flags = ROOM_MEMBER_WATCH;
        CHECK(room_pick_fighters(&s, m, 4, f) && f[0] == 2 && f[1] == 3,
              "a member sitting out is never picked");
        m[1].data.flags = m[2].data.flags = ROOM_MEMBER_WATCH;
        CHECK(!room_pick_fighters(&s, m, 4, f), "one player is not a match");
    }
    {
        /* rotate_queue_after_match, and what each member does with the result:
         * winner to the front and staying on their side, loser to the back. */
        room_state_t s;
        memset(&s, 0, sizeof(s));
        s.phase = ROOM_PHASE_MATCH; s.match = 5; s.fighter[0] = 1; s.fighter[1] = 2;
        s.line_count = 4; s.line[0] = 1; s.line[1] = 2; s.line[2] = 3; s.line[3] = 4;
        CHECK(room_rotate_line(&s, 1) == 1 && s.line[0] == 2 && s.line[1] == 3
              && s.line[2] == 4 && s.line[3] == 1,
              "2P wins: the winner goes to the front, the waiting keep their order, the loser goes last");

        room_member_data_t win, lose, watch;
        memset(&win, 0, sizeof(win)); memset(&lose, 0, sizeof(lose)); memset(&watch, 0, sizeof(watch));
        lose.entry = ROOM_ENTRY_1P;
        CHECK(room_after_result(&win, 2, &s, 5, 1) && win.games == 1 && win.wins == 1
              && win.points == 4 && win.entry == ROOM_ENTRY_2P,
              "the winner gets a game, a win, 4 points, and asks to stay on the side they won on");
        CHECK(room_after_result(&lose, 1, &s, 5, 1) && lose.games == 1 && lose.wins == 0
              && lose.points == 1 && lose.entry == ROOM_ENTRY_NONE,
              "the loser gets a game, 1 point, and loses their side");
        CHECK(room_after_result(&watch, 3, &s, 5, 1) && watch.games == 0 && watch.result_match == 5,
              "a watcher records the result and nothing else");
        CHECK(!room_after_result(&win, 2, &s, 5, 1) && win.wins == 1,
              "the same result is never counted twice");

        /* The next pick after that result: the winner holds 2P, the next in line
         * takes the 1P side the loser left. Arcade rules. */
        room_member_t m[4];
        memset(m, 0, sizeof(m));
        m[0].id = 1; m[0].data = lose;
        m[1].id = 2; m[1].data = win;
        m[2].id = 3; m[3].id = 4;
        for (int i = 0; i < 4; i++) m[i].known = true;
        uint16_t f[2];
        CHECK(room_pick_fighters(&s, m, 4, f) && f[0] == 3 && f[1] == 2,
              "the next match is the next in line on 1P against the winner on 2P");
        CHECK(room_reported_result(m, 4, 5) == 1 && room_reported_result(m, 4, 6) == -1,
              "a result is found in whichever member's attribute reports it");
    }

    /* ---- (F) watchers ---------------------------------------------------- */
    {
        /* Two fighters and a watcher who gets only every third record. The
         * watcher never gates anybody, and still ends up with every frame of both
         * sides, because each record carries the last ten. */
        lockstep_t a, b, w;
        lockstep_configure(&a, 0, 2, 2);
        lockstep_configure(&b, 1, 2, 2);
        lockstep_configure(&w, LOCKSTEP_WATCHER, 2, 2);
        lockstep_begin_round(&a, 4);
        lockstep_begin_round(&b, 4);
        lockstep_begin_round(&w, 4);
        CHECK(lockstep_is_watcher(&w) && w.announce_mask == 0, "a watcher announces nothing");
        lockstep_on_peer_announce(&a, 1, 4);
        lockstep_on_peer_announce(&b, 0, 4);
        CHECK(lockstep_barrier_released(&a) && lockstep_barrier_released(&b),
              "the fighters' barrier releases without the watcher");

        lockstep_record_t ra, rb;
        for (uint32_t f = 0; f < 60; f++) {
            lockstep_submit_local(&a, f, 0xA00u + f, &ra);
            lockstep_submit_local(&b, f, 0xB00u + f, &rb);
            lockstep_on_record(&a, &rb);
            lockstep_on_record(&b, &ra);
            if (f % 3 == 2) { lockstep_on_record(&w, &ra); lockstep_on_record(&w, &rb); }
        }
        bool all = true;
        for (uint32_t f = 0; f < 60; f++)
            if (!lockstep_ready(&w, f) || lockstep_input_for(&w, 0, f) != 0xA00u + f
                || lockstep_input_for(&w, 1, f) != 0xB00u + f) all = false;
        CHECK(all, "a watcher fed every third record has every frame of both sides");

        lockstep_record_t mine;
        lockstep_submit_local(&w, 3, 0x777, &mine);
        CHECK(mine.inputs[0] == 0 && lockstep_input_for(&w, 0, 3) == 0xA03u,
              "a watcher has no input of its own and cannot write a side");
    }

    /* ---- (G) a member heard from an address the server did not give ------ */
    /* The server told us member 34 is at 192.168.50.194:3658 (their LAN address:
     * we share a public one). They are really behind a container host's NAT, so
     * everything they send arrives from somewhere else -- here, 127.0.0.1. */
    {
        static rpcn_session_t s;
        memset(&s, 0, sizeof(s));
        g_notes[0] = 0;
        s.log = note_cb;
        CHECK(net_startup(), "(G) the socket library starts");
        uint16_t mine = free_udp_port(3760);
        net_sock_t them = NET_SOCK_INVALID, other = NET_SOCK_INVALID;
        uint16_t them_port = free_udp_port((uint16_t)(mine + 1));
        bool socks = mine && them_port && net_udp_open(&s.client.udp, mine) && net_udp_open(&them, them_port);
        uint16_t other_port = socks ? free_udp_port((uint16_t)(them_port + 1)) : 0;
        socks = socks && other_port && net_udp_open(&other, other_port);
        CHECK(socks, "(G) three loopback sockets");
        if (socks) {
            s.client.local_ip   = htonl(0xAC120002u);     /* 172.18.0.2, the bridge address */
            s.client.local_port = mine;
            s.room_id = 1;
            s.stage = RPCN_STAGE_HOSTING;
            s.my_member_id = s.owner_id = 16;
            rpcn_peer_t *p = &s.peers[0];
            p->used = true;
            p->member_id = 34;
            snprintf(p->npid, sizeof(p->npid), "guest");
            p->ip   = htonl(0xC0A832C2u);                 /* 192.168.50.194 */
            p->port = 3658;

            /* A game packet naming them from the wrong address: still refused. */
            uint8_t game[12] = { 0 };
            net_udp_send(them, htonl(0x7F000001u), mine, game, sizeof(game));
            uint16_t from = 0; uint32_t ip = 0; uint16_t port = 0;
            int got = pump_recv(&s, &from, &ip, &port);
            CHECK(got == (int)sizeof(game) && from == 0, "(G) a game packet from elsewhere is nobody's");
            CHECK(!rpcn_session_claim(&s, 34, ip, port) && !p->heard,
                  "(G) and cannot claim a member the server placed at another address");

            /* A punch from a member nobody knows: ignored. */
            punch_from(them, mine, 99);
            pump_recv(&s, &from, &ip, &port);
            CHECK(!p->heard, "(G) a punch naming a stranger changes nothing");

            /* Their punch, from the wrong address: they are linked where heard. */
            punch_from(them, mine, 34);
            pump_recv(&s, &from, &ip, &port);
            CHECK(p->heard && p->ip == htonl(0x7F000001u) && p->port == them_port
                  && s.stage == RPCN_STAGE_LINKED,
                  "(G) a punch names its sender, so it re-points them to where it came from");
            CHECK(strstr(g_notes, "not the address the server gave (192.168.50.194:3658)") != NULL,
                  "(G) and the log says which address the server gave");

            /* Their game packets now belong to them. */
            net_udp_send(them, htonl(0x7F000001u), mine, game, sizeof(game));
            from = 0;
            got = pump_recv(&s, &from, &ip, &port);
            CHECK(got == (int)sizeof(game) && from == 34, "(G) their game packets are theirs from then on");

            /* Once heard, nobody moves them: not even a punch naming them. */
            punch_from(other, mine, 34);
            pump_recv(&s, &from, &ip, &port);
            CHECK(p->ip == htonl(0x7F000001u) && p->port == them_port,
                  "(G) a later punch from a third address does not move a member already heard");

            /* ...until the address they were heard at goes quiet: their NAT made a
             * new mapping, and they are still talking to us from it. */
            p->last_heard_ms = net_now_ms() - RPCN_PEER_QUIET_MS - 1;
            net_udp_send(other, htonl(0x7F000001u), mine, game, sizeof(game));
            from = 0;
            got = pump_recv(&s, &from, &ip, &port);
            CHECK(got == (int)sizeof(game) && from == 0 && rpcn_session_claim(&s, 34, ip, port)
                  && p->port == other_port,
                  "(G) once quiet, their game packet from a new port on the same address moves them");
            CHECK(strstr(g_notes, "went quiet") != NULL, "(G) and the log says they were followed");
            punch_from(them, mine, 34);
            pump_recv(&s, &from, &ip, &port);
            CHECK(p->port == other_port, "(G) the old address, heard again straight after, does not move them back");
            net_udp_send(other, htonl(0x7F000001u), mine, game, sizeof(game));
            from = 0;
            got = pump_recv(&s, &from, &ip, &port);
            CHECK(got == (int)sizeof(game) && from == 34, "(G) their packets from the new address are theirs");

            /* A game packet from another ADDRESS is not enough, even when quiet:
             * that is first contact's rule too. A punch is. */
            p->last_heard_ms = net_now_ms() - RPCN_PEER_QUIET_MS - 1;
            CHECK(!rpcn_session_claim(&s, 34, htonl(0x7F000002u), 4000) && p->port == other_port,
                  "(G) a quiet member is not moved by a game packet from another address");
            CHECK(rpcn_session_hear(&s, p, htonl(0x7F000002u), 4000, true) && p->ip == htonl(0x7F000002u),
                  "(G) but a punch from one moves them");
            p->ip = htonl(0x7F000001u);
            p->port = them_port;
            p->last_heard_ms = net_now_ms();

            /* The keepalive tells the server the advertised address when one is set. */
            s.client.user_id = 7;
            s.client.signaling_addr = htonl(0x7F000001u);
            net_sock_t helper = NET_SOCK_INVALID;
            if (net_udp_open(&helper, RPCN_SIGNALING_PORT)) {
                uint8_t ka[32]; uint32_t kip; uint16_t kport;
                s.client.advertised_ip = htonl(0xC0A83205u);   /* 192.168.50.5 */
                rpcn_send_signaling_ping(&s.client, 0);
                int n = 0;
                for (uint64_t end = net_now_ms() + 200; n <= 0 && net_now_ms() < end;)
                    n = net_udp_recv(helper, ka, sizeof(ka), &kip, &kport);
                uint32_t said = 0;
                if (n == 13) memcpy(&said, ka + 9, 4);
                CHECK(n == 13 && said == htonl(0xC0A83205u), "(G) the keepalive carries the advertised LAN address");
                s.client.advertised_ip = 0;
                rpcn_send_signaling_ping(&s.client, 0);
                n = 0;
                for (uint64_t end = net_now_ms() + 200; n <= 0 && net_now_ms() < end;)
                    n = net_udp_recv(helper, ka, sizeof(ka), &kip, &kport);
                said = 0;
                if (n == 13) memcpy(&said, ka + 9, 4);
                CHECK(n == 13 && said == htonl(0xAC120002u), "(G) and the socket's own without one");
                net_close(&helper);
            } else {
                printf("skip: (G) keepalive: UDP %u is taken on this machine\n", (unsigned)RPCN_SIGNALING_PORT);
            }
            s.client.signaling_addr = 0;   /* not the helper's any more: the drain below */
        }
        net_close(&them);
        net_close(&other);
        net_close(&s.client.udp);
    }

    /* ---- (G2) the peer-to-peer port is taken --------------------------------
     * Another emulator, or RPCS3, holds it. The session steps to the next free
     * one before it reaches for the server; the server is a closed port here,
     * so the start fails after the bind, which is the part under test. */
    {
        static rpcn_session_t s;
        memset(&s, 0, sizeof(s));
        g_notes[0] = 0;
        uint16_t held_port = free_udp_port(3860);
        net_sock_t held = NET_SOCK_INVALID;
        bool ok = held_port && net_udp_open(&held, held_port);
        CHECK(ok, "(G2) a port to hold");
        if (ok) {
            rpcn_session_config_t cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.server = "127.0.0.1";
            cfg.port = 1;                         /* nothing listens: the connect fails */
            cfg.npid = "someone";
            cfg.password = "x";
            cfg.token = "";
            cfg.com_id = "NPWR99999_00";
            cfg.local_p2p_port = held_port;
            cfg.log = note_cb;
            rpcn_session_start(&s, &cfg);
            CHECK(strstr(g_notes, "is in use by another program; using") != NULL,
                  "(G2) a taken port is stepped past, not reported as a failure");
            CHECK(strstr(rpcn_session_error(&s), "bind UDP") == NULL,
                  "(G2) and the session did not fail on the bind");
            rpcn_session_stop(&s);
        }
        net_close(&held);
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
