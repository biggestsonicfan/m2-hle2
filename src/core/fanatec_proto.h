/*
 * fanatec_proto.h — what a Fanatec wheel base is sent to light its rev LEDs
 * and its three-digit display (Pinboard #1361): the reports, built from a
 * game's dashboard (game_dash_t). No device here; ui/fanatec.h writes them.
 *
 * Read from Fanatec's own driver 474 (FanatecCpl_EndorFanatecSdk64_VS2019.dll,
 * the SDK, and FAWForceFeedbackDrv64, its DirectInput force) in Ghidra; the
 * whole of it, with the functions each fact came from, is FANATEC.md. In short:
 *
 *   - Output reports go to the base's first collection: 8 bytes, a report ID
 *     then 7 data bytes. The ID is 0x01 on a base with several top-level
 *     collections (fanatec_multi_tlc), 0x00 on the others. Reports that start
 *     0xFF are 64 bytes and go to the third collection (multi-TLC bases only).
 *   - Base rev LEDs (CSL Elite, ClubSport V2.5...): f8 13 m0 m1, LED j = bit j
 *     of m0, LED 8 = bit 0 of m1.
 *   - Rim rev LEDs: f8 09 08 hi lo, 9 bits, LED 0 the top bit (bit 8). A rim
 *     with one rev stripe reads the same bits as a colour unless its translator
 *     is on (f8 09 07 01), which then shows them as revs again.
 *   - RGB rims on a multi-TLC base: ff 01 00, then each LED's colour as a
 *     big-endian b<<11 | g<<5 | r (RGB565 with red low).
 *   - Display: f8 09 01 02 d0 d1 d2, one byte a digit, bit 0-6 = segments
 *     a-g, bit 7 the point; f8 09 01 18 02 takes it from the wheel's own menu,
 *     f8 09 01 18 01 gives it back.
 *
 * The driver sends only what changed; so does this.
 */
#ifndef M2HLE_FANATEC_PROTO_H
#define M2HLE_FANATEC_PROTO_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "game_profile.h"

#define FANATEC_VID      0x0EB7
#define FANATEC_REV_LEDS 9
#define FANATEC_REPORT   8      /* a command: ID + 7 bytes */
#define FANATEC_REPORT64 64     /* an 0xFF report */

/* A wheel base, not pedals, a shifter or a handbrake, which share the vendor
 * ID and take none of this. Every base PID known is below 0x1000: the SDK's
 * PID -> base type table (0001-0007, 0020-002F, 038E, 0E03-0E30, 0F00-0FFF;
 * FANATEC.md) and hid-fanatecff's (0001, 0004-0007, 0011 CSR Elite, 0020 CSL
 * DD / GT DD Pro / ClubSport DD, 0197 Porsche 911, 0E03). The pedals known are
 * 0x1800 and up (183B, 6204-6206); no shifter or handbrake PID is known here,
 * and one plugged into a base is not a USB device of its own (Pinboard #1416). */
static inline bool fanatec_is_base(uint16_t pid) {
    return pid >= 0x0001 && pid < 0x1000;
}

/* Several top-level collections: report ID 1, and the 0xFF reports. */
static inline bool fanatec_multi_tlc(uint16_t pid) {
    return (pid >= 0x0004 && pid <= 0x0007) || (pid >= 0x0020 && pid <= 0x002F) ||
           (pid >= 0x0E04 && pid <= 0x0E30) || (pid >= 0x0F10 && pid <= 0x0F1F) ||
           (pid >= 0x0F40 && pid <= 0x0FEF);
}

/* An 8-byte command: the ID, then up to 7 bytes. */
static inline void fanatec_cmd(uint8_t out[FANATEC_REPORT], bool multi, const uint8_t *b, int n) {
    memset(out, 0, FANATEC_REPORT);
    out[0] = multi ? 0x01 : 0x00;
    memcpy(out + 1, b, (size_t)(n > 7 ? 7 : n));
}

/* ---- Rev LEDs ------------------------------------------------------------- */

/* Which of the 9 LEDs are lit, bit j = LED j from the left: none below rpm_lo,
 * all at rpm_max, and all flashing past it (`blink` toggles a few times a
 * second; the caller's clock). */
static inline uint16_t fanatec_rev_mask(const game_dash_t *d, bool blink) {
    if (!d->on || d->rpm <= 0 || d->rpm_max <= d->rpm_lo || d->rpm < d->rpm_lo) return 0;
    if (d->rpm >= d->rpm_max) return blink ? 0 : 0x1FF;
    int n = 1 + (d->rpm - d->rpm_lo) * FANATEC_REV_LEDS / (d->rpm_max - d->rpm_lo);
    if (n > FANATEC_REV_LEDS) n = FANATEC_REV_LEDS;
    return (uint16_t)((1u << n) - 1u);
}

