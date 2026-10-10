/*
 * det_digest.c -- one line per game frame saying what the board computed, so two
 * builds can be held against each other. The question it answers is the
 * cross-play gate (WEB-NETPLAY.md section 3): does the WebAssembly build compute
 * the same frames as the desktop one? Lockstep netplay sends inputs, not state,
 * so the two must agree to the bit.
 *
 * It runs the slice both hosts run (emu_slice_body / emu_slice_finish from
 * emu_thread.h: the i960, the COP, the timers and the sound board) with no
 * window, no GPU and no clock, from the same one-zip load the web build uses.
 * Inputs come from a script keyed to game frames, so every run of a script is
 * the same run.
 *
 *   det_digest <merged sfight zip> [--frames N] [--script "449:c,460:,..."]
 *              [--from F] [--out FILE] [--cop FROM:TO:FILE] [--trace F:FILE]
 *              [--profile ID]
 *
 * Script keys are the web build's ?script= (main_web.c): at frame N hold exactly
 * these; u d l r, 1-4 the buttons, s start, c coin; an uppercase letter is the
 * same control on player 2.
 *
 * Each line: frame, the netplay frame check (what two peers compare), then FNV-1a
 * of work RAM (both banks), buffer RAM, and the COP's data memory. The check
 * alone already fails on the first frame that differs; the rest say where.
 *
 * --cop FROM:TO:FILE writes every word of the COP conversation during those
 * frames (g_cop_tap: command/argument words in, replies out), one per line with
 * its frame, so the first differing reply names the command that split.
 *
 * --inputs FILE replays a netplay session's input log (netplay.h, "The session
 * input log"): each frame gets the two players' words the session ran on, and
 * the check the session logged for it is held against the replay's. Frame 0 of
 * a session is the first frame after the barrier's cold boot, which is this
 * program's first frame. The first frame whose check differs is where the board
 * that wrote the log stopped computing what the inputs say; replay the other
 * player's log too, and compare the two logs' words, to know which board it was
 * and whether they were ever fed the same inputs. --frames defaults to the log.
 *
 * --trace F:FILE writes one line per i960 instruction of game frame F: IP and a
 * hash of the registers (globals, locals, AC, the FP registers). The first line
 * that differs is the instruction that computed something different. The traced
 * frame runs through trace_slice, a copy of emu_slice_body with the line added,
 * so keep the two in step.
 *
 * --sound adds the sound board to each line: a hash of sound RAM and the
 * sample clock. Reading it waits for the sound thread (sound.h), so the run
 * then has no overlap to test; without it the sound thread runs as in a host,
 * and the last line on stderr hashes every sample the board produced, which
 * holds the overlapped run whole. --no-sound-thread (or M2HLE_SOUND_THREAD=0)
 * keeps the sound board on this thread, for the A/B. --sound-hle runs the
 * sound driver in C instead of on the 68000 (sound_hle.h): the i960 columns
 * are the same frame for frame if the i960 cannot tell. --pcm FILE writes
 * every sample the board produced (16-bit stereo, 44.1 kHz, raw).
 *
 * --follow-out DIR leads a one-way follow (core/follow.h) from the first frame:
 * DIR gets seg-N.sta / seg-N.feed, a segment every --follow-every frames, all
 * of them kept. --poke F:ADDR:HEX writes those bytes at the start of frame F as
 * the MCP bridge's write_memory does (recorded in the feed). --follow DIR:N
 * follows from segment N on, into the next segment at each END, feeding the
 * feed a few hundred bytes at a time; its lines from the join on are the
 * leader's, and the last line on stderr says how many frame checks held.
 *
 * --region japan|usa|export powers up in that region (USA by default, as the
 * emulator does); --nowarnskip leaves the Japan warning screen in, ~640 game
 * frames, as MAME does. --peek HEXADDR adds that byte to each line (the mode
 * at 50002A, the sub-mode at 500030), to pair frames with a MAME log.
 *
 * Two builds of it, one from each configuration, so each has exactly its
 * frontend's compiler flags:
 *   native  --target det_digest in a desktop build tree (not a ctest: it needs a ROM)
 *   wasm    --target det_digest in the web build tree; run as node det_digest.js
 * Then diff the two outputs; tools/README.md, "Cross-play".
 */
#define NDEBUG 1
#include "net/netplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "memory.h"
#include "i960.h"
#include "i960_exec.h"
#include "breakpoint.h"
#include "watchpoint.h"
#include "rom_loader.h"
#include "emu_thread.h"
#include "sound.h"
#include "input.h"
#include "registry.h"

static memory_bus_t     bus;
static i960_cpu_t       cpu;
static emu_thread_ctx_t emu;
static romset_t         romset;

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
#define FNV0 0xcbf29ce484222325ull

