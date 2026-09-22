/*
 * scsp.h — Yamaha YMF292 "SCSP" (Saturn Custom Sound Processor), the sound
 * chip on every Model 2 board.
 *
 * The chip runs one output sample at a time, in lockstep with the 68000 that
 * drives it (sound.h runs 256 68000 clock cycles per sample, 44.1 kHz). That is
 * not a nicety. The sound driver reads the chip back and decides what to do
 * from what it sees:
 *   - the slot monitor (0x408): MSLC selects a slot, and CA — the play
 *     position in 4096-sample steps — tells the driver's sample streaming when
 *     to copy the next 8 KB of a long sample into sound RAM, and when a voice
 *     can be taken back;
 *   - KYONB (slot word 0 bit 11) clears itself when a voice finishes releasing
 *     or a one-shot sample runs out;
 *   - the interrupt-pending register, the timers and the MIDI input buffer are
 *     the driver's clock and its command channel;
 *   - the DSP's delay line lives in sound RAM (RBP), which the 68000 shares.
 * A mixer that plays key-ons on the audio thread cannot answer any of those at
 * the moment the driver asks, and the driver's voice allocation drifts away
 * from the board's (tools/mame/snd-capture.lua measures it).
 *
 * Behaviour follows MAME's scsp.cpp / scspdsp.cpp, which is the oracle the
 * capture tools record. Where that is a MAME choice rather than known hardware
 * it is marked "MAME:" so it can be revisited against a real board.
 *
 * Registers (16-bit, big-endian, offsets from the chip base):
 *   0x000-0x3FF  32 slots x 0x20
 *   0x400-0x42F  common: MVOL, RBL/RBP, MIDI in/out, monitor, DMA, timers,
 *                interrupts (SCIEB/SCIPD/SCIRE/SCILV0-2), MCIEB/MCIPD/MCIRE
 *   0x600-0x6FF  sound stack (the last output of each slot, for FM)
 *   0x700-0x77F  DSP COEF, 0x780-0x7FF MADRS (mirrored), 0x800-0xBFF MPRO,
 *   0xC00-0xEFF  DSP TEMP / MEMS / MIXS / EFREG read-back
 */
#ifndef SCSP_H
#define SCSP_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define SCSP_SHIFT     12        /* play position: 20.12 samples */
#define SCSP_EG_SHIFT  16
#define SCSP_LFO_SHIFT 8

enum { SCSP_ATTACK, SCSP_DECAY1, SCSP_DECAY2, SCSP_RELEASE };

/* interrupt sources: bit numbers in SCIEB / SCIPD / SCIRE / SCILVn */
#define SCSP_INT_MIDI_IN  3
#define SCSP_INT_DMA      4
#define SCSP_INT_TIMER_A  6
#define SCSP_INT_TIMER_B  7      /* timer C shares timer B's SCILV bit */

typedef struct {
    uint16_t       phase;
    uint32_t       step;
    const int32_t *table;
    const int32_t *scale;
} scsp_lfo_t;

typedef struct {
    uint16_t   r[16];            /* register words 0x00..0x1E */
    uint8_t    active, backwards, state;
    uint32_t   cur, nxt, step;   /* play position and pitch, SCSP_SHIFT fixed */
    int32_t    vol;              /* envelope, 0 (silent) .. 0x3FF << SCSP_EG_SHIFT */
    int32_t    ar, d1r, d2r, rr, dl;
    int        eghold;
    scsp_lfo_t plfo, alfo;
} scsp_slot_t;

/* One MPRO step, decoded into what scsp_dsp_step does with it. The program
 * runs last_step of these every sample, and unpacking them from the four
 * microcode words each time was a quarter of the whole emulator thread; they
 * are a pure function of mpro[] and the step's parity, so they are decoded once
 * per program change instead (scsp_dsp_decode).
 *
 * The fields are the choices the step makes, not the microcode's bits. On an
 * in-order core (the handhelds' Cortex-A55) a branch or a byte load per field
 * was most of the DSP's time, so the yes/no choices sit in one word, f, and the
 * step selects between values it has already computed (SCSP_SEL). Writes a
 * step does not make go to a slot nobody reads. */
enum {
    SCSP_DF_IW_SELF,             /* INPUTS becomes MEMVAL (IWT with IRA == IWA) */
    SCSP_DF_TWT, SCSP_DF_XSEL, SCSP_DF_BSEL, SCSP_DF_ZERO, SCSP_DF_NEGB, SCSP_DF_YRL,
    SCSP_DF_Y_COEF, SCSP_DF_Y_YREG,     /* Y from COEF / from YREG; FRC otherwise */
    SCSP_DF_SAT,                 /* the shifter saturates (SHIFT 0, 1) rather than wraps */
    SCSP_DF_FRCL, SCSP_DF_ADRL,
    SCSP_DF_ADRL_SH,             /* ADRS from SHIFTED (SHIFT 3) rather than from INPUTS */
    SCSP_DF_TABLE, SCSP_DF_ADREB
};
#define SCSP_DM(f, bit) ((int32_t)((uint32_t)(f) << (31 - (bit))) >> 31)   /* flag -> 0 / ~0 */

typedef struct {
    uint32_t f;                  /* SCSP_DF_* */
    uint8_t  ira;                /* index into the step's input bank: MEMS, MIXS, EXTS */
    uint8_t  tra, twa, coef, masa;
    uint8_t  iwa;                /* 0x3F (a bank slot nothing reads) when the step writes no MEMS */
    uint8_t  ewa;                /* 16 (a spare slot) when the step writes no EFREG */
    uint8_t  dbl;                /* the shifter doubles ACC (SHIFT 1, 2) */
    uint8_t  ysh, frc_sh;        /* Y = sext13((src >> ysh) & ymask); FRC = (SHIFTED >> frc_sh) & frc_mask */
    uint16_t ymask, frc_mask;
    uint8_t  mem, mrd, mwt;      /* delay memory this step: only odd steps touch it (MAME) */
    uint8_t  nofl, nxadr;
} scsp_dsp_op_t;

typedef struct {
    int16_t  coef[64];
    uint16_t madrs[32];
    uint16_t mpro[128 * 4];
    int32_t  temp[128];
    int32_t  mems[32];
    int32_t  mixs[16];
    int16_t  efreg[16];
    int16_t  exts[2];
    uint32_t dec, rbp, rbl;
    int      stopped, last_step;
    scsp_dsp_op_t ops[128];      /* mpro[] decoded; valid while ops_ok */
    int      ops_ok;             /* cleared by every mpro write and by reset */
    int      ops_last;           /* the last_step ops_run was worked out for */
    int      ops_run;            /* steps before one whose IRA reads nothing (the program ends there) */
} scsp_dsp_t;

typedef struct {
    scsp_slot_t slot[32];
    uint16_t    c[0x18];         /* common registers 0x400..0x42E, as words */
    int16_t     sous[128];       /* sound stack; slots write [ptr & 63] in turn */
    uint8_t     sous_ptr;
    uint8_t     mi[32], mi_r, mi_w;
    uint32_t    mi_drops;        /* MIDI bytes lost to a full input ring (see scsp_midi_in) */
    uint8_t     mi_hi;           /* high-water fill of that ring, to see how close it gets */
    uint8_t     mo[32], mo_r, mo_w;
    uint16_t    tim_cnt[3];      /* MAME: 0xFFFF once expired, reload << 8 after a write */
    uint64_t    tim_due[3];      /* expiry, in clock periods of *clock; 0 = not running */
    uint64_t    tim_next;        /* the earliest tim_due, UINT64_MAX when none */
    const uint64_t *clock;       /* the driving CPU's clock-period count (256 per sample) */
    uint8_t     lvl_ta, lvl_tbc, lvl_midi;
    uint8_t     lines;           /* 68000 interrupt lines held asserted, bit n = level n */
    uint16_t    mcieb, mcipd;
    scsp_dsp_t  dsp;
    uint8_t    *ram;             /* sound RAM, shared with the 68000 (big-endian) */
    uint32_t    ram_size;
    uint32_t    noise;
    uint64_t    samples;         /* output samples produced since reset */
} scsp_t;

