/*
 * sound.h — the Model 2 sound board: a 68000 running the game's own sound
 * driver, an SCSP (scsp.h), the 512 KB of sound RAM they share, and the sample
 * ROMs. The same on Model 2A, 2B and 2C (MAME model2.cpp, model2_snd).
 *
 * 68000 address map:
 *   0x000000-0x07FFFF  sound RAM, shared with the SCSP (vectors copied in at reset)
 *   0x100000-0x100FFF  SCSP registers
 *   0x400000-0x400001  sound control (bit 5 picks the sample banks, for sets > 8 MB)
 *   0x600000-0x67FFFF  sound program ROM
 *   0x800000-0x9FFFFF  sample ROM, first 2 MB
 *   0xA00000-0xDFFFFF  bank 4: sample ROM +2 MB (or +8 MB)
 *   0xE00000-0xFFFFFF  bank 5: sample ROM +6 MB (or +10 MB)
 * The SCSP sees only the sound RAM, so the driver copies what it plays out of
 * the sample ROMs — and streams long samples 8 KB at a time, timed off the
 * SCSP's play position. STF's driver keeps three quarters of its 602 samples
 * above 0xA00000.
 *
 * The i960 talks to the board through a UART (0x9C0000 data, 0x9C0004
 * control/status) that sends each byte down a MIDI-rate serial line to the
 * SCSP's own receiver, which puts it in its MIDI input buffer 9.5 bit times
 * later (sound_uart_t, below).
 *
 * Time: the board runs in samples. Each output sample the 68000 gets 256 clock
 * periods (11.2896 MHz / 44.1 kHz, counted per bus access in m68k_exec.h), then
 * the SCSP produces the sample. The 68000 takes the SCSP's interrupt lines
 * between instructions, and every register access lands on a chip that is at
 * exactly that point in time — the driver's timing loops and its slot-monitor
 * polling depend on it. The emu thread hands a frame's worth of samples to
 * the sound thread when the game's frame ends (emu_thread.h,
 * emu_sound_slice_end; "The sound thread" below) and the host audio callback
 * (core/audio_out.h) drains them.
 */
#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "../core/build_features.h"
#include "log.h"
#include "memory.h"
#include "m68k.h"
#include "m68k_exec.h"
#include "scsp.h"
#include "emu_times.h"   /* host time spent here, for get_status */
#include "thread_mutex.h" /* the sound thread */

#define SOUND_RAM_SIZE            0x80000u
#define SOUND_RATE                44100u
#define SOUND_CYCLES_PER_SAMPLE   256
#ifndef SOUND_IPL_LEAD
#define SOUND_IPL_LEAD            4         /* see sound_run */
#endif
#define SOUND_OUT_FRAMES          16384u   /* host output ring, stereo frames (power of 2) */
#define SOUND_CODE_LOG            512u     /* i960 commands kept (power of 2) */
#define SOUND_AHEAD_STEP          16       /* samples per catch-up step (see sound_uart_make_room) */
#define SOUND_AHEAD_MAX           735      /* at most a frame of samples run ahead of it (44100 / 60) */

/* The sound board runs on a thread of its own ("The sound thread", below)
 * everywhere but the web build, which has one thread. */
#if !defined(__EMSCRIPTEN__) && !defined(M2HLE_NO_SOUND_THREAD)
#define SOUND_THREAD 1
#else
#define SOUND_THREAD 0
#endif

/* Publishing across threads: the output ring's write index (sound thread to
 * the audio callback) and the sound thread's "done" flag. A volatile store is
 * enough on x86, not on the handheld's ARM. MSVC gives volatile these
 * semantics itself (/volatile:ms, x86 and x64). */
#if defined(__GNUC__) || defined(__clang__)
#define SOUND_STORE_RELEASE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define SOUND_LOAD_ACQUIRE(x)     __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#else
#define SOUND_STORE_RELEASE(x, v) ((x) = (v))
#define SOUND_LOAD_ACQUIRE(x)     (x)
#endif

/* ---- the sound UART and its serial line ------------------------------------
 *
 * The i960's UART is a uPD71051 (an i8251) clocked at 16 x 31250 baud, and its
 * TxD runs into the SCSP's serial receiver (MAME model2.cpp model2_scsp: the
 * uart's txd_handler is scsp_device::midi_in). A byte the i960 writes does not
 * appear in the SCSP's MIDI buffer: it goes out as a start bit, eight data
 * bits and a stop bit, 32 us each, and the SCSP takes it when it samples the
 * stop bit. Measured off MAME's serial logging (diserial LOG_TX / LOG_RX):
 *   - the UART's bit clock is free-running from power-on, and its ticks fall
 *     at 18 us mod 32 us of board time (the i8251 counts its x16 clock's
 *     falling edges from reset);
 *   - a byte written to an idle UART starts at the next tick after the write;
 *   - the UART holds ONE more byte, which starts the tick the line frees (10
 *     bits after the previous start), and TxRDY -- the i960's interrupt -- is
 *     up whenever that holding register is empty;
 *   - the SCSP's receiver samples each bit 1.5 bit times after its edge, so
 *     the byte lands 9.5 bit times after the start bit: 304 us.
 * So a three-byte command takes about a millisecond to arrive, and in the
 * MAME capture its bytes are 320 us apart, because the i960 sends the third
 * on the TxRDY interrupt the second byte's start raises. Before this the byte
 * went straight into the SCSP's buffer, 4 us after the i960 wrote it, and
 * commands landed ~1 ms early against MAME.
 *
 * Time on the line is kept in units of 1/625 of a 68000 clock period, in
 * which a microsecond is exactly 7056: integers only, so two builds agree.
 * The bytes wait in a 32-deep queue rather than the chip's one holding
 * register: the run loop only offers the i960 its interrupt when TxRDY is up
 * (emu_thread.h emu_sound_ready), so the game never sees more than the chip
 * holds, and any other writer's burst goes out back to back instead of being
 * overwritten. */
#define SOUND_UART_FIFO   32u
#define SOUND_SER_UNIT    625u                     /* units per 68000 clock period */
#define SOUND_SER_US      7056u                    /* 1 us = 11.2896 clock periods */
#define SOUND_SER_BIT     (32u * SOUND_SER_US)     /* 31250 baud */
#define SOUND_SER_PHASE   (18u * SOUND_SER_US)     /* the bit ticks, mod SOUND_SER_BIT */
#define SOUND_SER_FRAME   (10u * SOUND_SER_BIT)    /* start, eight data bits, stop */
#define SOUND_SER_RX      (19u * SOUND_SER_BIT / 2u)   /* start bit to the SCSP's stop-bit sample */

typedef struct {
    uint8_t  byte[SOUND_UART_FIFO];
    uint64_t wrote[SOUND_UART_FIFO];  /* units: when the i960 wrote it */
    uint8_t  r, w, count;             /* [r] is the byte on the line */
    uint64_t start;                   /* units: the byte on the line began its start bit */
    uint64_t deliver;                 /* clock period the SCSP takes it, 0 = taken */
    uint64_t sent;                    /* bytes the SCSP has taken */
    uint64_t drops;                   /* bytes written to a full queue */
} sound_uart_t;

