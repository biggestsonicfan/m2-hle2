/*
 * dc_sound.h -- the Dreamcast port's sound (Pinboard #342): no sound board, as
 * in Sega's own console release. A trap at sound_request_special (0x3F268)
 * takes the i960's sound code out of g0 and returns, and the code plays a cue
 * out of STF.AFS (dreamcast/tools/mksound.py), a CRI AFS of ADX files made from
 * the PS3 release's ADX2 bank. Sega's DLL does the same with its own table
 * (CLAUDE.md, "Sound board"); CUES.BIN, the AFS's entry 0, is that table.
 *
 *   category 5   music: one at a time, streamed off the disc
 *   category 2   effects and voices: in RAM, loaded at boot (~1.1 MB)
 *   category 0   a stop code: stops the looping cue of that entry
 *   0xA00001/2/3 stop all / music / effects; 0xA003xx fades music over xx frames
 *
 * The ADX decode to PCM and the mix happen in SDL2's audio callback (S16 stereo
 * at 44.1 kHz). The callback never reads the disc: the pager owns the drive and
 * reads it with interrupts off (dc_pager.h), so the music is read in the main
 * loop (ds_pump) into a ring of ADX bytes the callback drains.
 */
#ifndef DC_SOUND_H
#define DC_SOUND_H

#include <SDL2/SDL.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dc_pager.h"

#define DS_RATE      44100
#define DS_VOICES    16
#define DS_RING      (128u << 10)       /* ~2.6 s of 44.1 kHz stereo ADX */
#define DS_STAGE_SEC 8                  /* sectors per music read */
#define DS_CAT_STOP  0
#define DS_CAT_SE    2
#define DS_CAT_BGM   5

static inline uint32_t ds_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline uint16_t ds_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t ds_le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- ADX ------------------------------------------------------------------- */

/* One ADX's header, and a decoder over its frames: ch frames of 18 bytes (a
 * 16-bit scale, then 32 4-bit samples) per 32 samples, CRI's two-tap predictor
 * from the header's highpass cutoff. Exactly ffmpeg's adpcm_adx, which encoded
 * them. A looping file plays frames up to the one holding the loop end, then
 * goes back to the frame holding the loop start. */
typedef struct {
    uint32_t data, ch, rate, total;
    uint32_t loop, ls, le;
    int      c1, c2;
} adx_hdr_t;

static int adx_parse(const uint8_t *h, uint32_t size, adx_hdr_t *a) {
    if (size < 0x2C || ds_be16(h) != 0x8000 || h[4] != 3 || h[5] != 18 || h[6] != 4) return -1;
    a->data  = ds_be16(h + 2) + 4u;
    a->ch    = h[7];
    a->rate  = ds_be32(h + 8);
    a->total = ds_be32(h + 12);
    a->loop  = h[0x12] == 3 && ds_be32(h + 0x18);
    a->ls    = a->loop ? ds_be32(h + 0x1C) : 0;
    a->le    = a->loop ? ds_be32(h + 0x24) : a->total;
    if (a->ch < 1 || a->ch > 2 || !a->rate || a->le <= a->ls) return -1;
    double x = M_SQRT2 - cos(2.0 * M_PI * ds_be16(h + 16) / a->rate), y = M_SQRT2 - 1.0;
    double c = (x - sqrt((x + y) * (x - y))) / y;
    a->c1 = (int)lrint(c * 2.0 * 4096);
    a->c2 = (int)lrint(-(c * c) * 4096);
    return 0;
}

/* Frames from where the decoder is: the end of the first pass, and where a
 * loop goes back to. Both the decoder and the music reader walk this. */
static inline uint32_t adx_frames_end(const adx_hdr_t *a) { return (a->le + 31) / 32; }

typedef struct {
    const adx_hdr_t *h;
    int32_t  s1[2], s2[2];
    int16_t  pcm[32][2];
    uint32_t frame;                    /* the next frame to decode */
    uint8_t  pos, end;                 /* what is left of pcm */
    uint8_t  done;
    int16_t  prev[2], cur[2];          /* the resampler's two samples */
    uint32_t frac, step;               /* 16.16 */
} adx_dec_t;

static void adx_start(adx_dec_t *d, const adx_hdr_t *h) {
    memset(d, 0, sizeof *d);
    d->h = h;
    d->step = (uint32_t)(((uint64_t)h->rate << 16) / DS_RATE);
    d->frac = 1u << 16;                /* load a sample on the first output */
}

