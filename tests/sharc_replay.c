/*
 * sharc_replay.c — replay a MAME capture of a Model 2B game's COP traffic
 * through the SHARC interpreter (sharc_cpu.h) and check every word it writes
 * back to the i960 (Pinboard #949).
 *
 * The capture (tools/mame-sharc-tap.lua) is a flat stream of 9-byte records
 * (u8 kind, u32 address, u32 value), little-endian, in MAME's order:
 *   0  a frame ended (address: the frame number)
 *   1  the i960 wrote 0x880000..0x887FFF: the function port or FIFO in
 *   2  the i960 wrote COPRO_CTL (0x980000)
 *   3  the i960 wrote buffer RAM (0x900000..0x91FFFF)
 *   4  the SHARC wrote FIFO out: the word to check
 * Every input comes from MAME, so a wrong word stays one wrong word: what the
 * i960 does with it never reaches this replay. Only the SHARC's own state (its
 * matrix stack, say) can carry a difference on to later commands.
 *
 * The SHARC runs here as model2b_cop.h runs it, eagerly: each push runs it until
 * it blocks. MAME's SHARC runs on its own clock, so a buffer RAM word it reads
 * may be older or newer there; the report counts such reads.
 *
 * Usage: sharc_replay <capture> <copro_data.bin> [examples-per-command]
 * (copro_data.bin: the tap's SHARC_TAP_ROM dump, the ROM as the SHARC reads it)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "board/sharc_cpu.h"

#define FIFO 4096u
#define BUFRAM_WORDS 0x8000u
#define CMD_READ_PC 0x2012Bu    /* Virtual-On's command loop: the word read here is a command */

static uint32_t fin[FIFO], fin_r, fin_n;
static uint32_t fout[FIFO], fout_r, fout_n;
static uint32_t bufram[BUFRAM_WORDS];
static uint8_t *rom;
static size_t rom_words;
static uint32_t coproctl, coprocnt, buf_reads;

/* the command the SHARC is running, and the arguments it has read */
static uint32_t cur_cmd = 0xFFFFFFFFu, args[64], nargs;

static void flags(void)
{
    shc_set_flag_input(0, fin_n == 0);
    shc_set_flag_input(1, fout_n >= FIFO);
}

static uint32_t rd(uint32_t addr, bool *stall)
{
    if (addr >= 0x0400000u && addr < 0x0C00000u) {
        if (fin_n == 0) { *stall = true; return 0; }
        uint32_t v = fin[fin_r];
        fin_r = (fin_r + 1u) & (FIFO - 1u);
        fin_n--;
        flags();
        if (g_shc.pc == CMD_READ_PC) {
            cur_cmd = v;
            nargs = 0;
        } else if (nargs < 64) {
            args[nargs++] = v;
        }
        return v;
    }
    if (addr >= 0x1400000u && addr < 0x1C00000u) {
        buf_reads++;
        return bufram[addr & (BUFRAM_WORDS - 1u)];
    }
    if (addr >= 0x1C00000u && addr < 0x1E00000u) {
        size_t w = addr - 0x1C00000u;
        if (w >= rom_words) return 0;
        const uint8_t *p = rom + w * 4u;
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    }
    return 0;
}

static void wr(uint32_t addr, uint32_t v, bool *stall)
{
    if (addr >= 0x0C00000u && addr < 0x1400000u) {
        if (fout_n >= FIFO) { *stall = true; return; }
        fout[(fout_r + fout_n++) & (FIFO - 1u)] = v;
        flags();
    } else if (addr >= 0x1400000u && addr < 0x1C00000u) {
        bufram[addr & (BUFRAM_WORDS - 1u)] = v;
    }
}

static void run(void)
{
    if (!g_shc.halted) shc_run(2000000);
}

static void push(uint32_t v)
{
    if (fin_n >= FIFO) { fprintf(stderr, "FIFO in overflow\n"); exit(2); }
    fin[(fin_r + fin_n++) & (FIFO - 1u)] = v;
    flags();
    run();
}