typedef struct {
    m68k_state_t   m68k;
    scsp_t         scsp;
    uint8_t        ram[SOUND_RAM_SIZE];
    uint8_t        rom[M68K_ROM_SIZE];
    bool           rom_loaded;
    bool           detached;            /* sound_detach: the host has taken the board off (heat) */
    const uint8_t *samples;             /* the whole sample set (romset.samples, not owned) */
    uint32_t       samples_size;
    uint32_t       bank4, bank5;        /* offsets into samples for 0xA00000 / 0xE00000 */
    int32_t        budget;              /* 68000 clock periods owed to the current sample */
    uint32_t       slice_frac;          /* sound_run_slice's remainder: part of the board, reset with it */
    int32_t        ahead;               /* samples run inside the slice, owed back by sound_run_slice */
    sound_uart_t   uart;                /* the i960's UART and the serial line to the SCSP */
    uint64_t       irqs[8];             /* interrupts taken, by level */

    /* host output: emu thread writes, audio thread reads */
    int16_t          out[SOUND_OUT_FRAMES * 2];
    volatile uint32_t out_w, out_r;
    uint64_t         out_dropped;      /* grows by design while nobody reads the ring (g_sound_out_reader) */
    /* Every sample the board has ever produced, whether the ring took it or
     * not. The one clock a consumer outside the ring can trust, and monotonic
     * across a board reset — sound_reset() leaves it, and the ring, alone. */
    uint64_t         out_total;
    /* out_total once every run already handed out has finished: what the emu
     * thread may read while the sound thread is still producing (the frame
     * clock). And out_pub, the ring's write index as far as runs are known to
     * be finished -- for a host that drains the ring on the emu thread's own
     * thread (libretro), so it takes whole slices. Both written by the emu
     * thread only (sound_advance). */
    uint64_t         out_due;
    uint32_t         out_pub;
    uint64_t         midi_drains;      /* catch-up steps run to make room in the MIDI ring */
    uint64_t         midi_holds;       /* times a byte had to wait for the next slice */

    /* i960 side */
    uint64_t write_count, read_count;
    bool     log_writes;
    uint8_t  midi_log[64];
    uint32_t midi_log_ip[64];
    uint32_t midi_log_n;

    /* Every command the i960 has sent, framed the way the driver frames them:
     * the codes are MIDI messages (0xAE1004 = status 0xAE, data 0x10 0x04), so
     * a byte with bit 7 set opens one and two data bytes close it. A command
     * cut short by another status byte is logged with bit 31 set. Stamped with
     * out_total, the board's sample clock. Across a sound_reset. */
    struct { uint32_t code; uint64_t sample; } code_log[SOUND_CODE_LOG];
    uint32_t code_n;                   /* commands ever logged; the ring holds the last SOUND_CODE_LOG */
    uint32_t code_acc;
    int      code_have;                /* bytes of the open command, 0 = none open */
    uint32_t queue_hi;                 /* deepest the game's own command queue has been (32 = full: it drops) */
} sound_state_t;

static sound_state_t g_sound;

/* Wait for the sound thread to finish what it was handed ("The sound thread",
 * below). Anything that reads or changes the sound board from the i960's side
 * calls it first. */
static inline void sound_settle(void);
static void sound_scsp_sink(void *ud, int16_t l, int16_t r);   /* the chip's samples, to the ring */

/* ---- capture, for grading against MAME ------------------------------------
 * The same files tools/mame/snd-capture.lua writes: records of four u32 words
 * [tag << 24 | offset, data, t, pc] — tag 1 a byte the i960 wrote to the sound
 * UART, 2 an SCSP register write (data = value | mask << 16, as a 16-bit bus
 * handler sees it), 3 an SCSP register read that differs from the last read of
 * that register (data = value | folded repeats << 16), 4 an interrupt taken
 * (offset = level) — and per frame a mark, sound RAM 0x1000-0x4FFF and the
 * SCSP register block as little-endian words. t is the 68000's clock-period
 * count, the unit MAME's capture uses. The slot monitor (0x408) is left out on
 * both sides: the driver polls it ~50,000 times a second.
 *
 * t is 32 bits, and at 11.2896 MHz that wraps after 380.4 seconds. A capture
 * longer than that silently folds back on itself -- every consumer here and in
 * tools/ reads t as monotonic -- so a session to be graded whole has to stay
 * under it, which is the shorter of the two limits on how long a run these tools
 * can hold against MAME. Widening it means widening the record, on both sides at
 * once. */
typedef struct {
    FILE    *f, *ramf, *regsf;
    int      active, done;
    uint32_t want, n, nmarks;
    uint32_t (*marks)[4];
    uint32_t buf[4096 * 4];
    int      nb;
    uint16_t lastr[0x1000];
    uint8_t  seen[0x1000];
    uint32_t reps[0x1000];
} sndcap_t;
static sndcap_t g_sndcap;

/* Whether a capture is running. Constant false in a build without the
 * debugger's hooks (build_features.h). */
static inline bool sndcap_on(void) { return M2HLE_DEV_TOOLS && g_sndcap.active; }

static inline void sndcap_put(uint32_t tag, uint32_t off, uint32_t data, uint32_t pc) {
    sndcap_t *c = &g_sndcap;
    uint32_t *r = c->buf + c->nb * 4;
    r[0] = (tag << 24) | (off & 0xFFFFFFu);
    r[1] = data;
    r[2] = (uint32_t)g_sound.m68k.cpu.cycles;
    r[3] = pc;
    if (++c->nb == 4096) { fwrite(c->buf, 4, 4096 * 4, c->f); c->nb = 0; }
    c->n++;
}

/* A 16-bit bus handler's view of an sz-byte access at off (as MAME's tap sees it). */
static inline void sndcap_scsp(int write, uint32_t off, uint32_t val, int sz) {
    uint32_t pc = g_sound.m68k.cpu.pc;
    for (int k = 0; k < (sz == 4 ? 2 : 1); k++) {
        uint32_t o, v, mask;
        if (sz == 1) {
            o = off & ~1u;
            if (off & 1u) { v = val & 0xFFu; mask = 0x00FFu; }
            else          { v = (val & 0xFFu) << 8; mask = 0xFF00u; }
        } else {
            o = (off & ~1u) + (uint32_t)k * 2u;
            v = sz == 4 ? (k ? val & 0xFFFFu : val >> 16) : val & 0xFFFFu;
            mask = 0xFFFFu;
        }
        if (o >= 0x1000u || o == 0x408u) continue;
        if (write) {
            sndcap_put(2, o, v | (mask << 16), pc);
        } else if (!g_sndcap.seen[o] || g_sndcap.lastr[o] != (uint16_t)v) {
            uint32_t reps = g_sndcap.reps[o] > 0xFFFFu ? 0xFFFFu : g_sndcap.reps[o];
            sndcap_put(3, o, v | (reps << 16), pc);
            g_sndcap.seen[o] = 1; g_sndcap.lastr[o] = (uint16_t)v; g_sndcap.reps[o] = 0;
        } else {
            g_sndcap.reps[o]++;
        }
    }
}

static inline void sndcap_stop(void) {
    sndcap_t *c = &g_sndcap;
    if (!c->active) return;
    if (c->nb) fwrite(c->buf, 4, (size_t)c->nb * 4, c->f);
    fclose(c->f); fclose(c->ramf); fclose(c->regsf);
    c->f = c->ramf = c->regsf = NULL;
    c->nb = 0;
    c->active = 0;
    c->done = 1;
}