/* The next frame set's bytes (ch x 18), or NULL: not there yet (music). */
typedef const uint8_t *(*adx_fetch_fn)(void *src, const adx_dec_t *d);

static int adx_next(adx_dec_t *d, adx_fetch_fn fetch, void *src) {
    const adx_hdr_t *h = d->h;
    uint32_t lo = 0;
    if (d->frame == adx_frames_end(h)) {
        if (!h->loop) { d->done = 1; return 0; }
        d->frame = h->ls / 32;
        lo = h->ls % 32;
    }
    const uint8_t *f = fetch(src, d);
    if (!f) return 0;
    for (uint32_t c = 0; c < h->ch; c++, f += 18) {
        int scale = ds_be16(f);
        int32_t s1 = d->s1[c], s2 = d->s2[c];
        int32_t c1 = h->c1, c2 = h->c2;
        for (int i = 0; i < 32; i++) {
            int32_t s = (int32_t)((uint32_t)f[2 + i / 2] << (i & 1 ? 28 : 24)) >> 28;  /* signed nibble */
            s = s * scale + ((c1 * s1 + c2 * s2) >> 12);
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            s2 = s1; s1 = s;
            d->pcm[i][c] = (int16_t)s;
        }
        d->s1[c] = s1; d->s2[c] = s2;
    }
    if (h->ch == 1)
        for (int i = 0; i < 32; i++) d->pcm[i][1] = d->pcm[i][0];
    uint32_t base = d->frame * 32, hi = 32;
    if (base + 32 > h->le) hi = h->le - base;
    d->frame++;
    d->pos = (uint8_t)lo;
    d->end = (uint8_t)hi;
    return 1;
}

/* Adds n stereo samples at DS_RATE into acc, scaled by vol (16.16). Stops at
 * an underrun or the end. */
static void adx_mix(adx_dec_t *d, adx_fetch_fn fetch, void *src, int32_t *acc, int n, int32_t vol) {
    if (d->step == 1u << 16 && d->frac == 1u << 16) {
        /* At DS_RATE (the music) each output is the sample before the one it
         * loads, as below with f = 0, without the per-sample bookkeeping. */
        int i = 0;
        while (i < n && !d->done) {
            if (d->pos >= d->end && !adx_next(d, fetch, src)) return;
            int k = d->end - d->pos;
            if (k > n - i) k = n - i;
            const int16_t (*p)[2] = &d->pcm[d->pos];
            int32_t l = d->cur[0], r = d->cur[1];
            int32_t *o = acc + 2 * i;
            if (vol == 1 << 16)
                for (int j = 0; j < k; j++) { o[2 * j] += l; o[2 * j + 1] += r; l = p[j][0]; r = p[j][1]; }
            else
                for (int j = 0; j < k; j++) {
                    o[2 * j] += (l * vol) >> 16; o[2 * j + 1] += (r * vol) >> 16;
                    l = p[j][0]; r = p[j][1];
                }
            d->cur[0] = (int16_t)l; d->cur[1] = (int16_t)r;
            d->pos += k; i += k;
        }
        return;
    }
    for (int i = 0; i < n && !d->done; i++) {
        while (d->frac >= (1u << 16)) {
            if (d->pos >= d->end && !adx_next(d, fetch, src)) return;
            if (d->pos >= d->end) continue;
            d->prev[0] = d->cur[0]; d->prev[1] = d->cur[1];
            d->cur[0] = d->pcm[d->pos][0]; d->cur[1] = d->pcm[d->pos][1];
            d->pos++;
            d->frac -= 1u << 16;
        }
        int32_t f = (int32_t)d->frac;
        int32_t l = d->prev[0] + (((d->cur[0] - d->prev[0]) * f) >> 16);
        int32_t r = d->prev[1] + (((d->cur[1] - d->prev[1]) * f) >> 16);
        /* vol is at most 1 << 16, so a 16-bit sample times it fits 32 bits. */
        if (vol == 1 << 16) { acc[2 * i] += l; acc[2 * i + 1] += r; }
        else { acc[2 * i] += (l * vol) >> 16; acc[2 * i + 1] += (r * vol) >> 16; }
        d->frac += d->step;
    }
}

/* ---- The disc file ------------------------------------------------------------ */

typedef struct { uint32_t code; uint8_t cat; uint16_t entry; } ds_cue_t;