/* ---- tables --------------------------------------------------------------- */

static int32_t scsp_eg_table[0x400];
static int32_t scsp_lpan[0x10000], scsp_rpan[0x10000];
static int32_t scsp_ar_table[64], scsp_dr_table[64];
static int32_t scsp_alfo_saw[256], scsp_alfo_sqr[256], scsp_alfo_tri[256], scsp_alfo_noi[256];
static int32_t scsp_plfo_saw[256], scsp_plfo_sqr[256], scsp_plfo_tri[256], scsp_plfo_noi[256];
static int32_t scsp_pscale[8][256], scsp_ascale[8][256];
static int     scsp_tables_ready;

/* Envelope times in ms for rates 0..63 (the SCSP manual's table). */
static const double SCSP_AR_MS[64] = {
    100000, 100000, 8100.0, 6900.0, 6000.0, 4800.0, 4000.0, 3400.0, 3000.0, 2400.0, 2000.0, 1700.0, 1500.0,
    1200.0, 1000.0, 860.0, 760.0, 600.0, 500.0, 430.0, 380.0, 300.0, 250.0, 220.0, 190.0, 150.0, 130.0, 110.0, 95.0,
    76.0, 63.0, 55.0, 47.0, 38.0, 31.0, 27.0, 24.0, 19.0, 15.0, 13.0, 12.0, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4, 3.0, 2.4,
    2.0, 1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0.0, 0.0 };
static const double SCSP_DR_MS[64] = {
    100000, 100000, 118200.0, 101300.0, 88600.0, 70900.0, 59100.0, 50700.0, 44300.0, 35500.0, 29600.0, 25300.0, 22200.0, 17700.0,
    14800.0, 12700.0, 11100.0, 8900.0, 7400.0, 6300.0, 5500.0, 4400.0, 3700.0, 3200.0, 2800.0, 2200.0, 1800.0, 1600.0, 1400.0, 1100.0,
    920.0, 790.0, 690.0, 550.0, 460.0, 390.0, 340.0, 270.0, 230.0, 200.0, 170.0, 140.0, 110.0, 98.0, 85.0, 68.0, 57.0, 49.0, 43.0, 34.0,
    28.0, 25.0, 22.0, 18.0, 14.0, 12.0, 11.0, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1 };
static const float SCSP_LFO_HZ[32] = {
    0.17f, 0.19f, 0.23f, 0.27f, 0.34f, 0.39f, 0.45f, 0.55f, 0.68f, 0.78f, 0.92f, 1.10f, 1.39f, 1.60f, 1.87f, 2.27f,
    2.87f, 3.31f, 3.92f, 4.79f, 6.15f, 7.18f, 8.60f, 10.8f, 14.4f, 17.2f, 21.5f, 28.7f, 43.1f, 57.4f, 86.1f, 172.3f };
static const float SCSP_ASCALE_DB[8]   = { 0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f };
static const float SCSP_PSCALE_CENT[8] = { 0.0f, 7.0f, 13.5f, 27.0f, 55.0f, 112.0f, 230.0f, 494.0f };
static const float SCSP_SDL_DB[8]      = { -1000000.0f, -36.0f, -30.0f, -24.0f, -18.0f, -12.0f, -6.0f, 0.0f };

static inline uint32_t scsp_lcg(uint32_t *s) { *s = *s * 1103515245u + 12345u; return *s >> 8; }

static void scsp_tables_init(void) {
    if (scsp_tables_ready) return;
    for (int i = 0; i < 0x400; i++) {
        float db = (float)(3 * (i - 0x3FF)) / 32.0f;
        scsp_eg_table[i] = (int32_t)(powf(10.0f, db / 20.0f) * (float)(1 << SCSP_SHIFT));
    }
    /* level/pan: index = TL | PAN << 8 | SDL << 13 */
    for (int i = 0; i < 0x10000; i++) {
        int itl = i & 0xFF, ipan = (i >> 8) & 0x1F, isdl = (i >> 13) & 7;
        float db = 0.0f;
        if (itl & 0x01) db -= 0.4f;
        if (itl & 0x02) db -= 0.8f;
        if (itl & 0x04) db -= 1.5f;
        if (itl & 0x08) db -= 3.0f;
        if (itl & 0x10) db -= 6.0f;
        if (itl & 0x20) db -= 12.0f;
        if (itl & 0x40) db -= 24.0f;
        if (itl & 0x80) db -= 48.0f;
        float tl = powf(10.0f, db / 20.0f);
        db = 0.0f;
        if (ipan & 0x1) db -= 3.0f;
        if (ipan & 0x2) db -= 6.0f;
        if (ipan & 0x4) db -= 12.0f;
        if (ipan & 0x8) db -= 24.0f;
        float pan = ((ipan & 0xF) == 0xF) ? 0.0f : powf(10.0f, db / 20.0f);
        float lpan = ipan < 0x10 ? pan : 1.0f;
        float rpan = ipan < 0x10 ? 1.0f : pan;
        float sdl = isdl ? powf(10.0f, SCSP_SDL_DB[isdl] / 20.0f) : 0.0f;
        scsp_lpan[i] = (int32_t)(uint32_t)((float)(1 << SCSP_SHIFT) * (4.0f * lpan * tl * sdl));
        scsp_rpan[i] = (int32_t)(uint32_t)((float)(1 << SCSP_SHIFT) * (4.0f * rpan * tl * sdl));
    }
    scsp_ar_table[0] = scsp_dr_table[0] = 0;
    scsp_ar_table[1] = scsp_dr_table[1] = 0;
    for (int i = 2; i < 64; i++) {
        double scale = (double)(1 << SCSP_EG_SHIFT);
        scsp_ar_table[i] = SCSP_AR_MS[i] != 0.0
            ? (int32_t)((1023 * 1000.0) / (44100.0 * SCSP_AR_MS[i]) * scale)
            : (int32_t)(1024 << SCSP_EG_SHIFT);
        scsp_dr_table[i] = (int32_t)((1023 * 1000.0) / (44100.0 * SCSP_DR_MS[i]) * scale);
    }
    uint32_t seed = 0x5C5Bu;
    for (int i = 0; i < 256; i++) {
        scsp_alfo_saw[i] = 255 - i;
        scsp_plfo_saw[i] = i < 128 ? i : i - 256;
        scsp_alfo_sqr[i] = i < 128 ? 255 : 0;
        scsp_plfo_sqr[i] = i < 128 ? 127 : -128;
        scsp_alfo_tri[i] = i < 128 ? 255 - i * 2 : i * 2 - 256;
        scsp_plfo_tri[i] = i < 64 ? i * 2 : i < 128 ? 255 - i * 2 : i < 192 ? 256 - i * 2 : i * 2 - 511;
        int a = (int)(scsp_lcg(&seed) & 0xFF);     /* MAME: machine().rand() */
        scsp_alfo_noi[i] = a;
        scsp_plfo_noi[i] = 128 - a;
    }
    for (int s = 0; s < 8; s++) {
        for (int i = -128; i < 128; i++) {
            float cents = SCSP_PSCALE_CENT[s] * (float)i / 128.0f;
            scsp_pscale[s][i + 128] = (int32_t)(uint32_t)((float)(1 << SCSP_LFO_SHIFT) * powf(2.0f, cents / 1200.0f));
        }
        for (int i = 0; i < 256; i++) {
            float db = -SCSP_ASCALE_DB[s] * (float)i / 256.0f;
            scsp_ascale[s][i] = (int32_t)(uint32_t)((float)(1 << SCSP_LFO_SHIFT) * powf(10.0f, db / 20.0f));
        }
    }
    scsp_tables_ready = 1;
}

