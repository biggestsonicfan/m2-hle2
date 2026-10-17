/*
 * link_pace_test.c -- a linked comm board's 60 Hz clock (core/link_pace.h,
 * Pinboard #978), on simulated hosts and a simulated pair of cabinets. No ROM.
 *
 * A called host (retro_run, the page's frame callback) at the display's rate,
 * from 30 Hz to fast-forward, runs the board at 60 Hz; a 60 Hz one with jitter
 * runs one frame a call. A late call repays at most LINK_PACE_MAX_DEBT frames
 * and drops the rest. A sleeping host (the emu thread) keeps 60 Hz across a
 * slow frame. Two cabinets whose clocks drift apart, each taking one of the
 * other's frames a board frame from an inbox that keeps the 8 newest (as
 * net/comm_link.h does), lose none of them with the backlog trim and lose
 * them without it. The real board on the link: det_digest --link-host-hz.
 *
 * A ctest (CMakeLists.txt, link_pace_test).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "link_pace.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t g_rng = 0x12345678u;
static int32_t jitter(int32_t span) {   /* -span..span */
    g_rng = g_rng * 1664525u + 1013904223u;
    return (int32_t)((g_rng >> 8) % (uint32_t)(2 * span + 1)) - span;
}

/* ---- a called host ---- */

typedef struct {
    uint64_t frames, calls, zero, multi;
    uint32_t most;
} called_t;

/* One call at `now`: the frames it runs. */
static uint32_t call(link_pace_t *p, int64_t now, called_t *c) {
    link_pace_owed(p, now);
    uint32_t n = 0;
    while (link_pace_due(p, now)) { link_pace_ran(p, 0); n++; }
    c->frames += n;
    c->calls++;
    if (!n) c->zero++;
    if (n > 1) c->multi++;
    if (n > c->most) c->most = n;
    return n;
}

/* `seconds` of calls at `hz`, each landing up to `jit_us` early or late. */
static called_t run_called(double hz, int32_t jit_us, int seconds) {
    link_pace_t p; memset(&p, 0, sizeof p);
    called_t c; memset(&c, 0, sizeof c);
    int64_t start = 1000000;
    for (uint64_t k = 0;; k++) {
        int64_t t = start + (int64_t)((double)k * 1e6 / hz);
        if (t - start >= (int64_t)seconds * 1000000) break;
        call(&p, t + (jit_us ? jitter(jit_us) : 0), &c);
    }
    return c;
}

static void test_called_hosts(void) {
    static const double rates[] = { 30.0, 59.94, 60.0, 75.0, 120.0, 144.0, 240.0, 1000.0 };
    char msg[160];
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        called_t c = run_called(rates[i], 0, 60);
        snprintf(msg, sizeof msg, "%.2f Hz host: %llu board frames in 60 s (3600), at most %u a call",
                 rates[i], (unsigned long long)c.frames, c.most);
        CHECK(c.frames >= 3599 && c.frames <= 3601 && c.most <= 2, msg);
    }
    called_t j = run_called(60.0, 4000, 60);
    snprintf(msg, sizeof msg, "60 Hz host, +-4 ms jitter: %llu frames, %llu calls with none and %llu with two or more",
             (unsigned long long)j.frames, (unsigned long long)j.zero, (unsigned long long)j.multi);
    CHECK(j.frames >= 3599 && j.frames <= 3601 && !j.zero && !j.multi, msg);
    called_t s = run_called(10.0, 0, 60);
    snprintf(msg, sizeof msg, "10 Hz host: %u frames a call, %llu in 60 s (it cannot keep up; the rest is dropped)",
             s.most, (unsigned long long)s.frames);
    CHECK(s.most == LINK_PACE_MAX_DEBT && s.frames >= 600u * LINK_PACE_MAX_DEBT - 4u, msg);
}