typedef struct {
    uint32_t  fad, nsec;               /* STF.AFS's first sector, its length */
    uint32_t  n;
    uint32_t *off, *size;              /* the AFS's entries */
    ds_cue_t *cue;
    uint32_t  ncue;
    uint8_t  **ram;                    /* an effect's ADX, in RAM; else NULL */
    adx_hdr_t *hdr;                    /* loaded ones' headers; music's when played */

    SDL_AudioDeviceID dev;
    adx_dec_t voice[DS_VOICES];
    uint16_t  voice_entry[DS_VOICES];
    uint32_t  voice_age[DS_VOICES], age;

    /* Music: the reader walks the file as the decoder will (adx_next) and puts
     * the bytes in the ring. */
    uint16_t  bgm;                     /* entry, or 0xFFFF */
    adx_hdr_t bgm_hdr;
    adx_dec_t bgm_dec;
    uint8_t  *ring;
    volatile uint32_t r_head, r_tail;  /* bytes in and out, free-running */
    uint32_t  rd_frame;                /* the reader's next frame */
    uint8_t  *stage;
    uint32_t  stage_sec, stage_n;      /* sectors in stage, from the entry's start */
    int32_t   bgm_vol, bgm_fade;       /* 16.16; per sample */
    uint8_t   frame_buf[36];           /* a frame set across the ring's wrap */

    uint32_t  codes, unknown, underruns;
} dc_sound_t;

static dc_sound_t g_ds;

/* Reads through KOS's driver: only before the pager is up (ds_init). */
static int ds_read_boot(void *dst, uint32_t fad, uint32_t nsec) {
    return cdrom_read_sectors(dst, fad, nsec) == ERR_OK ? 0 : -1;
}

static int ds_read_entry(uint32_t e, uint32_t from, uint32_t len, uint8_t *dst) {
    static uint8_t sec[2048] __attribute__((aligned(32)));
    uint32_t at = g_ds.off[e] + from;
    while (len) {
        uint32_t s = at / 2048, o = at % 2048, k = 2048 - o < len ? 2048 - o : len;
        if (!o && len >= 2048) {
            uint32_t ns = len / 2048;
            if (ds_read_boot(dst, g_ds.fad + s, ns)) return -1;
            k = ns * 2048;
        } else {
            if (ds_read_boot(sec, g_ds.fad + s, 1)) return -1;
            memcpy(dst, sec + o, k);
        }
        dst += k; at += k; len -= k;
    }
    return 0;
}

/* ---- The mix ---------------------------------------------------------------------- */

static const uint8_t *ds_fetch_ram(void *src, const adx_dec_t *d) {
    const uint8_t *p = src;
    return p + d->h->data + d->frame * 18 * d->h->ch;
}

static const uint8_t *ds_fetch_ring(void *src, const adx_dec_t *d) {
    (void)src;
    uint32_t fs = 18 * d->h->ch;
    if (g_ds.r_head - g_ds.r_tail < fs) { g_ds.underruns++; return NULL; }
    uint32_t t = g_ds.r_tail % DS_RING;
    const uint8_t *p = g_ds.ring + t;
    if (t + fs > DS_RING) {
        memcpy(g_ds.frame_buf, p, DS_RING - t);
        memcpy(g_ds.frame_buf + DS_RING - t, g_ds.ring, fs - (DS_RING - t));
        p = g_ds.frame_buf;
    }
    g_ds.r_tail += fs;
    return p;
}

