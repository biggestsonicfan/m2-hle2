/*
 * cop_timing.h — how long the board's SHARC takes over each COP command, and
 * the i960's waits for it (board-level; Pinboard #710).
 *
 * The HLE answers a command the moment its last argument arrives. On the
 * board the i960 waits twice: a read of the reply FIFO stalls until the SHARC
 * has pushed the word, and a write that would put a 17th word in the command
 * FIFO halts it until the SHARC takes one (MAME copro_fifo_r / copro_fifo_w,
 * tools/mame/cop_timing.py). Those waits are i960 cycles the timers count, and
 * STF's rand() adds the timers in, so they are part of what the board computes.
 *
 * With g_cop_timing on, cop.h keeps a clock for the SHARC beside the i960's
 * (cpu->cycles, through g_cop_clock) and steps it per FIFO event, as
 * cop_timing.py's replay does:
 *   - taking a word: at max(when the SHARC is free, when it was written), plus
 *     the work before it (a command word: the previous command's tail);
 *   - a reply: the work after the last argument (r1), then rn per later word;
 *   - the i960 is moved on to the time the word it reads, or the slot it
 *     writes, is there.
 * The work is MAME's, measured off a capture per command and event kind
 * (tools/mame/cop_cost_table.py), scaled to the board as #689 fitted it:
 * x 1.15, plus 6.5 counts per bufferram word the firmware touches.
 *
 * Off (the default), nothing here runs and the board is the one it was.
 */
#ifndef COP_TIMING_H
#define COP_TIMING_H

#include <stdint.h>
#include <stdbool.h>

#define COP_FIFO_DEPTH   16
#define COP_TQ           16     /* model clock ticks per board count (i960 cycle) */
#define COP_BOARD_NUM    115    /* the board's SHARC time is MAME's x 1.15 */
#define COP_BOARD_DEN    100
#define COP_BUF_WORD     104    /* 6.5 counts per bufferram word, in ticks */
#define COP_COPY_TICKS   (66 * COP_TQ)   /* one afterimage copy: _L20A47's loop, ~85 SHARC cycles */
#define COP_PART_TICKS   (66 * COP_TQ)   /* a part's set-up before its copies (_L20A17) */

/* One command's MAME work in ticks: the first and each later argument, the
 * first and each later reply, the tail before the next command word, and the
 * bufferram words it touches. per_word: r1 is r1 + per_word x the words the
 * port touched (Fn_calc_coli_flag, Fn_parts_oidasi); buf_reply: bufferram
 * words per reply, in ticks of a word (Fn_osage). */
typedef struct {
    uint32_t a1, an;
    int32_t  r1;                       /* signed: a fitted intercept can be below 0 */
    uint32_t rn, tail;
    uint32_t buf;
    uint32_t per_word, buf_reply;
} cop_cost_t;

static const cop_cost_t COP_COST_DEFAULT = { 15 * COP_TQ, 15 * COP_TQ, 18 * COP_TQ, 3 * COP_TQ, 20 * COP_TQ, 0, 0, 0 };

/* Indexed by the command's opcode byte. A row of zeros is a command no
 * capture saw, and takes COP_COST_DEFAULT. */
static const cop_cost_t g_cop_cost[256] = {
#include "cop_timing_rows.inc"
};

/* The SHARC's side of the clock. Board state: it is in g_cop, so savestates
 * and rollback carry it. */
typedef struct {
    uint64_t sh_t;                     /* when the SHARC finished its last FIFO event (ticks) */
    uint64_t pop_t[COP_FIFO_DEPTH];    /* when it took each of the last 16 input words */
    uint64_t rep_t;                    /* reply rep_k is ready then, each later one rep_dt on */
    uint64_t stall;                    /* i960 cycles spent waiting on the FIFOs */
    uint32_t in_n;                     /* input words taken, kept in [0, 32) once past 16 */
    uint32_t rep_dt;
    int32_t  rep_k;
    uint32_t op;                       /* the command in flight */
    uint32_t args;                     /* its arguments taken */
    uint32_t tail;                     /* work it owes before the next command word (ticks, board) */
} cop_timing_t;

static int       g_cop_timing = 0;       /* --cop-timing */
static uint64_t *g_cop_clock  = NULL;    /* the i960's cycle count (emu_ctx_init) */

static inline bool cop_timing_on(void) { return g_cop_timing && g_cop_clock; }