/* A 60 Hz host that stalls for half a second: four frames, then one a call. */
static void test_hitch(void) {
    link_pace_t p; memset(&p, 0, sizeof p);
    called_t c; memset(&c, 0, sizeof c);
    int64_t t = 1000000;
    for (int k = 0; k < 60; k++, t += LINK_PACE_FRAME_US) call(&p, t, &c);
    t += 500000;
    uint32_t after = call(&p, t, &c);
    uint64_t dropped = p.dropped;
    called_t c2; memset(&c2, 0, sizeof c2);
    for (int k = 0; k < 60; k++) { t += LINK_PACE_FRAME_US; call(&p, t, &c2); }
    char msg[160];
    snprintf(msg, sizeof msg, "a 500 ms stall: the next call runs %u frames, %llu are dropped, then one a call",
             after, (unsigned long long)dropped);
    CHECK(after == LINK_PACE_MAX_DEBT && dropped >= 25 && dropped <= 27 && c2.frames == 60 && c2.most == 1, msg);
    link_pace_stop(&p);
    CHECK(!link_pace_running(&p) && link_pace_due(&p, t), "stopped: the next link's first frame is due at once");
}

/* The emu thread: a frame's work, then a sleep to link_pace_slept's time. */
static void test_sleeping_host(void) {
    link_pace_t p; memset(&p, 0, sizeof p);
    int64_t t = 1000000, start = t;
    uint32_t frames = 0, back_to_back = 0;
    while (t - start < 60 * 1000000) {
        int64_t work = (frames % 600 == 300) ? 40000 : 3000;   /* every 10 s a 40 ms frame */
        t += work;
        frames++;
        int64_t next = link_pace_slept(&p, t, 0);
        if (next > t) t = next;
        else back_to_back++;
    }
    char msg[160];
    snprintf(msg, sizeof msg, "sleeping host, a 40 ms frame every 10 s: %u frames in 60 s, %u run back to back",
             frames, back_to_back);
    CHECK(frames >= 3599 && frames <= 3601 && back_to_back >= 6 && back_to_back <= 18 && !p.dropped, msg);
}

/* ---- two cabinets ---- */

#define INBOX 8u   /* net/comm_link.h COMM_LINK_INBOX */

typedef struct {
    link_pace_t pace;
    double      rate;           /* its clock's speed against true time */
    int64_t     next_true;      /* true time of its next frame */
    int64_t     q_at[64];       /* in flight: arrival times, oldest first */
    uint32_t    q_n;
    uint32_t    inbox, peak, overflow, took, idle;
    uint64_t    frames;
    bool        master;
} cab_t;

static void cab_send(cab_t *to, int64_t arrive) {
    if (to->q_n < 64u) to->q_at[to->q_n++] = arrive;
}

/* Everything that has arrived by `now` into the inbox: the 8 newest kept. */
static void cab_deliver(cab_t *c, int64_t now) {
    uint32_t k = 0;
    while (k < c->q_n && c->q_at[k] <= now) {
        if (c->inbox == INBOX) c->overflow++;
        else                   c->inbox++;
        k++;
    }
    memmove(c->q_at, c->q_at + k, (c->q_n - k) * sizeof c->q_at[0]);
    c->q_n -= k;
}

/* One board frame of `c` at true time `now`: take one, send (the master each
 * frame, the slave an answer to what it took), then its clock's next frame. */
static void cab_frame(cab_t *c, cab_t *other, int64_t now, int32_t latency_us, bool trim) {
    cab_deliver(c, now);
    bool took = c->inbox > 0;
    if (took) { c->inbox--; c->took++; } else c->idle++;
    if (c->master || took) cab_send(other, now + latency_us + jitter(latency_us / 2));
    if (c->inbox > c->peak) c->peak = c->inbox;
    c->frames++;
    int64_t local = (int64_t)((double)now * c->rate);
    int64_t next  = link_pace_slept(&c->pace, local, trim && !c->master ? c->inbox : 0);   /* m2comm_backlog */
    c->next_true  = (int64_t)((double)(next > local ? next : local) / c->rate);
}