static void ds_callback(void *user, Uint8 *stream, int len) {
    (void)user;
    static int32_t acc[2 * 4096];
    int n = len / 4;
    if (n > 4096) n = 4096;
    memset(acc, 0, (size_t)n * 8);
    if (g_ds.bgm != 0xFFFF && !g_ds.bgm_dec.done) {
        int32_t v = g_ds.bgm_vol;
        if (g_ds.bgm_fade) {
            v -= g_ds.bgm_fade * n;
            if (v <= 0) { v = 0; g_ds.bgm_dec.done = 1; }
            g_ds.bgm_vol = v;
        }
        adx_mix(&g_ds.bgm_dec, ds_fetch_ring, NULL, acc, n, v);
    }
    for (int i = 0; i < DS_VOICES; i++) {
        adx_dec_t *d = &g_ds.voice[i];
        if (d->h && !d->done)
            adx_mix(d, ds_fetch_ram, g_ds.ram[g_ds.voice_entry[i]], acc, n, 1 << 16);
    }
    int16_t *out = (int16_t *)stream;
    for (int i = 0; i < 2 * n; i++) {
        int32_t s = acc[i];
        out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    if (n * 4 < len) memset(stream + n * 4, 0, (size_t)(len - n * 4));
}

/* ---- Codes -------------------------------------------------------------------------- */

static void ds_stop_bgm(void) {
    g_ds.bgm = 0xFFFF;
    g_ds.bgm_fade = 0;
}

static void ds_stop_se(int entry) {
    for (int i = 0; i < DS_VOICES; i++)
        if (entry < 0 || g_ds.voice_entry[i] == entry) g_ds.voice[i].done = 1;
}

static void ds_play_se(uint16_t e) {
    if (e >= g_ds.n || !g_ds.ram[e]) return;
    int v = 0;
    for (int i = 0; i < DS_VOICES; i++) {
        if (!g_ds.voice[i].h || g_ds.voice[i].done) { v = i; break; }
        if (g_ds.voice_age[i] < g_ds.voice_age[v]) v = i;
    }
    adx_start(&g_ds.voice[v], &g_ds.hdr[e]);
    g_ds.voice_entry[v] = e;
    g_ds.voice_age[v] = ++g_ds.age;
}

static void ds_play_bgm(uint16_t e) {
    ds_stop_bgm();
    if (e >= g_ds.n || g_ds.hdr[e].rate == 0) return;
    g_ds.bgm_hdr = g_ds.hdr[e];
    adx_start(&g_ds.bgm_dec, &g_ds.bgm_hdr);
    g_ds.r_head = g_ds.r_tail = 0;
    g_ds.rd_frame = 0;
    g_ds.bgm_vol = 1 << 16;
    g_ds.bgm = e;
}

/* sound_request_special's code (the trap, on the emulation's thread). */
static void ds_code(uint32_t code) {
    if (!g_ds.dev) return;
    g_ds.codes++;
    SDL_LockAudioDevice(g_ds.dev);
    if (code == 0xA00001) { ds_stop_bgm(); ds_stop_se(-1); }
    else if (code == 0xA00002) ds_stop_bgm();
    else if (code == 0xA00003) ds_stop_se(-1);
    else if ((code & 0xFFFFFF00u) == 0xA00300) {
        uint32_t frames = code & 0xFF;
        if (!frames) ds_stop_bgm();
        else if (g_ds.bgm != 0xFFFF) g_ds.bgm_fade = (int32_t)((1u << 16) / (frames * DS_RATE / 60) + 1);
    } else {
        uint32_t lo = 0, hi = g_ds.ncue;
        while (lo < hi) {
            uint32_t m = (lo + hi) / 2;
            if (g_ds.cue[m].code < code) lo = m + 1; else hi = m;
        }
        if (lo == g_ds.ncue || g_ds.cue[lo].code != code) g_ds.unknown++;
        else if (g_ds.cue[lo].entry != 0xFFFF) {
            const ds_cue_t *c = &g_ds.cue[lo];
            if (c->cat == DS_CAT_BGM) ds_play_bgm(c->entry);
            else if (c->cat == DS_CAT_STOP) ds_stop_se(c->entry);
            else ds_play_se(c->entry);
        }
    }
    SDL_UnlockAudioDevice(g_ds.dev);
}

/* ---- The music's reader (main loop) ------------------------------------------------- */

/* Tops the ring up from the disc through the pager's reader. A music change
 * in between (the trap runs inside the slice, on this thread) only resets
 * the ring, so nothing here races it. */
static void ds_pump(void) {
    if (!g_ds.dev || g_ds.bgm == 0xFFFF) return;
    const adx_hdr_t *h = &g_ds.bgm_hdr;
    uint32_t e = g_ds.bgm, fs = 18 * h->ch, end = adx_frames_end(h);
    for (int rounds = 0; rounds < 4; rounds++) {
        if (g_ds.rd_frame == end) {
            if (!h->loop) return;
            g_ds.rd_frame = h->ls / 32;
        }
        uint32_t room = DS_RING - (g_ds.r_head - g_ds.r_tail);
        if (room < DS_STAGE_SEC * 2048) return;
        uint32_t at = h->data + g_ds.rd_frame * fs;
        uint32_t want = (end - g_ds.rd_frame) * fs;
        uint32_t s = (g_ds.off[e] + at) / 2048;
        if (!(g_ds.stage_n && s >= g_ds.stage_sec && s < g_ds.stage_sec + g_ds.stage_n)) {
            uint32_t ns = g_ds.nsec - s < DS_STAGE_SEC ? g_ds.nsec - s : DS_STAGE_SEC;
            if (pg_read(g_ds.stage, g_ds.fad + s, ns)) return;
            g_ds.stage_sec = s;
            g_ds.stage_n = ns;
        }
        uint32_t o = g_ds.off[e] + at - g_ds.stage_sec * 2048;
        uint32_t k = g_ds.stage_n * 2048 - o;
        if (k > want) k = want;
        if (k > room) k = room;
        k -= k % fs;
        if (!k) {                      /* a frame across the stage's end: next time */
            g_ds.stage_n = 0;
            continue;
        }
        SDL_LockAudioDevice(g_ds.dev);
        if (g_ds.bgm == e) {
            for (uint32_t i = 0; i < k; i++)
                g_ds.ring[(g_ds.r_head + i) % DS_RING] = g_ds.stage[o + i];
            g_ds.r_head += k;
            g_ds.rd_frame += k / fs;
        }
        SDL_UnlockAudioDevice(g_ds.dev);
    }
}

/* ---- Boot ------------------------------------------------------------------------- */

/* Before pg_init: reads the AFS's table and CUES.BIN, loads every effect into
 * RAM and every music header, and opens the audio device. Returns 0, or -1
 * with no sound (no STF.AFS, no audio device): the board runs either way. */
static int ds_init(void) {
    static uint8_t sec[2048] __attribute__((aligned(32)));
    uint32_t afs_size = 0;
    g_ds.bgm = 0xFFFF;
    if (!(g_ds.fad = pg_find_file("STF.AFS", &afs_size))) return -1;
    g_ds.nsec = (afs_size + 2047) / 2048;
    if (ds_read_boot(sec, g_ds.fad, 1) || memcmp(sec, "AFS", 4)) return -1;
    g_ds.n = ds_le32(sec + 4);
    if (g_ds.n < 2 || 8 + 8 * g_ds.n > 2048) return -1;
    g_ds.off  = calloc(g_ds.n, 4);
    g_ds.size = calloc(g_ds.n, 4);
    g_ds.ram  = calloc(g_ds.n, sizeof *g_ds.ram);
    g_ds.hdr  = calloc(g_ds.n, sizeof *g_ds.hdr);
    for (uint32_t i = 0; i < g_ds.n; i++) {
        g_ds.off[i]  = ds_le32(sec + 8 + 8 * i);
        g_ds.size[i] = ds_le32(sec + 12 + 8 * i);
    }
    uint8_t *cues = malloc(g_ds.size[0] + 2048);
    if (!cues || ds_read_entry(0, 0, g_ds.size[0], cues) || memcmp(cues, "STFC", 4)) return -1;
    g_ds.ncue = ds_be32(cues + 4);
    g_ds.cue = calloc(g_ds.ncue, sizeof *g_ds.cue);
    for (uint32_t i = 0; i < g_ds.ncue; i++) {
        const uint8_t *c = cues + 8 + 8 * i;
        g_ds.cue[i] = (ds_cue_t){ ds_be32(c), c[4], ds_be16(c + 6) };
    }
    free(cues);
    uint32_t ram = 0;
    for (uint32_t i = 0; i < g_ds.ncue; i++) {
        uint16_t e = g_ds.cue[i].entry;
        if (e == 0xFFFF || e >= g_ds.n || g_ds.hdr[e].rate) continue;
        uint8_t h[0x30];
        if (ds_read_entry(e, 0, sizeof h, h) || adx_parse(h, sizeof h, &g_ds.hdr[e])) continue;
        if (g_ds.cue[i].cat != DS_CAT_SE) continue;
        uint8_t *p = malloc(g_ds.size[e] + 64);   /* a last partial frame reads past */
        if (!p || ds_read_entry(e, 0, g_ds.size[e], p)) { free(p); g_ds.hdr[e].rate = 0; continue; }
        g_ds.ram[e] = p;
        ram += g_ds.size[e];
    }
    g_ds.ring  = malloc(DS_RING);
    g_ds.stage = memalign(32, DS_STAGE_SEC * 2048);
    if (!g_ds.ring || !g_ds.stage) return -1;

    if (SDL_Init(SDL_INIT_AUDIO) != 0) { printf("SDL audio: %s\n", SDL_GetError()); return -1; }
    SDL_AudioSpec want = { .freq = DS_RATE, .format = AUDIO_S16LSB, .channels = 2,
                           .samples = 2048, .callback = ds_callback }, have;
    g_ds.dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_ds.dev) { printf("SDL audio: %s\n", SDL_GetError()); return -1; }
    SDL_PauseAudioDevice(g_ds.dev, 0);
    printf("sound: %u cues, %u ADX, effects %u KB in RAM\n", (unsigned)g_ds.ncue,
           (unsigned)g_ds.n - 1, (unsigned)(ram >> 10));
    return 0;
}

#endif /* DC_SOUND_H */
