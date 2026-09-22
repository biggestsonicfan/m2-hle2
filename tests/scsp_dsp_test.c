/*
 * scsp_dsp_test.c — the SCSP DSP step against its own previous self.
 *
 * scsp_dsp_step was rewritten for the handhelds' in-order cores (the decoded
 * op now holds the step's choices as flags it selects on, and writes a step
 * does not make go to spare slots). The step it replaced is kept here verbatim
 * as the reference, and both run random microprograms over random registers,
 * sound RAM and ring buffers, compared after every sample: EFREG, TEMP, MEMS,
 * MIXS, DEC and the delay line in sound RAM. STF's own program exercises only
 * some fields, so this is what covers the rest -- TABLE, NOFL, every Y source
 * and shifter mode, a program cut short by a bad IRA, and a program rewritten
 * or lengthened between samples.
 *
 * Build it with SCSP_DSP_MASKS=0 and =1 (CMake does both): the two ways
 * SCSP_SEL is spelled must compute the same bits on every host.
 *
 * No ROM: pure functions of the chip's state.
 */
#define NDEBUG 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/board/scsp.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
} while (0)

/* ---- the previous step, verbatim ------------------------------------------ */

typedef struct {
    uint8_t tra, twt, twa, xsel, ysel, ira, iwt, iwa;
    uint8_t table, mwt, mrd, ewt, ewa, adrl, frcl, sh, yrl, negb, zero, bsel;
    uint8_t nofl, coef, masa, adreb, nxadr;
} ref_op_t;

static void ref_dsp_decode(const scsp_dsp_t *d, ref_op_t *ops) {
    for (int st = 0; st < 128; st++) {
        const uint16_t *p = d->mpro + st * 4;
        ref_op_t       *o = &ops[st];
        o->tra   = (p[0] >> 8) & 0x7F;  o->twt  = (p[0] >> 7) & 1;   o->twa   = p[0] & 0x7F;
        o->xsel  = (p[1] >> 15) & 1;    o->ysel = (p[1] >> 13) & 3;  o->ira   = (p[1] >> 6) & 0x3F;
        o->iwt   = (p[1] >> 5) & 1;     o->iwa  = p[1] & 0x1F;
        o->table = (p[2] >> 15) & 1;    o->mwt  = (p[2] >> 14) & 1;  o->mrd   = (p[2] >> 13) & 1;
        o->ewt   = (p[2] >> 12) & 1;    o->ewa  = (p[2] >> 8) & 0xF; o->adrl  = (p[2] >> 7) & 1;
        o->frcl  = (p[2] >> 6) & 1;     o->sh   = (p[2] >> 4) & 3;   o->yrl   = (p[2] >> 3) & 1;
        o->negb  = (p[2] >> 2) & 1;     o->zero = (p[2] >> 1) & 1;   o->bsel  = p[2] & 1;
        o->nofl  = (p[3] >> 15) & 1;    o->coef = (p[3] >> 9) & 0x3F;
        o->masa  = (p[3] >> 2) & 0x1F;  o->adreb = (p[3] >> 1) & 1;  o->nxadr = p[3] & 1;
    }
}

