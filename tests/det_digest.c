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

#define SCRIPT_MAX 1024
static struct { uint32_t frame, held; } script[SCRIPT_MAX];
static int script_n;

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
    g_frame_done = 0;
    bool board_vblank = g_active_profile->quirks.board_vblank;
    if (board_vblank) {
        irqt_raise(0x1u);
        g_vblank_acked = 0;
        g_cop.geo_frame_start = g_cop.geo_frame_end;
        g_cop.geo_frame_end   = g_cop.geo_capture_head;
    }
    emu_timers_slice_begin(ctx);
    emu_service_irq(ctx);
    for (int i = 0; i < g_emu_steps_per_slice && !g_frame_done && !(board_vblank && g_vblank_acked)
                    && !ctx->request_stop && !ctx->cpu->halted; i++) {
        if (ctx->step_over_bp) ctx->step_over_bp = 0;
        else if (bp_check(ctx->cpu->sfr.ip)) break;
        uint32_t ip = ctx->cpu->sfr.ip;
        if (i960_step_hot(ctx->cpu, ctx->bus) != 0) break;
        ctx->total_steps++;
        const i960_cpu_t *c = ctx->cpu;
        uint64_t h = fnv(FNV0, c->globals.g, sizeof c->globals.g);
        h = fnv(h, c->locals.r, sizeof c->locals.r);
        h = fnv(h, &c->sfr.ac, sizeof c->sfr.ac);
        h = fnv(h, c->fp_regs, sizeof c->fp_regs);
        fprintf(trace_out, "%08x %016llx\n", ip, (unsigned long long)h);
        if (s_irq_in_service && g_active_profile) emu_service_sound_again(ctx);
        if (g_irqt_live) emu_timers_after_step(ctx);
        if (g_log.warn_triggered) break;
        if (g_wp.hit) break;
        if (g_sharc.unknown_triggered) break;
    }
    if (g_frame_done || (board_vblank && g_vblank_acked)) {
        emu_timers_frame_edge(ctx);
        dl_frame_edge(ctx->bus, g_emu_frames);
        emu_match_replay_edge(ctx);
    }
    sound_run_slice(EMU_SLICES_PER_SEC);
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
        fprintf(stderr, "usage: det_digest <merged sfight zip> [--frames N] [--script S] [--from F] [--out FILE]\n");
        return 2;
    }
    uint32_t frames = 3600, from = 0;
    bool frames_given = false;
    const char *out_path = NULL, *script_text = NULL, *inputs_path = NULL;
    for (int i = 2; i < argc; i++) {
        if      (!strcmp(argv[i], "--frames") && i + 1 < argc) { frames = (uint32_t)strtoul(argv[++i], NULL, 10); frames_given = true; }
        else if (!strcmp(argv[i], "--inputs") && i + 1 < argc) inputs_path = argv[++i];
        else if (!strcmp(argv[i], "--from")   && i + 1 < argc) from   = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script_text = argv[++i];
        else if (!strcmp(argv[i], "--out")    && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--trace")  && i + 1 < argc) {
            char path[1024] = {0};
            if (sscanf(argv[++i], "%u:%1023s", &trace_frame, path) != 2 || !(trace_out = fopen(path, "wb"))) {
                fprintf(stderr, "--trace F:FILE\n");
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
        if (!strcmp(g_profiles[i]->id, "sfight")) g_active_profile = g_profiles[i];
    if (!g_active_profile) { fprintf(stderr, "no sfight profile\n"); return 2; }
    if (script_text) parse_script(script_text);
    if (inputs_path) {
        if (!load_inputs(inputs_path)) { fprintf(stderr, "cannot read an input log from %s\n", inputs_path); return 2; }
        if (!frames_given || frames > in_n) frames = in_n;
        fprintf(stderr, "replaying %u session frames from %s\n", (unsigned)in_n, inputs_path);
    }

    /* The web build's load: one zip, read whole, strict by CRC. */
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

    FILE *out = out_path ? fopen(out_path, "wb") : stdout;
    if (!out) { fprintf(stderr, "cannot write %s\n", out_path); return 2; }

    int at = 0;
    uint64_t slices = 0;
    uint32_t mismatches = 0, first_mismatch = UINT32_MAX;
    while (g_emu_frames < frames) {
        /* Inputs change only on a frame boundary, as the lockstep's do. */
        while (at < script_n && script[at].frame <= g_emu_frames) g_input.held = script[at++].held;
        if (in_n && g_emu_frames < in_n) g_input.held = words_mask(in_w0[g_emu_frames], in_w1[g_emu_frames]);
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
        if (r != EMU_SLICE_FRAME || g_emu_frames < from) continue;
        uint32_t check = netplay_frame_check(&emu.cpu_snapshot, emu.total_steps);
        uint64_t ram  = fnv(fnv(FNV0, bus.ram, RAM_SIZE), bus.ram2, RAM2_SIZE);
        uint64_t buf  = fnv(FNV0, bus.buff_ram, BUFF_RAM_SIZE);
        uint64_t dm   = fnv(FNV0, g_sharc.dm, sizeof g_sharc.dm);
        fprintf(out, "%u %08x %016llx %016llx %016llx %llu\n", (unsigned)g_emu_frames, check,
                (unsigned long long)ram, (unsigned long long)buf, (unsigned long long)dm,
                (unsigned long long)emu.total_steps);
    }
    if (out != stdout) fclose(out);
    if (cop_out) fclose(cop_out);
    if (trace_out) fclose(trace_out);
    fprintf(stderr, "%u frames, %llu slices, %llu i960 steps\n", (unsigned)g_emu_frames,
            (unsigned long long)slices, (unsigned long long)emu.total_steps);
    if (in_n) {
        if (mismatches) fprintf(stderr, "input log: %u of %u frames' checks differ, the first at session frame %u\n",
                                (unsigned)mismatches, (unsigned)g_emu_frames, (unsigned)first_mismatch);
        else            fprintf(stderr, "input log: every check matches (%u frames)\n", (unsigned)g_emu_frames);
        return mismatches ? 1 : 0;
    }
    return 0;
}