/* ---- slot register fields ---------------------------------------------------- */

#define SCSP_KYONEX(sl) ((sl)->r[0x0] & 0x1000)
#define SCSP_KYONB(sl)  ((sl)->r[0x0] & 0x0800)
#define SCSP_SBCTL(sl)  (((sl)->r[0x0] >> 9) & 3)
#define SCSP_SSCTL(sl)  (((sl)->r[0x0] >> 7) & 3)
#define SCSP_LPCTL(sl)  (((sl)->r[0x0] >> 5) & 3)
#define SCSP_PCM8B(sl)  ((sl)->r[0x0] & 0x0010)
#define SCSP_SA(sl)     ((((uint32_t)(sl)->r[0x0] & 0xF) << 16) | (sl)->r[0x1])
#define SCSP_LSA(sl)    ((sl)->r[0x2])
#define SCSP_LEA(sl)    ((sl)->r[0x3])
#define SCSP_D2R(sl)    (((sl)->r[0x4] >> 11) & 0x1F)
#define SCSP_D1R(sl)    (((sl)->r[0x4] >> 6) & 0x1F)
#define SCSP_EGHOLD(sl) ((sl)->r[0x4] & 0x0020)
#define SCSP_AR(sl)     ((sl)->r[0x4] & 0x1F)
#define SCSP_LPSLNK(sl) ((sl)->r[0x5] & 0x4000)
#define SCSP_KRS(sl)    (((sl)->r[0x5] >> 10) & 0xF)
#define SCSP_DL(sl)     (((sl)->r[0x5] >> 5) & 0x1F)
#define SCSP_RR(sl)     ((sl)->r[0x5] & 0x1F)
#define SCSP_STWINH(sl) ((sl)->r[0x6] & 0x0200)
#define SCSP_SDIR(sl)   ((sl)->r[0x6] & 0x0100)
#define SCSP_TL(sl)     ((sl)->r[0x6] & 0xFF)
#define SCSP_MDL(sl)    (((sl)->r[0x7] >> 12) & 0xF)
#define SCSP_MDXSL(sl)  (((sl)->r[0x7] >> 6) & 0x3F)
#define SCSP_MDYSL(sl)  ((sl)->r[0x7] & 0x3F)
#define SCSP_OCT(sl)    (((sl)->r[0x8] >> 11) & 0xF)
#define SCSP_FNS(sl)    ((sl)->r[0x8] & 0x3FF)
#define SCSP_LFOF(sl)   (((sl)->r[0x9] >> 10) & 0x1F)
#define SCSP_PLFOWS(sl) (((sl)->r[0x9] >> 8) & 3)
#define SCSP_PLFOS(sl)  (((sl)->r[0x9] >> 5) & 7)
#define SCSP_ALFOWS(sl) (((sl)->r[0x9] >> 3) & 3)
#define SCSP_ALFOS(sl)  ((sl)->r[0x9] & 7)
#define SCSP_ISEL(sl)   (((sl)->r[0xA] >> 3) & 0xF)
#define SCSP_IMXL(sl)   ((sl)->r[0xA] & 7)
#define SCSP_DISDL(sl)  (((sl)->r[0xB] >> 13) & 7)
#define SCSP_DIPAN(sl)  (((sl)->r[0xB] >> 8) & 0x1F)
#define SCSP_EFSDL(sl)  (((sl)->r[0xB] >> 5) & 7)
#define SCSP_EFPAN(sl)  ((sl)->r[0xB] & 0x1F)

/* common register words (index = (offset - 0x400) / 2) */
#define SCSP_C_MVOL(s)   ((s)->c[0x00] & 0xF)
#define SCSP_C_DAC18B(s) ((s)->c[0x00] & 0x0100)

/* ---- slots ----------------------------------------------------------------- */

static inline uint32_t scsp_slot_step(const scsp_slot_t *sl) {
    int oct = ((int)SCSP_OCT(sl) ^ 8) - 8 + SCSP_SHIFT - 10;
    uint32_t fn = SCSP_FNS(sl) + (1u << 10);
    return oct >= 0 ? fn << oct : fn >> -oct;
}

static inline int32_t scsp_rate(const int32_t *table, int base, int r) {
    int rate = base + (r << 1);
    return table[rate < 0 ? 0 : rate > 63 ? 63 : rate];
}

static void scsp_slot_lfo(scsp_slot_t *sl) {
    float step = SCSP_LFO_HZ[SCSP_LFOF(sl)] * 256.0f / 44100.0f;
    uint32_t phase_step = (uint32_t)((float)(1 << SCSP_LFO_SHIFT) * step);
    if (SCSP_PLFOS(sl)) {
        static const int32_t *const T[4] = { scsp_plfo_saw, scsp_plfo_sqr, scsp_plfo_tri, scsp_plfo_noi };
        sl->plfo.step = phase_step; sl->plfo.table = T[SCSP_PLFOWS(sl)]; sl->plfo.scale = scsp_pscale[SCSP_PLFOS(sl)];
    }
    if (SCSP_ALFOS(sl)) {
        static const int32_t *const T[4] = { scsp_alfo_saw, scsp_alfo_sqr, scsp_alfo_tri, scsp_alfo_noi };
        sl->alfo.step = phase_step; sl->alfo.table = T[SCSP_ALFOWS(sl)]; sl->alfo.scale = scsp_ascale[SCSP_ALFOS(sl)];
    }
}

static void scsp_slot_start(scsp_slot_t *sl) {
    int octave = ((int)SCSP_OCT(sl) ^ 8) - 8;
    /* MAME: key rate scaling adds to the base even for rate 0 (hardware: 0 = never) */
    int base = SCSP_KRS(sl) != 0xF ? octave + 2 * (int)SCSP_KRS(sl) + ((SCSP_FNS(sl) >> 9) & 1) : 0;
    sl->active    = 1;
    sl->cur       = 0;
    sl->nxt       = 1u << SCSP_SHIFT;
    sl->step      = scsp_slot_step(sl);
    sl->ar        = scsp_rate(scsp_ar_table, base, SCSP_AR(sl));
    sl->d1r       = scsp_rate(scsp_dr_table, base, SCSP_D1R(sl));
    sl->d2r       = scsp_rate(scsp_dr_table, base, SCSP_D2R(sl));
    sl->rr        = scsp_rate(scsp_dr_table, base, SCSP_RR(sl));
    sl->dl        = 0x1F - (int32_t)SCSP_DL(sl);
    sl->eghold    = SCSP_EGHOLD(sl) != 0;
    sl->state     = SCSP_ATTACK;
    sl->vol       = 0x17F << SCSP_EG_SHIFT;
    sl->backwards = 0;
    scsp_slot_lfo(sl);
}

/* keyoff: into release (KYONB is cleared either way, which the driver reads) */
static inline void scsp_slot_stop(scsp_slot_t *sl, int keyoff) {
    if (keyoff) sl->state = SCSP_RELEASE;
    else        sl->active = 0;
    sl->r[0] &= (uint16_t)~0x0800;
}

