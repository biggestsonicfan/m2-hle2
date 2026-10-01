/*
 * host_prof.h — a sampling profiler inside the emulator (Linux only; a stub
 * elsewhere).
 *
 * perf on the handheld cannot attribute m2hle's time: the whole board is
 * inlined into one or two functions, the device's perf cannot read inline
 * debug info, and the ARC-S overheats before an ssh session has finished
 * setting up a run (tools/README.md, "The process profiling itself"). So the
 * process samples itself:
 *
 *  - every thread of the process (the GL driver's and RetroArch's too) gets a
 *    POSIX timer on its own CPU-time clock that raises SIGPROF in that thread
 *    every 1/hz of CPU it burns. The kernel checks CPU timers once a scheduler
 *    tick, so a period shorter than the tick fires once a tick with an overrun
 *    count, and each sample is weighted by the periods it stands for. (One
 *    process-wide ITIMER_PROF, the first version, fired once a tick for the
 *    whole process: on the ARC-S it saw 21% of the CPU time.) Each thread's
 *    CPU time is also read off its clock at both ends, so the report's
 *    milliseconds are measured, and the samples only split them. A sleeping
 *    thread is never sampled: this measures heat, not latency.
 *  - each sample keeps the thread, the PC, the link register, and the thread's
 *    ZONE: a one-byte tag the code sets at subsystem boundaries (the i960, the
 *    COP, the 68000, the SCSP, the frame's compose / scan / upload / 3D / tile
 *    stages, the present). The tag survives any amount of inlining, so the
 *    subsystem split needs no symbols at all. Setting it is a store to a
 *    thread-local byte.
 *  - at the end the process writes its own report: time per thread, per zone
 *    and per module + symbol (dladdr), then the raw (thread, zone, PC) counts
 *    and its executable mappings, which tools/hostprof.py resolves into inline
 *    function names with addr2line against an unstripped copy of the binary.
 *
 * Arming it, all without touching the launcher:
 *   M2HLE_HOSTPROF="start=15 secs=20 hz=997 out=/tmp/prof.txt"   (environment)
 *   echo "start=0 secs=20" > /tmp/m2hle-hostprof                (at any time)
 *   m2hle --host-prof 15:20[:FILE]                               (sdl3 frontend)
 * The trigger file is looked for about once a second and removed once read,
 * so a game started from EmulationStation (standalone or the libretro core)
 * can be profiled over ssh while someone plays it. `start` counts seconds from
 * when the profiler sees the request.
 *
 * The run also keeps a frame timeline: when each host frame began
 * (hprof_tick), how long the core's own work in it took (hprof_frame_end),
 * which board frame it ended on, and every half second the hottest thermal
 * zone and the CPU and GPU clocks. A stutter is a long gap between two frames,
 * and the timeline says whether the core's work filled it (too slow), or
 * something outside it (the frontend, the driver, a throttled clock).
 *
 * Nothing here changes what the board computes: the signal handler reads
 * registers and a byte and writes to its own buffer. A signal can cut a sleep
 * short (EINTR), which the pacing loops already tolerate.
 */
#ifndef M2HLE_HOST_PROF_H
#define M2HLE_HOST_PROF_H

#include <stdint.h>
#include <stdbool.h>

enum {
    HPROF_OTHER = 0,    /* untagged: the frontend, the driver, libc, RetroArch */
    HPROF_I960,         /* the i960 run loop: the core, the bus, the hooks */
    HPROF_COP,          /* a COP (SHARC) command */
    HPROF_M68K,         /* the sound 68000 */
    HPROF_SCSP,         /* the SCSP making samples (slots, DSP, mix) */
    HPROF_COMPOSE,      /* tile layers (video_update) */
    HPROF_SCAN,         /* the GEO display list walk + model decode */
    HPROF_UPLOAD,       /* texture atlas / LUT upload */
    HPROF_DRAW3D,       /* 3D batches handed to the GPU */
    HPROF_TILES,        /* tile layer draws */
    HPROF_PRESENT,      /* sg_commit + swap / video_cb */
    HPROF_ZONES
};

#if defined(__linux__) && !defined(__EMSCRIPTEN__) && defined(__LP64__)
#define HPROF_AVAILABLE 1

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

/* A translation unit that never ticks the profiler still includes it. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

/* dladdr and Dl_info are GNU extensions, and nothing defines _GNU_SOURCE
 * before the first system header in these translation units. The layout is
 * glibc's. Bionic declares them unconditionally. */
#if !defined(__USE_GNU) && !defined(__BIONIC__)
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } Dl_info;
extern int dladdr(const void *addr, Dl_info *info);
#endif

