/*
 * dc_sdlog.h -- the console's log to an SD card on the serial port (Pinboard
 * #519, make SDLOG=1).
 *
 * The SD adapters for the Dreamcast's serial port (an SD card in SPI mode on the
 * port's pins, KallistiOS's hardware/sd.c) take a FAT card, first partition, as a
 * PC formats one. Everything printed (printf, KOS's dbglog, the board's log,
 * the stats rows, which SDLOG prints as DC_STATS_DBGIO does) goes through a dbgio
 * device of ours into RAM; a thread of its own appends it to M2LOGnnn.TXT
 * every second and syncs the FAT every DC_SDLOG_SYNC_MS, so pulling the card
 * or the power loses at most that. Every line starts with the ms since boot.
 *
 * The sync is the cost: KallistiOS's FAT caches whole clusters and writes a
 * dirty one back whole, so each sync rewrites the log's last cluster and its
 * directory's, 64 KB on a big card's 32 KB clusters, whatever was added. On a
 * console a sync a second took ~190 ms of every second, bit by bit on the SH-4
 * and with the G1 lock held (Pinboard #1876's first log); writes between syncs
 * stay in that cache. The first line says the card's cluster size.
 *
 * The serial port is the card's, so SDLOG leaves out LINK (the MAME lockstep)
 * and the serial console. SPI on that port is the SH-4 setting its pins a bit
 * at a time: the writer's time is the emulator's (its own SD line says how
 * much). No card, or one KOS cannot read, and the game runs as it would
 * without: one line on screen says why.
 *
 * The card may also be the one the game runs from: DreamShell's ISO loader
 * (isoldr) reads the GDI off an SD card on the same port, inside the GD-ROM
 * syscalls, and only while KOS's cdrom layer holds the G1 ATA lock (a command,
 * or a DMA read the vblank finishes). So every touch of the card here holds that
 * lock too, a chunk at a time, so a disc read waits at most one chunk (Pinboard
 * #1314). With a real disc the lock costs only that wait.
 */
#ifndef DC_SDLOG_H
#define DC_SDLOG_H

#include <kos.h>
#include <dc/sd.h>
#include <dc/g1ata.h>
#include <kos/blockdev.h>
#include <kos/dbgio.h>
#include <fat/fs_fat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef DC_SDLOG_BUF
#define DC_SDLOG_BUF (32u << 10)   /* bytes held between writes: some 10 s of HUD=prof */
#endif
#define DC_SDLOG_CHUNK 4096u        /* bytes written a lock: what a disc read can wait behind */
#ifndef DC_SDLOG_SYNC_MS
#define DC_SDLOG_SYNC_MS 5000u      /* between FAT syncs: what a pulled card or power loses */
#endif

static struct {
    char      buf[2][DC_SDLOG_BUF];
    uint32_t  len;          /* filled in buf[cur] */
    int       cur;
    int       fd;           /* -1: no card, the lines are dropped */
    int       line_start;   /* the next byte begins a line (its time goes first) */
    uint64_t  t0;
    char      name[32];
    const char *why;        /* no card: why, for the SD line */
    /* for the SD line: bytes written, lost to a full buffer; writes and their time */
    uint32_t  in, written, dropped, writes, syncs, errors;
    uint64_t  us_write, us_max;
    uint64_t  t_sync;           /* the last sync */
    uint32_t  cluster;          /* the card's cluster, bytes (the first line) */
    volatile uint32_t rounds;   /* the writer's wake-ups, empty ones too (sl_drain) */
    volatile int sync_now;      /* sl_drain: sync at the next round */
    kthread_t *thd;
    kos_blockdev_t bd;
} g_sl = { .fd = -1, .line_start = 1 };

/* Into the buffer, IRQs off: dbgio is called from any thread, KOS's own
 * dbglog included. Never touches the card, so the SD driver's own messages
 * cannot come back into it. */