/* envelope level for this sample, SCSP_SHIFT fixed (linear in attack) */
static int32_t scsp_eg_update(scsp_slot_t *sl) {
    switch (sl->state) {
    case SCSP_ATTACK:
        sl->vol += sl->ar;
        if (sl->vol >= (0x3FF << SCSP_EG_SHIFT)) {
            if (!SCSP_LPSLNK(sl)) {
                sl->state = SCSP_DECAY1;
                if (sl->d1r >= (1024 << SCSP_EG_SHIFT)) sl->state = SCSP_DECAY2;
            }
            sl->vol = 0x3FF << SCSP_EG_SHIFT;
        }
        if (sl->eghold) return 0x3FF << (SCSP_SHIFT - 10);
        break;
    case SCSP_DECAY1:
        sl->vol -= sl->d1r;
        if (sl->vol <= 0) sl->vol = 0;
        if (sl->vol >> (SCSP_EG_SHIFT + 5) <= sl->dl) sl->state = SCSP_DECAY2;
        break;
    case SCSP_DECAY2:
        if (SCSP_D2R(sl) == 0) break;
        sl->vol -= sl->d2r;
        if (sl->vol <= 0) sl->vol = 0;
        break;
    case SCSP_RELEASE:
        sl->vol -= sl->rr;
        if (sl->vol <= 0) { sl->vol = 0; scsp_slot_stop(sl, 0); }
        break;
    }
    return (sl->vol >> SCSP_EG_SHIFT) << (SCSP_SHIFT - 10);
}

static inline uint8_t scsp_ram_b(const scsp_t *s, uint32_t a) {
    a &= 0xFFFFFu;
    return a < s->ram_size ? s->ram[a] : 0;
}
static inline uint16_t scsp_ram_w(const scsp_t *s, uint32_t a) {
    return (uint16_t)((scsp_ram_b(s, a) << 8) | scsp_ram_b(s, a + 1));
}
static inline void scsp_ram_ww(scsp_t *s, uint32_t a, uint16_t v) {
    a &= 0xFFFFFu;
    if (a + 1 < s->ram_size) { s->ram[a] = (uint8_t)(v >> 8); s->ram[a + 1] = (uint8_t)v; }
}

static inline int32_t scsp_lfo_p(scsp_lfo_t *l) {
    l->phase = (uint16_t)(l->phase + l->step);
    return l->scale[l->table[l->phase >> SCSP_LFO_SHIFT] + 128] << (SCSP_SHIFT - SCSP_LFO_SHIFT);
}
static inline int32_t scsp_lfo_a(scsp_lfo_t *l) {
    l->phase = (uint16_t)(l->phase + l->step);
    return l->scale[l->table[l->phase >> SCSP_LFO_SHIFT]] << (SCSP_SHIFT - SCSP_LFO_SHIFT);
}

/* one sample of one active slot; writes the sound stack entry at *sous */
static int32_t scsp_slot_sample(scsp_t *s, scsp_slot_t *sl, int16_t *sous) {
    if (SCSP_SSCTL(sl) == 3) return 0;                 /* "cannot be used" */

    int32_t  sample = 0;
    uint32_t step = sl->step;
    if (SCSP_PLFOS(sl)) step = (uint32_t)(((int64_t)step * scsp_lfo_p(&sl->plfo)) >> SCSP_SHIFT);

    uint32_t a1, a2;
    if (SCSP_PCM8B(sl)) { a1 = sl->cur >> SCSP_SHIFT; a2 = sl->nxt >> SCSP_SHIFT; }
    else { a1 = (sl->cur >> (SCSP_SHIFT - 1)) & ~1u; a2 = (sl->nxt >> (SCSP_SHIFT - 1)) & ~1u; }

    if (SCSP_MDL(sl) || SCSP_MDXSL(sl) || SCSP_MDYSL(sl)) {    /* FM from the sound stack */
        int32_t smp = (s->sous[(s->sous_ptr + SCSP_MDXSL(sl)) & 63] + s->sous[(s->sous_ptr + SCSP_MDYSL(sl)) & 63]) / 2;
        smp *= 1 << 10;
        smp >>= 0x1A - SCSP_MDL(sl);
        if (!SCSP_PCM8B(sl)) smp *= 2;
        a1 += (uint32_t)smp; a2 += (uint32_t)smp;
    }

    uint32_t sa = SCSP_SA(sl);
    int32_t  frac = (int32_t)(sl->cur & ((1u << SCSP_SHIFT) - 1));
    if (SCSP_SSCTL(sl) == 0) {
        if (SCSP_PCM8B(sl)) {
            int32_t p1 = (int8_t)scsp_ram_b(s, sa + a1), p2 = (int8_t)scsp_ram_b(s, sa + a2);
            sample = ((p1 * 256) * ((1 << SCSP_SHIFT) - frac) + (p2 * 256) * frac) >> SCSP_SHIFT;
        } else {
            int32_t p1 = (int16_t)scsp_ram_w(s, sa + a1), p2 = (int16_t)scsp_ram_w(s, sa + a2);
            sample = (p1 * ((1 << SCSP_SHIFT) - frac) + p2 * frac) >> SCSP_SHIFT;
        }
    } else if (SCSP_SSCTL(sl) == 1) {
        sample = (int16_t)(scsp_lcg(&s->noise) & 0xFFFF);  /* MAME: unknown algorithm */
    }
    if (SCSP_SBCTL(sl) & 1) sample ^= 0x7FFF;
    if (SCSP_SBCTL(sl) & 2) sample = (int16_t)(sample ^ 0x8000);

    if (sl->backwards) sl->cur -= step;
    else               sl->cur += step;
    sl->nxt = sl->cur + (1u << SCSP_SHIFT);

    uint32_t lsa = SCSP_LSA(sl), lea = SCSP_LEA(sl);
    uint32_t addr[2] = { sl->cur >> SCSP_SHIFT, sl->nxt >> SCSP_SHIFT };
    uint32_t *pos[2] = { &sl->cur, &sl->nxt };
    if (addr[0] >= lsa && !sl->backwards && SCSP_LPSLNK(sl) && sl->state == SCSP_ATTACK)
        sl->state = SCSP_DECAY1;
    for (int k = 0; k < 2; k++) {
        int32_t rem;
        switch (SCSP_LPCTL(sl)) {
        case 0:                                         /* no loop */
            if (addr[k] >= lsa && addr[k] >= lea) scsp_slot_stop(sl, 0);
            break;
        case 1:                                         /* forward loop */
            if (addr[k] >= lea) {
                rem = (int32_t)(*pos[k] - (lea << SCSP_SHIFT));
                *pos[k] = (lsa << SCSP_SHIFT) + (uint32_t)rem;
            }
            break;
        case 2:                                         /* reverse loop */
            if (addr[k] >= lsa && !sl->backwards) {
                rem = (int32_t)(*pos[k] - (lsa << SCSP_SHIFT));
                *pos[k] = (lea << SCSP_SHIFT) - (uint32_t)rem;
                sl->backwards = 1;
            } else if ((addr[k] < lsa || (*pos[k] & 0x80000000u)) && sl->backwards) {
                rem = (int32_t)((lsa << SCSP_SHIFT) - *pos[k]);
                *pos[k] = (lea << SCSP_SHIFT) - (uint32_t)rem;
            }
            break;
        case 3:                                         /* alternating */
            if (addr[k] >= lea) {
                rem = (int32_t)(*pos[k] - (lea << SCSP_SHIFT));
                *pos[k] = (lea << SCSP_SHIFT) - (uint32_t)rem;
                sl->backwards = 1;
            } else if ((addr[k] < lsa || (*pos[k] & 0x80000000u)) && sl->backwards) {
                rem = (int32_t)((lsa << SCSP_SHIFT) - *pos[k]);
                *pos[k] = (lsa << SCSP_SHIFT) + (uint32_t)rem;
                sl->backwards = 0;
            }
            break;
        }
    }

    if (!SCSP_SDIR(sl)) {
        if (SCSP_ALFOS(sl)) sample = (int32_t)(((int64_t)sample * scsp_lfo_a(&sl->alfo)) >> SCSP_SHIFT);
        if (sl->state == SCSP_ATTACK)
            sample = (int32_t)(((int64_t)sample * scsp_eg_update(sl)) >> SCSP_SHIFT);
        else
            sample = (int32_t)(((int64_t)sample * scsp_eg_table[scsp_eg_update(sl) >> (SCSP_SHIFT - 10)]) >> SCSP_SHIFT);
    }
    if (!SCSP_STWINH(sl)) {
        uint16_t enc = (uint16_t)((SCSP_SDIR(sl) ? 0 : SCSP_TL(sl)) | (7u << 13));
        *sous = (int16_t)((sample * scsp_lpan[enc]) >> (SCSP_SHIFT + 1));
    }
    return sample;
}