/* initial-exec: a plain load/store from the thread pointer even in the
 * libretro .so, where the default model would call __tls_get_addr. glibc
 * keeps static TLS room for a dlopen'd library; Android's linker refuses one
 * that asks for it, so the core there takes the default model. */
#if defined(__GLIBC__)
static __thread volatile uint8_t g_hprof_zone __attribute__((tls_model("initial-exec")));
#else
static __thread volatile uint8_t g_hprof_zone;
#endif

static inline int  hprof_enter(int z)   { int p = g_hprof_zone; g_hprof_zone = (uint8_t)z; return p; }
static inline void hprof_leave(int prev) { g_hprof_zone = (uint8_t)prev; }

/* Name the calling thread (what the report and top show). */
static inline void hprof_name_thread(const char *name) { prctl(PR_SET_NAME, name, 0, 0, 0); }

#define HPROF_TRIGGER_PATH "/tmp/m2hle-hostprof"

typedef struct {
    uintptr_t pc, lr;
    int32_t   tid;
    uint16_t  weight;           /* timer periods this sample stands for (1 + overrun) */
    uint8_t   zone;
} hprof_sample_t;

#define HPROF_MAX_THREADS 128
#define HPROF_PHASES      8

typedef struct {
    uint32_t t_us;              /* the frame's start, from the run's start */
    uint32_t work_us;           /* the core's work in it; 0 = not closed */
    uint32_t board;             /* the board frame it ended on */
    uint32_t ph[HPROF_PHASES];  /* hprof_phase stamps, us from the frame's start; 0 = not reached */
} hprof_frame_t;

typedef struct {
    uint32_t t_ms;
    int16_t  temp_c;            /* hottest thermal zone; -1 = none */
    uint16_t cpu_mhz, gpu_mhz;  /* 0 = unknown */
} hprof_heat_t;

typedef struct {
    int     tid;
    int     timer;              /* the kernel's timer id; -1 if none */
    int64_t cpu0_ns, cpu1_ns;   /* the thread's CPU clock at the start and the end */
    bool    ended;              /* gone before the end: its CPU time is estimated */
    char    name[20];
} hprof_thread_t;

typedef struct {
    /* the request */
    int      hz;
    double   start_s, secs;
    char     out[256];
    bool     pending;           /* a request waits for its start */
    int64_t  start_at_us, stop_at_us;
    /* the run */
    hprof_sample_t *buf;
    uint32_t        cap;
    atomic_uint     n;          /* samples taken (may pass cap: those are dropped) */
    volatile bool   running;    /* the handler takes samples only while set */
    struct sigaction old_sa;
    int64_t         t0_us, t1_us;
    struct rusage   ru0;
    hprof_thread_t  thr[HPROF_MAX_THREADS];
    int             nthr;
    int64_t         next_scan_us;
    /* the frame timeline */
    hprof_frame_t  *frames;
    uint32_t        frames_cap, nframes;
    hprof_heat_t    heat[1024];
    uint32_t        nheat;
    /* the trigger file poll */
    int64_t  next_poll_us;
    bool     env_read;
} hprof_t;

static hprof_t g_hprof;

static inline int64_t hprof__now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void hprof__on_sigprof(int sig, siginfo_t *si, void *ucv) {
    (void)sig;
    if (!g_hprof.running) return;
    int saved = errno;
    unsigned i = atomic_fetch_add_explicit(&g_hprof.n, 1, memory_order_relaxed);
    if (i < g_hprof.cap) {
        const ucontext_t *uc = (const ucontext_t *)ucv;
        hprof_sample_t *s = &g_hprof.buf[i];
#if defined(__aarch64__)
        s->pc = (uintptr_t)uc->uc_mcontext.pc;
        s->lr = (uintptr_t)uc->uc_mcontext.regs[30];
#elif defined(__x86_64__)
        s->pc = (uintptr_t)uc->uc_mcontext.gregs[16]   /* REG_RIP, a GNU name */;
        s->lr = 0;
#elif defined(__arm__)
        s->pc = (uintptr_t)uc->uc_mcontext.arm_pc;
        s->lr = (uintptr_t)uc->uc_mcontext.arm_lr;
#else
        s->pc = 0; s->lr = 0;
#endif
        int over = si && si->si_code == SI_TIMER ? si->si_overrun : 0;
        s->weight = (uint16_t)(over < 0 ? 1 : over > 65534 ? 65535 : over + 1);
        s->tid    = (int32_t)syscall(SYS_gettid);
        s->zone   = g_hprof_zone;
    }
    errno = saved;
}