/* Call once per game frame, on the emu thread. */
static inline void sndcap_frame(uint32_t frame, uint32_t frame_counter) {
    sndcap_t *c = &g_sndcap;
    if (!c->active) return;
    sound_settle();
    uint32_t *mk = c->marks[c->nmarks++];
    mk[0] = frame; mk[1] = frame_counter; mk[2] = (uint32_t)g_sound.m68k.cpu.cycles; mk[3] = c->n;
    fwrite(g_sound.ram + 0x1000, 1, 0x4000, c->ramf);
    uint8_t regs[1072] = {0};
    for (uint32_t o = 0; o < 0x430u; o += 2) {
        if (o >= 0x404u && o < 0x40Au) continue;
        uint16_t w = scsp_peek16(&g_sound.scsp, o);
        regs[o] = (uint8_t)w; regs[o + 1] = (uint8_t)(w >> 8);
    }
    fwrite(regs, 1, sizeof regs, c->regsf);
    if (c->nmarks > c->want) sndcap_stop();
}

/* ---- the streaming watchdog ------------------------------------------------
 *
 * The driver does not hand the SCSP a sample and let it play: it gives every
 * voice a window in sound RAM, loops the slot inside it, and keeps refilling the
 * 4 KB chunk the chip is NOT playing out of the sample ROMs. Which chunk that is
 * it learns from the slot monitor CA field, 4096 samples per step -- STF drives
 * it by writing the slot number to MSLC (0x408), waiting out ten ror.l, and
 * testing CA bit 0 with btst.b #7,0x409(a5) (sound ROM 0x604224, 0x60452C,
 * 0x604544). In STF all 32 slots stream this way, each out of its own 8 KB
 * window from 0x010000 up: two 4 KB chunks, double-buffered.
 *
 * That refill is a hard real-time race, and losing it is audible. A copy into a
 * chunk that only STARTS after the chip has already entered it leaves everything
 * from the chunk boundary up to the play position holding the bytes of the
 * previous lap, which are played before the new ones -- a fragment of stale
 * audio, and for a voice that stays late, one every lap until the game restarts
 * the track.
 *
 * READ late_chunk_up, NOT late. A pass that begins at offset 0 with the chip
 * already somewhere in chunk 0 is almost always not a late refill at all but the
 * driver giving the slot a DIFFERENT sample: the window address is fixed per
 * slot, so switching samples means copying the new one over the window from
 * offset 0, and the old sample is still releasing while that happens. There is
 * no register change to spot it by -- SA, LSA and LEA all stay as they were.
 * What separates the two is which chunk the pass was filling. A driver losing
 * the race loses it in both directions, so chunk 1 would be hit too; a reload
 * only ever starts at 0. Measured over 330 s of fights, all 587 flagged passes
 * were chunk 0 at offset 0, with the lateness spread evenly across the 4096
 * samples instead of clustered just past the boundary -- so on this board the
 * race is not being lost, and late_chunk_up is the number to watch for a
 * regression.
 *
 * Nothing in the capture harness can see this. tools/mame/snd-capture.lua leaves
 * the monitor register out of both sides on purpose (the driver polls it around
 * 50,000 times a second, which would slow MAME to a crawl), so the one register
 * the streaming is paced by is the one register never compared. So it is
 * measured here instead, against the play position the chip actually has.
 *
 * A page can belong to several slots at once -- the driver plays one loaded
 * sample from more than one voice -- so the map is a slot MASK per page, not a
 * slot. Getting that wrong attributes a write to whichever slot was stored last,
 * and with 32 voices sharing buffers that is most of them.
 *
 * Off by default and armed over the bridge (snd_watch): it costs a page map
 * rebuild per output sample and a lookup per sound-RAM write. */
#define SND_WATCH_PAGES  (SOUND_RAM_SIZE >> 12)
#define SND_WATCH_EVENTS 128

typedef struct {
    int      on;
    uint32_t page_mask[SND_WATCH_PAGES];   /* 4 KB page -> bit per slot playing through it */
    uint8_t  active[32];
    uint64_t on_sample[32];                /* out_total when this sample started in the slot */
    int32_t  last_chunk[32];               /* chunk the last write to this slot hit, -1 none */
    uint64_t refills[32], late[32];        /* refill passes seen, and those that started late */
    uint32_t worst[32];                    /* largest lateness, in samples into the chunk */
    uint64_t total_writes, total_refills, total_late;
    uint64_t late_chunk0, late_chunk_up;   /* see the note above: only the second is a defect */
    uint32_t nev;
    struct {
        uint64_t sample;                   /* board sample the refill started on */
        uint32_t addr, pc, play, off, lateness;
        uint8_t  slot, chunk;
    } ev[SND_WATCH_EVENTS];
} snd_watch_t;
static snd_watch_t g_snd_watch;

/* The byte of its window a slot is playing, addressed the way scsp_slot_sample
 * addresses the sample data: a PCM8B slot steps one byte per sample, a 16-bit
 * one two. */
static inline uint32_t snd_watch_play_byte(const scsp_slot_t *sl) {
    return SCSP_PCM8B(sl) ? (sl->cur >> SCSP_SHIFT)
                          : ((sl->cur >> (SCSP_SHIFT - 1)) & ~1u);
}

/* Rebuild the page map and notice slots that have just started. Once per output
 * sample, which is 4096 times finer than a chunk. */
static inline void snd_watch_sample(void) {
    snd_watch_t *w = &g_snd_watch;
    scsp_t *sc = &g_sound.scsp;
    memset(w->page_mask, 0, sizeof w->page_mask);
    for (int i = 0; i < 32; i++) {
        scsp_slot_t *sl = &sc->slot[i];
        if (sl->active && !w->active[i]) {
            w->on_sample[i]  = g_sound.out_total;
            w->last_chunk[i] = -1;
        }
        w->active[i] = sl->active;
        if (!sl->active) continue;
        uint32_t sa = SCSP_SA(sl), end = sa + SCSP_LEA(sl);
        if (sa >= SOUND_RAM_SIZE) continue;
        if (end >= SOUND_RAM_SIZE) end = SOUND_RAM_SIZE - 1;
        for (uint32_t pg = sa >> 12; pg <= (end >> 12); pg++) w->page_mask[pg] |= 1u << i;
    }
}

static inline void snd_watch_write(uint32_t addr, int sz) {
    snd_watch_t *w = &g_snd_watch;
    scsp_t *sc = &g_sound.scsp;
    for (int k = 0; k < sz; k++) {
        uint32_t a = addr + (uint32_t)k;
        if (a >= SOUND_RAM_SIZE) break;
        for (uint32_t m = w->page_mask[a >> 12]; m; m &= m - 1) {
            int i = 0;
            for (uint32_t b = m & (~m + 1u); b > 1u; b >>= 1) i++;
            scsp_slot_t *sl = &sc->slot[i];
            uint32_t sa = SCSP_SA(sl);
            if (a < sa) continue;
            uint32_t off = a - sa;
            if (off > SCSP_LEA(sl)) continue;
            w->total_writes++;
            int32_t chunk = (int32_t)(off >> 12);
            if (chunk == w->last_chunk[i]) continue;   /* still inside the same pass */
            w->last_chunk[i] = chunk;
            /* The fill right after a key-on legitimately covers the whole window,
             * playback included, so a voice is only held to the rule once it has
             * been running for a chunk. */
            if (g_sound.out_total - w->on_sample[i] < 4096) continue;
            w->refills[i]++;
            w->total_refills++;
            uint32_t play = snd_watch_play_byte(sl);
            if ((int32_t)(play >> 12) != chunk) continue;   /* the chip is elsewhere: in time */
            uint32_t lateness = play & 0xFFFu;
            w->late[i]++;
            w->total_late++;
            if (chunk) w->late_chunk_up++; else w->late_chunk0++;
            if (lateness > w->worst[i]) w->worst[i] = lateness;
            if (w->nev < SND_WATCH_EVENTS) {
                uint32_t e = w->nev++;
                w->ev[e].sample   = g_sound.out_total;
                w->ev[e].addr     = a;
                w->ev[e].pc       = g_sound.m68k.cpu.pc;
                w->ev[e].play     = play;
                w->ev[e].off      = off;
                w->ev[e].lateness = lateness;
                w->ev[e].slot     = (uint8_t)i;
                w->ev[e].chunk    = (uint8_t)chunk;
            }
        }
    }
}