static void ref_dsp_step(scsp_t *s, const ref_op_t *ops) {
    scsp_dsp_t *d = &s->dsp;
    if (d->stopped) return;
    memset(d->efreg, 0, sizeof d->efreg);
    int32_t  acc = 0, memval = 0, frc = 0, yreg = 0;
    uint32_t adrs = 0;
    for (int st = 0; st < d->last_step; st++) {
        const ref_op_t *o = &ops[st];

        int32_t inputs;
        if (o->ira <= 0x1F)      inputs = d->mems[o->ira];
        else if (o->ira <= 0x2F) inputs = d->mixs[o->ira - 0x20] * 16;
        else if (o->ira <= 0x31) inputs = d->exts[o->ira - 0x30] * 256;
        else return;
        inputs = scsp_sext(inputs, 24);
        if (o->iwt) {
            d->mems[o->iwa] = memval;
            if (o->ira == o->iwa) inputs = memval;
        }

        int32_t b = 0;
        if (!o->zero) {
            b = o->bsel ? acc : scsp_sext(d->temp[(o->tra + d->dec) & 0x7F], 24);
            if (o->negb) b = -b;
        }
        int32_t x = o->xsel ? inputs : scsp_sext(d->temp[(o->tra + d->dec) & 0x7F], 24);
        int32_t y = 0;
        if (o->ysel == 0)      y = frc;
        else if (o->ysel == 1) y = d->coef[o->coef] >> 3;
        else if (o->ysel == 2) y = (yreg >> 11) & 0x1FFF;
        else                   y = (yreg >> 4) & 0x0FFF;
        if (o->yrl) yreg = inputs;

        int32_t shifted;
        if (o->sh == 0)      shifted = acc < -0x800000 ? -0x800000 : acc > 0x7FFFFF ? 0x7FFFFF : acc;
        else if (o->sh == 1) { int64_t v2 = (int64_t)acc * 2; shifted = v2 < -0x800000 ? -0x800000 : v2 > 0x7FFFFF ? 0x7FFFFF : (int32_t)v2; }
        else if (o->sh == 2) shifted = scsp_sext((int32_t)((uint32_t)acc * 2u), 24);
        else                 shifted = scsp_sext(acc, 24);

        y = scsp_sext(y, 13);
        acc = (int32_t)(((int64_t)x * (int64_t)y) >> 12) + b;

        if (o->twt) d->temp[(o->twa + d->dec) & 0x7F] = shifted;
        if (o->frcl) frc = o->sh == 3 ? (shifted & 0x0FFF) : ((shifted >> 11) & 0x1FFF);

        if (o->mrd || o->mwt) {
            uint32_t addr = d->madrs[o->masa];
            if (!o->table) addr += d->dec;
            if (o->adreb)  addr += adrs & 0x0FFF;
            if (o->nxadr)  addr++;
            addr &= o->table ? 0xFFFFu : d->rbl - 1;
            addr += d->rbp << 12;
            addr <<= 1;
            /* MAME: the delay memory is only touched on odd steps */
            if (o->mrd && (st & 1)) memval = o->nofl ? scsp_ram_w(s, addr) << 8 : scsp_dsp_unpack(scsp_ram_w(s, addr));
            if (o->mwt && (st & 1)) scsp_ram_ww(s, addr, o->nofl ? (uint16_t)(shifted >> 8) : scsp_dsp_pack(shifted));
        }
        if (o->adrl) adrs = o->sh == 3 ? (uint32_t)((shifted >> 12) & 0xFFF) : (uint32_t)(inputs >> 16);
        if (o->ewt) d->efreg[o->ewa] = (int16_t)(d->efreg[o->ewa] + (shifted >> 8));
    }
    d->dec--;
    memset(d->mixs, 0, sizeof d->mixs);
}

/* ---- random chip state ------------------------------------------------------ */

static uint32_t s_rng = 1;
static uint32_t rnd(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng >> 8; }
static uint32_t rnd32(void) { return (rnd() << 16) ^ rnd(); }

#define RAM_MAX 0x80000u          /* the SCSP's 512 KB */

/* One microcode word set. Most scenarios read only valid inputs; a bad IRA
 * (0x32..0x3F, which ends the program) turns up in about one step in 200. */
static void random_step(uint16_t *p) {
    for (int k = 0; k < 4; k++) p[k] = (uint16_t)rnd();
    if ((p[1] >> 6 & 0x3F) > 0x31 && rnd() % 16) p[1] = (uint16_t)((p[1] & ~0x0FC0u) | ((rnd() % 0x32) << 6));
}

static void scenario(scsp_t *s, uint8_t *ram, unsigned seed) {
    s_rng = seed * 2654435761u + 7;
    memset(s, 0, sizeof *s);
    s->ram = ram;
    /* sound RAM: the full 512 KB, or smaller so ring addresses run off its end */
    s->ram_size = (seed % 4 == 0) ? RAM_MAX / 2 + (rnd() & 0xFFFE) : RAM_MAX;
    for (uint32_t i = 0; i < RAM_MAX; i++) ram[i] = (uint8_t)rnd();
    scsp_dsp_t *d = &s->dsp;
    for (int i = 0; i < 64; i++)  d->coef[i]  = (int16_t)rnd();
    for (int i = 0; i < 32; i++)  d->madrs[i] = (uint16_t)rnd();
    for (int i = 0; i < 128; i++) d->temp[i]  = scsp_sext((int32_t)rnd32(), 24);
    for (int i = 0; i < 32; i++)  d->mems[i]  = scsp_sext((int32_t)rnd32(), 24);
    d->dec = rnd32();
    d->rbl = (8u * 1024u) << (rnd() & 3);
    d->rbp = rnd() & 0x3F;
    int len = 1 + (int)(rnd() % 128);
    for (int st = 0; st < 128; st++) {
        if (st < len) random_step(d->mpro + st * 4);
        else          memset(d->mpro + st * 4, 0, 8);
    }
    scsp_dsp_start(d);
}

