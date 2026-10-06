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
 * The AICA plays the effects itself (Pinboard #394): at boot each one's ADX is
 * decoded and written again as the AICA's 4-bit ADPCM into sound RAM, and a
 * cue starts it on a channel of its own. Nothing is mixed on the SH-4, and the
 * effects' 1.1 MB stay out of main RAM. A channel holds at most 65534
 * samples, so the four effects longer than that go at half their rate (or less).
 *
 * The music stays ADX, decoded on the SH-4 into a KOS stream (S16 stereo at
 * the file's 44.1 kHz) that a thread of ours tops up. The stream's callback
 * never reads the disc: the pager owns the drive and reads it with
 * interrupts off (dc_pager.h), so the music is read in the main loop
 * (ds_pump) into a ring of ADX bytes the callback drains.
 */
#ifndef DC_SOUND_H
#define DC_SOUND_H

#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <kos/mutex.h>
#include <kos/thread.h>
#include <arch/timer.h>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dc_pager.h"

#define DS_RATE      44100
#define DS_VOICES    16
#define DS_AICA_MAX  65534u             /* samples a channel can play */
#define DS_STREAM    (32u << 10)        /* the stream's buffer per channel, bytes */
#define DS_RING      (128u << 10)       /* ~2.6 s of 44.1 kHz stereo ADX */
_Static_assert((DS_RING & (DS_RING - 1u)) == 0, "DS_RING: a power of two");
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
} adx_dec_t;

static void adx_start(adx_dec_t *d, const adx_hdr_t *h) {
    memset(d, 0, sizeof *d);
    d->h = h;
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
    if (!f) {   /* not read yet: at the seam, wrap again next time, or the loop start's offset is lost */
        if (lo) d->frame = adx_frames_end(h);
        return 0;
    }
    for (uint32_t c = 0; c < h->ch; c++, f += 18) {
        int scale = ds_be16(f);
        int32_t s1 = d->s1[c], s2 = d->s2[c];
        int32_t c1 = h->c1, c2 = h->c2;
        int16_t *o = &d->pcm[0][c];
        for (int i = 0; i < 16; i++) {
            int32_t b = (int32_t)((uint32_t)f[2 + i] << 24);
            int32_t s = (b >> 28) * scale + ((c1 * s1 + c2 * s2) >> 12);  /* high nibble first */
            s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
            s2 = s1; s1 = s; o[0] = (int16_t)s;
            s = ((b << 4) >> 28) * scale + ((c1 * s1 + c2 * s2) >> 12);
            s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
            s2 = s1; s1 = s; o[2] = (int16_t)s;
            o += 4;
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

/* Up to n stereo samples into out (S16, interleaved) at the file's own rate:
 * how many, short of n only at an underrun or the end. */
static int adx_out(adx_dec_t *d, adx_fetch_fn fetch, void *src, int16_t *out, int n) {
    int i = 0;
    while (i < n && !d->done) {
        if (d->pos >= d->end && !adx_next(d, fetch, src)) break;
        int k = d->end - d->pos;
        if (k > n - i) k = n - i;
        memcpy(out + 2 * i, d->pcm[d->pos], (size_t)k * 4);
        d->pos += k; i += k;
    }
    return i;
}

/* ---- The disc file ------------------------------------------------------------ */

typedef struct { uint32_t code; uint8_t cat; uint16_t entry; } ds_cue_t;

/* An effect as the AICA plays it: ADPCM in sound RAM, in samples. */
typedef struct { sfxhnd_t h; uint32_t rate, len, ls, le; uint8_t loop; } ds_se_t;

typedef struct {
    uint32_t  fad, nsec;               /* STF.AFS's first sector, its length */
    uint32_t  n;
    uint32_t *off, *size;              /* the AFS's entries */
    ds_cue_t *cue;
    uint32_t  ncue;
    ds_se_t  *se;                      /* an effect in sound RAM; else .h 0 */
    adx_hdr_t *hdr;                    /* loaded ones' headers; music's when played */

    int       on;
    snd_stream_hnd_t stream;
    mutex_t   mx;                      /* the music, between the stream's thread and ours */
    int       voice_chn[DS_VOICES];    /* the AICA channels the effects play on */
    uint16_t  voice_entry[DS_VOICES];
    uint64_t  voice_end[DS_VOICES];    /* ms it stops by itself; ~0 looping */
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

/* The stream's data: n bytes of S16 stereo. The music, its fade, else
 * silence; an underrun is silence too, the stream goes on. */
static void *ds_stream_cb(snd_stream_hnd_t hnd, int req, int *got) {
    (void)hnd;
    static int16_t out[DS_STREAM] __attribute__((aligned(32)));
    int n = req / 4, k = 0;
    if (n > (int)(DS_STREAM / 2)) n = DS_STREAM / 2;
    mutex_lock(&g_ds.mx);
    if (g_ds.bgm != 0xFFFF && !g_ds.bgm_dec.done) {
        k = adx_out(&g_ds.bgm_dec, ds_fetch_ring, NULL, out, n);
        if (g_ds.bgm_fade) {
            int32_t v = g_ds.bgm_vol;
            for (int i = 0; i < k; i++) {
                out[2 * i] = (int16_t)((out[2 * i] * v) >> 16);
                out[2 * i + 1] = (int16_t)((out[2 * i + 1] * v) >> 16);
                if ((v -= g_ds.bgm_fade) <= 0) { v = 0; g_ds.bgm_dec.done = 1; k = i + 1; break; }
            }
            g_ds.bgm_vol = v;
        }
    }
    mutex_unlock(&g_ds.mx);
    if (k < n) memset(out + 2 * k, 0, (size_t)(n - k) * 4);
    *got = n * 4;
    return out;
}

static void *ds_thread(void *arg) {
    (void)arg;
    for (;;) {
        snd_stream_poll(g_ds.stream);
        thd_sleep(10);
    }
    return NULL;
}

/* ---- Codes -------------------------------------------------------------------------- */

static void ds_stop_bgm(void) {
    g_ds.bgm = 0xFFFF;
    g_ds.bgm_fade = 0;
}

static void ds_stop_se(int entry) {
    for (int i = 0; i < DS_VOICES; i++)
        if (g_ds.voice_end[i] && (entry < 0 || g_ds.voice_entry[i] == entry)) {
            snd_sfx_stop(g_ds.voice_chn[i]);
            g_ds.voice_end[i] = 0;
        }
}

static void ds_play_se(uint16_t e) {
    if (e >= g_ds.n || !g_ds.se[e].h) return;
    const ds_se_t *se = &g_ds.se[e];
    uint64_t now = timer_ms_gettime64();
    int v = 0;
    for (int i = 0; i < DS_VOICES; i++) {
        if (g_ds.voice_end[i] <= now) { v = i; break; }
        if (g_ds.voice_age[i] < g_ds.voice_age[v]) v = i;
    }
    sfx_play_data_t p = { .chn = g_ds.voice_chn[v], .idx = se->h, .vol = 255, .pan = 128,
                          .loop = se->loop, .loopstart = se->ls, .loopend = se->le };
    snd_sfx_play_ex(&p);
    g_ds.voice_entry[v] = e;
    g_ds.voice_end[v] = se->loop ? ~(uint64_t)0 : now + (uint64_t)se->len * 1000u / se->rate + 1u;
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
    if (!g_ds.on) return;
    g_ds.codes++;
    /* The lock is the music's (the stream's thread decodes it); the effects'
     * voices are this thread's alone, and their AICA calls stay outside it. */
    if (code == 0xA00001) { mutex_lock(&g_ds.mx); ds_stop_bgm(); mutex_unlock(&g_ds.mx); ds_stop_se(-1); }
    else if (code == 0xA00002) { mutex_lock(&g_ds.mx); ds_stop_bgm(); mutex_unlock(&g_ds.mx); }
    else if (code == 0xA00003) ds_stop_se(-1);
    else if ((code & 0xFFFFFF00u) == 0xA00300) {
        uint32_t frames = code & 0xFF;
        mutex_lock(&g_ds.mx);
        if (!frames) ds_stop_bgm();
        else if (g_ds.bgm != 0xFFFF) g_ds.bgm_fade = (int32_t)((1u << 16) / (frames * g_ds.bgm_hdr.rate / 60) + 1);
        mutex_unlock(&g_ds.mx);
    } else {
        uint32_t lo = 0, hi = g_ds.ncue;
        while (lo < hi) {
            uint32_t m = (lo + hi) / 2;
            if (g_ds.cue[m].code < code) lo = m + 1; else hi = m;
        }
        if (lo == g_ds.ncue || g_ds.cue[lo].code != code) g_ds.unknown++;
        else if (g_ds.cue[lo].entry != 0xFFFF) {
            const ds_cue_t *c = &g_ds.cue[lo];
            if (c->cat == DS_CAT_BGM) { mutex_lock(&g_ds.mx); ds_play_bgm(c->entry); mutex_unlock(&g_ds.mx); }
            else if (c->cat == DS_CAT_STOP) ds_stop_se(c->entry);
            else ds_play_se(c->entry);
        }
    }
}

/* ---- The music's reader (main loop) ------------------------------------------------- */

/* Tops the ring up from the disc through the pager's reader. A music change
 * in between (the trap runs inside the slice, on this thread) only resets
 * the ring, so nothing here races it. */
static void ds_pump(void) {
    if (!g_ds.on || g_ds.bgm == 0xFFFF) return;
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
        mutex_lock(&g_ds.mx);
        if (g_ds.bgm == e) {   /* two copies, across the ring's wrap: the stream waits on this lock */
            uint32_t h0 = g_ds.r_head & (DS_RING - 1u), k0 = DS_RING - h0 < k ? DS_RING - h0 : k;
            memcpy(g_ds.ring + h0, g_ds.stage + o, k0);
            memcpy(g_ds.ring, g_ds.stage + o + k0, k - k0);
            g_ds.r_head += k;
            g_ds.rd_frame += k / fs;
        }
        mutex_unlock(&g_ds.mx);
    }
}

/* ---- Boot ------------------------------------------------------------------------- */

/* The AICA's ADPCM (Yamaha's 4-bit, low nibble first), the encoder the
 * hardware's decoder undoes: ffmpeg's adpcm_yamaha. */
static void ds_adpcm(const int16_t *pcm, uint32_t n, uint8_t *out) {
    static const int16_t scale[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
    int32_t pred = 0, step = 127;
    for (uint32_t i = 0; i < n; i++) {
        int32_t d = pcm[i] - pred, a = d < 0 ? -d : d;
        int32_t q = a * 4 / step;
        if (q > 7) q = 7;
        int32_t v = (step * (2 * q + 1)) >> 3;
        pred += d < 0 ? -v : v;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        step = (step * scale[q]) >> 8;
        step = step < 127 ? 127 : step > 24576 ? 24576 : step;
        uint8_t nib = (uint8_t)(q | (d < 0 ? 8 : 0));
        if (i & 1) out[i >> 1] |= (uint8_t)(nib << 4); else out[i >> 1] = nib;
    }
}

/* An effect's ADX (whole, in buf) into sound RAM as ADPCM: the samples up to
 * its loop end, at half the rate if that is more than a channel holds. */
static int ds_load_se(uint16_t e, const uint8_t *buf) {
    const adx_hdr_t *h = &g_ds.hdr[e];
    uint32_t n = h->le, rate = h->rate, ls = h->ls;
    int16_t *pcm = malloc((size_t)n * 2 + 8);
    uint8_t *ad = memalign(32, n / 2 + 8);
    if (!pcm || !ad) { free(pcm); free(ad); return -1; }
    adx_dec_t d;
    adx_start(&d, h);
    uint32_t k = 0;
    while (k < n && d.frame < adx_frames_end(h) && adx_next(&d, ds_fetch_ram, (void *)buf))
        for (; d.pos < d.end && k < n; d.pos++) pcm[k++] = d.pcm[d.pos][0];
    n = k;
    while (n > DS_AICA_MAX) {   /* halve until it fits: one halving holds only twice the limit */
        for (uint32_t i = 0; i < n / 2; i++) pcm[i] = (int16_t)((pcm[2 * i] + pcm[2 * i + 1]) >> 1);
        n /= 2; rate /= 2; ls /= 2;
    }
    n &= ~7u;                          /* whole 32-bit words of nibbles */
    ds_adpcm(pcm, n, ad);
    free(pcm);
    sfxhnd_t sh = n ? snd_sfx_load_raw_buf((char *)ad, n / 2, rate, 4, 1) : SFXHND_INVALID;
    free(ad);
    if (sh == SFXHND_INVALID) return -1;
    g_ds.se[e] = (ds_se_t){ sh, rate, n, ls < n ? ls : 0, n, (uint8_t)(h->loop != 0) };
    return 0;
}

/* ds_init gave up: give back what it took (main RAM is 16 MB). */
static int ds_init_fail(void) {
    if (g_ds.se)
        for (uint32_t i = 0; i < g_ds.n; i++)
            if (g_ds.se[i].h) snd_sfx_unload(g_ds.se[i].h);
    for (int i = 0; i < DS_VOICES; i++)
        if (g_ds.voice_chn[i] >= 0) snd_sfx_chn_free(g_ds.voice_chn[i]);
    free(g_ds.off); free(g_ds.size); free(g_ds.se); free(g_ds.hdr); free(g_ds.cue);
    free(g_ds.ring); free(g_ds.stage);
    memset(&g_ds, 0, sizeof g_ds);
    g_ds.bgm = 0xFFFF;
    for (int i = 0; i < DS_VOICES; i++) g_ds.voice_chn[i] = -1;
    return -1;
}

/* Before pg_init: reads the AFS's table and CUES.BIN, loads every effect into
 * RAM and every music header, and opens the audio device. Returns 0, or -1
 * with no sound (no STF.AFS, no audio device): the board runs either way. */
static int ds_init(void) {
    static uint8_t sec[2048] __attribute__((aligned(32)));
    uint32_t afs_size = 0;
    g_ds.bgm = 0xFFFF;
    for (int i = 0; i < DS_VOICES; i++) g_ds.voice_chn[i] = -1;
    if (!(g_ds.fad = pg_find_file("STF.AFS", &afs_size))) return -1;
    g_ds.nsec = (afs_size + 2047) / 2048;
    if (ds_read_boot(sec, g_ds.fad, 1) || memcmp(sec, "AFS", 4)) return -1;
    g_ds.n = ds_le32(sec + 4);
    if (g_ds.n < 2 || g_ds.n > (2048 - 8) / 8) return -1;
    g_ds.off  = calloc(g_ds.n, 4);
    g_ds.size = calloc(g_ds.n, 4);
    g_ds.se   = calloc(g_ds.n, sizeof *g_ds.se);
    g_ds.hdr  = calloc(g_ds.n, sizeof *g_ds.hdr);
    if (!g_ds.off || !g_ds.size || !g_ds.se || !g_ds.hdr) return ds_init_fail();
    for (uint32_t i = 0; i < g_ds.n; i++) {
        g_ds.off[i]  = ds_le32(sec + 8 + 8 * i);
        g_ds.size[i] = ds_le32(sec + 12 + 8 * i);
    }
    uint8_t *cues = malloc(g_ds.size[0] + 2048);
    if (!cues || g_ds.size[0] < 8 || ds_read_entry(0, 0, g_ds.size[0], cues) || memcmp(cues, "STFC", 4)) {
        free(cues);
        return ds_init_fail();
    }
    g_ds.ncue = ds_be32(cues + 4);
    if (g_ds.ncue > (g_ds.size[0] - 8) / 8 || !(g_ds.cue = calloc(g_ds.ncue ? g_ds.ncue : 1, sizeof *g_ds.cue))) {
        free(cues);
        return ds_init_fail();
    }
    for (uint32_t i = 0; i < g_ds.ncue; i++) {
        const uint8_t *c = cues + 8 + 8 * i;
        g_ds.cue[i] = (ds_cue_t){ ds_be32(c), c[4], ds_be16(c + 6) };
    }
    free(cues);
    mutex_init(&g_ds.mx, MUTEX_TYPE_NORMAL);
    /* KOS splits a stereo block into two buffers half its size apart: at its
     * default 64 KB that is 32 KB, the same line of the SH-4's 16 KB direct-
     * mapped cache, so every store of the split missed and wrote one back
     * (Pinboard #513). 48 KB puts them half a cache apart and still holds the
     * most a poll asks for, half of DS_STREAM a channel. */
    if (snd_stream_init_ex(2, 48u << 10) < 0) return ds_init_fail();
    for (int i = 0; i < DS_VOICES; i++)
        if ((g_ds.voice_chn[i] = snd_sfx_chn_alloc()) < 0) return ds_init_fail();
    uint32_t ram = 0;
    for (uint32_t i = 0; i < g_ds.ncue; i++) {
        uint16_t e = g_ds.cue[i].entry;
        if (e == 0xFFFF || e >= g_ds.n || g_ds.hdr[e].rate) continue;
        uint8_t h[0x30];
        if (ds_read_entry(e, 0, sizeof h, h) || adx_parse(h, sizeof h, &g_ds.hdr[e])) continue;
        if (g_ds.cue[i].cat != DS_CAT_SE) continue;
        uint8_t *p = malloc(g_ds.size[e] + 64);   /* a last partial frame reads past */
        if (!p || ds_read_entry(e, 0, g_ds.size[e], p) || ds_load_se(e, p)) g_ds.hdr[e].rate = 0;
        else ram += g_ds.se[e].len / 2;
        free(p);
    }
    g_ds.ring  = malloc(DS_RING);
    g_ds.stage = memalign(32, DS_STAGE_SEC * 2048);
    if (!g_ds.ring || !g_ds.stage) return ds_init_fail();

    if ((g_ds.stream = snd_stream_alloc(ds_stream_cb, DS_STREAM)) < 0) return ds_init_fail();
    snd_stream_start(g_ds.stream, DS_RATE, 1);
    g_ds.on = 1;
    thd_create(1, ds_thread, NULL);
    printf("sound: %u cues, %u ADX, effects %u KB in sound RAM\n", (unsigned)g_ds.ncue,
           (unsigned)g_ds.n - 1, (unsigned)(ram >> 10));
    return 0;
}

#endif /* DC_SOUND_H */