static inline const cop_cost_t *cop_cost(uint32_t op) {
    const cop_cost_t *c = &g_cop_cost[op & 0xFFu];
    return (c->a1 | c->an | (uint32_t)c->r1 | c->rn | c->tail) ? c : &COP_COST_DEFAULT;
}

static inline uint64_t cop_t_board(uint64_t mame) { return mame * COP_BOARD_NUM / COP_BOARD_DEN; }
static inline uint64_t cop_t_now(void)            { return *g_cop_clock * COP_TQ; }

/* Move the i960 on to `until`, if it is not there yet. */
static inline void cop_t_wait(cop_timing_t *t, uint64_t until) {
    uint64_t now = cop_t_now();
    if (until <= now) return;
    uint64_t c = (until - now + COP_TQ - 1) / COP_TQ;
    *g_cop_clock += c;
    t->stall += c;
}

/* The i960 writes a word that `work` (board ticks) of SHARC time precedes. */
static inline void cop_t_input(cop_timing_t *t, uint64_t work) {
    if (t->in_n >= COP_FIFO_DEPTH) cop_t_wait(t, t->pop_t[t->in_n % COP_FIFO_DEPTH]);
    uint64_t now = cop_t_now();
    uint64_t at  = (t->sh_t > now ? t->sh_t : now) + work;
    t->sh_t = at;
    t->pop_t[t->in_n % COP_FIFO_DEPTH] = at;
    if (++t->in_n >= 2 * COP_FIFO_DEPTH) t->in_n -= COP_FIFO_DEPTH;
}

static inline void cop_t_command(cop_timing_t *t, uint32_t op) {
    cop_t_input(t, t->tail);
    t->op   = op & 0xFFu;
    t->args = 0;
    t->tail = 0;
}

static inline void cop_t_arg(cop_timing_t *t) {
    const cop_cost_t *c = cop_cost(t->op);
    cop_t_input(t, cop_t_board(t->args++ ? c->an : c->a1));
}

/* Replies [k0, k1) were pushed, `work` (MAME ticks) after the SHARC's last
 * event for the first and rn apart. */
static inline void cop_t_replies(cop_timing_t *t, int k0, int k1, uint64_t work) {
    if (k1 <= k0) return;
    const cop_cost_t *c = cop_cost(t->op);
    t->rep_k  = k0;
    t->rep_t  = t->sh_t + cop_t_board(work);
    t->rep_dt = (uint32_t)cop_t_board(c->rn);
    t->sh_t   = t->rep_t + (uint64_t)t->rep_dt * (uint64_t)(k1 - k0 - 1);
}

/* A fixed-length command ran: it answered `replies` words and its port
 * touched `ext_words` of bufferram. */
static inline void cop_t_done(cop_timing_t *t, int replies, uint32_t ext_words) {
    const cop_cost_t *c = cop_cost(t->op);
    uint64_t words = c->per_word ? ext_words
                   : c->buf + ((uint64_t)c->buf_reply * (uint64_t)replies) / COP_TQ;
    uint64_t buf   = words * COP_BUF_WORD;
    int64_t  r1s   = (int64_t)c->r1 + (int64_t)c->per_word * ext_words;
    uint64_t r1    = r1s > 0 ? (uint64_t)r1s : 0;
    if (replies > 0) {
        cop_t_replies(t, 0, replies, r1);
        t->rep_t += buf;
        t->sh_t  += buf;
        t->tail   = (uint32_t)cop_t_board(c->tail);
    } else {
        t->tail   = (uint32_t)(cop_t_board(c->tail) + buf);
    }
}

/* Fn_zanzou_reserve's stream: each word is an argument, and a reply it
 * pushed comes r1 later; the terminator's answer comes after the copies. */
static inline void cop_t_stream(cop_timing_t *t, int k0, int k1, bool done, uint32_t parts, uint32_t copies) {
    const cop_cost_t *c = cop_cost(t->op);
    if (k1 > k0) {
        uint64_t r1 = c->r1 > 0 ? (uint64_t)c->r1 : 0;
        uint64_t w  = done ? r1 + (uint64_t)parts * COP_PART_TICKS + (uint64_t)copies * COP_COPY_TICKS : r1;
        cop_t_replies(t, k0, k1, w);
    }
    if (done) t->tail = (uint32_t)cop_t_board(c->tail);
}

/* When reply k of the command answering is in the FIFO. */
static inline uint64_t cop_t_ready(const cop_timing_t *t, int k) {
    return k >= t->rep_k ? t->rep_t + (uint64_t)t->rep_dt * (uint64_t)(k - t->rep_k) : 0;
}

#endif /* COP_TIMING_H */