/* Every sample the board makes, folded in as it is made (on the sound thread). */
static uint64_t snd_out_hash = FNV0, snd_out_n;
static FILE *snd_pcm;          /* --pcm FILE */
static bool snd_replaying;   /* --rewind: frames run a second time are not hashed */

static void snd_out_tap(int16_t l, int16_t r, uint64_t index, void *ud) {
    (void)index; (void)ud;
    if (snd_replaying) return;
    int16_t lr[2] = { l, r };
    snd_out_hash = fnv(snd_out_hash, lr, sizeof lr);
    if (snd_pcm) fwrite(lr, sizeof lr, 1, snd_pcm);
    snd_out_n++;
}

#define SCRIPT_MAX 1024
static struct { uint32_t frame, held; } script[SCRIPT_MAX];
static int script_n;

/* --save-at F:FILE writes a savestate at the end of frame F; --load FILE starts
 * from one. A loaded run prints what the saving run would have printed after F,
 * line for line, and its sample hash (the last lines) covers the samples after
 * F in both. With --mem the state goes through memory as the libretro core's
 * retro_serialize / retro_unserialize take it (rollback.h's flat state, padded
 * to its size), and the file holds that buffer; a zip state loads that way too.
 *
 * --rewind K does what RetroArch's rewind does to the core (Pinboard #585): a
 * flat state every frame, and at every K-th frame, the first time it is
 * reached, the state from K frames back loaded and the K frames run again.
 * Frames are printed and samples hashed only the first time, so the output has
 * to be the plain run's (the samples with --no-sound-thread, which makes them
 * inside their own slice). */
static const char *follow_out, *follow_in;
static uint32_t    follow_every;
#define POKE_MAX 16
static struct { uint32_t frame, addr, n; uint8_t b[64]; } pokes[POKE_MAX];
static int poke_n;

static bool parse_poke(const char *a) {
    char hex[129] = {0};
    if (poke_n >= POKE_MAX || sscanf(a, "%u:%x:%128s", &pokes[poke_n].frame, &pokes[poke_n].addr, hex) != 3)
        return false;
    uint32_t n = 0;
    for (; hex[n * 2] && hex[n * 2 + 1] && n < 64; n++) {
        char b[3] = { hex[n * 2], hex[n * 2 + 1], 0 };
        pokes[poke_n].b[n] = (uint8_t)strtoul(b, NULL, 16);
    }
    pokes[poke_n++].n = n;
    return n > 0;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *len = (size_t)n;
    return p;
}

/* The follower's side: one segment's state and feed, the feed handed over a
 * piece at a time as a page would fetch it. */
static struct { char dir[900]; uint32_t seg; uint8_t *feed; size_t len, at; uint32_t segs, checks; } fol;

static bool follow_open_seg(emu_thread_ctx_t *emu) {
    char path[1024];
    size_t n;
    snprintf(path, sizeof path, "%s/seg-%u.sta", fol.dir, fol.seg);
    uint8_t *st = read_file(path, &n);
    if (!st) return false;
    fol.checks += g_follow.checks;
    const char *err = follow_join(emu, st, n);
    free(st);
    if (err) { fprintf(stderr, "follow: %s: %s\n", path, err); return false; }
    free(fol.feed);
    snprintf(path, sizeof path, "%s/seg-%u.feed", fol.dir, fol.seg);
    fol.feed = read_file(path, &fol.len);
    fol.at = 0;
    fol.segs++;
    fprintf(stderr, "follow: joined segment %u at frame %u\n", fol.seg, (unsigned)g_emu_frames);
    return fol.feed != NULL;
}

/* True when the next slice may run: feed more, or move on to the next segment. */
static bool follow_next_ready(emu_thread_ctx_t *emu) {
    for (;;) {
        if (follow_slice_ready()) return true;
        if (g_follow.split) return false;
        if (g_follow.ended) {
            fol.seg++;
            if (!follow_open_seg(emu)) return false;
            continue;
        }
        if (fol.at >= fol.len) return false;
        size_t n = fol.len - fol.at < 333 ? fol.len - fol.at : 333;
        const char *err = follow_feed(fol.feed + fol.at, n);
        if (err) { fprintf(stderr, "follow: %s\n", err); return false; }
        fol.at += n;
    }
}

static const char *load_path;
static uint32_t    save_frame;
static char        save_path[1024];
static bool        state_mem;