/* ---- DSP ----------------------------------------------------------------------- */

static inline int32_t scsp_sext(int32_t v, int bits) { return (int32_t)((uint32_t)v << (32 - bits)) >> (32 - bits); }

static uint16_t scsp_dsp_pack(int32_t val) {
    int sign = (val >> 23) & 1;
    uint32_t temp = ((uint32_t)val ^ ((uint32_t)val << 1)) & 0xFFFFFFu;
    int exponent = 0;
    for (int k = 0; k < 12; k++) {
        if (temp & 0x800000u) break;
        temp <<= 1;
        exponent++;
    }
    if (exponent < 12) val = (int32_t)(((uint32_t)val << exponent) & 0x3FFFFF);
    else               val = (int32_t)((uint32_t)val << 11);
    val >>= 11;
    val &= 0x7FF;
    val |= sign << 15;
    val |= exponent << 11;
    return (uint16_t)val;
}

static int32_t scsp_dsp_unpack(uint16_t val) {
    int sign = (val >> 15) & 1;
    int exponent = (val >> 11) & 0xF;
    int32_t uval = (val & 0x7FF) << 11;
    if (exponent > 11) { exponent = 11; uval |= sign << 22; }
    else               uval |= (sign ^ 1) << 22;
    uval |= sign << 23;
    uval = scsp_sext(uval, 24);
    return uval >> exponent;
}

static void scsp_dsp_start(scsp_dsp_t *d) {
    d->stopped = 0;
    int i;
    for (i = 127; i >= 0; i--) {
        const uint16_t *p = d->mpro + i * 4;
        if (p[0] || p[1] || p[2] || p[3]) break;
    }
    d->last_step = i + 1;
}

static void scsp_dsp_decode(scsp_dsp_t *d) {
    d->ops_run = d->last_step;
    for (int st = 0; st < 128; st++) {
        const uint16_t *p = d->mpro + st * 4;
        scsp_dsp_op_t  *o = &d->ops[st];
        int ysel = (p[1] >> 13) & 3, sh = (p[2] >> 4) & 3;
        int iwt  = (p[1] >> 5) & 1,  iwa = p[1] & 0x1F;
        int mwt  = (p[2] >> 14) & 1, mrd = (p[2] >> 13) & 1;
        o->ira   = (p[1] >> 6) & 0x3F;
        o->tra   = (p[0] >> 8) & 0x7F;
        o->twa   = p[0] & 0x7F;
        o->coef  = (p[3] >> 9) & 0x3F;
        o->masa  = (p[3] >> 2) & 0x1F;
        o->iwa   = (uint8_t)(iwt ? iwa : 0x3F);
        o->ewa   = (uint8_t)((p[2] >> 12) & 1 ? (p[2] >> 8) & 0xF : 16);
        o->dbl   = sh == 1 || sh == 2;
        /* Y: FRC, COEF >> 3, YREG[23:11] or YREG[15:4], all read as 13-bit signed */
        o->ysh   = (uint8_t)(ysel == 0 ? 0 : ysel == 1 ? 3 : ysel == 2 ? 11 : 4);
        o->ymask = (uint16_t)(ysel == 3 ? 0x0FFF : 0x1FFF);
        o->frc_sh   = (uint8_t)(sh == 3 ? 0 : 11);
        o->frc_mask = (uint16_t)(sh == 3 ? 0x0FFF : 0x1FFF);
        /* MAME: the delay memory is only touched on odd steps */
        o->mrd   = mrd && (st & 1);
        o->mwt   = mwt && (st & 1);
        o->mem   = o->mrd || o->mwt;
        o->nofl  = (p[3] >> 15) & 1;
        o->nxadr = p[3] & 1;
        uint32_t f = 0;
        f |= (uint32_t)(iwt && o->ira == iwa) << SCSP_DF_IW_SELF;
        f |= (uint32_t)((p[0] >> 7) & 1)  << SCSP_DF_TWT;
        f |= (uint32_t)((p[1] >> 15) & 1) << SCSP_DF_XSEL;
        f |= (uint32_t)(p[2] & 1)         << SCSP_DF_BSEL;
        f |= (uint32_t)((p[2] >> 1) & 1)  << SCSP_DF_ZERO;
        f |= (uint32_t)((p[2] >> 2) & 1)  << SCSP_DF_NEGB;
        f |= (uint32_t)((p[2] >> 3) & 1)  << SCSP_DF_YRL;
        f |= (uint32_t)(ysel == 1)        << SCSP_DF_Y_COEF;
        f |= (uint32_t)(ysel >= 2)        << SCSP_DF_Y_YREG;
        f |= (uint32_t)(sh <= 1)          << SCSP_DF_SAT;
        f |= (uint32_t)((p[2] >> 6) & 1)  << SCSP_DF_FRCL;
        f |= (uint32_t)((p[2] >> 7) & 1)  << SCSP_DF_ADRL;
        f |= (uint32_t)(sh == 3)          << SCSP_DF_ADRL_SH;
        f |= (uint32_t)((p[2] >> 15) & 1) << SCSP_DF_TABLE;
        f |= (uint32_t)((p[3] >> 1) & 1)  << SCSP_DF_ADREB;
        o->f = f;
        /* An IRA past EXTS ends the program on that step, before it does anything. */
        if (o->ira > 0x31 && st < d->ops_run) d->ops_run = st;
    }
    d->ops_ok = 1;
    d->ops_last = d->last_step;
}

/* SCSP_SEL(f, bit, a, b): a when the step's flag is set, else b. How that is
 * best spelled depends on the core, and the two spellings compute the same
 * bits (tests/scsp_dsp_test.c builds both):
 *   - masks, for 64-bit ARM. The handhelds are in-order Cortex-A55/A53/A35,
 *     where GCC's branches for these mispredicted about once a step: the DSP
 *     ran 25% faster on the ARC-S with masks than with the ternary.
 *   - a ternary everywhere else. On x86 the masks are five dependent ALU ops a
 *     select where a well-predicted branch or cmov is one; the DSP ran 1.7x
 *     slower with masks under MSVC.
 * SCSP_DSP_MASKS set to 0 or 1 overrides the choice. */
#ifndef SCSP_DSP_MASKS
#if defined(__aarch64__) || defined(_M_ARM64)
#define SCSP_DSP_MASKS 1
#else
#define SCSP_DSP_MASKS 0
#endif
#endif
#if SCSP_DSP_MASKS
static inline int32_t scsp_sel(int32_t m, int32_t a, int32_t b) { return (a & m) | (b & ~m); }
#define SCSP_SEL(f, bit, a, b) scsp_sel(SCSP_DM(f, bit), (a), (b))
#else
#define SCSP_SEL(f, bit, a, b) (((f) & (1u << (bit))) ? (a) : (b))
#endif

