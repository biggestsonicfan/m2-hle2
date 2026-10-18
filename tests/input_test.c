/*
 * input_test.c — Phase 12 verification for the interrupt-driven input path.
 *
 * Two parts:
 *  (A) Unit: input_io_read_cb serves the correct ACTIVE-LOW IN0/IN1/IN2 bytes
 *      from the host held mask.
 *  (B) End-to-end: load STF, attach the I/O read callback, run the emu thread,
 *      set a host held bit, and confirm the GAME's own read_sw (running from the
 *      VsyncScr vblank interrupt) reads the port and writes INTERUPT_FLAGS_HELD
 *      (0x500700). This proves input arrives via the interrupt, not a RAM poke.
 */
#define NDEBUG 1
#include <stdio.h>

#include "sfight.h"        /* profile, load/install */
#include "emu_thread.h"    /* emu thread */
#include "input.h"         /* g_input, input_attach, input_io_read_cb */
#include "lightgun_host.h"  /* the trigger's flash (Pinboard #1045) */

#include "test_rom_dir.h"
#define HELD_ADDR 0x00500700u

/* hle_check / input bits dispatch through g_active_profile. */
const game_profile_t *g_active_profile = &sfight_profile;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    /* ---- (A) port byte mapping ---- */
    mem_region_t io = { .name="IO", .base=IO_BASE, .size=IO_SIZE, .data=NULL };
    g_input.held = 0;
    CHECK((input_io_read_cb(&io, IO_BASE+0x02,1)&0xFF)==0xFF &&
          (input_io_read_cb(&io, IO_BASE+0x04,1)&0xFF)==0xFF &&
          (input_io_read_cb(&io, IO_BASE+0x06,1)&0xFF)==0xFF,
          "idle: all I/O port bytes read 0xFF (active-low, nothing pressed)");

    g_input.held = 0x00002000;  /* P1_UP (held bit13 -> IN1/byte1 bit5) */
    CHECK((input_io_read_cb(&io, IO_BASE+0x04,1)&0x20)==0,
          "P1 UP pulls IN1 (IO+0x04) bit5 low");
    g_input.held = 0x00000010;  /* P1_START (held bit4 -> IN0 bit4) */
    CHECK((input_io_read_cb(&io, IO_BASE+0x02,1)&0x10)==0,
          "P1 START pulls IN0 (IO+0x02) bit4 low");
    g_input.held = 0x00000001;  /* COIN1 (held bit0 -> IN0 bit0) */
    CHECK((input_io_read_cb(&io, IO_BASE+0x02,1)&0x01)==0,
          "P1 COIN pulls IN0 (IO+0x02) bit0 low");
    g_input.held = 0;

    /* ---- (A2) combos ("macros") and counted presses ---- */
    const uint32_t *bits = sfight_profile.input.bits;
    CHECK(input_combo_parse("b1+b2") == ((1u << GAME_INPUT_P1_B1) | (1u << GAME_INPUT_P1_B2)) &&
          input_combo_parse("p2:B1+b3") == ((1u << GAME_INPUT_P2_B1) | (1u << GAME_INPUT_P2_B3)) &&
          input_combo_parse("b1+b5") == 0 && input_combo_parse("") == 0,
          "combo text parses to action masks, and junk to 0");
    CHECK(input_keycode_by_name("A") == SAPP_KEYCODE_A && input_keycode_by_name("kp1") == SAPP_KEYCODE_KP_1 &&
          input_keycode_by_name("f12") == SAPP_KEYCODE_F12 && input_keycode_by_name("pageup") == SAPP_KEYCODE_PAGE_UP &&
          input_keycode_by_name("nope") == -1,
          "--macro key names resolve to sokol key codes");

    input_reset();
    input_combo_bind(SAPP_KEYCODE_A, input_combo_parse("b1+b2"));
    input_key_down(SAPP_KEYCODE_A);
    CHECK((g_input.held & (bits[GAME_INPUT_P1_B1] | bits[GAME_INPUT_P1_B2])) ==
          (bits[GAME_INPUT_P1_B1] | bits[GAME_INPUT_P1_B2]),
          "a combo key holds both of its buttons");
    input_key_down(SAPP_KEYCODE_Z);   /* B1 on its own key as well */
    input_key_up(SAPP_KEYCODE_A);
    CHECK((g_input.held & bits[GAME_INPUT_P1_B1]) && !(g_input.held & bits[GAME_INPUT_P1_B2]),
          "letting go of the combo keeps B1, which Z still holds");
    input_key_up(SAPP_KEYCODE_Z);
    CHECK(g_input.held == 0, "letting go of Z releases B1");
    input_key_down(SAPP_KEYCODE_A);
    input_release_all();
    input_key_down(SAPP_KEYCODE_Z);
    input_key_up(SAPP_KEYCODE_Z);
    CHECK(g_input.held == 0, "a release-all forgets the counts, so a later press and release clears");
    input_reset();
    g_input_combo_count = 0;

    /* ---- (A2) a light gun's flash: off by default, N drawn frames a pull ---- */
    {
        static game_profile_t gun_profile;
        const game_profile_t *was = g_active_profile;
        gun_profile.quirks.lightgun = 1;
        g_active_profile = &gun_profile;
        g_lightgun_controls[0].trigger = 0x100;
        g_lightgun_controls[1].trigger = 0x10000;
        g_lightgun_held_fn = input_board_held;
        lightgun_host_bar_t bars[LIGHTGUN_HOST_BARS];
        int frames[6];
        #define GUN_FLASHED() (lightgun_host_crosshairs(0, 0, 496, 384, bars) > 0 && bars[0].rgb == 0xFFFFFFu \
                               && bars[0].x1 == 496.0f && bars[0].y1 == 384.0f)
        g_input.held = 0x100;
        CHECK(!GUN_FLASHED(), "no flash unless --gun-flash asks for one");
        g_input.held = 0;
        GUN_FLASHED();
        CHECK(lightgun_host_option("--gun-flash", "2") && g_lightgun_flash == 2 &&
              lightgun_host_option("--gun-flash", "99") && g_lightgun_flash == LIGHTGUN_HOST_FLASH_MAX &&
              lightgun_host_option("--gun-flash", "on") && g_lightgun_flash == 2 &&
              !lightgun_host_option("--wheel-range", "2"), "--gun-flash N, clamped; on is 2");
        for (int i = 0; i < 6; i++) { g_input.held = i < 4 ? 0x100 : 0; frames[i] = GUN_FLASHED(); }
        CHECK(frames[0] && frames[1] && !frames[2] && !frames[3] && !frames[4] && !frames[5],
              "a pull flashes 2 frames, holding it does not flash again");
        g_input.held = 0x10000;
        CHECK(GUN_FLASHED(), "P2's trigger flashes too");
        g_input.held = 0;
        GUN_FLASHED();
        lightgun_host_option("--gun-flash", "off");
        g_input.held = 0x100;
        CHECK(!GUN_FLASHED(), "--gun-flash off");
        #undef GUN_FLASHED
        g_input.held = 0;
        g_lightgun_held_fn = NULL;
        g_lightgun_controls[0].trigger = g_lightgun_controls[1].trigger = 0;
        g_active_profile = was;
    }

    /* ---- (A3) a four-player cabinet's side switch (Air Walkers, Pinboard #1063) ---- */
    {
        static game_profile_t four_profile = { .id = "four" };
        const game_profile_t *was = g_active_profile;
        four_profile.quirks.four_player_sides = 1;
        four_profile.input = sfight_profile.input;   /* the standard ports' bits */
        input_reset();
        input_far_down(GAME_INPUT_P1_B1);
        CHECK(g_input.far_held == 0, "a two-player profile has no players 3 and 4");
        g_active_profile = &four_profile;
        mem_region_t sio = { .name="IO", .base=IO_BASE, .size=IO_SIZE, .data=NULL };
        input_attach_sides(&sio);
        g_input.held = 0x00010330;   /* START1, START2, P1 B1 + B2, P2 B1 */
        CHECK((sio.read_cb(&sio, IO_BASE+0x02,1)&0xFF)==0xCF &&
              (sio.read_cb(&sio, IO_BASE+0x04,1)&0xFF)==0xFC &&
              (sio.read_cb(&sio, IO_BASE+0x06,1)&0xFF)==0xFE,
              "side 1/2: IN0 starts, IN1 P1, IN2 P2 as any game");
        sio.write_cb(&sio, IO_BASE+0x0A, 0x80, 1);
        CHECK(g_io_side == 1 &&
              (sio.read_cb(&sio, IO_BASE+0x02,1)&0xFF)==0xFF &&
              (sio.read_cb(&sio, IO_BASE+0x04,1)&0xFF)==0xFF &&
              (sio.read_cb(&sio, IO_BASE+0x06,1)&0xFF)==0xFF,
              "port F bit 7: players 3 and 4, who are not held, on IN1 / IN2 / the starts");
        sio.write_cb(&sio, IO_BASE+0x0A, 0x7F, 1);
        CHECK(g_io_side == 0 && (sio.read_cb(&sio, IO_BASE+0x04,1)&0xFF)==0xFC,
              "bit 7 down: back to players 1 and 2");

        /* Players 3 and 4 (Pinboard #1073): player 3 on player 1's bits, 4 on 2's. */
        input_far_down(GAME_INPUT_P1_B1);      /* P3 B1 */
        input_far_down(GAME_INPUT_P2_UP);      /* P4 UP */
        input_far_down(GAME_INPUT_P2_START);   /* START4 */
        CHECK((sio.read_cb(&sio, IO_BASE+0x04,1)&0xFF)==0xFC && (sio.read_cb(&sio, IO_BASE+0x02,1)&0xFF)==0xCF,
              "players 3 and 4 held: side 1/2 still reads players 1 and 2");
        sio.write_cb(&sio, IO_BASE+0x0A, 0x80, 1);
        CHECK((sio.read_cb(&sio, IO_BASE+0x04,1)&0xFF)==0xFE &&
              (sio.read_cb(&sio, IO_BASE+0x06,1)&0xFF)==0xDF &&
              (sio.read_cb(&sio, IO_BASE+0x02,1)&0xFF)==0xDF,
              "port F bit 7: P3 B1 on IN1, P4 UP on IN2, START4 on IN0");
        input_far_down(GAME_INPUT_P1_COIN);   /* COIN3 */
        CHECK((sio.read_cb(&sio, IO_BASE+0x02,1)&0xFF)==0x9F, "COIN3 is IN0 bit 6");
        sio.write_cb(&sio, IO_BASE+0x0A, 0x00, 1);
        CHECK((sio.read_cb(&sio, IO_BASE+0x02,1)&0xFF)==0x8F, "COIN3 on side 1/2 too, beside the held starts");
        sio.write_cb(&sio, IO_BASE+0x0A, 0x80, 1);
        g_input.use_net = 1; g_input.net_held = 0;
        CHECK((sio.read_cb(&sio, IO_BASE+0x04,1)&0xFF)==0xFF && (sio.read_cb(&sio, IO_BASE+0x02,1)&0xFF)==0xFF,
              "a session has the input: players 3 and 4 read released");
        g_input.use_net = 0;
        input_far_up(GAME_INPUT_P1_B1);
        CHECK((sio.read_cb(&sio, IO_BASE+0x04,1)&0xFF)==0xFF, "P3 lets go of B1");
        CHECK(input_combo_parse("p3:up") == (INPUT_COMBO_FAR | (1u << GAME_INPUT_P1_UP)) &&
              input_combo_parse("P4:b1+b2") == (INPUT_COMBO_FAR | (1u << GAME_INPUT_P2_B1) | (1u << GAME_INPUT_P2_B2)) &&
              input_combo_parse("p3:") == 0 && input_combo_parse("p5:b1") == 0,
              "p3: and p4: combos are players 3 and 4's");
        input_release_all();
        input_combo_bind(SAPP_KEYCODE_T, input_combo_parse("p3:right"));
        input_key_down(SAPP_KEYCODE_T);
        CHECK(g_input.held == 0 && (sio.read_cb(&sio, IO_BASE+0x04,1)&0xFF)==0xBF, "a p3: key holds player 3's RIGHT");
        input_key_up(SAPP_KEYCODE_T);
        CHECK(g_input.far_held == 0, "and lets go of it");
        sio.write_cb(&sio, IO_BASE+0x0A, 0x00, 1);
        g_input_combo_count = 0;
        g_input.held = 0;
        input_reset();
        g_active_profile = was;
    }

    /* ---- (B) end-to-end through the game's read_sw interrupt ---- */
    static romset_t rs; static memory_bus_t bus; static i960_cpu_t cpu; static emu_thread_ctx_t ctx;
    if (sfight_load(&rs, test_rom("sfight.zip"), test_rom("schamp.zip")) != 0) {
        printf("FAIL: ROM load\n"); return 1;
    }
    sfight_install(&rs, &cpu, &bus);
    input_attach(&bus);
    emu_thread_init(&ctx, &cpu, &bus);
    emu_run(&ctx);

    /* Let STF boot far enough that read_sw runs each frame (via VsyncScr). */
    emu_sleep_ms(1500);

    /* Press P1 START: held bit 0x10. Within a couple frames the game's read_sw
     * should pick it up and set 0x500700 bit 0x10. */
    g_input.held = 0x00000010;
    emu_sleep_ms(300);
    uint32_t held_ram_pressed = mem_read32(&bus, HELD_ADDR);
    g_input.held = 0;
    emu_sleep_ms(300);
    uint32_t held_ram_released = mem_read32(&bus, HELD_ADDR);

    printf("info: 0x500700 pressed=0x%08X released=0x%08X\n", held_ram_pressed, held_ram_released);
    CHECK((held_ram_pressed & 0x10) != 0, "game read_sw set 0x500700 START bit from the I/O port");
    CHECK((held_ram_released & 0x10) == 0, "releasing clears it on the next interrupt read");

    /* A P1 button (B1 = held bit 0x100 = IN1 bit0). */
    g_input.held = 0x00000100;
    emu_sleep_ms(300);
    uint32_t b1 = mem_read32(&bus, HELD_ADDR);
    g_input.held = 0;
    printf("info: 0x500700 with P1_B1 held = 0x%08X\n", b1);
    CHECK((b1 & 0x100) != 0, "game read_sw set 0x500700 P1_B1 bit from the I/O port");

    emu_thread_shutdown(&ctx);
    romset_free(&rs);
    mem_shutdown(&bus);
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