static inline void snd_watch_arm(int on) {
    sound_settle();
    memset(&g_snd_watch, 0, sizeof g_snd_watch);
    for (int i = 0; i < 32; i++) g_snd_watch.last_chunk[i] = -1;
    g_snd_watch.on = on;
}

/* ---- 68000 bus ------------------------------------------------------------- */

static inline uint8_t sound_rom_byte(const sound_state_t *ss, uint32_t a) {
    if (a < 0x080000u) return ss->ram[a];
    if (a >= M68K_ROM_BASE && a < M68K_ROM_BASE + M68K_ROM_SIZE) return ss->rom[a - M68K_ROM_BASE];
    if (!ss->samples) return 0;
    uint32_t o;
    if      (a >= 0x800000u && a < 0xA00000u) o = a - 0x800000u;
    else if (a >= 0xA00000u && a < 0xE00000u) o = ss->bank4 + (a - 0xA00000u);
    else if (a >= 0xE00000u)                  o = ss->bank5 + (a - 0xE00000u);
    else return 0;
    return o < ss->samples_size ? ss->samples[o] : 0;
}

static uint32_t sound_bus_read(sound_state_t *ss, uint32_t addr, int sz, int peek) {
    addr &= 0xFFFFFFu;
    if (addr >= M68K_SCSP_BASE && addr < M68K_SCSP_BASE + M68K_SCSP_SIZE) {
        uint32_t off = addr - M68K_SCSP_BASE;
        if (peek) {
            uint16_t w = scsp_peek16(&ss->scsp, off & ~1u);
            if (sz == 1) return (off & 1) ? (w & 0xFFu) : (uint32_t)(w >> 8);
            if (sz == 2) return w;
            return ((uint32_t)w << 16) | scsp_peek16(&ss->scsp, (off & ~1u) + 2);
        }
        uint32_t v = scsp_read(&ss->scsp, off, sz);
        if (sndcap_on()) sndcap_scsp(0, off, v, sz);
        return v;
    }
    /* The hot paths: sound RAM, and the program ROM the driver runs from (every
     * opcode fetch), each read in place when the whole access lies inside it.
     * sound_rom_byte, a byte at a time through every region test, was an
     * eighth of the sound board. */
    const uint8_t *p = NULL;
    if (addr + (uint32_t)sz <= 0x080000u) {
        /* only pages sound_map_pages leaves off come here: the DSP's delay line */
        if (!peek) scsp_ram_touch(&ss->scsp, addr, (uint32_t)sz, 0);
        p = ss->ram + addr;
    }
    else if (addr - M68K_ROM_BASE <= M68K_ROM_SIZE - (uint32_t)sz) p = ss->rom + (addr - M68K_ROM_BASE);
    if (p) {
        if (sz == 1) return p[0];
        if (sz == 2) return ((uint32_t)p[0] << 8) | p[1];
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
    uint32_t v = 0;
    for (int i = 0; i < sz; i++) v = (v << 8) | sound_rom_byte(ss, (addr + (uint32_t)i) & 0xFFFFFFu);
    return v;
}

static uint32_t sound_m68k_read(void *ctx, uint32_t addr, int sz) {
    return sound_bus_read((sound_state_t *)ctx, addr, sz, 0);
}

/* a debugger's read: no side effects on the SCSP */
static inline uint32_t sound_m68k_peek(sound_state_t *ss, uint32_t addr, int sz) {
    return sound_bus_read(ss, addr, sz, 1);
}

/* The 68000's direct-read pages (m68k_state_t.rmap): sound RAM, the program
 * ROM and the sample ROM windows -- every region a read does nothing to but
 * read. A page is mapped only when all of it reads from one run of bytes the
 * way sound_bus_read would, so a sample window past the end of the set stays
 * on the callback and reads its zeros there. Rebuilt whenever what backs a
 * page changes (reset, ROM load, a bank switch). */
static inline void sound_map_pages(sound_state_t *ss) {
    const uint8_t **m = ss->m68k.rmap;
    memset((void *)m, 0, sizeof ss->m68k.rmap);
    /* MAME model2.cpp model2_snd: one wait state on every sound RAM and SCSP
     * access ("the same waitstate weights as Saturn"). Measured in its timer
     * handlers: the interrupt-to-reload latency is 166 / 200 clocks against
     * 128 / 160 without them. */
    memset(ss->m68k.wmap, 0, sizeof ss->m68k.wmap);
    for (uint32_t pg = 0; pg < 0x080000u >> 16; pg++) ss->m68k.wmap[pg] = 1;
    ss->m68k.wmap[M68K_SCSP_BASE >> 16] = 1;
    /* Not the pages the DSP keeps its delay line in: the chip makes its samples
     * late (scsp.h, "The chip's own time"), so a read there has to go through
     * sound_bus_read and have the DSP catch up first. */
    for (uint32_t pg = 0; pg < 0x080000u >> 16; pg++)
        if (!((pg << 16) < ss->scsp.dsp_hi && ss->scsp.dsp_lo < ((pg + 1) << 16))) m[pg] = ss->ram + (pg << 16);
    ss->scsp.dsp_moved = 0;
    if (ss->rom_loaded)
        for (uint32_t pg = 0; pg < M68K_ROM_SIZE >> 16; pg++) m[(M68K_ROM_BASE >> 16) + pg] = ss->rom + (pg << 16);
    if (!ss->samples) return;
    for (uint32_t pg = 0x80; pg < 0x100; pg++) {
        uint32_t a = pg << 16, o;
        if      (a < 0xA00000u) o = a - 0x800000u;
        else if (a < 0xE00000u) o = ss->bank4 + (a - 0xA00000u);
        else                    o = ss->bank5 + (a - 0xE00000u);
        if (o + 0x10000u <= ss->samples_size) m[pg] = ss->samples + o;
    }
}

static inline void sound_ctrl_w(sound_state_t *ss, uint32_t val) {
    if (ss->samples_size > 0x800000u) {                    /* bigger sets bank their upper half */
        ss->bank4 = (val & 0x20) ? 0x200000u : 0x800000u;
        ss->bank5 = (val & 0x20) ? 0x600000u : 0xA00000u;
        sound_map_pages(ss);
    }
}

static void sound_m68k_write(void *ctx, uint32_t addr, uint32_t val, int sz) {
    sound_state_t *ss = (sound_state_t *)ctx;
    addr &= 0xFFFFFFu;
    if (addr + (uint32_t)sz <= 0x080000u) {
        if (g_snd_watch.on) snd_watch_write(addr, sz);
        scsp_ram_touch(&ss->scsp, addr, (uint32_t)sz, 1);   /* what the chip owes reads the old bytes */
        uint8_t *p = ss->ram + addr;
        if (sz == 1) { p[0] = (uint8_t)val; return; }
        if (sz == 2) { p[0] = (uint8_t)(val >> 8); p[1] = (uint8_t)val; return; }
        p[0] = (uint8_t)(val >> 24); p[1] = (uint8_t)(val >> 16); p[2] = (uint8_t)(val >> 8); p[3] = (uint8_t)val;
        return;
    }
    if (addr >= M68K_SCSP_BASE && addr < M68K_SCSP_BASE + M68K_SCSP_SIZE) {
        uint32_t off = addr - M68K_SCSP_BASE;
        if (sndcap_on()) sndcap_scsp(1, off, val, sz);
        scsp_write(&ss->scsp, off, val, sz);
        if (ss->scsp.dsp_moved) sound_map_pages(ss);
        return;
    }
    if (addr >= M68K_SNDCTL_BASE && addr < M68K_SNDCTL_BASE + M68K_SNDCTL_SIZE) {
        sound_ctrl_w(ss, val);
        return;
    }
    /* ROM and unmapped space ignore writes */
}

/* ---- the serial line (see sound_uart_t) --------------------------------------- */

/* the first bit tick strictly after u (units) */
static inline uint64_t sound_ser_tick_after(uint64_t u) {
    if (u < SOUND_SER_PHASE) return SOUND_SER_PHASE;
    return SOUND_SER_PHASE + ((u - SOUND_SER_PHASE) / SOUND_SER_BIT + 1u) * SOUND_SER_BIT;
}

/* The byte at the head of the queue goes onto the line: at free_at (units,
 * the tick the previous byte's stop bit ended on) if it was waiting for the
 * line, else at the first tick after its write. */
static inline void sound_uart_begin(sound_uart_t *u, uint64_t free_at) {
    uint64_t w = u->wrote[u->r];
    u->start   = w < free_at ? free_at : sound_ser_tick_after(w);
    u->deliver = (u->start + SOUND_SER_RX + SOUND_SER_UNIT - 1u) / SOUND_SER_UNIT;
}

/* Bring the line up to the 68000's clock: hand the SCSP the byte whose stop
 * bit it has sampled, and start the next byte where the line freed. */
static inline void sound_uart_service(sound_state_t *ss, uint64_t now) {
    sound_uart_t *u = &ss->uart;
    while (u->count) {
        if (u->deliver) {
            if (now < u->deliver) return;
            scsp_midi_in(&ss->scsp, u->byte[u->r]);
            u->deliver = 0;
            u->sent++;
        }
        uint64_t end = u->start + SOUND_SER_FRAME;
        if (now * SOUND_SER_UNIT < end) return;          /* the stop bit is still going out */
        u->r = (uint8_t)((u->r + 1u) & (SOUND_UART_FIFO - 1u));
        u->count--;
        if (u->count) sound_uart_begin(u, end);
    }
}

/* A byte written to the UART at clock period `clock`. */
static inline void sound_uart_write(sound_state_t *ss, uint8_t b, uint64_t clock) {
    sound_uart_t *u = &ss->uart;
    sound_uart_service(ss, clock);
    if (u->count == SOUND_UART_FIFO) { u->drops++; return; }
    u->byte[u->w]  = b;
    u->wrote[u->w] = clock * SOUND_SER_UNIT;
    u->w = (uint8_t)((u->w + 1u) & (SOUND_UART_FIFO - 1u));
    if (u->count++ == 0) sound_uart_begin(u, 0);
}

/* TxRDY: the holding register is empty (the chip holds one byte behind the one
 * on the line). TxEMPTY: nothing on the line either. */
static inline bool sound_uart_txrdy(const sound_state_t *ss)   { return ss->uart.count < 2; }
static inline bool sound_uart_txempty(const sound_state_t *ss) { return ss->uart.count == 0; }

#include "sound_hle.h"   /* the driver in C, in place of the 68000 (Pinboard #173) */

/* ---- i960 side: the sound UART ---------------------------------------------- */

static inline void sound_code_put(uint32_t code) {
    uint32_t i = g_sound.code_n++ & (SOUND_CODE_LOG - 1);
    g_sound.code_log[i].code   = code;
    g_sound.code_log[i].sample = g_sound.out_total;
}

static inline void sound_code_byte(uint8_t b) {
    if (b & 0x80u) {
        if (g_sound.code_have) sound_code_put(g_sound.code_acc | 0x80000000u);
        g_sound.code_acc  = b;
        g_sound.code_have = 1;
        return;
    }
    if (!g_sound.code_have) { sound_code_put(0x80000000u | b); return; }   /* a stray data byte */
    g_sound.code_acc = (g_sound.code_acc << 8) | b;
    if (++g_sound.code_have == 3) { sound_code_put(g_sound.code_acc); g_sound.code_have = 0; }
}

/* The SCSP's MIDI output is wired back to the UART's receive line (MAME
 * model2.cpp, midi_out_cb -> i8251 write_rxd): what the 68000 writes to MOBUF
 * is the byte the i960 reads at +0, with RxRDY (bit 1) up while one waits.
 * STF's driver never writes MOBUF; m2-pacman's answers the i960's ping with
 * 0x5A there, and without the line the game ran with no sound ("NO SOUND").
 * The byte is there as soon as the 68000 has written it: the line's 320 us is
 * not modelled, and bytes do not overrun, the ring holds them. */
static inline bool sound_uart_rxrdy(const sound_state_t *ss) { return ss->scsp.mo_r != ss->scsp.mo_w; }

static void sound_run(uint32_t n);                      /* below */
static inline bool sound_uart_make_room(bool for_game);  /* below; both callbacks need it */

static uint32_t sound_midi_read_cb(mem_region_t *r, uint32_t addr, int size) {
    (void)r; (void)size;
    sound_settle();
    g_sound.read_count++;
    if ((addr - MIDI_BASE) == 0) {           /* i8251 data: the received byte */
        scsp_t *s = &g_sound.scsp;
        if (s->mo_r == s->mo_w) return 0u;
        uint8_t b = s->mo[s->mo_r];
        s->mo_r = (uint8_t)((s->mo_r + 1u) & 31u);
        return b;
    }
    /* i8251 status at +4: TxRDY (bit 0), RxRDY (bit 1) and TxEMPTY (bit 2) as the line stands */
    if ((addr - MIDI_BASE) != 4) return 0u;
    sound_uart_service(&g_sound, g_sound.m68k.cpu.cycles);
    /* A program that polls TxRDY instead of taking the interrupt (m2-pacman's
     * flush loop) would otherwise see the holding register full for the rest of
     * the slice: nothing clocks the line while the i960 runs. Run the sound
     * board on until TxRDY rises, as the interrupt path does (make_room). */
    if (!sound_uart_txrdy(&g_sound)) sound_uart_make_room(true);
    return (sound_uart_txrdy(&g_sound) ? 0x01u : 0u) | (sound_uart_rxrdy(&g_sound) ? 0x02u : 0u)
         | (sound_uart_txempty(&g_sound) ? 0x04u : 0u);
}

static void sound_midi_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    (void)r; (void)size;
    sound_settle();
    if (sndcap_on()) sndcap_put(1, addr, (val & 0xFFu) | 0xFF0000u, 0);
    if ((addr - MIDI_BASE) != 0) return;                   /* +4 is the UART's control register */
    g_sound.write_count++;
    if (g_sound.midi_log_n < 64) {
        g_sound.midi_log[g_sound.midi_log_n]    = (uint8_t)val;
        g_sound.midi_log_ip[g_sound.midi_log_n] = g_mem_last_write_ip;
        g_sound.midi_log_n++;
    }
    if (g_sound.log_writes) LOG_INFO("SOUND write @ 0x%08X sz=%d val=0x%08X", addr, size, val);
    sound_code_byte((uint8_t)val);
    /* The run loop only hands the i960 a TxRDY interrupt when the UART can take
     * a byte (emu_sound_ready), so a byte the game sends always fits. This is
     * for any other writer: make room in the queue the same accounted way, and
     * if the line truly cannot take it, sound_uart_write drops it and counts it. */
    sound_uart_make_room(false);
    sound_uart_write(&g_sound, (uint8_t)val, g_sound.m68k.cpu.cycles);
}

