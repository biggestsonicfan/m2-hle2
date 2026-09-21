/*
 * net_test.c — the netplay pieces that can be proven without a server.
 *
 * The RPCN half of src/net needs a live server and is exercised against one by
 * hand (README, "Netplay"). Everything below it does not: the lockstep engine,
 * the protobuf writer/reader, the ComId standard and the canonical input word
 * are pure functions of their inputs, and they are also where a quiet mistake
 * costs a desync rather than an error message. So they get a test.
 *
 * Four parts:
 *  (A) The input rings: newest-wins ingest, redundancy repair, ring aliasing.
 *  (B) The barrier and the generation fence.
 *  (C) protobuf round trips, including the uint8/uint16 WRAPPER trap that
 *      np2_structs.proto sets for anyone who reads those fields as bare varints.
 *  (D) ComId derivation: stable, case- and punctuation-insensitive, well formed
 *      by RPCN's own rule, and never colliding between two different games.
 */
#define NDEBUG 1
#include <stdio.h>
#include <string.h>

#include "com_id.h"
#include "lockstep.h"
#include "protobuf.h"

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

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