static const char *mem_state_save(emu_thread_ctx_t *emu, const char *path) {
    size_t len = emu_state_flat_size(emu);
    size_t size = len + 4096;   /* as main_libretro.c's LR_STATE_SLACK */
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) return "out of memory";
    savestate_rom_t rom = savestate_rom_id(emu->bus);
    const char *err = emu_state_flat_save(emu, buf, &rom);
    if (!err) {
        memset(buf + len, 0, size - len);
        FILE *f = fopen(path, "wb");
        if (!f || fwrite(buf, 1, size, f) != size) err = "cannot write the file";
        if (f) fclose(f);
    }
    free(buf);
    return err;
}

static const char *mem_state_load(emu_thread_ctx_t *emu, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return "cannot open the file";
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(size > 0 ? (size_t)size : 1);
    const char *err = NULL;
    if (!buf || size <= 0 || fread(buf, 1, (size_t)size, f) != (size_t)size) err = "cannot read the file";
    fclose(f);
    if (!err && rollback_flat_is(buf, (size_t)size)) {
        savestate_rom_t rom = savestate_rom_id(emu->bus);
        (void)emu_state_flat_size(emu);
        err = emu_state_flat_load(emu, buf, (size_t)size, &rom);
    } else if (!err) err = emu_state_load_mem(emu, buf, (size_t)size);
    free(buf);
    return err;
}

/* --rewind K: a ring of K + 1 flat states, one a frame. */
static uint32_t rewind_k;
static struct {
    uint8_t        *buf;
    size_t          size;
    savestate_rom_t rom;
    uint32_t        top;       /* the furthest frame reached */
    uint32_t        jump_to;   /* a frame to go back to before the next one, or 0 */
    uint32_t        loads;
    int64_t         save_us, load_us;
    uint32_t        saves;
    uint64_t        moved[ROLLBACK_PARTS_MAX];   /* --rewind-census: 64-byte lines that changed */
    uint32_t        compared;
} rw;
static bool rewind_census;

/* What RetroArch's rewind sees of a frame: per part, the 64-byte lines that
 * differ from the frame before. */
static void rewind_count(const uint8_t *now, const uint8_t *prev) {
    size_t off = ROLLBACK_FLAT_HEAD;
    for (int i = 0; i < g_rollback.n; i++) {
        size_t n = g_rollback.part[i].n;
        for (size_t o = 0; o < n; o += 64) {
            size_t len = n - o < 64 ? n - o : 64;
            if (memcmp(now + off + o, prev + off + o, len)) rw.moved[i]++;
        }
        off += n;
    }
    rw.compared++;
}

static bool rewind_init(emu_thread_ctx_t *emu) {
    rw.size = emu_state_flat_size(emu);
    rw.rom  = savestate_rom_id(emu->bus);
    rw.buf  = (uint8_t *)malloc(rw.size * (rewind_k + 1));
    return rw.buf != NULL;
}

/* After frame f: keep its state, and go back K frames before the next one if
 * f is a K-th frame reached for the first time. Returns whether f is new. */
static bool rewind_frame(emu_thread_ctx_t *emu, uint32_t f) {
    int64_t t0 = emu_now_us();
    const char *err = emu_state_flat_save(emu, rw.buf + rw.size * (f % (rewind_k + 1)), &rw.rom);
    rw.save_us += emu_now_us() - t0;
    rw.saves++;
    if (err) { fprintf(stderr, "--rewind: %s\n", err); exit(2); }
    if (rewind_census && f > 1)
        rewind_count(rw.buf + rw.size * (f % (rewind_k + 1)), rw.buf + rw.size * ((f - 1) % (rewind_k + 1)));
    bool fresh = f > rw.top;
    if (fresh) rw.top = f;
    if (fresh && f > rewind_k && f % rewind_k == 0) rw.jump_to = f - rewind_k;
    return fresh;
}

static void rewind_jump(emu_thread_ctx_t *emu) {
    if (!rw.jump_to) return;
    int64_t t0 = emu_now_us();
    const char *err = emu_state_flat_load(emu, rw.buf + rw.size * (rw.jump_to % (rewind_k + 1)), rw.size, &rw.rom);
    rw.load_us += emu_now_us() - t0;
    rw.loads++;
    if (err) { fprintf(stderr, "--rewind: %s\n", err); exit(2); }
    if (g_emu_frames != rw.jump_to) { fprintf(stderr, "--rewind: loaded frame %u for %u\n", g_emu_frames, rw.jump_to); exit(2); }
    rw.jump_to = 0;
}

/* The top of each slice: goes back if a jump is due, and says whether the
 * frame about to run has run before. True when it went back. */
static bool rewind_slice(emu_thread_ctx_t *emu) {
    bool back = rewind_k && rw.jump_to;
    if (back) rewind_jump(emu);
    snd_replaying = rewind_k && g_emu_frames < rw.top;
    return back;
}