static void scsp_dsp_step(scsp_t *s) {
    scsp_dsp_t *d = &s->dsp;
    if (d->stopped) return;
    if (!d->ops_ok || d->ops_last != d->last_step) scsp_dsp_decode(d);
    int16_t efreg[17] = {0};     /* [16] takes the steps that write no EFREG */

    /* The input bank IRA indexes: MEMS, then MIXS << 4, then EXTS << 8. MEMS
     * is written back below, and only the step writes it. */
    int32_t in[0x40];
    memcpy(in, d->mems, sizeof d->mems);
    for (int i = 0; i < 16; i++) in[0x20 + i] = (int32_t)((uint32_t)d->mixs[i] << 4);
    in[0x30] = d->exts[0] * 256;
    in[0x31] = d->exts[1] * 256;

    int32_t *temp = d->temp;
    const uint32_t dec = d->dec, rbl_mask = d->rbl - 1, rbp = d->rbp << 12;
    int32_t  acc = 0, memval = 0, frc = 0, yreg = 0;
    uint32_t adrs = 0;
    const int run = d->ops_run;
    for (int st = 0; st < run; st++) {
        const scsp_dsp_op_t *o = &d->ops[st];
        const uint32_t f = o->f;

        int32_t inputs = scsp_sext(in[o->ira], 24);
        inputs = SCSP_SEL(f, SCSP_DF_IW_SELF, memval, inputs);
        in[o->iwa] = memval;

        int32_t tr = scsp_sext(temp[(o->tra + dec) & 0x7F], 24);
        int32_t b  = SCSP_SEL(f, SCSP_DF_BSEL, acc, tr);
        b = SCSP_SEL(f, SCSP_DF_NEGB, (int32_t)(0u - (uint32_t)b), b);
        b = SCSP_SEL(f, SCSP_DF_ZERO, 0, b);
        int32_t x  = SCSP_SEL(f, SCSP_DF_XSEL, inputs, tr);

        int32_t ys = SCSP_SEL(f, SCSP_DF_Y_COEF, d->coef[o->coef], frc);
        ys = SCSP_SEL(f, SCSP_DF_Y_YREG, yreg, ys);
        int32_t y  = scsp_sext((ys >> o->ysh) & o->ymask, 13);
        yreg = SCSP_SEL(f, SCSP_DF_YRL, inputs, yreg);

        int64_t v = (int64_t)((uint64_t)(int64_t)acc << o->dbl);
        int64_t c = v > 0x7FFFFF ? 0x7FFFFF : v;
        c = c < -0x800000 ? -0x800000 : c;
        int32_t shifted = SCSP_SEL(f, SCSP_DF_SAT, (int32_t)c, scsp_sext((int32_t)(uint32_t)v, 24));

        acc = (int32_t)(((int64_t)x * (int64_t)y) >> 12) + b;

        int32_t *tw = &temp[(o->twa + dec) & 0x7F];
        *tw = SCSP_SEL(f, SCSP_DF_TWT, shifted, *tw);
        frc = SCSP_SEL(f, SCSP_DF_FRCL, (shifted >> o->frc_sh) & o->frc_mask, frc);

        if (o->mem) {
            uint32_t table = (uint32_t)SCSP_DM(f, SCSP_DF_TABLE);
            uint32_t addr = d->madrs[o->masa] + (dec & ~table)
                          + (adrs & 0x0FFF & (uint32_t)SCSP_DM(f, SCSP_DF_ADREB)) + o->nxadr;
            addr &= (0xFFFFu & table) | (rbl_mask & ~table);
            addr = (addr + rbp) << 1;
            if (o->mrd) memval = o->nofl ? scsp_ram_w(s, addr) << 8 : scsp_dsp_unpack(scsp_ram_w(s, addr));
            if (o->mwt) scsp_ram_ww(s, addr, o->nofl ? (uint16_t)(shifted >> 8) : scsp_dsp_pack(shifted));
        }
        int32_t na = SCSP_SEL(f, SCSP_DF_ADRL_SH, (shifted >> 12) & 0xFFF, inputs >> 16);
        adrs = (uint32_t)SCSP_SEL(f, SCSP_DF_ADRL, na, (int32_t)adrs);
        efreg[o->ewa] = (int16_t)(efreg[o->ewa] + (shifted >> 8));
    }
    memcpy(d->mems, in, sizeof d->mems);
    memcpy(d->efreg, efreg, sizeof d->efreg);
    if (run < d->last_step) return;   /* the program ended on a bad IRA: no DEC, MIXS kept */
    d->dec--;
    memset(d->mixs, 0, sizeof d->mixs);
}

/* ---- interrupts ------------------------------------------------------------------- */

static inline uint8_t scsp_level(const scsp_t *s, int src) {
    return (uint8_t)(((s->c[0x12] >> src) & 1) | (((s->c[0x13] >> src) & 1) << 1) | (((s->c[0x14] >> src) & 1) << 2));
}

/* MAME: the first pending, enabled source in the order timer A, B, C, MIDI
 * raises its line and the rest wait; lines stay up until SCIRE (or an empty
 * MIDI buffer) drops them, and the 68000 takes the highest line held. */
static void scsp_check_irq(scsp_t *s) {
    uint16_t pend = s->c[0x10], en = s->c[0x0F];
    if (s->mi_w != s->mi_r) { s->c[0x10] |= 0x08; pend |= 0x08; }
    if (!pend) return;
    if (pend & en & 0x040) { s->lines |= (uint8_t)(1u << s->lvl_ta);   return; }
    if (pend & en & 0x080) { s->lines |= (uint8_t)(1u << s->lvl_tbc);  return; }
    if (pend & en & 0x100) { s->lines |= (uint8_t)(1u << s->lvl_tbc);  return; }
    if (pend & en & 0x008) { s->lines |= (uint8_t)(1u << s->lvl_midi); return; }
}

/* the interrupt level the 68000 sees, 0 = none */
static inline int scsp_irq_level(const scsp_t *s) {
    uint8_t l = (uint8_t)(s->lines & 0xFE);
    int lvl = 0;
    while (l >>= 1) lvl++;
    return lvl;
}

static inline void scsp_timer_next(scsp_t *s) {
    s->tim_next = UINT64_MAX;
    for (int t = 0; t < 3; t++)
        if (s->tim_due[t] && s->tim_due[t] < s->tim_next) s->tim_next = s->tim_due[t];
}

static inline void scsp_timer_fire(scsp_t *s, int t) {
    s->tim_due[t] = 0;
    s->tim_cnt[t] = 0xFFFF;
    s->c[0x10] |= (uint16_t)(0x40u << t);
    s->c[0x0C + t] |= 0x00FF;
    if (t == 0) s->mcipd |= 0x40;                      /* the main-CPU side, unused on Model 2 */
    scsp_check_irq(s);
}

/* ---- DMA -------------------------------------------------------------------------- */

static uint16_t scsp_r16(scsp_t *s, uint32_t addr);
static void     scsp_w16(scsp_t *s, uint32_t addr, uint16_t v);

static void scsp_dma(scsp_t *s) {
    uint32_t dmea = ((uint32_t)(s->c[0x0A] & 0xF000) << 4) | (s->c[0x09] & 0xFFFE);
    uint32_t drga = s->c[0x0A] & 0x0FFE;
    uint32_t dtlg = s->c[0x0B] & 0x0FFE;
    int ddir = (s->c[0x0B] >> 13) & 1, dgate = (s->c[0x0B] >> 14) & 1;
    uint16_t keep[3] = { s->c[0x09], s->c[0x0A], s->c[0x0B] };
    for (uint32_t i = 0; i < dtlg; i += 2, dmea += 2) {
        if (ddir) { scsp_ram_ww(s, dmea, dgate ? 0 : scsp_r16(s, drga)); if (!dgate) drga += 2; }
        else      { scsp_w16(s, drga, dgate ? 0 : scsp_ram_w(s, dmea)); drga += 2; }
    }
    if (!ddir) { s->c[0x09] = keep[0]; s->c[0x0A] = keep[1]; s->c[0x0B] = keep[2]; }
    s->c[0x0B] &= (uint16_t)~0x1000;
    /* MAME pulses the DMA-end interrupt (HOLD_LINE) when SCIEB bit 4 is set; no
     * Model 2 driver enables it, and a held line would never be released here. */
}

