/*
 * drive_board.h — the driving cabinet's drive board: the wheel's force
 * feedback, as its commands (board-level; Pinboard #1047).
 *
 * Daytona USA's wheel is turned by a motor on the drive board (838-10646, a
 * Z80 running epr-16488a; MAME sj25_0207_01 runs the program but wires it to
 * nothing). The game sends it one byte at a time through the Model 1 I/O
 * board's port E (model1io.h keeps the last one as `drive`). The board's ROM
 * is not in the sets we load, so the commands are decoded here, not run.
 *
 * What the game sends (Daytona ROM 0x1BD5C, called each frame for the
 * player's car at 0x501220; queued at 0x502900, sent one a frame by 0x1C4A8).
 * A command is sent again only when it changes, so each is a state the board
 * holds until the next:
 *   0x20-0x27  standing or slow (car +0x18 < 64): 0x27 at rest, falling to 0x20
 *   0x38-0x3F  faster: 0x38 | (speed - 63) >> 6, rising with speed
 *   0x42 for 20 frames, then 0x32 for 12, over and over, while the car moves
 *              with surface bits 2-3 of +0x1BE / +0x1C0 set (the grass)
 *   0x50, 0x60 bit 8 of +0x1C0 / +0x1BE (at most one a 32 frames)
 *   0x53, 0x63 contact (+0x9D 0x10 or 0x0A; which of the two is +0xF1 bit 1)
 *   0x57, 0x67 a crash (+0xF0 0x20 or 0x0A; which is +0x00 bits 23 / 25)
 *   0x10       spinning (+0xD2 1..0x50), or stopped with those surface bits
 *   0x27       also while +0xD8 is -112 or 400
 *   0x52/0x62/0x42  when +0x281 is 0..2 (outside the race)
 *   0x00-0x0F, 0x71, 0xFF  at power-on and the race start: set-up, no force
 * The high nibble is the effect and the low three bits its strength (0-7).
 * Sega's later drive boards keep the families (Supermodel's DriveBoard.cpp,
 * Scud Race: 0x1x centring, 0x2x friction, 0x5x / 0x6x turn the wheel right /
 * left). 0x2x is the clutch: the motor held, the wheel heavy, a steady
 * resistance on the owner's cabinets (Pinboard #1447), strongest standing.
 * 0x3x takes over from it as the car speeds up and grows from there: the
 * centring at speed. 0x4x is the uncentering (Boomslangnz's FFB plugin names
 * it so, and the owner's cabinets do it on the grass): the wheel is pushed on
 * the way it is already turned, as round a banked corner. The side of 0x5x /
 * 0x6x is Supermodel's; the game alone does not tell it.
 *
 * Sega Rally speaks another language to its drive board: drive_board_decode_srally.
 *
 * Nothing on the board reads the drive board back (port E reads 0xFF), so
 * this is output only: it changes no frame, and needs no savestate of its own
 * (the command is model1io's `drive`, already in the state).
 */
#ifndef M2HLE_DRIVE_BOARD_H
#define M2HLE_DRIVE_BOARD_H

#include <stdint.h>

typedef enum {
    DRIVE_NONE = 0,      /* set-up, test or unknown: the motor is free */
    DRIVE_SPRING,        /* 0x1x: centring, standing (0x10: none) */
    DRIVE_CLUTCH,        /* 0x2x: heavy wheel, the motor held */
    DRIVE_CENTER,        /* 0x3x: centring that grows with speed */
    DRIVE_UNCENTER,      /* 0x4x: pushed on the way it is turned */
    DRIVE_PUSH,          /* 0x5x right, 0x6x left */
    DRIVE_TORQUE,        /* Sega Rally: a steady turning force, 1-32 */
} drive_kind_t;

typedef struct {
    uint8_t cmd;
    uint8_t kind;        /* drive_kind_t */
    uint8_t level;       /* 0-7; DRIVE_TORQUE 1-32 */
    int8_t  dir;         /* DRIVE_PUSH, DRIVE_TORQUE: +1 right, -1 left; else 0 */
} drive_effect_t;

static inline drive_effect_t drive_board_decode(uint8_t cmd) {
    drive_effect_t e = { cmd, DRIVE_NONE, (uint8_t)(cmd & 7u), 0 };
    switch (cmd >> 4) {
        case 0x1: e.kind = DRIVE_SPRING;   break;
        case 0x2: e.kind = DRIVE_CLUTCH;   break;
        case 0x3: e.kind = DRIVE_CENTER;   break;
        case 0x4: e.kind = DRIVE_UNCENTER; break;
        case 0x5: e.kind = DRIVE_PUSH; e.dir = +1; break;
        case 0x6: e.kind = DRIVE_PUSH; e.dir = -1; break;
        default:  e.level = 0; break;
    }
    return e;
}

/*
 * Sega Rally's drive board (Pinboard #1082). The game writes one byte to the
 * 315-5649's port E (IO+0x08; io_5649.h, io5649_drive), its RAM copy at
 * 0x202049, and sends it again only when it changes. It is not Daytona's
 * language: the i960 works the force out itself, each frame, from the car
 * (the race's physics at 0x5D6870 call 0x5D8C40) or from the wheel's pot
 * alone (0x5D8750, a spring back to the centre), and the board only turns
 * the motor:
 *   0x80-0x9F  turn the wheel right (toward a higher pot), strength 1-32
 *   0xC0-0xDF  turn it left, strength 1-32
 *   0x00       motor off (attract, a standing car)
 *   0x10-0x1F  set-up (0x10 at power-on and reset, 0x15 at the race start)
 * The test menu's strength setting (0x20201F) scales the force and its
 * switch (0x20201A == 1) sends only 0x00, all on the i960. The side was
 * measured: wheel right of centre (pot 0x8C) brings 0xCx, left of it (0x74)
 * 0x8x, the centring a cabinet feels. 0xE0-0xFF is never sent (0xFF is the
 * port before the first write): nothing.
 */
static inline drive_effect_t drive_board_decode_srally(uint8_t cmd) {
    drive_effect_t e = { cmd, DRIVE_NONE, 0, 0 };
    if ((cmd & 0xE0u) == 0x80u || (cmd & 0xE0u) == 0xC0u) {
        e.kind  = DRIVE_TORQUE;
        e.level = (uint8_t)((cmd & 0x1Fu) + 1u);
        e.dir   = (cmd & 0x40u) ? -1 : +1;
    }
    return e;
}

#endif /* M2HLE_DRIVE_BOARD_H */