/* What the slots and the CD/EXTS inputs hand the DSP each sample. */
static void feed(scsp_t *a, scsp_t *b) {
    /* 32 slots of 16-bit samples at most, so within 22 bits either way */
    for (int i = 0; i < 16; i++) a->dsp.mixs[i] = b->dsp.mixs[i] = ((int32_t)(rnd32() & 0x3FFFFF) - 0x200000) >> (rnd() & 15);
    a->dsp.exts[0] = b->dsp.exts[0] = (int16_t)rnd();
    a->dsp.exts[1] = b->dsp.exts[1] = (int16_t)rnd();
}

static int same_state(const scsp_t *a, const scsp_t *b) {
    const scsp_dsp_t *x = &a->dsp, *y = &b->dsp;
    return !memcmp(x->efreg, y->efreg, sizeof x->efreg) && !memcmp(x->temp, y->temp, sizeof x->temp) &&
           !memcmp(x->mems, y->mems, sizeof x->mems) && !memcmp(x->mixs, y->mixs, sizeof x->mixs) &&
           x->dec == y->dec;
}

int main(void) {
    static scsp_t ours, ref;
    static uint8_t ram_ours[RAM_MAX], ram_ref[RAM_MAX];
    static ref_op_t ref_ops[128];
    int cut_short = 0, rewritten = 0, samples = 0;
    for (unsigned seed = 1; seed <= 400; seed++) {
        scenario(&ours, ram_ours, seed);
        ref = ours;
        ref.ram = ram_ref;
        memcpy(ram_ref, ram_ours, RAM_MAX);
        ref_dsp_decode(&ref.dsp, ref_ops);

        int first_bad = -1;
        for (int st = 0; st < ours.dsp.last_step && first_bad < 0; st++)
            if (((ours.dsp.mpro[st * 4 + 1] >> 6) & 0x3F) > 0x31) first_bad = st;
        if (first_bad >= 0) cut_short++;

        for (int n = 0; n < 48; n++) {
            /* a program change between samples, as the 68000 makes one: a
             * rewritten step (an MPRO write clears ops_ok), or a longer program
             * found by scsp_dsp_start without any decode being asked for */
            if (n == 24 && (seed & 1)) {
                int st = (int)(rnd() % 128);
                random_step(ours.dsp.mpro + st * 4);
                memcpy(ref.dsp.mpro + st * 4, ours.dsp.mpro + st * 4, 8);
                ours.dsp.ops_ok = 0;
                ref_dsp_decode(&ref.dsp, ref_ops);
                scsp_dsp_start(&ours.dsp);
                scsp_dsp_start(&ref.dsp);
                rewritten++;
            }
            if (n == 36 && seed % 5 == 0) {
                ours.dsp.last_step = ref.dsp.last_step = 1 + (int)(rnd() % 128);
                rewritten++;
            }
            feed(&ours, &ref);
            scsp_dsp_step(&ours);
            ref_dsp_step(&ref, ref_ops);
            samples++;
            if (!same_state(&ours, &ref)) {
                CHECK(0, "seed %u sample %d: DSP registers differ (last_step %d)", seed, n, ref.dsp.last_step);
                break;
            }
        }
        CHECK(memcmp(ram_ours, ram_ref, RAM_MAX) == 0, "seed %u: sound RAM (delay line) differs", seed);
        if (g_fail > 10) break;
    }
    CHECK(cut_short > 0, "some program was cut short by a bad IRA");
    printf("%s: %d samples of 400 random programs (%d cut short, %d changed mid-run), %s selects\n",
           g_fail ? "FAILED" : "ok", samples, cut_short, rewritten, SCSP_DSP_MASKS ? "mask" : "ternary");
    return g_fail ? 1 : 0;
}