/* ---- lifecycle ------------------------------------------------------------------ */

/* MAME model2.cpp reset_model2_scsp: the 68000's reset vectors come from the
 * first 16 bytes of the program ROM, copied into sound RAM. */
static inline void sound_boot_68k(void) {
    sound_settle();
    memcpy(g_sound.ram, g_sound.rom, 16);
    m68k_reset(&g_sound.m68k);
    g_sound.m68k.read_cb  = sound_m68k_read;
    g_sound.m68k.write_cb = sound_m68k_write;
    g_sound.m68k.mem_ctx  = &g_sound;
    sound_map_pages(&g_sound);
    m68k_startup(&g_sound.m68k);
    shle_reset();
}

static inline void sound_reset(void) {
    sound_settle();
    bool log_writes = g_sound.log_writes;
    const uint8_t *samples = g_sound.samples;
    uint32_t samples_size = g_sound.samples_size;
    bool rom_loaded = g_sound.rom_loaded;
    uint8_t rom_vectors[16];
    memcpy(rom_vectors, g_sound.rom, 16);

    memset(g_sound.ram, 0, sizeof g_sound.ram);
    memset(&g_sound.m68k, 0, sizeof g_sound.m68k);
    scsp_reset(&g_sound.scsp, g_sound.ram, SOUND_RAM_SIZE, &g_sound.m68k.cpu.cycles);
    g_sound.scsp.sink = sound_scsp_sink;
    g_sound.budget = 0;
    g_sound.slice_frac = 0;
    g_sound.ahead = 0;
    g_sound.out_due = g_sound.out_total;
    memset(&g_sound.uart, 0, sizeof g_sound.uart);
    memset(g_sound.irqs, 0, sizeof g_sound.irqs);
    g_sound.write_count = g_sound.read_count = 0;
    g_sound.midi_log_n = 0;
    g_sound.log_writes = log_writes;
    g_sound.samples = samples;
    g_sound.samples_size = samples_size;
    g_sound.bank4 = 0x200000u;
    g_sound.bank5 = 0x600000u;
    g_sound.m68k.read_cb  = sound_m68k_read;
    g_sound.m68k.write_cb = sound_m68k_write;
    g_sound.m68k.mem_ctx  = &g_sound;
    sound_map_pages(&g_sound);
    if (rom_loaded) sound_boot_68k();
}

