/*
 * qtlink.c — the QT960's half of the QT960 <-> Model 2B link (Pinboard #312).
 *
 * Runs on an Intel QT960 (i960KB) under NINDY, loaded at 0x10100000 and started with
 * "go 10100000". It talks to m2-kernel on a Model 2B (the kernel's serial protocol,
 * m2-kernel's src/m2k_proto.h) and checks that both i960s give the same answer to the
 * same instruction: stubs.s holds the instructions, qtlink copies them to the Model 2B,
 * then runs each one on both boards with the same random operands.
 *
 * The QT960 has one serial port, and NINDY's terminal is on it, so the link rides on it
 * as text: a command frame goes out as a line ">A50004...\r" and the reply comes back as
 * "<5A0004...\r". Whatever sits on the PC end (qt960_link.lua in MAME; a script on a
 * real PC) moves those frames to the kernel's serial port and back. Every other byte is
 * ordinary terminal output for the person watching.
 */
typedef unsigned int  u32;
typedef unsigned char u8;

/* ---- the 82510 (polled, as NINDY's own board_co / board_ci) ------------------------ */
#define SER_DATA (*(volatile u32 *)0x20000000u)
#define SER_LSR  (*(volatile u32 *)0x20000014u)
#define LSR_DR   0x01u
#define LSR_THRE 0x20u

static void put(int c) { while (!(SER_LSR & LSR_THRE)) ; SER_DATA = (u32)(c & 0xFF); }
static void puts_(const char *s) { while (*s) put(*s++); }
static void nl(void) { put('\r'); put('\n'); }
static void hex(u32 v, int digits) { while (digits--) put("0123456789ABCDEF"[(v >> (digits * 4)) & 15]); }
static void dec(u32 v, int width) {
    char b[11]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (width-- > n) put('0');
    while (n) put(b[--n]);
}
static void pad(const char *s, int width) { while (*s) { put(*s++); width--; } while (width-- > 0) put(' '); }

/* a byte from the line, or -1 after about `spins` empty polls */
static int get(u32 spins) {
    while (spins--) if (SER_LSR & LSR_DR) return (int)(SER_DATA & 0xFF);
    return -1;
}

/* ---- the kernel's protocol (m2k_proto.h) --------------------------------------- */
#define SOF_CMD  0xA5u
#define SOF_RSP  0x5Au
#define CMD_PING        0x00u
#define CMD_STATUS      0x04u
#define CMD_POKE        0x21u
#define CMD_PEEK_BLOCK  0x22u
#define CMD_CALL        0x52u
#define CMD_OVERLAY     0x54u
#define CMD_WATCH       0x55u
#define M2K_NOTE        0x0023F034u  /* m2k_shared_t.note: the overlay's last row */

/* where the stubs and qtlink's counters go in the kernel's RAM (0x200000-0x23FFFF; the
 * kernel's own variables sit at its bottom, the link record at 0x23F000) */
#define REMOTE_STUBS    0x00230000u
#define REMOTE_COUNT    0x0023E000u  /* tests run    (QTTESTS) */
#define REMOTE_BAD      0x0023E004u  /* mismatches   (QTBAD)   */
#define REMOTE_LAST     0x0023E008u  /* last answer  (QTLAST)  */

/* polls before a reply counts as lost: some seconds on the board. The Model 2B answers
 * a frame within a video frame or two, but two MAMEs need not run at the same speed. */
#define SPINS_BYTE  40000000u

static u8 rbuf[208];
static int rlen;

static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 get32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static int unhex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* one exchange: the frame goes out as a '>' line, the reply comes in as a '<' line.
 * The '>' line is wiped again so the terminal keeps only what a person wants to read.
 * Returns the reply's status, or -1 (lost or garbled: the caller retries). */
