/*
 * mdfn_scsp.cpp -- compiles Mednafen's src/ss/scsp.inc on its own.
 *
 * Mednafen is GPL-2.0-or-later and is NOT in this repository: fetch.sh
 * downloads the release this was written against, and build.sh compiles it
 * into a local test binary that is not distributed. This file only supplies
 * the few Mednafen types and helpers scsp.inc leans on, written here from
 * what they do, plus the two interrupt callbacks the Saturn wires it to.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

typedef int8_t int8;   typedef uint8_t uint8;
typedef int16_t int16; typedef uint16_t uint16;
typedef int32_t int32; typedef uint32_t uint32;
typedef int64_t int64; typedef uint64_t uint64;

#define INLINE inline __attribute__((always_inline))
#define NO_INLINE __attribute__((noinline))
#define MDFN_COLD
#define MDFN_LIKELY(x) __builtin_expect(!!(x), 1)
#define MDFN_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define sign_x_to_s32(n, v) ((int32)((uint32)(v) << (32 - (n))) >> (32 - (n)))

/* Big-endian byte addressing into an array of host-order 16-bit words, as
 * the 68000 sees sound RAM and the registers. */
template<typename T, bool IsWrite>
static INLINE void ne16_rwbo_be(uint16 *base, uint32 A, T *v) {
    if (sizeof(T) == 1) {
        uint16 *w = &base[A >> 1];
        unsigned sh = (A & 1) ? 0 : 8;
        if (IsWrite) *w = (uint16)((*w & ~(0xFF << sh)) | ((uint16)(uint8)*v << sh));
        else *v = (T)(uint8)(*w >> sh);
    } else {
        if (IsWrite) base[A >> 1] = (uint16)*v; else *v = (T)base[A >> 1];
    }
}
template<typename T>
static INLINE T ne16_rbo_be(const uint16 *base, uint32 A) {
    T v; ne16_rwbo_be<T, false>((uint16 *)base, A, &v); return v;
}

/* The same, into 64-bit words (the DSP microprogram). */
template<typename T, bool IsWrite>
static INLINE void ne64_rwbo_be(uint64 *base, uint32 A, T *v) {
    uint64 *w = &base[A >> 3];
    unsigned sh = (unsigned)(8 - sizeof(T) - (A & 7)) * 8;
    uint64 mask = (sizeof(T) == 1 ? 0xFFull : 0xFFFFull) << sh;
    if (IsWrite) *w = (*w & ~mask) | ((uint64)*v << sh & mask);
    else *v = (T)((*w & mask) >> sh);
}
static INLINE unsigned MDFN_tzcount16(uint16 v) { return v ? (unsigned)__builtin_ctz(v) : 16; }
static INLINE unsigned MDFN_lzcount32(uint32 v) { return v ? (unsigned)__builtin_clz(v) : 32; }

/* Save states and the debugger's register view are not used. */
struct StateMem;
typedef int SFORMAT;
#define SFVAR(...) 0
#define SFVARN(...) 0
#define SFPTR16(...) 0
#define SFEND 0
#define MDAP(x) (&(x)[0][0])
static void MDFNSS_StateAction(StateMem *, unsigned, bool, SFORMAT *, const char *) {}
#define SS_DBG(...) do {} while (0)
enum { SS_DBG_ERROR = 1, SS_DBG_WARNING = 2, SS_DBG_SCSP = 4, SS_DBG_SCSP_REGW = 8,
       SS_DBG_SCSP_MOBUF = 16 };
#define trio_snprintf snprintf

/* The harness reads a little private state (the MIDI input count). */
#define private public
#include "scsp.h"
#undef private

static int g_ipl;
static void SCSP_SoundIntChanged(SS_SCSP *, unsigned level) { g_ipl = (int)level; }
static void SCSP_MainIntChanged(SS_SCSP *, bool) {}

#include "scsp.inc"

static SS_SCSP *chip;

extern "C" {
void mdfn_scsp_reset(void) {
    if (!chip) chip = new SS_SCSP();
    g_ipl = 0;
    chip->Reset(true);
}
uint16_t *mdfn_scsp_ram(void) { return chip->GetRAMPtr(); }
void mdfn_scsp_write(uint32_t addr, uint32_t v, int size) {
    if (size == 1) { uint8 b = (uint8)v; chip->RW<uint8, true>(addr, b); }
    else { uint16 w = (uint16)v; chip->RW<uint16, true>(addr & ~1u, w); }
}
uint32_t mdfn_scsp_read(uint32_t addr, int size) {
    if (size == 1) { uint8 b = 0; chip->RW<uint8, false>(addr, b); return b; }
    uint16 w = 0; chip->RW<uint16, false>(addr & ~1u, w); return w;
}
int mdfn_scsp_midi_in(uint8_t b) { int full = chip->MIDI.InputCount == 4; chip->WriteMIDI(b); return !full; }
void mdfn_scsp_sample(int16_t *l, int16_t *r) {
    int16 o[2];
    chip->RunSample<int16>(o);
    *l = o[0]; *r = o[1];
}
int mdfn_scsp_irq_level(void) { return g_ipl; }
}