/* Parse "start=15 secs=20 hz=997 out=/path" (spaces, commas or newlines). */
static void hprof__parse(const char *spec) {
    g_hprof.hz = 997; g_hprof.start_s = 0; g_hprof.secs = 20;
    snprintf(g_hprof.out, sizeof g_hprof.out, "/tmp/m2hle-hostprof-%d.txt", (int)getpid());
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", spec);
    for (char *tok = strtok(tmp, " ,\t\r\n"); tok; tok = strtok(NULL, " ,\t\r\n")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if      (!strcmp(tok, "start")) g_hprof.start_s = atof(v);
        else if (!strcmp(tok, "secs"))  g_hprof.secs    = atof(v);
        else if (!strcmp(tok, "hz"))    g_hprof.hz      = atoi(v);
        else if (!strcmp(tok, "out"))   snprintf(g_hprof.out, sizeof g_hprof.out, "%s", v);
    }
    if (g_hprof.hz < 10)    g_hprof.hz = 10;
    if (g_hprof.hz > 10000) g_hprof.hz = 10000;
    if (g_hprof.secs <= 0)  g_hprof.secs = 20;
}

/* Ask for a run; `start` seconds from now. Ignored while one is armed. */
__attribute__((noinline)) static void hprof_request(const char *spec) {
    if (g_hprof.pending || g_hprof.running) return;
    hprof__parse(spec);
    g_hprof.start_at_us = hprof__now_us() + (int64_t)(g_hprof.start_s * 1e6);
    g_hprof.pending = true;
    fprintf(stderr, "hostprof: armed: start in %.1f s, %.1f s at %d Hz -> %s\n",
            g_hprof.start_s, g_hprof.secs, g_hprof.hz, g_hprof.out);
}

/* A thread's CPU-time clock: the kernel's MAKE_THREAD_CPUCLOCK(tid,
 * CPUCLOCK_SCHED), which is how glibc builds pthread_getcpuclockid's. */
static inline clockid_t hprof__thread_clock(int tid) { return (clockid_t)((~(unsigned)tid << 3) | 6u); }

static int64_t hprof__thread_cpu_ns(int tid) {
    struct timespec ts;
    if (clock_gettime(hprof__thread_clock(tid), &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static long hprof__read_long(const char *path) {
    FILE *f = fopen(path, "r");
    long v = -1;
    if (f) { if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); }
    return v;
}

/* One heat sample: the hottest zone, CPU 0's clock, the first devfreq (the GPU). */
static void hprof__heat_sample(int64_t now) {
    if (g_hprof.nheat >= sizeof g_hprof.heat / sizeof g_hprof.heat[0]) return;
    hprof_heat_t *h = &g_hprof.heat[g_hprof.nheat++];
    h->t_ms = (uint32_t)((now - g_hprof.t0_us) / 1000);
    long hot = -1;
    for (int z = 0; z < 8; z++) {
        char path[64];
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
        long m = hprof__read_long(path);
        if (m < 0) break;
        if (m / 1000 > hot) hot = m / 1000;
    }
    h->temp_c = (int16_t)hot;
    long khz = hprof__read_long("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    h->cpu_mhz = (uint16_t)(khz > 0 ? khz / 1000 : 0);
    h->gpu_mhz = 0;
    DIR *d = opendir("/sys/class/devfreq");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char path[300];
            snprintf(path, sizeof path, "/sys/class/devfreq/%s/cur_freq", e->d_name);
            long hz = hprof__read_long(path);
            if (hz > 0) { h->gpu_mhz = (uint16_t)(hz / 1000000); break; }
        }
        closedir(d);
    }
}

/* The kernel's struct sigevent, for SIGEV_THREAD_ID, which glibc only names
 * under _GNU_SOURCE. Raw syscalls keep librt out of the link on old glibc. */
typedef struct { union sigval value; int signo; int notify; int tid; int pad[11]; } hprof_kevent_t;

static void hprof__thread_name(int tid, char *out, size_t n);

/* Put a timer on every thread that does not have one yet. */
static void hprof__arm_threads(void) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        int k = 0;
        for (; k < g_hprof.nthr; k++) if (g_hprof.thr[k].tid == tid) break;
        if (k < g_hprof.nthr || g_hprof.nthr == HPROF_MAX_THREADS) continue;
        hprof_thread_t *t = &g_hprof.thr[g_hprof.nthr];
        memset(t, 0, sizeof *t);
        t->tid   = tid;
        t->timer = -1;
        hprof__thread_name(tid, t->name, sizeof t->name);
        hprof_kevent_t ev;
        memset(&ev, 0, sizeof ev);
        ev.signo  = SIGPROF;
        ev.notify = 4;                                  /* SIGEV_THREAD_ID */
        ev.tid    = tid;
        int id = -1;
        if (syscall(SYS_timer_create, hprof__thread_clock(tid), &ev, &id) != 0) continue;   /* it just ended */
        int64_t period = 1000000000 / g_hprof.hz;
        struct itimerspec its = { { 0, period }, { 0, period } };
        t->cpu0_ns = hprof__thread_cpu_ns(tid);
        if (t->cpu0_ns < 0 || syscall(SYS_timer_settime, id, 0, &its, NULL) != 0) {
            syscall(SYS_timer_delete, id);
            continue;
        }
        t->timer = id;
        g_hprof.nthr++;
    }
    closedir(d);
}