static inline void fanatec_base_revs(uint8_t out[FANATEC_REPORT], bool multi, uint16_t mask) {
    const uint8_t b[] = { 0xF8, 0x13, (uint8_t)mask, (uint8_t)(mask >> 8 & 1) };
    fanatec_cmd(out, multi, b, 4);
}

/* The rim's bits run the other way: LED 0 is bit 8. */
static inline void fanatec_rim_revs(uint8_t out[FANATEC_REPORT], bool multi, uint16_t mask) {
    uint16_t v = 0;
    for (int j = 0; j < FANATEC_REV_LEDS; j++) if (mask >> j & 1) v |= (uint16_t)(1u << (8 - j));
    const uint8_t b[] = { 0xF8, 0x09, 0x08, (uint8_t)(v >> 8), (uint8_t)v };
    fanatec_cmd(out, multi, b, 5);
}

static inline void fanatec_rev_translator(uint8_t out[FANATEC_REPORT], bool multi, bool on) {
    const uint8_t b[] = { 0xF8, 0x09, 0x07, (uint8_t)on };
    fanatec_cmd(out, multi, b, 4);
}

/* An RGB rim: the lit LEDs green, then red, then blue from the left, as a
 * shift light goes; the rest dark. */
static inline void fanatec_rgb_revs(uint8_t out[FANATEC_REPORT64], uint16_t mask) {
    memset(out, 0, FANATEC_REPORT64);
    out[0] = 0xFF; out[1] = 0x01; out[2] = 0x00;
    for (int j = 0; j < FANATEC_REV_LEDS; j++) {
        uint16_t c = 0;
        if (mask >> j & 1) c = j < 3 ? (uint16_t)(63u << 5) : j < 6 ? 31u : (uint16_t)(31u << 11);
        out[3 + 2 * j] = (uint8_t)(c >> 8);
        out[4 + 2 * j] = (uint8_t)c;
    }
}

/* ---- Display -------------------------------------------------------------- */

/* A character's segments, as the SDK's font draws it; 0 (blank) for one it
 * has not. '-' is the middle bar (the SDK has none). */
static inline uint8_t fanatec_seg(char c) {
    static const uint8_t digits[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    static const uint8_t letters[26] = {
        0x77, 0x7C, 0x58, 0x5E, 0x79, 0x71, 0x7D, 0x76, 0x06, 0x06, 0x75, 0x38, 0x37,
        0x54, 0x5C, 0x73, 0x67, 0x50, 0x6D, 0x78, 0x3E, 0x18, 0x7E, 0x76, 0x6E, 0x5B,
    };
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c == '-') return 0x40;
    if (c == '_') return 0x08;
    return 0;
}

/* What the display shows: the gear in the middle ("N" neutral, "R" reverse),
 * or the speed when `speed` is asked for or the game has no gears. Three
 * characters, blank when there is nothing to show. */
static inline void fanatec_dash_text(const game_dash_t *d, bool speed, char text[4]) {
    memcpy(text, "   ", 4);
    if (!d->on) return;
    if (speed || d->gear < 0) {
        if (d->speed < 0) return;
        int v = d->speed > 999 ? 999 : d->speed;
        for (int i = 2; i >= 0; i--) { text[i] = (char)('0' + v % 10); v /= 10; if (!v) break; }
        return;
    }
    text[1] = d->gear == 0 ? 'N' : d->gear < 10 ? (char)('0' + d->gear) : '-';
}

static inline void fanatec_display(uint8_t out[FANATEC_REPORT], bool multi, const char text[3]) {
    const uint8_t b[] = { 0xF8, 0x09, 0x01, 0x02, fanatec_seg(text[0]), fanatec_seg(text[1]), fanatec_seg(text[2]) };
    fanatec_cmd(out, multi, b, 7);
}

/* Take the display from the wheel's own menu (true) or give it back. */
static inline void fanatec_display_mode(uint8_t out[FANATEC_REPORT], bool multi, bool ours) {
    const uint8_t b[] = { 0xF8, 0x09, 0x01, 0x18, (uint8_t)(ours ? 0x02 : 0x01) };
    fanatec_cmd(out, multi, b, 5);
}

#endif /* M2HLE_FANATEC_PROTO_H */