/* The sound program ROM, big-endian as it comes out of the zip. */
static inline void sound_load_rom(const uint8_t *bytes, uint32_t size) {
    sound_settle();
    if (size > M68K_ROM_SIZE) {
        LOG_WARN("sound: ROM size %u > %u — truncating", size, M68K_ROM_SIZE);
        size = M68K_ROM_SIZE;
    }
    memcpy(g_sound.rom, bytes, size);
    if (size < M68K_ROM_SIZE) memset(g_sound.rom + size, 0xFF, M68K_ROM_SIZE - size);
    g_sound.rom_loaded = true;
    sound_boot_68k();
    LOG_INFO("sound: 68000 ROM loaded (%u bytes), SSP=0x%08X PC=0x%08X", size, g_sound.m68k.cpu.ssp, g_sound.m68k.cpu.pc);
}

/* The sample ROMs, concatenated (not copied: the caller keeps them alive). */
static inline void sound_load_samples(const uint8_t *bytes, uint32_t size) {
    sound_settle();
    g_sound.samples      = bytes;
    g_sound.samples_size = size;
    g_sound.bank4 = 0x200000u;
    g_sound.bank5 = 0x600000u;
    sound_map_pages(&g_sound);
    LOG_INFO("sound: sample ROM mapped (%u bytes)", size);
}

/* Hook the UART callbacks. Call after mem_init(). */
static inline void sound_attach(memory_bus_t *bus) {
    sound_settle();
    g_sound.detached = false;
    for (int i = 0; i < bus->region_count; i++) {
        mem_region_t *r = &bus->regions[i];
        if (r->base == MIDI_BASE) {
            r->read_cb  = sound_midi_read_cb;
            r->write_cb = sound_midi_write_cb;
            mem_regions_changed(bus);
            LOG_INFO("sound: UART callbacks attached");
            return;
        }
    }
    LOG_WARN("sound: MIDI region not found — sound inactive");
}

/* Take the board off again, mid-run: the UART goes back to the plain region
 * mem_init made it (the i960's bytes land in bus->midi and mean nothing, as
 * with the sound board never attached -- the game writes the UART's control
 * register at boot and never reads its status), and sound_run steps nothing
 * until the next sound_attach. The driver's state goes with it, so coming
 * back is sound_reset + sound_attach, a fresh boot of the 68000. A host does
 * this when the device has to run cooler (main_libretro.c, the heat guard),
 * and never inside a netplay session: the other board runs the sound board,
 * so this one has to. The output ring is the reader's; a host that stops
 * reading moves out_r itself. */
static inline void sound_detach(memory_bus_t *bus) {
    sound_settle();
    g_sound.detached = true;
    for (int i = 0; i < bus->region_count; i++) {
        mem_region_t *r = &bus->regions[i];
        if (r->base == MIDI_BASE) {
            r->read_cb  = NULL;
            r->write_cb = NULL;
            mem_regions_changed(bus);
            break;
        }
    }
    LOG_INFO("sound: board detached");
}

/* ---- the sample clock ------------------------------------------------------------- */

/* ---- the producer tap ----------------------------------------------------
 *
 * sound_out_push below is the ONLY producer of board audio, and the ring it
 * feeds has a single reader (core/audio_out.h). A host with no audio device
 * drains nothing, so a second reader of that ring would only ever see samples
 * the first one had already counted into out_dropped: anything wanting its own
 * copy of the board's output — the A/V stream, a capture — has to take it
 * HERE, ahead of the ring. The tap is handed the sample's absolute index, so
 * what it keeps carries the same clock as g_sound.out_total.
 *
 * Called on the emu thread, once per 44.1 kHz sample, with the emu mutex held.
 * Keep it to a ring write. */
static void (*g_sound_tap)(int16_t l, int16_t r, uint64_t index, void *ud);
static void  *g_sound_tap_ud;