/* ---- register access ---------------------------------------------------------------- */

static void scsp_w16(scsp_t *s, uint32_t addr, uint16_t v) {
    addr &= 0xFFFF;
    if (addr < 0x400) {
        scsp_slot_t *sl = &s->slot[addr / 0x20];
        unsigned r = (addr & 0x1F) >> 1;
        sl->r[r] = v;
        switch (r) {
        case 0x0:
            if (SCSP_KYONEX(sl)) {                      /* one strobe commits every slot's KYONB */
                for (int i = 0; i < 32; i++) {
                    scsp_slot_t *s2 = &s->slot[i];
                    if (SCSP_KYONB(s2) && s2->state == SCSP_RELEASE) scsp_slot_start(s2);
                    if (!SCSP_KYONB(s2)) scsp_slot_stop(s2, 1);
                }
                sl->r[0] &= (uint16_t)~0x1000;
            }
            break;
        case 0x8: sl->step = scsp_slot_step(sl); break;
        case 0x5: sl->rr = scsp_rate(scsp_dr_table, 0, SCSP_RR(sl)); sl->dl = 0x1F - (int32_t)SCSP_DL(sl); break;
        case 0x9: scsp_slot_lfo(sl); break;
        }
        return;
    }
    if (addr < 0x430) {
        unsigned r = (addr - 0x400) >> 1;
        s->c[r] = v;
        switch (r) {
        case 0x01:
            s->dsp.rbl = (8u * 1024u) << ((v >> 7) & 3);
            s->dsp.rbp = v & 0x3F;
            break;
        case 0x03:                                      /* MIDI out */
            s->mo[s->mo_w++ & 31] = (uint8_t)v;
            s->mo_w &= 31;
            break;
        case 0x04: s->c[0x04] &= 0xF800; break;         /* only MSLC is writable */
        case 0x0B: if (v & 0x1000) scsp_dma(s); break;
        case 0x0C: case 0x0D: case 0x0E: {              /* timers A, B, C */
            int t = (int)r - 0x0C;
            uint32_t reload = v & 0xFF;
            s->tim_cnt[t] = (uint16_t)(reload << 8);
            /* Timed from the write, to the clock period: the driver reloads its
             * timers inside the interrupt handler, a few hundred periods after
             * they fire, and that delay is part of every period (MAME measures
             * 49.9 samples fire to fire for a 49-sample timer). Rounding to whole
             * samples plays the music 2% fast. MAME: 255 leaves a running timer
             * running. */
            if (reload != 255) {
                s->tim_due[t] = *s->clock + (((uint64_t)(255u - reload) << ((v >> 8) & 7)) << 8);
                scsp_timer_next(s);
            }
            break;
        }
        case 0x0F: scsp_check_irq(s); break;            /* SCIEB */
        case 0x11: {                                    /* SCIRE */
            s->c[0x10] &= (uint16_t)~s->c[0x11];
            if (v & 0x040) s->lines &= (uint8_t)~(1u << s->lvl_ta);
            if (v & 0x180) s->lines &= (uint8_t)~(1u << s->lvl_tbc);
            if (v & 0x008) s->lines &= (uint8_t)~(1u << s->lvl_midi);
            scsp_check_irq(s);
            /* MAME, from hardware: an acknowledged timer that has expired pends again */
            for (int t = 0; t < 3; t++) if (s->tim_cnt[t] == 0xFFFF) s->c[0x10] |= (uint16_t)(0x40u << t);
            break;
        }
        case 0x12: case 0x13: case 0x14:                /* SCILV0-2 */
            s->lvl_ta   = scsp_level(s, SCSP_INT_TIMER_A);
            s->lvl_tbc  = scsp_level(s, SCSP_INT_TIMER_B);
            s->lvl_midi = scsp_level(s, SCSP_INT_MIDI_IN);
            break;
        case 0x15: s->mcieb = v; break;
        case 0x16: if (v & 0x20) s->mcipd |= 0x20; break;
        case 0x17: s->mcipd &= (uint16_t)~v; break;
        }
        return;
    }
    if (addr < 0x600) return;
    if (addr < 0x700) { s->sous[(addr - 0x600) >> 1] = (int16_t)v; return; }
    if (addr < 0x780) { s->dsp.coef[(addr - 0x700) >> 1] = (int16_t)v; return; }
    if (addr < 0x7C0) { s->dsp.madrs[(addr - 0x780) >> 1] = v; return; }
    if (addr < 0x800) { s->dsp.madrs[(addr - 0x7C0) >> 1] = v; return; }
    if (addr < 0xC00) {
        s->dsp.mpro[(addr - 0x800) >> 1] = v;
        s->dsp.ops_ok = 0;
        if (addr == 0xBF0) scsp_dsp_start(&s->dsp);
    }
}

/* a read with the side effects the 68000's own reads have (MIDI buffer, monitor) */
static uint16_t scsp_r16(scsp_t *s, uint32_t addr) {
    addr &= 0xFFFF;
    if (addr < 0x400) return s->slot[addr / 0x20].r[(addr & 0x1F) >> 1];
    if (addr < 0x430) {
        unsigned r = (addr - 0x400) >> 1;
        switch (r) {
        case 0x02: {                                    /* MIDI in */
            uint16_t v = (uint16_t)((s->c[0x02] & 0xFF00) | s->mi[s->mi_r]);
            if (s->mi_r != s->mi_w) s->mi_r = (uint8_t)((s->mi_r + 1) & 31);
            if (s->mi_r == s->mi_w) {
                s->lines &= (uint8_t)~(1u << s->lvl_midi);
                s->c[0x10] &= (uint16_t)~0x08;
            }
            s->c[0x02] = v;
            break;
        }
        case 0x04: {                                    /* slot monitor */
            /* MAME: MSLC comes from the register word, which the read then
             * overwrites — a second read without a new MSLC monitors slot 0. */
            scsp_slot_t *sl = &s->slot[(s->c[0x04] >> 11) & 0x1F];
            uint32_t sgc = sl->state & 3;
            uint32_t ca  = (sl->cur >> (SCSP_SHIFT + 12)) & 0xF;
            uint32_t eg  = (0x1Fu - (uint32_t)(sl->vol >> (SCSP_EG_SHIFT + 5))) & 0x1F;
            s->c[0x04] = (uint16_t)((ca << 7) | (sgc << 5) | eg);
            break;
        }
        case 0x15: s->c[0x15] = s->mcieb; break;
        case 0x16: s->c[0x16] = s->mcipd; break;
        }
        return s->c[r];
    }
    if (addr < 0x600) return 0;
    if (addr < 0x700) return (uint16_t)s->sous[(addr - 0x600) >> 1];
    if (addr < 0x780) return (uint16_t)s->dsp.coef[(addr - 0x700) >> 1];
    if (addr < 0x7C0) return s->dsp.madrs[(addr - 0x780) >> 1];
    if (addr < 0x800) return s->dsp.madrs[(addr - 0x7C0) >> 1];
    if (addr < 0xC00) return s->dsp.mpro[(addr - 0x800) >> 1];
    if (addr < 0xE00) return (addr & 2) ? (uint16_t)s->dsp.temp[(addr >> 2) & 0x7F] : (uint16_t)(s->dsp.temp[(addr >> 2) & 0x7F] >> 16);
    if (addr < 0xE80) return (addr & 2) ? (uint16_t)s->dsp.mems[(addr >> 2) & 0x1F] : (uint16_t)(s->dsp.mems[(addr >> 2) & 0x1F] >> 16);
    if (addr < 0xEC0) return (addr & 2) ? (uint16_t)s->dsp.mixs[(addr >> 2) & 0xF] : (uint16_t)(s->dsp.mixs[(addr >> 2) & 0xF] >> 16);
    if (addr < 0xEE0) return (uint16_t)s->dsp.efreg[(addr - 0xEC0) >> 1];
    if (addr < 0xEE4) return (uint16_t)s->dsp.exts[(addr - 0xEE0) >> 1];
    return 0;
}