/* "--rewind K": K + 1 states of ~16 MB each are kept, so K is capped. */
static bool rewind_arg(const char *v) {
    rewind_k = (uint32_t)atoi(v);
    if (rewind_k >= 1 && rewind_k <= 60) return true;
    fprintf(stderr, "--rewind K: 1..60\n");
    return false;
}

static void rewind_report(void) {
    if (!rewind_k) return;
    fprintf(stderr, "rewind: %u states of %zu bytes, %.0f us a save; %u loads, %.0f us a load\n",
            rw.saves, rw.size, rw.saves ? (double)rw.save_us / rw.saves : 0.0,
            rw.loads, rw.loads ? (double)rw.load_us / rw.loads : 0.0);
    if (!rewind_census || !rw.compared) return;
    fprintf(stderr, "%-14s %9s %12s\n", "part", "bytes", "lines/frame");
    for (int i = 0; i < g_rollback.n; i++)
        fprintf(stderr, "%-14s %9zu %12.1f\n", g_rollback.part[i].name, g_rollback.part[i].n,
                (double)rw.moved[i] / rw.compared);
}

static uint32_t keys_mask(const char *p, const char *end) {
    const game_input_map_t *in = &g_active_profile->input;
    uint32_t m = 0;
    for (; p < end; p++) {
        switch (*p) {
            case 'u': m |= in->bits[GAME_INPUT_P1_UP];    break;
            case 'd': m |= in->bits[GAME_INPUT_P1_DOWN];  break;
            case 'l': m |= in->bits[GAME_INPUT_P1_LEFT];  break;
            case 'r': m |= in->bits[GAME_INPUT_P1_RIGHT]; break;
            case '1': m |= in->bits[GAME_INPUT_P1_B1];    break;
            case '2': m |= in->bits[GAME_INPUT_P1_B2];    break;
            case '3': m |= in->bits[GAME_INPUT_P1_B3];    break;
            case '4': m |= in->bits[GAME_INPUT_P1_B4];    break;
            case 's': m |= in->bits[GAME_INPUT_P1_START]; break;
            case 'c': m |= in->bits[GAME_INPUT_P1_COIN];    break;
            case 'U': m |= in->bits[GAME_INPUT_P2_UP];    break;
            case 'D': m |= in->bits[GAME_INPUT_P2_DOWN];  break;
            case 'L': m |= in->bits[GAME_INPUT_P2_LEFT];  break;
            case 'R': m |= in->bits[GAME_INPUT_P2_RIGHT]; break;
            case '!': m |= in->bits[GAME_INPUT_P2_B1];    break;
            case '@': m |= in->bits[GAME_INPUT_P2_B2];    break;
            case '#': m |= in->bits[GAME_INPUT_P2_B3];    break;
            case '$': m |= in->bits[GAME_INPUT_P2_B4];    break;
            case 'S': m |= in->bits[GAME_INPUT_P2_START]; break;
            case 'C': m |= in->bits[GAME_INPUT_P2_COIN];    break;
            default: break;
        }
    }
    return m;
}

static FILE    *cop_out;
static uint32_t cop_from, cop_to;
static void cop_tap(uint32_t tag, uint32_t val) {
    if (g_emu_frames + 1 >= cop_from && g_emu_frames + 1 <= cop_to)
        fprintf(cop_out, "%u %08x %08x\n", (unsigned)g_emu_frames + 1, tag, val);
}

static FILE    *trace_out;
static uint32_t trace_frame;

/* emu_slice_body (emu_thread.h), with a line per instruction. */
static void trace_slice(emu_thread_ctx_t *ctx) {
    g_vblank_edge = 0;
    emu_timers_slice_begin(ctx);
    emu_service_irq(ctx);
    uint64_t steps = 0;
    for (int i = 0; i < g_emu_steps_per_slice && !ctx->request_stop && !ctx->cpu->halted; i++) {
        if (g_irqt_vblank) { g_irqt_vblank = 0; g_vblank_edge = 1; break; }
        if (ctx->step_over_bp) ctx->step_over_bp = 0;
        else if (bp_check(ctx->cpu->sfr.ip)) break;
        uint32_t ip = ctx->cpu->sfr.ip;
        if (i960_step_hot(ctx->cpu, ctx->bus) != 0) break;
        ctx->total_steps++;
        steps++;
        const i960_cpu_t *c = ctx->cpu;
        uint64_t h = fnv(FNV0, c->globals.g, sizeof c->globals.g);
        h = fnv(h, c->locals.r, sizeof c->locals.r);
        h = fnv(h, &c->sfr.ac, sizeof c->sfr.ac);
        h = fnv(h, c->fp_regs, sizeof c->fp_regs);
        fprintf(trace_out, "%08x %016llx\n", ip, (unsigned long long)h);
        if (g_hle_extra) { ctx->total_steps += g_hle_extra; steps += g_hle_extra; i += (int)g_hle_extra; g_hle_extra = 0; }
        if (s_irq_in_service && g_active_profile) emu_service_sound_again(ctx);
        else if (g_irqt_sound_kick && g_active_profile) { g_irqt_sound_kick = 0; emu_offer_sound(ctx); }
        emu_timers_after_step(ctx);
        if (g_log.warn_triggered) break;
        if (g_wp.hit) break;
        if (g_sharc.unknown_triggered) break;
    }
    bool frame = g_vblank_edge != 0;
    if (frame) {
        if (g_active_profile->quirks.board_vblank) { cop_geo_frame_edge(); dl_frame_edge(ctx->bus, g_emu_frames); hle_match_replay_edge(ctx->bus); }
    }
    emu_sound_slice_end(frame);
    if (g_active_profile->quirks.geo_displaylist) geodl_capture(ctx->bus);
    ctx->cpu_prev_snapshot = ctx->cpu_snapshot;
    ctx->cpu_snapshot      = *ctx->cpu;
}

