/*
 * comm_role_test.c -- the room's cabinet role for a linked comm board
 * (net/comm_role.h, Pinboard #969), on a bare memory bus. No ROM.
 *
 * Virtual-On's profile writes the role into the backup block its ROM made
 * (both copies, each with the ROM's CRC) and nothing into a block it did not
 * make. comm_role waits, frame edge by frame edge, until the profile can hold
 * the role, then resets the board with its backup RAM kept and only then
 * plugs the peer in; ending the link unplugs it.
 *
 * The ROM here is made up: a header and a CRC table of its own. The real
 * ROM's block is what det_digest --room-role proves on (tools/README.md).
 *
 * A ctest (CMakeLists.txt, comm_role_test).
 */
#define NDEBUG 1
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "memory.h"
#include "i960.h"
#include "emu_thread.h"
#include "registry.h"
#include "comm_role.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static memory_bus_t bus;
static uint8_t      rom[0x4000];
static uint8_t     *back = bus.back;
static const game_profile_t *vonj;

/* A made-up ROM: the header the block starts with and a CRC table. */
static void make_rom(void) {
    for (uint32_t i = 0; i < 16; i++) rom[VONJ_ROM_HEADER + i] = (uint8_t)("VON-TEST-HEADER!"[i]);
    for (uint32_t i = 0; i < 256; i++) {
        uint16_t v = (uint16_t)(i * 0x1021u ^ i << 9);
        rom[VONJ_ROM_CRC_TABLE + i * 2u] = (uint8_t)v;
        rom[VONJ_ROM_CRC_TABLE + i * 2u + 1u] = (uint8_t)(v >> 8);
    }
    bus.rom = rom;
    bus.rom_size = sizeof rom;
}

/* The block the ROM's factory init leaves at `at`: header, size, settings,
 * CRC. role is its cabinet-role byte. */
static void make_block(uint32_t at, uint8_t role) {
    uint8_t *b = back + at;
    memcpy(b, rom + VONJ_ROM_HEADER, 16);
    b[0x10] = 0x0C; b[0x11] = 0x02; b[0x12] = b[0x13] = 0;
    for (uint32_t i = 0x16; i < 0x38; i++) b[i] = (uint8_t)(i * 3u);
    b[VONJ_BACKUP_ROLE] = role;
    uint16_t crc = vonj_backup_crc(&bus, b + 0x16, 0x22);
    b[VONJ_BACKUP_CRC] = (uint8_t)crc; b[VONJ_BACKUP_CRC + 1u] = (uint8_t)(crc >> 8);
}

static bool both_say(uint8_t role) {
    return vonj_backup_valid(&bus, back) && vonj_backup_valid(&bus, back + VONJ_BACKUP_BLOCK)
        && back[VONJ_BACKUP_ROLE] == role && back[VONJ_BACKUP_BLOCK + VONJ_BACKUP_ROLE] == role;
}

static void test_profile_role(void) {
    memset(back, 0, BACKUP_RAM_KEEP);
    CHECK(!vonj->link_role(&bus, GAME_LINK_MASTER) && back[VONJ_BACKUP_ROLE] == 0,
          "vonj: blank backup RAM takes no role (the ROM has not made its block)");
    make_block(0, 2);
    make_block(VONJ_BACKUP_BLOCK, 2);
    CHECK(both_say(2), "vonj: the made block is valid, NOLINK (2)");
    CHECK(vonj->link_role(&bus, GAME_LINK_MASTER) && both_say(1), "vonj: MASTER is 1 in both copies, CRCs good");
    CHECK(vonj->link_role(&bus, GAME_LINK_SLAVE) && both_say(0), "vonj: SLAVE is 0 in both copies");
    CHECK(vonj->link_role(&bus, GAME_LINK_ALONE) && both_say(2), "vonj: alone is NOLINK again");
    vonj_apply_menu_settings(&bus, (const uint8_t[8]){ 2, 1, 0, 3, 2, 0, 0, 0 });
    CHECK(!vonj_backup_valid(&bus, back), "vonj: the shell's Arcade settings leave the first copy's CRC stale");
    CHECK(vonj->link_role(&bus, GAME_LINK_MASTER) && both_say(1) && back[0x16] == 0xFF && back[0x1B] == 2
          && back[0x21] == 1 && back[0x34] == 1,
          "vonj: the role keeps them, under a fresh CRC (DEATH-MATCH, 3 rounds, HARD, free play)");
    memset(back + VONJ_BACKUP_BLOCK, 0, 16);
    CHECK(vonj->link_role(&bus, GAME_LINK_SLAVE) && back[VONJ_BACKUP_ROLE] == 0
          && back[VONJ_BACKUP_BLOCK + VONJ_BACKUP_ROLE] == 1, "vonj: a copy the ROM did not make is left alone");
}

static uint8_t sent;
static void peer_send(void *ctx, const uint8_t *msg, uint32_t len) { (void)ctx; (void)msg; (void)len; sent++; }
static uint32_t peer_recv(void *ctx, uint8_t *msg, uint32_t cap) { (void)ctx; (void)msg; (void)cap; return 0; }
static const m2comm_peer_t peer = { NULL, peer_send, peer_recv };

static int resets;
static const m2comm_peer_t *peer_at_reset;
/* The cold boot: mem_init clears backup RAM on a board without its battery. */
static void reset(void *ctx) {
    (void)ctx;
    resets++;
    peer_at_reset = g_m2comm_link.peer;
    memset(back, 0, BACKUP_RAM_KEEP);
}

static void test_sequence(void) {
    memset(back, 0, BACKUP_RAM_KEEP);
    m2comm_set_peer(&peer);
    comm_role_begin(GAME_LINK_SLAVE, &peer);
    CHECK(comm_role_waiting() && !g_m2comm_link.peer, "begin: the board is alone until its reset");
    for (int i = 0; i < 3; i++) comm_role_reset(&bus, vonj, reset, NULL);
    CHECK(comm_role_waiting() && !resets && g_comm_role.waited == 3,
          "no block yet: no reset, three frame edges waited");
    make_block(0, 2);
    make_block(VONJ_BACKUP_BLOCK, 2);
    CHECK(comm_role_reset(&bus, vonj, reset, NULL) && resets == 1, "the block is made: one reset");
    CHECK(!peer_at_reset, "the peer is not plugged in before the reset");
    CHECK(both_say(0), "the reset keeps backup RAM, with the role in it (SLAVE)");
    CHECK(comm_role_linked() && g_m2comm_link.peer == &peer && !sent,
          "after it the peer is plugged in, and nothing has been sent");
    CHECK(!comm_role_reset(&bus, vonj, reset, NULL) && resets == 1, "linked: no second reset");
    comm_role_end();
    CHECK(!comm_role_waiting() && !comm_role_linked() && !g_m2comm_link.peer, "end: the board is alone again");
    comm_role_begin(GAME_LINK_MASTER, &peer);
    CHECK(!comm_role_reset(&bus, NULL, reset, NULL) && resets == 1, "a profile with no link is never reset");
    comm_role_end();
}

int main(void) {
    for (size_t i = 0; i < g_profile_count; i++)
        if (!strcmp(g_profiles[i]->id, "vonj")) vonj = g_profiles[i];
    if (!vonj || !vonj->link_role) { printf("FAIL: no vonj profile with a link role\n"); return 1; }
    mem_init(&bus, NULL, 0);   /* backup RAM on the bus, for the shell's writes */
    make_rom();
    test_profile_role();
    test_sequence();
    printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