/* model2.cpp copro_function_port_w / copro_fifo_w / copro_ctl1_w */
static void i960_write(uint32_t addr, uint32_t v)
{
    uint32_t off = addr - 0x880000u;
    if (off < 0x4000u) {
        push((v & 0x800FFFFFu) | (((off >> 4) & 0xFFu) << 23));
    } else if (coproctl & 0x80000000u) {
        shc_boot_write(coprocnt++, (uint16_t)v);
    } else {
        push(v);
    }
}

static void ctl_write(uint32_t v)
{
    if ((v ^ coproctl) == 0x80000000u) {
        if (v & 0x80000000u) {
            coprocnt = 0;
            shc_reset();
        } else {
            coproctl = v;
            printf("boot: %u halfwords\n", (unsigned)coprocnt);
            shc_boot();
            flags();
            run();
            return;
        }
    }
    coproctl = v;
}

/* ---- The report ------------------------------------------------------------ */

typedef struct { uint32_t words, bad, ulp1, shown; } cmd_stat_t;
static cmd_stat_t stats[256];
static uint32_t total, bad, missing;

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static void show(uint32_t frame, uint32_t want, uint32_t got)
{
    printf("frame %u cmd %08x (%u args):", (unsigned)frame, (unsigned)cur_cmd, (unsigned)nargs);
    for (uint32_t i = 0; i < nargs && i < 16; i++) printf(" %08x", (unsigned)args[i]);
    printf("\n    MAME %08x (%.9g)  ours %08x (%.9g)\n", (unsigned)want, u2f(want), (unsigned)got, u2f(got));
}

static void check(uint32_t frame, uint32_t want, uint32_t per_cmd)
{
    total++;
    if (fout_n == 0) {
        if (missing++ < 8) printf("frame %u: MAME wrote %08x, ours nothing yet\n", (unsigned)frame, (unsigned)want);
        return;
    }
    uint32_t got = fout[fout_r];
    fout_r = (fout_r + 1u) & (FIFO - 1u);
    fout_n--;
    flags();
    run();
    cmd_stat_t *s = &stats[cur_cmd & 0xFF];
    s->words++;
    if (got == want) return;
    bad++;
    s->bad++;
    uint32_t d = got > want ? got - want : want - got;
    if (d == 1) s->ulp1++;
    if (s->shown++ < per_cmd) show(frame, want, got);
}

static void *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "%s: read failed\n", path); exit(1); }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: sharc_replay <capture> <copro_data.bin> [examples-per-command]\n");
        return 1;
    }
    size_t cap_size, rom_size;
    uint8_t *cap = slurp(argv[1], &cap_size);
    rom = slurp(argv[2], &rom_size);
    rom_words = rom_size / 4u;
    uint32_t const per_cmd = argc > 3 ? (uint32_t)atoi(argv[3]) : 4u;

    for (uint32_t i = 0; i < BUFRAM_WORDS; i++) bufram[i] = 0x07800F0Fu;  /* MAME machine_reset */
    shc_ext_rd = rd;
    shc_ext_wr = wr;
    shc_reset();
    flags();

    uint32_t frame = 0;
    for (size_t i = 0; i + 9u <= cap_size; i += 9u) {
        uint32_t const a = le32(cap + i + 1), v = le32(cap + i + 5);
        switch (cap[i]) {
        case 0: frame = a; break;
        case 1: i960_write(a, v); break;
        case 2: ctl_write(v); break;
        case 3: bufram[(a - 0x900000u) / 4u & (BUFRAM_WORDS - 1u)] = v; break;
        case 4: check(frame, v, per_cmd); break;
        default: fprintf(stderr, "bad record kind %u at %zu\n", cap[i], i); return 1;
        }
        if (g_shc.fault) { printf("frame %u: SHARC fault at pc %05x\n", (unsigned)frame, (unsigned)g_shc.pc); break; }
    }

    printf("\n%u frames, %u words checked, %u differ, %u missing, %u left over, %u buffer RAM reads\n",
           (unsigned)frame, (unsigned)total, (unsigned)bad, (unsigned)missing, (unsigned)fout_n, (unsigned)buf_reads);
    for (int c = 0; c < 256; c++)
        if (stats[c].bad)
            printf("  cmd %02x: %u of %u words differ (%u by 1 ulp)\n", c,
                   (unsigned)stats[c].bad, (unsigned)stats[c].words, (unsigned)stats[c].ulp1);
    return bad || missing ? 3 : 0;
}