/* --inputs: a session's words and checks, indexed by session frame. */
static uint32_t *in_w0, *in_w1, *in_check;
static uint32_t  in_n;

static bool load_inputs(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    uint32_t cap = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned frame, w0, w1, check;
        if (line[0] == '#' || sscanf(line, "%u %x %x %x", &frame, &w0, &w1, &check) != 4) continue;
        if (frame >= cap) {
            uint32_t n = cap ? cap * 2 : 65536;
            while (n <= frame) n *= 2;
            in_w0 = realloc(in_w0, n * sizeof *in_w0);
            in_w1 = realloc(in_w1, n * sizeof *in_w1);
            in_check = realloc(in_check, n * sizeof *in_check);
            if (!in_w0 || !in_w1 || !in_check) { fclose(f); return false; }
            cap = n;
        }
        in_w0[frame] = w0; in_w1[frame] = w1; in_check[frame] = check;
        if (frame + 1 > in_n) in_n = frame + 1;
    }
    fclose(f);
    return in_n > 0;
}

/* Both words into the board's held mask: netplay_apply_inputs, whose bit tables
 * only exist inside a session. */
static uint32_t words_mask(uint32_t w0, uint32_t w1) {
    static const int p1[10] = { GAME_INPUT_P1_UP, GAME_INPUT_P1_DOWN, GAME_INPUT_P1_LEFT, GAME_INPUT_P1_RIGHT,
        GAME_INPUT_P1_B1, GAME_INPUT_P1_B2, GAME_INPUT_P1_B3, GAME_INPUT_P1_B4, GAME_INPUT_P1_START, GAME_INPUT_P1_COIN };
    static const int p2[10] = { GAME_INPUT_P2_UP, GAME_INPUT_P2_DOWN, GAME_INPUT_P2_LEFT, GAME_INPUT_P2_RIGHT,
        GAME_INPUT_P2_B1, GAME_INPUT_P2_B2, GAME_INPUT_P2_B3, GAME_INPUT_P2_B4, GAME_INPUT_P2_START, GAME_INPUT_P2_COIN };
    const game_input_map_t *in = &g_active_profile->input;
    uint32_t held = 0;
    for (int i = 0; i < 10; i++) {
        if (w0 & (1u << i)) held |= in->bits[p1[i]];
        if (w1 & (1u << i)) held |= in->bits[p2[i]];
    }
    if (w0 & (1u << NP_BIT_SERVICE)) held |= in->bits[GAME_INPUT_SERVICE];
    if (w0 & (1u << NP_BIT_TEST))    held |= in->bits[GAME_INPUT_TEST];
    return held;
}