static int xact_once(u8 cmd, const u8 *p, int n) {
    u8 sum = (u8)(cmd + n);
    int i, c, h, lo, got = 0, width = 2 * (n + 4) + 1;
    u8 b[4 + 210];

    put('>'); hex(SOF_CMD, 2); hex(cmd, 2); hex((u32)n, 2);
    for (i = 0; i < n; i++) { hex(p[i], 2); sum = (u8)(sum + p[i]); }
    hex(sum, 2); put('\r');

    do c = get(SPINS_BYTE); while (c >= 0 && c != '<');
    while (c >= 0) {
        c = get(SPINS_BYTE);
        if (c == '\r' || c < 0) break;
        if ((h = unhex(c)) < 0 || (c = get(SPINS_BYTE)) < 0 || (lo = unhex(c)) < 0) { c = -1; break; }
        if (got < (int)sizeof b) b[got++] = (u8)(h << 4 | lo);
    }
    for (i = 0; i < width; i++) put(' ');
    put('\r');
    if (c < 0 || got < 4 || b[0] != SOF_RSP || got != 4 + b[2]) return -1;
    for (sum = 0, i = 1; i < got - 1; i++) sum = (u8)(sum + b[i]);
    if (sum != b[got - 1]) return -1;
    rlen = b[2];
    for (i = 0; i < rlen; i++) rbuf[i] = b[3 + i];
    return b[1];
}

static u32 lost;

static int xact(u8 cmd, const u8 *p, int n) {
    int tries, st = -1;
    for (tries = 0; tries < 5 && st < 0; tries++)
        if ((st = xact_once(cmd, p, n)) < 0) lost++;
    return st;
}

static int poke32(u32 addr, u32 v) {
    u8 p[9];
    put32(p, addr); p[4] = 4; put32(p + 5, v);
    return xact(CMD_POKE, p, 9);
}

static int watch(int slot, u32 addr, const char *label) {
    u8 p[14]; int n = 6;
    p[0] = (u8)slot; put32(p + 1, addr); p[5] = 4;
    while (*label && n < 14) p[n++] = (u8)*label++;
    return xact(CMD_WATCH, p, n);
}

/* the kernel's note line (the overlay's last row): a word at a time, through the NUL */
static void note(const char *s) {
    int i = 0, k;
    for (;;) {
        u32 w = 0; int end = 0;
        for (k = 0; k < 4; k++) {
            u8 c = end ? 0 : (u8)s[i + k];
            if (!c) end = 1;
            w |= (u32)c << (8 * k);
        }
        poke32(M2K_NOTE + (u32)i, w);
        if (end) break;
        i += 4;
    }
}

/* ---- the checks ------------------------------------------------------------------ */
struct test { const char *name; const u8 *start, *end; u32 mode; };
extern const struct test tests[];
extern const u8 __stubs_start[], __stubs_end[];

#define M_DIV   1u
#define M_CARRY 2u
#define M_COUNT 4u
#define M_BIT   8u
#define M_CC    16u

typedef u32 (*stub_fn)(u32, u32, u32, u32);

static u32 rng = 0x2F6E2B1u;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* operands that show edges more often than plain random words do */
static u32 operand(void) {
    switch (rnd() & 7) {
    case 0: return 0;
    case 1: return 0xFFFFFFFFu;
    case 2: return 0x80000000u;
    case 3: return 0x7FFFFFFFu;
    case 4: return rnd() & 0xFF;
    default: return rnd();
    }
}

static int fail(const char *what) {
    nl(); puts_("qtlink: "); puts_(what); puts_(" -- no answer from the Model 2B"); nl();
    puts_("qtlink: stopped; reset the QT960 to get NINDY back"); nl();
    return 1;
}