static void hprof__disarm_threads(void) {
    for (int k = 0; k < g_hprof.nthr; k++) {
        hprof_thread_t *t = &g_hprof.thr[k];
        if (t->timer >= 0) syscall(SYS_timer_delete, t->timer);
        t->timer = -1;
        t->cpu1_ns = hprof__thread_cpu_ns(t->tid);
        t->ended = t->cpu1_ns < 0;
    }
}

static bool hprof__start(void) {
    /* Room for every core busy the whole run, plus slack. */
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1) cores = 1;
    double want = (double)g_hprof.hz * g_hprof.secs * (double)cores * 1.25 + 1024;
    if (want > 4e6) want = 4e6;
    uint32_t cap = (uint32_t)want;
    if (cap > g_hprof.cap || !g_hprof.buf) {
        free(g_hprof.buf);
        g_hprof.buf = (hprof_sample_t *)calloc(cap, sizeof *g_hprof.buf);
        g_hprof.cap = g_hprof.buf ? cap : 0;
    }
    if (!g_hprof.buf) { fprintf(stderr, "hostprof: out of memory\n"); return false; }
    uint32_t fcap = (uint32_t)(g_hprof.secs * 250.0) + 64;   /* room for a 240 Hz frontend */
    if (fcap > g_hprof.frames_cap || !g_hprof.frames) {
        free(g_hprof.frames);
        g_hprof.frames = (hprof_frame_t *)calloc(fcap, sizeof *g_hprof.frames);
        g_hprof.frames_cap = g_hprof.frames ? fcap : 0;
    }
    g_hprof.nframes = 0;
    g_hprof.nheat = 0;
    atomic_store(&g_hprof.n, 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = hprof__on_sigprof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, &g_hprof.old_sa) != 0) {
        fprintf(stderr, "hostprof: sigaction: %s\n", strerror(errno));
        return false;
    }
    getrusage(RUSAGE_SELF, &g_hprof.ru0);
    g_hprof.running = true;
    g_hprof.nthr = 0;
    hprof__arm_threads();
    if (!g_hprof.nthr) {
        fprintf(stderr, "hostprof: no thread timers: %s\n", strerror(errno));
        g_hprof.running = false;
        sigaction(SIGPROF, &g_hprof.old_sa, NULL);
        return false;
    }
    g_hprof.t0_us = hprof__now_us();
    g_hprof.next_scan_us = g_hprof.t0_us + 500000;
    hprof__heat_sample(g_hprof.t0_us);
    g_hprof.stop_at_us = g_hprof.t0_us + (int64_t)(g_hprof.secs * 1e6);
    fprintf(stderr, "hostprof: sampling %d threads for %.1f s\n", g_hprof.nthr, g_hprof.secs);
    return true;
}

/* ---- the report ---------------------------------------------------------- */

static const char *const k_hprof_zone_names[HPROF_ZONES] = {
    "other", "i960", "cop", "m68k", "scsp", "compose", "scan", "upload", "draw3d", "tiles", "present"
};

typedef struct { uintptr_t pc; int32_t tid; uint8_t zone; uint32_t count; } hprof_agg_t;
typedef struct { char key[160]; uint32_t count; uint32_t zone_count[HPROF_ZONES]; } hprof_sym_t;

static int hprof__cmp_sample(const void *a, const void *b) {
    const hprof_sample_t *x = (const hprof_sample_t *)a, *y = (const hprof_sample_t *)b;
    if (x->tid != y->tid)   return x->tid < y->tid ? -1 : 1;
    if (x->zone != y->zone) return x->zone < y->zone ? -1 : 1;
    if (x->pc != y->pc)     return x->pc < y->pc ? -1 : 1;
    return 0;
}
static int hprof__cmp_sym_count(const void *a, const void *b) {
    uint32_t x = ((const hprof_sym_t *)a)->count, y = ((const hprof_sym_t *)b)->count;
    return x > y ? -1 : x < y ? 1 : 0;
}