/* `minutes` of a master and a slave, the slave's clock `ppm` slower. */
static void run_pair(cab_t *m, cab_t *s, double ppm, int minutes, int32_t latency_us, bool trim) {
    memset(m, 0, sizeof *m); memset(s, 0, sizeof *s);
    m->master = true;
    m->rate = 1.0; s->rate = 1.0 - ppm * 1e-6;
    m->next_true = 1000000; s->next_true = 1000000 + 7000;
    int64_t end = 1000000 + (int64_t)minutes * 60 * 1000000;
    while (m->next_true < end || s->next_true < end) {
        if (m->next_true <= s->next_true) cab_frame(m, s, m->next_true, latency_us, trim);
        else                              cab_frame(s, m, s->next_true, latency_us, trim);
    }
}

static void test_pair(void) {
    cab_t m, s;
    char msg[200];
    run_pair(&m, &s, 0.0, 10, 20000, true);
    snprintf(msg, sizeof msg, "same clocks, 20 ms +-10 ms latency, 10 min: slave took %u of the master's %llu, "
             "inbox peak %u, %u lost, %u trims", s.took, (unsigned long long)m.frames, s.peak, s.overflow, s.pace.trims);
    CHECK(!s.overflow && !m.overflow && s.peak <= 3 && s.pace.trims <= 2, msg);

    run_pair(&m, &s, 500.0, 30, 20000, false);
    uint32_t lost = s.overflow;
    snprintf(msg, sizeof msg, "slave clock 500 ppm slow, 30 min, no trim: the slave's inbox fills (peak %u), %u frames lost",
             s.peak, lost);
    CHECK(s.peak >= INBOX - 1u && lost > 0, msg);

    run_pair(&m, &s, 500.0, 30, 20000, true);
    snprintf(msg, sizeof msg, "the same with the trim: %u trims, inbox peak %u, %u lost; master %llu frames, slave %llu",
             s.pace.trims, s.peak, s.overflow, (unsigned long long)m.frames, (unsigned long long)s.frames);
    CHECK(!s.overflow && !m.overflow && s.peak <= 3 && s.pace.trims >= 40 && s.pace.trims <= 80, msg);

    run_pair(&m, &s, -500.0, 30, 20000, true);
    snprintf(msg, sizeof msg, "slave clock 500 ppm fast: master inbox peak %u, %u lost, master %u trims, "
             "slave idle %u frames", m.peak, m.overflow, m.pace.trims, s.idle);
    CHECK(!s.overflow && !m.overflow && m.peak <= 3, msg);
}

/* A slave whose host stalled long enough to drop frames: the master's frames
 * pile up, and the trim works them off without losing one. */
static void test_pair_stall(void) {
    cab_t m, s;
    memset(&m, 0, sizeof m); memset(&s, 0, sizeof s);
    m.master = true; m.rate = s.rate = 1.0;
    m.next_true = 1000000; s.next_true = 1000000 + 7000;
    int64_t end = 1000000 + 60 * 1000000, stall_at = 1000000 + 20 * 1000000;
    bool stalled = false;
    uint32_t peak_after = 0;
    while (m.next_true < end || s.next_true < end) {
        if (!stalled && s.next_true >= stall_at) { s.next_true += 100000; stalled = true; }
        if (m.next_true <= s.next_true) cab_frame(&m, &s, m.next_true, 20000, true);
        else                            cab_frame(&s, &m, s.next_true, 20000, true);
        if (stalled && s.next_true > stall_at + 15 * 1000000 && s.inbox > peak_after) peak_after = s.inbox;
    }
    char msg[200];
    snprintf(msg, sizeof msg, "slave stalls 100 ms (%llu frames dropped): %u trims, %u lost, inbox %u from 15 s after it",
             (unsigned long long)s.pace.dropped, s.pace.trims, s.overflow, peak_after);
    CHECK(s.pace.dropped >= 2 && s.pace.trims >= 1 && !s.overflow && !m.pace.trims && peak_after <= 2, msg);
}

int main(void) {
    test_called_hosts();
    test_hitch();
    test_sleeping_host();
    test_pair();
    test_pair_stall();
    printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
