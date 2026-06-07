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

#define ROMDIR "c:/Users/bigge/source/repos/ai/claude_mame/mame/roms/"
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

    /* ---- (B) end-to-end through the game's read_sw interrupt ---- */
    static romset_t rs; static memory_bus_t bus; static i960_cpu_t cpu; static emu_thread_ctx_t ctx;
    if (sfight_load(&rs, ROMDIR "sfight.zip", ROMDIR "schamp.zip") != 0) {
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
