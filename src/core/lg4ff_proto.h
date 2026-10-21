/*
 * lg4ff_proto.h — what a Logitech G25 (and its kin: the Driving Force Pro,
 * Driving Force GT and G27) is sent to leave its compatibility mode, to set
 * its turning range and to play a force (Pinboard #1370): the reports, from
 * the game's force (ffb_out_t). No device here; ui/lg4ff.h writes them.
 *
 * Read from Logitech Gaming Software 5.10.127, the last that drives a G25
 * (G Hub does not), in Ghidra and rea: wmfilter.sys (the HID filter that
 * switches the mode), wmxlcore.sys (the range) and wmjoyfrc.dll (the
 * DirectInput force). The whole of it, with the functions each fact came
 * from, is LOGITECH.md. In short:
 *
 *   - Every command is an 8-byte output report: report ID 0, then 7 bytes.
 *     The driver writes them as any program can (WriteFile on the HID handle).
 *   - Out of the box the wheel says it is a Driving Force EX (PID C294); the
 *     revision (bcdDevice) tells which wheel it is. f8 10 makes a G25 itself
 *     (PID C299); a G27 or a DFGT takes f8 0a then f8 09 04 01 / 03 01; a DFP
 *     f8 01. The wheel drops off the bus and comes back as the new PID.
 *   - Range: f8 81 lo hi, in degrees (G25, DFGT, G27; at most 900).
 *   - Constant force, slot 1: 11 08 f 80, f = 0x80 + the force (-127..127);
 *     0x80 is none, sent as 13 00 (slot 1 stopped).
 *   - The wheel's own centring spring: f5 off; fe 0d a a b, then 14 on.
 *   - Friction, slot 4: 81 0e k k clip, then 8c to change it, 83 to stop.
 *     Not in Logitech's DLL (it plays friction on the host); from the Linux
 *     and SDL driver (new-lg4ff), which drive these wheels.
 */
#ifndef M2HLE_LG4FF_PROTO_H
#define M2HLE_LG4FF_PROTO_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LOGITECH_VID   0x046D
#define LG4FF_REPORT   8        /* report ID 0 + 7 bytes */
#define LG4FF_RANGE_MIN 40
#define LG4FF_RANGE_MAX 900

typedef enum { LG4FF_NONE, LG4FF_DFP, LG4FF_G25, LG4FF_DFGT, LG4FF_G27 } lg4ff_model_t;

typedef struct {
    lg4ff_model_t model;    /* the wheel it really is */
    bool          compat;   /* still under another PID: it wants the switch */
} lg4ff_id_t;

static inline const char *lg4ff_name(lg4ff_model_t m) {
    switch (m) {
        case LG4FF_DFP:  return "Driving Force Pro";
        case LG4FF_G25:  return "G25";
        case LG4FF_DFGT: return "Driving Force GT";
        case LG4FF_G27:  return "G27";
        default:         return "wheel";
    }
}

/* wmfilter.sys's table: the PID and the revision. A G27 is rev 123x under any
 * of its PIDs; a G25 12xx; a DFGT 13xx; a DFP 1xxx otherwise. A Driving Force
 * EX that is only that (rev 2xxx), and every other PID, is none of them. */
static inline lg4ff_id_t lg4ff_identify(uint16_t pid, uint16_t rev) {
    bool g27 = (rev & 0xFFF0) == 0x1230;
    switch (pid) {
        case 0xC29B: return (lg4ff_id_t){ LG4FF_G27, false };
        case 0xC29A: return (lg4ff_id_t){ LG4FF_DFGT, false };
        case 0xC299: return (lg4ff_id_t){ g27 ? LG4FF_G27 : LG4FF_G25, g27 };
        case 0xC298:
            if (g27) return (lg4ff_id_t){ LG4FF_G27, true };
            if ((rev & 0xFF00) == 0x1200) return (lg4ff_id_t){ LG4FF_G25, true };
            return (lg4ff_id_t){ LG4FF_DFP, false };
        case 0xC294:
            if (g27) return (lg4ff_id_t){ LG4FF_G27, true };
            if ((rev & 0xFF00) == 0x1200) return (lg4ff_id_t){ LG4FF_G25, true };
            if ((rev & 0xFF00) == 0x1300) return (lg4ff_id_t){ LG4FF_DFGT, true };
            if ((rev & 0xF000) == 0x1000) return (lg4ff_id_t){ LG4FF_DFP, true };
            return (lg4ff_id_t){ LG4FF_NONE, false };
        default: return (lg4ff_id_t){ LG4FF_NONE, false };
    }
}

/* An 8-byte command: ID 0, then up to 7 bytes. */
static inline void lg4ff_cmd(uint8_t out[LG4FF_REPORT], const uint8_t *b, int n) {
    memset(out, 0, LG4FF_REPORT);
    memcpy(out + 1, b, (size_t)(n > 7 ? 7 : n));
}