int main(void) {
    const struct test *t;
    u8 p[20];
    u32 count = 0, bad = 0, a[4], here, there, i, n;
    int st, k;

    nl();
    puts_("qtlink: QT960 (i960KB, NINDY) <-> Model 2B (m2-kernel) instruction check"); nl();
    puts_("qtlink: frames go out as >hex lines and come back as <hex lines"); nl();

    /* PING until the kernel answers: the other MAME may still be booting */
    for (k = 0; (st = xact_once(CMD_PING, 0, 0)) != 0 || rlen != 4; k++) {
        if (k == 0) { puts_("qtlink: waiting for the Model 2B ..."); nl(); }
    }
    puts_("PING      -> "); for (i = 0; i < 4; i++) put(rbuf[i]); puts_("  link up"); nl();

    if (xact(CMD_STATUS, 0, 0) != 0 || rlen < 13) return fail("STATUS");
    puts_("STATUS    -> kernel v"); dec(rbuf[0], 1);
    puts_(", tick "); hex(get32(rbuf + 1), 8);
    puts_(", "); dec(get32(rbuf + 5), 1); puts_(" frames served"); nl();

    p[0] = 1;
    xact(CMD_OVERLAY, p, 1);
    note("LINKED TO QT960: I960KB UNDER NINDY, CHECKING INSTRUCTIONS");
    poke32(REMOTE_COUNT, 0); poke32(REMOTE_BAD, 0); poke32(REMOTE_LAST, 0);
    watch(0, REMOTE_COUNT, "QTTESTS");
    watch(1, REMOTE_BAD, "QTBAD");
    watch(2, REMOTE_LAST, "QTLAST");
    puts_("OVERLAY   -> note, watches QTTESTS QTBAD QTLAST on the Model 2B's screen"); nl();

    /* the stubs, word by word, then read back */
    n = (u32)(__stubs_end - __stubs_start);
    puts_("UPLOAD    -> "); dec(n / 4, 1); puts_(" words of stubs to "); hex(REMOTE_STUBS, 8); nl();
    for (i = 0; i < n; i += 4)
        if (poke32(REMOTE_STUBS + i, get32(__stubs_start + i)) != 0) return fail("POKE");
    for (i = 0; i < n; i += 120) {
        u32 len = n - i < 120 ? n - i : 120, j;
        put32(p, REMOTE_STUBS + i); p[4] = (u8)len;
        if (xact(CMD_PEEK_BLOCK, p, 5) != 0 || rlen != (int)len) return fail("PEEK_BLOCK");
        for (j = 0; j < len; j++)
            if (rbuf[j] != __stubs_start[i + j]) {
                puts_("READBACK  -> differs at "); hex(REMOTE_STUBS + i + j, 8); nl();
                return fail("READBACK");
            }
    }
    puts_("READBACK  -> identical"); nl();
    note("QT960 LINK UP: SAME INSTRUCTION, BOTH BOARDS, COMPARE G0");
    nl();
    puts_("    # INSTR     G0       G1       G2       QT960    MODEL2B"); nl();

    for (;;) {
        for (t = tests; t->name; t++) {
            for (k = 0; k < 4; k++) a[k] = operand();
            if (t->mode & M_DIV)   a[1] |= 1;
            if (t->mode & M_CARRY) a[2] = rnd() & 2;
            if (t->mode & M_COUNT) a[1] = rnd() % 40;
            if (t->mode & M_BIT)   { a[1] = rnd() & 31; a[2] = rnd() & 31; }
            if (t->mode & M_CC)    a[2] = rnd() & 7;

            here = ((stub_fn)(const void *)t->start)(a[0], a[1], a[2], a[3]);

            put32(p, REMOTE_STUBS + (u32)(t->start - __stubs_start));
            for (k = 0; k < 4; k++) put32(p + 4 + 4 * k, a[k]);
            if (xact(CMD_CALL, p, 20) != 0 || rlen != 4) return fail("CALL");
            there = get32(rbuf);

            count++;
            dec(count % 100000, 5); put(' '); pad(t->name, 10);
            hex(a[0], 8); put(' '); hex(a[1], 8); put(' '); hex(a[2], 8); put(' ');
            hex(here, 8); put(' '); hex(there, 8);
            if (here == there) puts_("  OK");
            else { bad++; puts_("  DIFF"); }
            nl();

            if (here != there || (count & 7) == 0) {
                poke32(REMOTE_COUNT, count);
                poke32(REMOTE_BAD, bad);
                poke32(REMOTE_LAST, there);
            }
            if (here != there && bad == 1) note("QT960 LINK: FIRST DIFFERENCE, SEE THE QT960 TERMINAL");
        }
    }
}