static inline void sound_set_tap(void (*fn)(int16_t, int16_t, uint64_t, void *), void *ud) {
    g_sound_tap_ud = ud;
    g_sound_tap    = fn;      /* last: ud has to be in place before the first call */
}

static inline void sound_out_push(int16_t l, int16_t r) {
    uint64_t index = g_sound.out_total++;
    if (g_sound_tap) g_sound_tap(l, r, index, g_sound_tap_ud);
    uint32_t w = g_sound.out_w;
    if (((w + 1) & (SOUND_OUT_FRAMES - 1)) == g_sound.out_r) { g_sound.out_dropped++; return; }
    g_sound.out[w * 2] = l;
    g_sound.out[w * 2 + 1] = r;
    SOUND_STORE_RELEASE(g_sound.out_w, (w + 1) & (SOUND_OUT_FRAMES - 1));
}

static void sound_scsp_sink(void *ud, int16_t l, int16_t r) { (void)ud; sound_out_push(l, r); }

/* Whether a host drains the ring: a device is open (audio_out_init) or a
 * push host has begun (audio_out_push_begin). Without one out_dropped is
 * every sample the board makes, which says nothing about real drops. */
static volatile int g_sound_out_reader;

/* A per-instruction tap for the graders: called with the 68000's PC and its
 * clock before every instruction (tests/snd_replay.c $SND_TRACE writes them
 * out, to hold the instruction timing against a MAME trace). NULL when off. */
static void (*g_sound_step_trace)(uint32_t pc, uint64_t cycles, void *ud);
static void  *g_sound_step_trace_ud;

/* Run the board for n output samples. */
static void sound_run(uint32_t n) {
    if (!g_sound.rom_loaded || g_sound.detached) return;
    if (g_shle.on) {
        int64_t t0 = emu_now_us();
        shle_run(n);
        g_emu_times.sound_us      += emu_now_us() - t0;
        g_emu_times.sound_samples += n;
        return;
    }
    m68k_state_t *m = &g_sound.m68k;
    int64_t run_t0 = emu_now_us();
    int zone = hprof_enter(HPROF_M68K);
    for (uint32_t i = 0; i < n; i++) {
        if (g_snd_watch.on) snd_watch_sample();
        g_sound.budget += SOUND_CYCLES_PER_SAMPLE;
        while (g_sound.budget > 0) {
            if (m->cpu.halted) { g_sound.budget = 0; break; }
            uint64_t c0 = m->cpu.cycles;
            /* The 68000 samples its interrupt lines while an instruction
             * runs, SOUND_IPL_LEAD periods before it ends (MAME's m68000 loads
             * the pending interrupt where the microcode moves IR to IRD, with
             * the instruction's last prefetch): a timer that expires later
             * than that, or a byte the serial line lands later than that, is
             * only acted on after the next instruction. The mask it compares
             * against is the one the instruction leaves: after an RTE that
             * lowers it, a pending interrupt is taken before the next
             * instruction (MAME's trace shows timer C's handler entered
             * straight from timer B's RTE; a rule that let one instruction run
             * first put the two handlers 70 clocks further apart, every
             * period). Measurable in the driver's timer periods, fire to fire:
             * MAME has 50.014 samples for timer B and 505.437 for timer A over
             * 90 s of attract (tools/README.md, "The sound board"). */
            uint64_t sampled = c0 >= SOUND_IPL_LEAD ? c0 - SOUND_IPL_LEAD : 0;
            scsp_timers(&g_sound.scsp, sampled);
            if (g_sound.uart.count) sound_uart_service(&g_sound, sampled);
            int lvl = scsp_irq_level(&g_sound.scsp);
            int ipl = m68k_ipl(&m->cpu);
            if (lvl > ipl && m68k_interrupt(m, lvl)) {
                g_sound.irqs[lvl]++;
                if (sndcap_on()) sndcap_put(4, (uint32_t)lvl, 0, m->cpu.pc);
            } else if (m->cpu.stopped) {
                m->cpu.cycles += (uint64_t)g_sound.budget;   /* time passes while it waits */
                g_sound.budget = 0;
                break;
            } else {
                if (g_sound_step_trace) g_sound_step_trace(m->cpu.pc, c0, g_sound_step_trace_ud);
                m68k_step(m);
                g_sound.budget -= (int32_t)(m->cpu.cycles - c0);
                continue;
            }
            g_sound.budget -= (int32_t)(m->cpu.cycles - c0);
        }
        /* The chip owes the sample and makes it when something needs it
         * (scsp.h, "The chip's own time"). The watchdog and a capture look at
         * the slots every sample, so they have them made every sample. */
        scsp_tick(&g_sound.scsp);
        if (g_snd_watch.on || sndcap_on()) scsp_sync(&g_sound.scsp);
    }
    /* The rest of the run's samples, which is most of the chip's work: timed,
     * for its share (emu_times.h). Samples a register write or a sound RAM
     * access made inside the run are counted in the 68000's time. */
    uint64_t made = g_sound.scsp.samples;
    int64_t t0 = emu_now_us();
    scsp_sync(&g_sound.scsp);
    g_emu_times.scsp_timed_us += emu_now_us() - t0;
    g_emu_times.scsp_timed    += g_sound.scsp.samples - made;
    g_emu_times.sound_us      += emu_now_us() - run_t0;
    g_emu_times.sound_samples += n;
    hprof_leave(zone);
}

/* ---- the sound thread ---------------------------------------------------------
 *
 * The sound board is ~40% of the emulation on the handheld, and it talks to the
 * i960 at only a few points: the UART's data and status registers
 * (sound_midi_read_cb / _write_cb), the backpressure that runs it early
 * (sound_uart_make_room), and the frame's samples charged at the frame edge
 * (sound_run_slice). So the slice's samples are handed to a thread of its own,
 * and the emu thread goes on to the next slice of the i960 meanwhile. Every one
 * of those points -- and anything else that reads or changes the board:
 * a reset, a capture, the bridge -- first waits for the run to finish
 * (sound_settle). The sound board then does exactly the same work in exactly
 * the same order as when the emu thread ran it itself, and sees every byte from
 * the i960 at the same 68000 clock, so the board -- and a netplay session, and
 * every grader -- cannot tell the difference. What changes is only WHEN the
 * samples appear: up to a frame later, while the next frame's i960 runs.
 *
 * Runs that are short and waited on at once stay on the emu thread (the
 * UART's run-ahead: there is nothing to overlap). So does everything while the
 * run has to be watched from the emu thread: a sound capture, break-on-warn (a
 * WARN from the 68000 has to stop the slice it belongs to) and the 68000
 * trace. M2HLE_SOUND_THREAD=0 in the environment turns the thread off, for an
 * A/B.
 *
 * Where the overlap is lost: a slice that starts with a sound byte queued in
 * the game. Whether the UART can take it (TxRDY) depends on the 68000's clock
 * to the cycle, so the interrupt offer at the top of the slice waits for the
 * run. Measured unthrottled on x86, STF attract and a scripted two-player
 * game: a third of frames wait, for most of a run (~0.5 ms). A host that has
 * other work between slices -- the libretro core draws, a paced host sleeps --
 * gives the run that long to finish first. Taking that wait out would mean
 * answering TxRDY and time-stamping the i960's bytes without the 68000's exact
 * clock, which cannot be done and stay identical in every case (a halted 68000
 * stops its clock). */