static void sl_put(const char *s, int n) {
    int old = irq_disable();
    g_sl.in += (uint32_t)n;
    char *b = g_sl.buf[g_sl.cur];
    for (int i = 0; i < n; i++) {
        if (g_sl.line_start) {
            char ts[16];
            int m = snprintf(ts, sizeof ts, "%7u ", (unsigned)((timer_us_gettime64() - g_sl.t0) / 1000u));
            if (g_sl.len + (uint32_t)m + 1u > DC_SDLOG_BUF) { g_sl.dropped += (uint32_t)(n - i); break; }
            memcpy(b + g_sl.len, ts, (size_t)m);
            g_sl.len += (uint32_t)m;
            g_sl.line_start = 0;
        }
        if (g_sl.len >= DC_SDLOG_BUF) { g_sl.dropped += (uint32_t)(n - i); break; }
        b[g_sl.len++] = s[i];
        if (s[i] == '\n') g_sl.line_start = 1;
    }
    irq_restore(old);
}

static int sl_detected(void) { return 1; }
static int sl_ok(void) { return 0; }
static int sl_irq(int mode) { (void)mode; return 0; }
static int sl_write(const uint8_t *data, int len, int xlat) {
    (void)xlat;
    sl_put((const char *)data, len);
    return len;
}
static int sl_read(uint8_t *data, int len) { (void)data; (void)len; errno = EAGAIN; return -1; }
static dbgio_handler_t s_sl_dbgio = {
    .name = "sdlog", .detected = sl_detected, .init = sl_ok, .shutdown = sl_ok,
    .set_irq_usage = sl_irq, .flush = sl_ok, .write_buffer = sl_write, .read_buffer = sl_read,
};

/* The writer: the other buffer into the file (KallistiOS's cache, mostly),
 * then, every DC_SDLOG_SYNC_MS or when sl_drain asks, the cache and the FAT's
 * own blocks to the card. */
static void sl_flush(void) {
    int old = irq_disable();
    int full = g_sl.cur;
    uint32_t n = g_sl.len;
    g_sl.cur ^= 1;
    g_sl.len = 0;
    irq_restore(old);
    uint64_t t0 = timer_us_gettime64();
    int sync = g_sl.sync_now || t0 - g_sl.t_sync >= DC_SDLOG_SYNC_MS * 1000ull;
    if (!n && !sync) return;
    uint32_t done = 0;
    while (done < n) {
        uint32_t c = n - done < DC_SDLOG_CHUNK ? n - done : DC_SDLOG_CHUNK;
        g1_ata_mutex_lock();
        ssize_t w = write(g_sl.fd, g_sl.buf[full] + done, c);
        g1_ata_mutex_unlock();
        if (w > 0) { done += (uint32_t)w; g_sl.written += (uint32_t)w; }
        if (w != (ssize_t)c) { g_sl.errors++; break; }
    }
    if (sync) {
        g_sl.sync_now = 0;
        g1_ata_mutex_lock();
        if (fs_fat_sync("/sd") < 0) g_sl.errors++;
        g1_ata_mutex_unlock();
        g_sl.t_sync = timer_us_gettime64();
        g_sl.syncs++;
    }
    uint64_t us = timer_us_gettime64() - t0;
    g_sl.writes++;
    g_sl.us_write += us;
    if (us > g_sl.us_max) g_sl.us_max = us;
}

static void *sl_thread(void *arg) {
    (void)arg;
    for (;;) {
        thd_sleep(1000);
        sl_flush();
        g_sl.rounds++;
    }
    return NULL;
}

/* What is printed so far, onto the card before a stop (dc_assert): a sync
 * asked for and the writer's own rounds waited for, two, so one began after
 * the last line (it
 * takes the locks a write needs; a stop may come with them held, hence the
 * limit). Not from an interrupt or the writer. */