static void parse_script(const char *s) {
    while (*s && script_n < SCRIPT_MAX) {
        const char *comma = strchr(s, ',');
        const char *end = comma ? comma : s + strlen(s);
        const char *colon = memchr(s, ':', (size_t)(end - s));
        if (colon) {
            script[script_n].frame = (uint32_t)strtoul(s, NULL, 10);
            script[script_n].held  = keys_mask(colon + 1, end);
            script_n++;
        }
        s = comma ? comma + 1 : end;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: det_digest <merged sfight zip> [--frames N] [--script S] [--from F] [--out FILE] [--save-at F:FILE] [--load FILE] [--mem] [--profile ID]\n");
        return 2;
    }
    uint32_t frames = 3600, from = 0;
    uint32_t peek_addr = 0;
    bool frames_given = false, sound_cols = false, cpu_cols = false;
    const char *out_path = NULL, *script_text = NULL, *inputs_path = NULL, *profile_id = "sfight";
    for (int i = 2; i < argc; i++) {
        if      (!strcmp(argv[i], "--frames") && i + 1 < argc) { frames = (uint32_t)strtoul(argv[++i], NULL, 10); frames_given = true; }
        else if (!strcmp(argv[i], "--inputs") && i + 1 < argc) inputs_path = argv[++i];
        else if (!strcmp(argv[i], "--from")   && i + 1 < argc) from   = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script_text = argv[++i];
        else if (!strcmp(argv[i], "--out")    && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--sound"))                   sound_cols = true;
        else if (!strcmp(argv[i], "--cpu"))                     cpu_cols = true;
        else if (!strcmp(argv[i], "--no-sound-thread"))         g_sound_thread_want = 0;
        else if (!strcmp(argv[i], "--sound-hle"))               g_sound_hle_want = 1;
        else if (!strcmp(argv[i], "--texload-i960"))            g_texload_hle = 0;
        else if (!strcmp(argv[i], "--spin-i960"))               g_spin_skip = 0;
        else if (!strcmp(argv[i], "--live-timers"))             ;   /* always on now */
        else if (!strcmp(argv[i], "--nowarnskip"))              g_warning_skip = 0;
        else if (!strcmp(argv[i], "--steps-per-slice") && i + 1 < argc) g_emu_steps_per_slice = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--region") && i + 1 < argc) { const char *r = argv[++i]; g_region = !strcmp(r, "japan") ? GAME_REGION_JAPAN : !strcmp(r, "export") ? GAME_REGION_EXPORT : GAME_REGION_USA; }
        else if (!strcmp(argv[i], "--peek")   && i + 1 < argc) peek_addr = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--pcm")    && i + 1 < argc) snd_pcm = fopen(argv[++i], "wb");
        else if (!strcmp(argv[i], "--trace")  && i + 1 < argc) {
            char path[1024] = {0};
            if (sscanf(argv[++i], "%u:%1023s", &trace_frame, path) != 2 || !(trace_out = fopen(path, "wb"))) {
                fprintf(stderr, "--trace F:FILE\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--load")   && i + 1 < argc) load_path = argv[++i];
        else if (!strcmp(argv[i], "--follow-out")   && i + 1 < argc) follow_out = argv[++i];
        else if (!strcmp(argv[i], "--follow-every") && i + 1 < argc) follow_every = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--follow") && i + 1 < argc) follow_in = argv[++i];
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc) {
            if (!parse_poke(argv[++i])) { fprintf(stderr, "--poke F:ADDR:HEX\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile_id = argv[++i];
        else if (!strcmp(argv[i], "--mem")) state_mem = true;
        else if (!strcmp(argv[i], "--rewind-census")) rewind_census = true;
        else if (!strcmp(argv[i], "--rewind") && i + 1 < argc) {
            if (!rewind_arg(argv[++i])) return 2;
        }
        else if (!strcmp(argv[i], "--save-at") && i + 1 < argc) {
            if (sscanf(argv[++i], "%u:%1023s", &save_frame, save_path) != 2) {
                fprintf(stderr, "--save-at F:FILE\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--cop")    && i + 1 < argc) {
            char path[1024] = {0};
            if (sscanf(argv[++i], "%u:%u:%1023s", &cop_from, &cop_to, path) != 3 || !(cop_out = fopen(path, "wb"))) {
                fprintf(stderr, "--cop FROM:TO:FILE\n");
                return 2;
            }
            g_cop_tap = cop_tap;
        }
    }

    log_init();
    mem_init(&bus, NULL, 0);
    i960_reset(&cpu);
    bp_init();
    wp_init();
    for (size_t i = 0; i < g_profile_count; i++)
        if (!strcmp(g_profiles[i]->id, profile_id)) g_active_profile = g_profiles[i];
    if (!g_active_profile) { fprintf(stderr, "no %s profile\n", profile_id); return 2; }
    if (script_text) parse_script(script_text);
    if (inputs_path) {
        if (!load_inputs(inputs_path)) { fprintf(stderr, "cannot read an input log from %s\n", inputs_path); return 2; }
        if (!frames_given || frames > in_n) frames = in_n;
        fprintf(stderr, "replaying %u session frames from %s\n", (unsigned)in_n, inputs_path);
    }

    /* A directory of region images (--export-roms) loads as it is. Otherwise
     * the web build's load: one zip, read whole, strict by CRC. */
    if (romset_is_dir(argv[1])) {
        if (romset_load_dir(&romset, argv[1]) != 0) return 2;
    } else {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *zip = (uint8_t *)malloc((size_t)len);
        if (!zip || fread(zip, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
        fclose(f);
        rl_mem_zip_set(zip, (size_t)len, true);
        int rc = g_active_profile->load_fn(&romset, NULL, NULL);
        rl_mem_zip_clear();
        free(zip);
        if (rc != 0) { fprintf(stderr, "ROM load failed; missing: %s\n", g_rl_mem_zip.missing_names); return 2; }
    }

    /* main_web.c web_install_board, which is also what a netplay reset runs. */
    g_active_profile->install_fn(&romset, &cpu, &bus);
    irqt_reset();
    if (g_active_profile->quirks.enable_68k_sound) {
        sound_reset();
        sound_attach(&bus);
        if (romset.audiocpu && romset.audiocpu_size > 0) sound_load_rom(romset.audiocpu, (uint32_t)romset.audiocpu_size);
        if (romset.samples && romset.samples_size > 0)   sound_load_samples(romset.samples, (uint32_t)romset.samples_size);
    }
    input_reset();
    input_attach(&bus);
    emu_board_reset_state();
    emu_ctx_init(&emu, &cpu, &bus);
    emu_run(&emu);
    sound_set_tap(snd_out_tap, NULL);
    if (load_path) {
        /* A board from a state: what it prints from here on has to be what
         * the run that saved it printed. The sample hash starts here. */
        const char *err = state_mem ? mem_state_load(&emu, load_path) : emu_state_load_now(&emu, load_path);
        if (err) { fprintf(stderr, "--load %s: %s\n", load_path, err); return 2; }
        snd_out_hash = FNV0; snd_out_n = 0;
        fprintf(stderr, "loaded %s at frame %u\n", load_path, (unsigned)g_emu_frames);
    }

    FILE *out = out_path ? fopen(out_path, "wb") : stdout;
    if (!out) { fprintf(stderr, "cannot write %s\n", out_path); return 2; }
    if (follow_out) {
        const char *err = follow_lead_start(follow_out, follow_every ? follow_every : 1000000000u);
        if (err) { fprintf(stderr, "--follow-out %s: %s\n", follow_out, err); return 2; }
        g_follow_lead.keep = 0;
    }
    if (follow_in) {
        const char *c = strrchr(follow_in, ':');
        if (!c) { fprintf(stderr, "--follow DIR:SEG\n"); return 2; }
        snprintf(fol.dir, sizeof fol.dir, "%.*s", (int)(c - follow_in), follow_in);
        fol.seg = (uint32_t)atoi(c + 1);
        if (!follow_open_seg(&emu)) { fprintf(stderr, "--follow: no segment %u in %s\n", fol.seg, fol.dir); return 2; }
    }

    if (rewind_k && !rewind_init(&emu)) { fprintf(stderr, "--rewind: out of memory\n"); return 2; }
    int at = 0;
    uint64_t slices = 0;
    uint32_t mismatches = 0, first_mismatch = UINT32_MAX;
    while (g_emu_frames < frames) {
        if (rewind_slice(&emu)) at = 0;   /* the script again from its start, up to the frame gone back to */
        /* Inputs change only on a frame boundary, as the lockstep's do. */
        while (at < script_n && script[at].frame <= g_emu_frames) g_input.held = script[at++].held;
        if (in_n && g_emu_frames < in_n) g_input.held = words_mask(in_w0[g_emu_frames], in_w1[g_emu_frames]);
        for (int p = 0; p < poke_n; p++) {
            if (pokes[p].frame != g_emu_frames || !pokes[p].n) continue;
            follow_bus_write(&bus, pokes[p].addr, false, pokes[p].b, pokes[p].n);
            follow_lead_write(pokes[p].addr, false, pokes[p].b, pokes[p].n);
            pokes[p].n = 0;
        }
        if (follow_in && !follow_next_ready(&emu)) break;
        if (trace_out && g_emu_frames + 1 == trace_frame) trace_slice(&emu);
        else                                              emu_slice_body(&emu);
        emu_slice_result_t r = emu_slice_finish(&emu);
        slices++;
        if (r == EMU_SLICE_STOPPED) {
            fprintf(stderr, "board stopped at frame %u, IP 0x%08X\n", (unsigned)g_emu_frames, cpu.sfr.ip);
            break;
        }
        if (r == EMU_SLICE_FRAME && in_n && g_emu_frames - 1 < in_n) {
            uint32_t f = g_emu_frames - 1, c = netplay_frame_check(&emu.cpu_snapshot, emu.total_steps);
            if (c != in_check[f]) {
                if (!mismatches) {
                    first_mismatch = f;
                    fprintf(stderr, "first mismatch at session frame %u: the log says %08X, the replay %08X\n",
                            (unsigned)f, in_check[f], c);
                }
                mismatches++;
            }
        }
        if (r == EMU_SLICE_FRAME && save_frame && g_emu_frames == save_frame) {
            /* ... and the samples from here on are what the loaded run's are. */
            const char *err = state_mem ? mem_state_save(&emu, save_path) : emu_state_save_now(&emu, save_path);
            if (err) { fprintf(stderr, "--save-at %s: %s\n", save_path, err); return 2; }
            snd_out_hash = FNV0; snd_out_n = 0;
            fprintf(stderr, "saved %s at frame %u\n", save_path, (unsigned)g_emu_frames);
        }
        if (r == EMU_SLICE_FRAME && rewind_k && !rewind_frame(&emu, g_emu_frames)) continue;
        if (r != EMU_SLICE_FRAME || g_emu_frames < from) continue;
        uint32_t check = netplay_frame_check(&emu.cpu_snapshot, emu.total_steps);
        uint64_t ram  = fnv(fnv(FNV0, bus.ram, RAM_SIZE), bus.ram2, RAM2_SIZE);
        uint64_t buf  = fnv(FNV0, bus.buff_ram, BUFF_RAM_SIZE);
        uint64_t dm   = fnv(FNV0, g_sharc.dm, sizeof g_sharc.dm);
        fprintf(out, "%u %08x %016llx %016llx %016llx %llu", (unsigned)g_emu_frames, check,
                (unsigned long long)ram, (unsigned long long)buf, (unsigned long long)dm,
                (unsigned long long)emu.total_steps);
        if (cpu_cols) {
            uint64_t tex  = fnv(fnv(FNV0, bus.texram0, TEXRAM0_SIZE), bus.texram1, TEXRAM1_SIZE);
            uint64_t regs = fnv(fnv(fnv(FNV0, cpu.globals.g, sizeof cpu.globals.g), cpu.locals.r, sizeof cpu.locals.r), &cpu.sfr, sizeof cpu.sfr);
            fprintf(out, " %016llx %016llx %llu", (unsigned long long)tex, (unsigned long long)regs,
                    (unsigned long long)cpu.cycles);
        }
        if (sound_cols) {
            sound_settle();
            fprintf(out, " %016llx %llu", (unsigned long long)fnv(FNV0, g_sound.ram, sizeof g_sound.ram),
                    (unsigned long long)g_sound.out_total);
        }
        if (peek_addr) fprintf(out, " %02x", mem_read8(&bus, peek_addr));
        fputc('\n', out);
    }
    if (out != stdout) fclose(out);
    if (follow_out) {
        fprintf(stderr, "follow: led %u segments, the last join point took %u ms\n", g_follow_lead.seg, g_follow_lead.join_ms);
        follow_lead_stop();
    }
    if (follow_in) {
        fprintf(stderr, "follow: %u segments, %u frame checks held%s%s%s\n", fol.segs, fol.checks + g_follow.checks,
                g_follow.split ? "; SPLIT: " : "", g_follow.split ? g_follow.why : "",
                g_follow.ended ? "; the feed ended" : "");
        if (g_follow.split) return 1;
    }
    rewind_report();
    fprintf(stderr, "texload: %llu rows in C\n", (unsigned long long)g_texload_rows);
    fprintf(stderr, "spin: %llu idle iterations skipped\n", (unsigned long long)g_spin_iters);
    if (cop_out) fclose(cop_out);
    if (trace_out) fclose(trace_out);
    fprintf(stderr, "%u frames, %llu slices, %llu i960 steps\n", (unsigned)g_emu_frames,
            (unsigned long long)slices, (unsigned long long)emu.total_steps);
    sound_settle();
    fprintf(stderr, "sound: %llu samples, output %016llx, sound RAM %016llx (%s, %llu slices handed over)\n",
            (unsigned long long)snd_out_n, (unsigned long long)snd_out_hash,
            (unsigned long long)fnv(FNV0, g_sound.ram, sizeof g_sound.ram),
            sound_thread_on() ? "sound thread" : "one thread", (unsigned long long)g_emu_times.sound_jobs);
    fprintf(stderr, "sound: %s; i960 commands %u, bytes that waited a slice %llu, midi drops %u\n",
            g_shle.on ? "driver in C (--sound-hle)" : "68000", g_sound.code_n,
            (unsigned long long)g_sound.midi_holds, g_sound.scsp.mi_drops);
    if (in_n) {
        if (mismatches) fprintf(stderr, "input log: %u of %u frames' checks differ, the first at session frame %u\n",
                                (unsigned)mismatches, (unsigned)g_emu_frames, (unsigned)first_mismatch);
        else            fprintf(stderr, "input log: every check matches (%u frames)\n", (unsigned)g_emu_frames);
        return mismatches ? 1 : 0;
    }
    return 0;
}