typedef struct {
    int         state;        /* 0 not started, 1 running, -1 off / could not start */
    emu_mutex_t lock;
    emu_cond_t  job_cv, done_cv;
    uint32_t    job;          /* samples to run, 0 = nothing to do (under lock) */
    volatile int busy;        /* a run is handed out and not finished */
    emu_thread_t thread;
} sound_thread_t;
static sound_thread_t g_sound_thr;
/* A host's (or a test's) switch: 0 keeps every run on the emu thread from the
 * next one on. */
static volatile int g_sound_thread_want = 1;

static inline void sound_settle(void) {
#if SOUND_THREAD
    if (!SOUND_LOAD_ACQUIRE(g_sound_thr.busy)) return;
    int64_t t0 = emu_now_us();
    emu_mutex_lock(&g_sound_thr.lock);
    while (g_sound_thr.busy) emu_cond_wait(&g_sound_thr.done_cv, &g_sound_thr.lock);
    g_emu_times.sound_wait_us += emu_now_us() - t0;
    emu_mutex_unlock(&g_sound_thr.lock);
#endif
}

#if SOUND_THREAD
static void sound_thread_loop(void) {
    emu_mutex_lock(&g_sound_thr.lock);
    for (;;) {
        while (!g_sound_thr.job) emu_cond_wait(&g_sound_thr.job_cv, &g_sound_thr.lock);
        uint32_t n = g_sound_thr.job;
        emu_mutex_unlock(&g_sound_thr.lock);
        sound_run(n);
        emu_mutex_lock(&g_sound_thr.lock);
        g_sound_thr.job = 0;
        SOUND_STORE_RELEASE(g_sound_thr.busy, 0);
        emu_cond_broadcast(&g_sound_thr.done_cv);   /* the bridge may be waiting too */
    }
}
#ifdef _WIN32
static DWORD WINAPI sound_thread_proc(LPVOID p) { (void)p; sound_thread_loop(); return 0; }
#else
static void *sound_thread_proc(void *p) { (void)p; hprof_name_thread("m2-sound"); sound_thread_loop(); return NULL; }
#endif
#endif

/* Whether a run may go to the sound thread now; starts it the first time. */
static inline bool sound_thread_usable(void) {
#if SOUND_THREAD
    if (!g_sound_thread_want || sndcap_on() || g_log.break_on_warn || g_sound_step_trace) return false;
    if (g_sound_thr.state == 0) {
        const char *e = getenv("M2HLE_SOUND_THREAD");
        g_sound_thr.state = -1;
        if (e && e[0] == '0') {
            LOG_INFO("sound: M2HLE_SOUND_THREAD=0, the sound board runs on the emu thread");
            return false;
        }
        emu_mutex_init(&g_sound_thr.lock);
        emu_cond_init(&g_sound_thr.job_cv);
        emu_cond_init(&g_sound_thr.done_cv);
#ifdef _WIN32
        g_sound_thr.thread = CreateThread(NULL, 0, sound_thread_proc, NULL, 0, NULL);
        bool ok = g_sound_thr.thread != NULL;
#else
        bool ok = pthread_create(&g_sound_thr.thread, NULL, sound_thread_proc, NULL) == 0;
        if (ok) pthread_detach(g_sound_thr.thread);
#endif
        if (!ok) { LOG_WARN("sound: could not start the sound thread; running it on the emu thread"); return false; }
        g_sound_thr.state = 1;
        LOG_INFO("sound: the sound board runs on its own thread");
    }
    return g_sound_thr.state == 1;
#else
    return false;
#endif
}

static inline bool sound_thread_on(void) { return SOUND_THREAD && g_sound_thr.state == 1; }

/* Run the board n samples on from where every earlier run leaves it: on the
 * sound thread if `hand_off` and it can, else here and now. Emu thread only. */
static inline void sound_advance(uint32_t n, bool hand_off) {
    sound_settle();
    if (!n || !g_sound.rom_loaded || g_sound.detached) return;   /* sound_run would step nothing */
    g_sound.out_due = g_sound.out_total + n;
    g_sound.out_pub = g_sound.out_w;
    if (hand_off && sound_thread_usable()) {
#if SOUND_THREAD
        emu_mutex_lock(&g_sound_thr.lock);
        g_sound_thr.busy = 1;
        g_sound_thr.job  = n;
        g_emu_times.sound_jobs++;
        emu_cond_signal(&g_sound_thr.job_cv);
        emu_mutex_unlock(&g_sound_thr.lock);
#endif
        return;
    }
    int64_t t0 = emu_now_us();
    sound_run(n);
    g_emu_times.sound_inline_us += emu_now_us() - t0;
    g_sound.out_pub = g_sound.out_w;
}

/* ---- the UART's backpressure ----------------------------------------------------
 *
 * The board runs a slice of the i960 and then a slice of sound, so the line
 * cannot carry a byte while the i960 is sending it. On the board it would: the
 * UART sends a byte in a third of a millisecond, raises TxRDY, and the i960
 * sends the next (MAME's capture: 320 us apart). Here a burst meets a line
 * nobody is clocking.
 *
 * So when the UART cannot take what is about to be sent, the sound board runs
 * on now, SOUND_AHEAD_STEP samples at a time, until it can. Those samples are
 * the slice's own, run early: `ahead` counts them and sound_run_slice owes them
 * back, so the board's clock -- and the host's audio -- never gain a sample. At
 * most SOUND_AHEAD_MAX may be run early; past that the byte waits for the next
 * slice, as it would wait on the UART. The game is paced like the i960 on the
 * board, one byte per TxRDY (for_game: the holding register must be empty);
 * another writer is only held when the 32-deep queue is full.
 *
 * The old drain valve ran the 68000 without owing the samples back, and gave up
 * after 32 passes and dropped the byte. Returns whether the byte now fits. */
static inline bool sound_uart_make_room(bool for_game) {
    sound_settle();
    for (;;) {
        sound_uart_service(&g_sound, g_sound.m68k.cpu.cycles);
        if (for_game ? sound_uart_txrdy(&g_sound) : g_sound.uart.count < SOUND_UART_FIFO) return true;
        if (!g_sound.rom_loaded || g_sound.detached ||
            g_sound.ahead + SOUND_AHEAD_STEP > SOUND_AHEAD_MAX) return false;
        g_sound.midi_drains++;
        sound_advance(SOUND_AHEAD_STEP, false);
        g_sound.ahead += SOUND_AHEAD_STEP;
    }
}

/* One emu slice (1/60 s) of sound, less what was run early inside it, handed
 * to the sound thread (see above): the samples are there once sound_settle
 * returns, and g_sound.out_due is the clock they end on. The
 * remainder carries from slice to slice and is reset with the board: two
 * boards cold-booted together have to put the same samples in every slice. */
static inline void sound_run_slice(uint32_t slices_per_sec) {
    g_sound.slice_frac += SOUND_RATE;
    uint32_t n = g_sound.slice_frac / slices_per_sec;
    g_sound.slice_frac -= n * slices_per_sec;
    uint32_t early = (uint32_t)g_sound.ahead < n ? (uint32_t)g_sound.ahead : n;
    g_sound.ahead -= (int32_t)early;
    sound_advance(n - early, true);
}

#endif /* SOUND_H */