static void hprof__thread_name(int tid, char *out, size_t n) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (f) { if (fgets(out, (int)n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
    if (!out[0]) snprintf(out, n, "(gone)");
}

/* "module:symbol", or "module+0xoffset" when the symbol table does not say. */
static void hprof__sym_key(uintptr_t pc, char *out, size_t n) {
    Dl_info di;
    if (pc && dladdr((void *)pc, &di) && di.dli_fname) {
        const char *base = strrchr(di.dli_fname, '/');
        base = base ? base + 1 : di.dli_fname;
        if (di.dli_sname) snprintf(out, n, "%s:%s", base, di.dli_sname);
        else snprintf(out, n, "%s+0x%lx", base, (unsigned long)(pc - (uintptr_t)di.dli_fbase));
    } else {
        snprintf(out, n, "[unknown]");
    }
}

static int hprof__cmp_u32_desc(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x > y ? -1 : x < y ? 1 : 0;
}

/* The frame timeline: a summary, the worst gaps, the heat, and every frame. */
static void hprof__write_frames(FILE *f) {
    uint32_t n = g_hprof.nframes;
    fprintf(f, "\n## frames (host frames: gap = start to next start, work = the core's part of it)\n");
    if (n < 2) { fprintf(f, "frames %u\n", n); return; }
    uint32_t *gap = (uint32_t *)malloc(sizeof(uint32_t) * n), *work = (uint32_t *)malloc(sizeof(uint32_t) * n);
    if (!gap || !work) { free(gap); free(work); return; }
    static const uint32_t edges[] = { 18000, 25000, 34000, 50000, 100000 };
    uint32_t hist[6] = { 0 };
    for (uint32_t i = 0; i + 1 < n; i++) {
        gap[i]  = g_hprof.frames[i + 1].t_us - g_hprof.frames[i].t_us;
        work[i] = g_hprof.frames[i].work_us;
        int b = 0;
        while (b < 5 && gap[i] > edges[b]) b++;
        hist[b]++;
    }
    uint32_t m = n - 1;
    uint32_t board0 = g_hprof.frames[0].board, board1 = g_hprof.frames[m - 1].board;
    double span = (double)(g_hprof.frames[m].t_us - g_hprof.frames[0].t_us) / 1e6;
    fprintf(f, "frames %u over %.2f s = %.2f Hz; board frames %u = %.2f Hz\n", m, span, span > 0 ? m / span : 0,
            board1 - board0, span > 0 ? (board1 - board0) / span : 0);
    fprintf(f, "gaps <=18ms %u, <=25 %u, <=34 %u, <=50 %u, <=100 %u, >100 %u\n",
            hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
    uint32_t *sw = (uint32_t *)malloc(sizeof(uint32_t) * m);
    if (sw) {
        memcpy(sw, work, sizeof(uint32_t) * m);
        qsort(sw, m, sizeof *sw, hprof__cmp_u32_desc);
        fprintf(f, "work ms: max %.2f, p1 %.2f, p5 %.2f, median %.2f\n", sw[0] / 1e3, sw[m / 100] / 1e3,
                sw[m / 20] / 1e3, sw[m / 2] / 1e3);
        free(sw);
    }
    fprintf(f, "\n## heat (t_s temp_c cpu_mhz gpu_mhz)\n");
    for (uint32_t i = 0; i < g_hprof.nheat; i++)
        fprintf(f, "%.1f %d %u %u\n", g_hprof.heat[i].t_ms / 1e3, g_hprof.heat[i].temp_c,
                g_hprof.heat[i].cpu_mhz, g_hprof.heat[i].gpu_mhz);
    fprintf(f, "\n## frame-log (t_ms gap_ms work_ms board phase_ms...)\n");
    for (uint32_t i = 0; i < m; i++) {
        fprintf(f, "%.1f %.2f %.2f %u", g_hprof.frames[i].t_us / 1e3, gap[i] / 1e3, work[i] / 1e3,
                g_hprof.frames[i].board);
        for (int k = 0; k < HPROF_PHASES; k++) fprintf(f, " %.2f", g_hprof.frames[i].ph[k] / 1e3);
        fprintf(f, "\n");
    }
    free(gap);
    free(work);
}

static void hprof__write(void) {
    unsigned taken = atomic_load(&g_hprof.n);
    unsigned n = taken < g_hprof.cap ? taken : g_hprof.cap;
    double wall = (double)(g_hprof.t1_us - g_hprof.t0_us) / 1e6;
    FILE *f = fopen(g_hprof.out, "w");
    if (!f) { fprintf(stderr, "hostprof: cannot write %s: %s\n", g_hprof.out, strerror(errno)); return; }

    struct rusage ru1;
    getrusage(RUSAGE_SELF, &ru1);
    double cpu = (double)(ru1.ru_utime.tv_sec - g_hprof.ru0.ru_utime.tv_sec)
               + (double)(ru1.ru_utime.tv_usec - g_hprof.ru0.ru_utime.tv_usec) / 1e6
               + (double)(ru1.ru_stime.tv_sec - g_hprof.ru0.ru_stime.tv_sec)
               + (double)(ru1.ru_stime.tv_usec - g_hprof.ru0.ru_stime.tv_usec) / 1e6;
    struct utsname un;
    uname(&un);
    char exe[256] = "";
    ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (el > 0) exe[el] = 0;
    Dl_info self;
    memset(&self, 0, sizeof self);
    const char *self_mod = dladdr((void *)hprof__write, &self) && self.dli_fname ? self.dli_fname : exe;

    qsort(g_hprof.buf, n, sizeof *g_hprof.buf, hprof__cmp_sample);
    uint64_t wtot = 0;
    for (unsigned i = 0; i < n; i++) wtot += g_hprof.buf[i].weight;

    fprintf(f, "# m2hle host profile (src/core/host_prof.h; names: tools/hostprof.py)\n");
    fprintf(f, "wall_s %.3f\ncpu_s %.3f\nhz %d\nsamples %u\nperiods %llu\ndropped %u\nkernel %s %s\nexe %s\nmodule %s\n",
            wall, cpu, g_hprof.hz, n, (unsigned long long)wtot, taken - n, un.release, un.machine, exe, self_mod);
    fprintf(f, "# CPU %.2f s over %.2f s wall = %.2f cores busy. A period is %.3f ms of one thread's CPU;\n"
               "# a sample stands for 1 + its timer's overrun periods. %% below is of all periods.\n\n",
            cpu, wall, wall > 0 ? cpu / wall : 0.0, 1000.0 / g_hprof.hz);

    /* Threads: measured CPU time, and the zones within each by sample weight. */
    fprintf(f, "## threads (ms = CPU time measured on the thread's clock; ~ = estimated, it ended)\n");
    fprintf(f, "%-8s %-16s %9s %6s %6s  zones\n", "tid", "name", "ms", "%cpu", "cores");
    for (int k = 0; k < g_hprof.nthr; k++) {
        const hprof_thread_t *t = &g_hprof.thr[k];
        uint64_t zc[HPROF_ZONES] = {0}, tw = 0;
        for (unsigned i = 0; i < n; i++)
            if (g_hprof.buf[i].tid == t->tid) { zc[g_hprof.buf[i].zone % HPROF_ZONES] += g_hprof.buf[i].weight; tw += g_hprof.buf[i].weight; }
        double ms = t->ended ? 1000.0 * (double)tw / g_hprof.hz : (double)(t->cpu1_ns - t->cpu0_ns) / 1e6;
        if (ms < 0.05 && !tw) continue;
        fprintf(f, "%-8d %-16s %s%8.1f %5.1f%% %6.3f ", t->tid, t->name, t->ended ? "~" : " ", ms,
                cpu > 0 ? 100.0 * ms / 1000.0 / cpu : 0.0, wall > 0 ? ms / 1000.0 / wall : 0.0);
        for (int z = 0; z < HPROF_ZONES; z++)
            if (zc[z]) fprintf(f, " %s %.1f%%", k_hprof_zone_names[z], 100.0 * (double)zc[z] / (double)tw);
        fprintf(f, "\n");
    }

    /* Zones over the whole process. */
    uint64_t zall[HPROF_ZONES] = {0};
    for (unsigned i = 0; i < n; i++) zall[g_hprof.buf[i].zone % HPROF_ZONES] += g_hprof.buf[i].weight;
    fprintf(f, "\n## zones (all threads; ms = share of the process's CPU time)\n");
    for (int z = 0; z < HPROF_ZONES; z++)
        if (zall[z]) {
            double share = (double)zall[z] / (double)(wtot ? wtot : 1);
            fprintf(f, "%-8s %5.1f%% %9.1f ms %6.3f cores\n", k_hprof_zone_names[z], 100.0 * share,
                    1000.0 * cpu * share, wall > 0 ? cpu * share / wall : 0.0);
        }

    /* Aggregate identical (tid, zone, pc): the raw section and the symbol table. */
    hprof_agg_t *agg = (hprof_agg_t *)malloc((n ? n : 1) * sizeof *agg);
    unsigned na = 0;
    for (unsigned i = 0; i < n; i++) {
        const hprof_sample_t *s = &g_hprof.buf[i];
        if (na && agg[na - 1].tid == s->tid && agg[na - 1].zone == s->zone && agg[na - 1].pc == s->pc)
            agg[na - 1].count += s->weight;
        else agg[na++] = (hprof_agg_t){ s->pc, s->tid, s->zone, s->weight };
    }

    /* By module + dynamic symbol. The board's own code has no dynamic symbols
     * (it is static and inlined), so it rolls up as one line per module;
     * tools/hostprof.py names it. */
    hprof_sym_t *syms = (hprof_sym_t *)calloc(na ? na : 1, sizeof *syms);
    unsigned ns = 0;
    for (unsigned i = 0; i < na; i++) {
        char key[160];
        hprof__sym_key(agg[i].pc, key, sizeof key);
        char *plus = strstr(key, "+0x");
        if (plus) strcpy(plus, " (no symbol)");
        unsigned k = 0;
        for (; k < ns; k++) if (!strcmp(syms[k].key, key)) break;
        if (k == ns) { snprintf(syms[ns].key, sizeof syms[ns].key, "%s", key); ns++; }
        syms[k].count += agg[i].count;
        syms[k].zone_count[agg[i].zone % HPROF_ZONES] += agg[i].count;
    }
    qsort(syms, ns, sizeof *syms, hprof__cmp_sym_count);
    fprintf(f, "\n## module:symbol (leaf, periods; top 40)\n");
    for (unsigned k = 0; k < ns && k < 40; k++) {
        fprintf(f, "%7u %5.1f%%  %s  [", syms[k].count, 100.0 * syms[k].count / (double)(wtot ? wtot : 1), syms[k].key);
        int first = 1;
        for (int z = 0; z < HPROF_ZONES; z++)
            if (syms[k].zone_count[z]) {
                fprintf(f, "%s%s %u", first ? "" : ", ", k_hprof_zone_names[z], syms[k].zone_count[z]);
                first = 0;
            }
        fprintf(f, "]\n");
    }
    free(syms);

    /* The executable mappings, for tools/hostprof.py. */
    hprof__write_frames(f);
    fprintf(f, "\n## maps\n");
    FILE *m = fopen("/proc/self/maps", "r");
    if (m) {
        char line[512];
        while (fgets(line, sizeof line, m))
            if (strstr(line, " r-xp ") || strstr(line, " r-xs ")) fputs(line, f);
        fclose(m);
    }
    fprintf(f, "\n## raw tid zone pc periods\n");
    for (unsigned i = 0; i < na; i++)
        fprintf(f, "%d %s %lx %u\n", agg[i].tid, k_hprof_zone_names[agg[i].zone % HPROF_ZONES],
                (unsigned long)agg[i].pc, agg[i].count);
    /* Samples outside our module (libc, the GL driver) by their link register:
     * for a leaf function that is the return address, which usually names the
     * code of ours that called into the library. */
    fprintf(f, "\n## raw-lr tid zone lr periods\n");
    for (unsigned i = 0; i < n; i++) {                  /* the buffer is spent after this */
        Dl_info di;
        bool ours = dladdr((void *)g_hprof.buf[i].pc, &di) && di.dli_fbase == self.dli_fbase;
        g_hprof.buf[i].pc = ours ? 0 : g_hprof.buf[i].lr;
    }
    qsort(g_hprof.buf, n, sizeof *g_hprof.buf, hprof__cmp_sample);
    for (unsigned i = 0; i < n;) {
        unsigned j = i;
        uint32_t w = 0;
        while (j < n && !hprof__cmp_sample(&g_hprof.buf[i], &g_hprof.buf[j])) w += g_hprof.buf[j++].weight;
        if (g_hprof.buf[i].pc)
            fprintf(f, "%d %s %lx %u\n", g_hprof.buf[i].tid,
                    k_hprof_zone_names[g_hprof.buf[i].zone % HPROF_ZONES], (unsigned long)g_hprof.buf[i].pc, w);
        i = j;
    }
    free(agg);
    fclose(f);
    fprintf(stderr, "hostprof: %u samples, %llu periods (%.2f cores over %.1f s) -> %s\n",
            n, (unsigned long long)wtot, wall > 0 ? cpu / wall : 0.0, wall, g_hprof.out);
}

static void hprof__stop(void) {
    g_hprof.running = false;
    hprof__disarm_threads();
    g_hprof.t1_us = hprof__now_us();
    /* A signal already on its way still finds our handler; restore the old
     * one only after the buffer has been read. */
    hprof__write();
    sigaction(SIGPROF, &g_hprof.old_sa, NULL);
}

/* The part of hprof_tick that does anything: out of line, so the run loop it
 * sits in is laid out as it is without the profiler. */
__attribute__((noinline)) static void hprof__tick_slow(int64_t now) {
    if (!g_hprof.env_read) {
        g_hprof.env_read = true;
        const char *e = getenv("M2HLE_HOSTPROF");
        if (e && *e) { hprof_request(e); return; }
    }
    if (g_hprof.running) {
        if (now >= g_hprof.stop_at_us) hprof__stop();
        else {                                          /* threads started since */
            g_hprof.next_scan_us = now + 500000;
            hprof__arm_threads();
            hprof__heat_sample(now);
        }
        return;
    }
    if (g_hprof.pending) {
        g_hprof.pending = false;
        hprof__start();
        return;
    }
    g_hprof.next_poll_us = now + 1000000;
    struct stat st;
    if (stat(HPROF_TRIGGER_PATH, &st) != 0) return;
    char spec[512] = "";
    FILE *f = fopen(HPROF_TRIGGER_PATH, "r");
    if (f) { size_t k = fread(spec, 1, sizeof spec - 1, f); spec[k] = 0; fclose(f); }
    unlink(HPROF_TRIGGER_PATH);
    hprof_request(spec);
}

/* Call once per host frame from any one thread: reads the environment the
 * first time, polls the trigger file about once a second, and starts and stops
 * a requested run. Cheap when idle: a clock read and a compare. */
static inline void hprof_tick(void) {
    int64_t now = hprof__now_us();
    if (g_hprof.running && g_hprof.nframes < g_hprof.frames_cap)
        g_hprof.frames[g_hprof.nframes++] = (hprof_frame_t){ (uint32_t)(now - g_hprof.t0_us), 0, 0, { 0 } };
    int64_t due = g_hprof.running ? (g_hprof.stop_at_us < g_hprof.next_scan_us ? g_hprof.stop_at_us : g_hprof.next_scan_us)
                : g_hprof.pending ? g_hprof.start_at_us
                : g_hprof.next_poll_us;
    if (now >= due || !g_hprof.env_read) hprof__tick_slow(now);
}

/* Stamp phase k (0..HPROF_PHASES-1) of the frame hprof_tick opened: the time
 * since its start. The host decides what its phases are; the frame log lists
 * them in order. */
static inline void hprof_phase(int k) {
    if (!g_hprof.running || !g_hprof.nframes || (unsigned)k >= HPROF_PHASES) return;
    hprof_frame_t *fr = &g_hprof.frames[g_hprof.nframes - 1];
    uint32_t t = (uint32_t)(hprof__now_us() - g_hprof.t0_us);
    fr->ph[k] = t > fr->t_us ? t - fr->t_us : 1;
}

/* Put a duration of the host's own (us) in phase slot k instead of a stamp. */
static inline void hprof_phase_set(int k, int64_t us) {
    if (!g_hprof.running || !g_hprof.nframes || (unsigned)k >= HPROF_PHASES) return;
    g_hprof.frames[g_hprof.nframes - 1].ph[k] = (uint32_t)(us < 0 ? 0 : us);
}

/* Close the frame hprof_tick opened: the core's work is done, and the board is
 * at `board`. Call it from the same thread, at the end of the host frame. */
static inline void hprof_frame_end(uint32_t board) {
    if (!g_hprof.running || !g_hprof.nframes) return;
    hprof_frame_t *fr = &g_hprof.frames[g_hprof.nframes - 1];
    uint32_t t = (uint32_t)(hprof__now_us() - g_hprof.t0_us);
    fr->work_us = t > fr->t_us ? t - fr->t_us : 1;
    fr->board = board;
}

/* Finish a run early (the process is quitting): write what there is. */
__attribute__((noinline)) static void hprof_shutdown(void) {
    if (g_hprof.running) hprof__stop();
}

#pragma GCC diagnostic pop

#else  /* not Linux, or not 64-bit */
#define HPROF_AVAILABLE 0
static inline int  hprof_enter(int z)                { (void)z; return 0; }
static inline void hprof_leave(int prev)             { (void)prev; }
static inline void hprof_name_thread(const char *n)  { (void)n; }
static inline void hprof_request(const char *spec)   { (void)spec; }
static inline void hprof_tick(void)                  {}
static inline void hprof_frame_end(uint32_t board)   { (void)board; }
static inline void hprof_phase(int k)                { (void)k; }
static inline void hprof_phase_set(int k, int64_t us) { (void)k; (void)us; }
static inline void hprof_shutdown(void)              {}
#endif

#endif /* M2HLE_HOST_PROF_H */