static void sl_drain(uint32_t ms) {
    if (g_sl.fd < 0 || !g_sl.thd || irq_inside_int() || thd_get_current() == g_sl.thd) return;
    g_sl.sync_now = 1;
    uint32_t r = g_sl.rounds;
    for (uint32_t t = 0; t < ms && g_sl.rounds - r < 2u; t += 50) thd_sleep(50);
}

/* The SD line, for the stats: KB in and written, writer rounds, their ms (the
 * longest), bytes lost to a full buffer, errors, FAT syncs; or why there is
 * no card. */
static void sl_stats(char *out, size_t size) {
    if (g_sl.fd < 0) { snprintf(out, size, "sd off: %s", g_sl.why ? g_sl.why : "not started"); return; }
    snprintf(out, size, "sd %s in=%u kb=%u wr=%u ms=%u max=%u drop=%u err=%u sy=%u", g_sl.name + 4, (unsigned)(g_sl.in >> 10),
             (unsigned)(g_sl.written >> 10), (unsigned)g_sl.writes, (unsigned)(g_sl.us_write / 1000u),
             (unsigned)(g_sl.us_max / 1000u), (unsigned)g_sl.dropped, (unsigned)g_sl.errors, (unsigned)g_sl.syncs);
}

static int sl_fail(const char *why) { g_sl.why = why; return -1; }

/* The card up and a new file on it, printf into the buffer: 0; or -1, and
 * g_sl.why says why (the caller selects another device). */
static int sl_init_card(uint8_t *type);
static int sl_init(void) {
    g_sl.t0 = timer_us_gettime64();
    dbgio_add_handler(&s_sl_dbgio);
    dbgio_dev_select("sdlog");
    uint8_t type = 0;
    g1_ata_mutex_lock();
    int r = sl_init_card(&type);
    g1_ata_mutex_unlock();
    if (r < 0) return r;
    printf("m2-hle2 sdlog %s (partition type %02x, %u MB, cluster %u B, sync %u ms)\n", g_sl.name + 4, type,
           (unsigned)(sd_get_size() >> 20), (unsigned)g_sl.cluster, (unsigned)DC_SDLOG_SYNC_MS);
    g_sl.t_sync = timer_us_gettime64();
    g_sl.thd = thd_create(1, sl_thread, NULL);
    return 0;
}

/* sl_init's part on the card, under the G1 lock. */
static int sl_init_card(uint8_t *type) {
    if (sd_init() < 0) return sl_fail("no SD card on the serial port");
    if (sd_blockdev_for_partition(0, &g_sl.bd, type) < 0) return sl_fail("the card has no MBR partition");
    if (fs_fat_init() < 0 || fs_fat_mount("/sd", &g_sl.bd, FS_FAT_MOUNT_READWRITE) < 0)
        return sl_fail("the card's first partition is not FAT");
    /* the first M2LOGnnn.TXT not on the card: one file a boot */
    for (int i = 0; i < 1000 && g_sl.fd < 0; i++) {
        snprintf(g_sl.name, sizeof g_sl.name, "/sd/M2LOG%03d.TXT", i);
        int fd = open(g_sl.name, O_RDONLY);
        if (fd >= 0) { close(fd); continue; }
        g_sl.fd = open(g_sl.name, O_WRONLY | O_CREAT | O_TRUNC);
        if (g_sl.fd < 0) return sl_fail("cannot make a file on the card (read-only?)");
    }
    if (g_sl.fd < 0) return sl_fail("M2LOG000-999.TXT all taken");
    /* the cluster, off the partition's boot sector: what a sync writes back */
    static uint8_t bs[512] __attribute__((aligned(32)));
    if (g_sl.bd.l_block_size == 9 && g_sl.bd.read_blocks(&g_sl.bd, 0, 1, bs) == 0)
        g_sl.cluster = (uint32_t)(bs[11] | bs[12] << 8) * bs[13];
    return 0;
}

#endif /* DC_SDLOG_H */
