/*
 * seat_board.h — Rail Chase 2's motion seat (board-level; Pinboard #1917).
 *
 * The cabinet's seat rides on air: a left, a right and a rear cylinder with
 * their up, down and reverse valves, a compression valve, an unloader valve
 * and a compressor (the test menu's VALVE TEST, strings at 0x10500-0x10990).
 * The game drives them through the 315-5649's port E (IO+0x08), one byte a
 * frame from its RAM copy at 0x501D8C (sent at 0x1C2C), and reads the seat
 * board back on port D (IO+0x06, into 0x501D8D). The bits are valve lines,
 * active low. The test menu's valve screens (0x10E40-0x112D0) start from
 * 0x8F (4-port, the rear), 0x7F (2-port right) and 0x5F (2-port left) and
 * clear bits 0x01, 0x02, 0x04 and 0x10 for the buttons held; which line is
 * which valve past that is not worked out. What the game sends:
 *   0x3B       at power-on, and in attract
 *   E0 D0 B0 70 0E 0D 0B 07, three frames each, over and over: the board
 *              check. The board answers each with one bit of port D low (E0/0E
 *              bit 0, D0/0D bit 1, B0/0B bit 2, 70/07 bit 3; MAME's
 *              rchase2_drive_board_r); the game stops at a seat error without.
 *   0x3F, 0x2F the seat parked, a coin in
 *   the ride: a scripted move from the tables at 0x2C080-0x2C190 (command,
 *              frames pairs, played by 0x2C1C0: F4/AF a shot's jolt, 9F/AF
 *              the rocking of a hit, 96/C8 a lurch) or, between them, the seat
 *              following the camera (0x3AC00): 0xF8/0xF0 level, 0xC8/0xC0
 *              banked one way, 0xB8/0xB0 the other (bits 0x30 / 0x40 low),
 *              bit 3 the pitch.
 * The seat board's ROM is not in the set, so nothing here runs it: port D
 * answers as MAME's does, and ffb.h turns the commands into what a pad can
 * feel: a jolt each time valve lines change (seat_board_motion).
 */
#ifndef M2HLE_SEAT_BOARD_H
#define M2HLE_SEAT_BOARD_H

#include <stdbool.h>
#include <stdint.h>

/* Port D: the board check's answer to port E's `cmd`. */
static inline uint8_t seat_board_reply(uint8_t cmd) {
    uint8_t data = 0xFF;
    if (cmd == 0xE0 || cmd == 0x0E) data &= (uint8_t)~1u;
    if (cmd == 0xD0 || cmd == 0x0D) data &= (uint8_t)~2u;
    if (cmd == 0xB0 || cmd == 0x0B) data &= (uint8_t)~4u;
    if (cmd == 0x70 || cmd == 0x07) data &= (uint8_t)~8u;
    return data;
}

/* Only the board check sends these: the seat is not riding. */
static inline bool seat_board_check(uint8_t cmd) {
    return cmd == 0x70 || cmd < 0x10;
}

/* Only a ride (or the coin before it) sends these: the seat is live from one
 * of them to the next board check. B0, E0 and D0 are in both, and 0xFF (all
 * lines off) is the port before the game's first write as well. */
static inline bool seat_board_ride(uint8_t cmd) {
    uint8_t hi = cmd >> 4;
    return cmd == 0x2F || cmd == 0x3F || (cmd != 0xFF && (hi == 0x9 || hi == 0xA || hi == 0xC || hi == 0xF));
}

/* How far the seat moves from `from` to `to`: the valve lines that change,
 * 0-4 a side. Bits 0x30 bank it one way, 0x40 and 0x80 the other (which is
 * left is a guess: the left 2-port test has 0x20 low, the right does not);
 * the rear's 0x0C count on both. The compressor's 0x03 are not a move. */
static inline void seat_board_motion(uint8_t from, uint8_t to, int *left, int *right) {
    uint8_t d = (uint8_t)(from ^ to);
    int rear = (d >> 2 & 1) + (d >> 3 & 1);
    *left  = (d >> 5 & 1) + (d >> 4 & 1) + rear;
    *right = (d >> 7 & 1) + (d >> 6 & 1) + rear;
}

#endif /* M2HLE_SEAT_BOARD_H */