/* a debugger's read: no side effects */
static inline uint16_t scsp_peek16(const scsp_t *s, uint32_t addr) {
    addr &= 0xFFFF;
    if (addr < 0x400) return s->slot[addr / 0x20].r[(addr & 0x1F) >> 1];
    if (addr < 0x430) return s->c[(addr - 0x400) >> 1];
    if (addr >= 0x700 && addr < 0x780) return (uint16_t)s->dsp.coef[(addr - 0x700) >> 1];
    if (addr >= 0x800 && addr < 0xC00) return s->dsp.mpro[(addr - 0x800) >> 1];
    return 0;
}

/* The 68000's view: an sz-byte access at off. The chip is a 16-bit device, so
 * a byte access reads the whole word (with its side effects) and a byte write
 * merges into it; a long is two words. */
static inline uint32_t scsp_read(scsp_t *s, uint32_t off, int sz) {
    if (sz == 1) {
        uint16_t w = scsp_r16(s, off & ~1u);
        return (off & 1) ? (w & 0xFFu) : (uint32_t)(w >> 8);
    }
    if (sz == 2) return scsp_r16(s, off & ~1u);
    uint32_t hi = scsp_r16(s, off & ~1u);
    return (hi << 16) | scsp_r16(s, (off & ~1u) + 2);
}

static inline void scsp_write(scsp_t *s, uint32_t off, uint32_t val, int sz) {
    if (sz == 1) {
        uint32_t a = off & ~1u;
        uint16_t w = scsp_r16(s, a);
        w = (off & 1) ? (uint16_t)((w & 0xFF00) | (val & 0xFF)) : (uint16_t)((w & 0x00FF) | ((val & 0xFF) << 8));
        scsp_w16(s, a, w);
    } else if (sz == 2) {
        scsp_w16(s, off & ~1u, (uint16_t)val);
    } else {
        scsp_w16(s, off & ~1u, (uint16_t)(val >> 16));
        scsp_w16(s, (off & ~1u) + 2, (uint16_t)val);
    }
}

static inline void scsp_midi_in(scsp_t *s, uint8_t b) {
    /* A full ring must not advance past the read pointer. The fill is read as
     * (mi_w - mi_r) & 31, so lapping mi_r makes 32 pending bytes look like none,
     * and scsp_check_irq then drops the MIDI line with a command half delivered
     * -- the driver loses the stream and never resyncs, which is silence for the
     * rest of the run. The i960 side bursts (emu_service_sound_again re-runs the
     * sound handler within a slice, and the 68000 only drains in sound_run_slice
     * afterwards), so this is reachable here in a way it is not on the board,
     * where the UART paces the bytes out. Drop the byte and count it instead;
     * emu_service_sound_again backs off before it gets here. */
    if ((uint8_t)((s->mi_w + 1) & 31) == s->mi_r) { s->mi_drops++; return; }
    s->mi[s->mi_w] = b;
    s->mi_w = (uint8_t)((s->mi_w + 1) & 31);
    { uint8_t fill = (uint8_t)((s->mi_w - s->mi_r) & 31); if (fill > s->mi_hi) s->mi_hi = fill; }
    scsp_check_irq(s);
}

/* free space in the MIDI input ring, for the i960 side's backpressure */
static inline uint32_t scsp_midi_room(const scsp_t *s) {
    return 31u - (uint32_t)((s->mi_w - s->mi_r) & 31);
}

/* ---- lifecycle and the sample clock --------------------------------------------------- */

static void scsp_reset(scsp_t *s, uint8_t *ram, uint32_t ram_size, const uint64_t *clock) {
    scsp_tables_init();
    memset(s, 0, sizeof *s);
    s->ram = ram;
    s->ram_size = ram_size;
    s->clock = clock;
    s->tim_next = UINT64_MAX;
    s->noise = 0x12345678u;
    for (int i = 0; i < 32; i++) s->slot[i].state = SCSP_RELEASE;
    for (int t = 0; t < 3; t++) s->tim_cnt[t] = 0xFFFF;
    s->dsp.rbl = 8u * 1024u;
    s->dsp.stopped = 1;
}

/* Fire any timer whose moment has come. The driving CPU calls this between
 * instructions, so an interrupt lands on the clock period it is due. */
static inline void scsp_timers(scsp_t *s, uint64_t now) {
    if (now < s->tim_next) return;
    for (int t = 0; t < 3; t++)
        if (s->tim_due[t] && s->tim_due[t] <= now) scsp_timer_fire(s, t);
    scsp_timer_next(s);
}

/* Produce one sample: every slot, the DSP, the mix. */
static void scsp_sample(scsp_t *s, int16_t *out_l, int16_t *out_r) {
    int32_t l = 0, r = 0;
    for (int i = 0; i < 32; i++) {
        scsp_slot_t *sl = &s->slot[i];
        int16_t *sous = &s->sous[s->sous_ptr];
        if (sl->active) {
            int32_t smp = scsp_slot_sample(s, sl, sous);
            s->dsp.mixs[SCSP_ISEL(sl)] += (smp * scsp_lpan[SCSP_TL(sl) | (SCSP_IMXL(sl) << 13)]) >> (SCSP_SHIFT - 2);
            uint16_t enc = (uint16_t)(SCSP_TL(sl) | (SCSP_DIPAN(sl) << 8) | (SCSP_DISDL(sl) << 13));
            l += (smp * scsp_lpan[enc]) >> SCSP_SHIFT;
            r += (smp * scsp_rpan[enc]) >> SCSP_SHIFT;
        }
        s->sous_ptr = (uint8_t)((s->sous_ptr + 1) & 63);
    }
    scsp_dsp_step(s);
    for (int i = 0; i < 16; i++) {
        const scsp_slot_t *sl = &s->slot[i];
        if (!SCSP_EFSDL(sl)) continue;
        uint16_t enc = (uint16_t)((SCSP_EFPAN(sl) << 8) | (SCSP_EFSDL(sl) << 13));
        l += (s->dsp.efreg[i] * scsp_lpan[enc]) >> SCSP_SHIFT;
        r += (s->dsp.efreg[i] * scsp_rpan[enc]) >> SCSP_SHIFT;
    }
    if (SCSP_C_DAC18B(s)) {
        l = l < -131072 ? -131072 : l > 131071 ? 131071 : l;
        r = r < -131072 ? -131072 : r > 131071 ? 131071 : r;
        l >>= 2; r >>= 2;
    } else {
        l >>= 2; r >>= 2;
        l = l < -32768 ? -32768 : l > 32767 ? 32767 : l;
        r = r < -32768 ? -32768 : r > 32767 ? 32767 : r;
    }
    int mvol = (int)SCSP_C_MVOL(s);                   /* MAME: output gain MVOL / 15 */
    *out_l = (int16_t)(l * mvol / 15);
    *out_r = (int16_t)(r * mvol / 15);
    s->samples++;
}

#endif /* SCSP_H */