/* The commands that make a compat-mode wheel its own model: how many (0-2). */
static inline int lg4ff_mode_switch(uint8_t out[2][LG4FF_REPORT], lg4ff_model_t m) {
    static const uint8_t ext[] = { 0xF8, 0x0A };
    switch (m) {
        case LG4FF_G25: { const uint8_t b[] = { 0xF8, 0x10 }; lg4ff_cmd(out[0], b, 2); return 1; }
        case LG4FF_DFP: { const uint8_t b[] = { 0xF8, 0x01 }; lg4ff_cmd(out[0], b, 2); return 1; }
        case LG4FF_G27: { const uint8_t b[] = { 0xF8, 0x09, 0x04, 0x01 };
                          lg4ff_cmd(out[0], ext, 2); lg4ff_cmd(out[1], b, 4); return 2; }
        case LG4FF_DFGT: { const uint8_t b[] = { 0xF8, 0x09, 0x03, 0x01 };
                           lg4ff_cmd(out[0], ext, 2); lg4ff_cmd(out[1], b, 4); return 2; }
        default: return 0;
    }
}

/* The wheels that take f8 81 (wmxlcore.sys sends it to the DFP's successors
 * only; the DFP's own range is two other commands, not sent here). */
static inline bool lg4ff_has_range(lg4ff_model_t m) {
    return m == LG4FF_G25 || m == LG4FF_DFGT || m == LG4FF_G27;
}

static inline int lg4ff_clamp_range(int deg) {
    return deg < LG4FF_RANGE_MIN ? LG4FF_RANGE_MIN : deg > LG4FF_RANGE_MAX ? LG4FF_RANGE_MAX : deg;
}

static inline void lg4ff_range(uint8_t out[LG4FF_REPORT], int deg) {
    deg = lg4ff_clamp_range(deg);
    const uint8_t b[] = { 0xF8, 0x81, (uint8_t)deg, (uint8_t)(deg >> 8) };
    lg4ff_cmd(out, b, 4);
}

/* The constant force's byte for a push (ffb_out_t.push, -0x7FFF..0x7FFF, +
 * right): 0x80 + -127..127, above 0x80 to the right as SDL's lg4ff haptics
 * have it. */
static inline uint8_t lg4ff_force_byte(int push) {
    if (push > 0x7FFF) push = 0x7FFF;
    if (push < -0x7FFF) push = -0x7FFF;
    return (uint8_t)(0x80 + push * 127 / 0x7FFF);
}

/* Slot 1's constant force, or slot 1 stopped when there is none. */
static inline void lg4ff_constant(uint8_t out[LG4FF_REPORT], int push) {
    uint8_t f = lg4ff_force_byte(push);
    if (f == 0x80) { const uint8_t b[] = { 0x13, 0x00 }; lg4ff_cmd(out, b, 2); return; }
    const uint8_t b[] = { 0x11, 0x08, f, 0x80 };
    lg4ff_cmd(out, b, 4);
}

/* The wheel's centring spring at `k` (ffb_out_t.spring, 0-0x7FFF): f5 (off)
 * alone for none, else its strength and 14 (on). How many reports (1 or 3).
 * The strength curve is Linux's (hid-lg4ff), halved for every wheel but the
 * MOMO, as it and SDL send it. */
static inline int lg4ff_autocenter(uint8_t out[3][LG4FF_REPORT], int k) {
    const uint8_t off[] = { 0xF5 };
    lg4ff_cmd(out[0], off, 1);
    if (k <= 0) return 1;
    uint32_t m = (uint32_t)(k > 0x7FFF ? 0x7FFF : k) * 2u;   /* 0-0xFFFE */
    uint32_t a = m <= 0xAAAA ? 0x0C * m : 0x0C * 0xAAAA + 0x06 * (m - 0xAAAA);
    uint32_t b = m <= 0xAAAA ? 0x80 * m : 0x80 * 0xAAAA + 0xFF * (m - 0xAAAA);
    a >>= 1;
    const uint8_t s[] = { 0xFE, 0x0D, (uint8_t)(a / 0xAAAA), (uint8_t)(a / 0xAAAA), (uint8_t)(b / 0xAAAA) };
    lg4ff_cmd(out[1], s, 5);
    const uint8_t on[] = { 0x14 };
    lg4ff_cmd(out[2], on, 1);
    return 3;
}

/* Slot 4's friction at `k` (ffb_out_t.friction, 0-0x7FFF), the same both
 * ways, saturating at full strength. `playing`: the slot already runs (8c
 * changes it; 81 starts it). None stops it (83). */
static inline void lg4ff_friction(uint8_t out[LG4FF_REPORT], int k, bool playing) {
    if (k <= 0) { const uint8_t b[] = { 0x83 }; lg4ff_cmd(out, b, 1); return; }
    uint32_t c = (uint32_t)(k > 0x7FFF ? 0x7FFF : k) * 2u;
    if (c > 0xFFFF) c = 0xFFFF;
    const uint8_t b[] = { (uint8_t)(playing ? 0x8C : 0x81), 0x0E, (uint8_t)(c >> 8), (uint8_t)(c >> 8), 0xFF, 0x00 };
    lg4ff_cmd(out, b, 6);
}

#endif /* M2HLE_LG4FF_PROTO_H */
