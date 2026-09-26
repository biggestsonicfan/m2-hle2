/*
 * av_stream.h — hand the board's raw picture and sound to one local client
 * over TCP, on one socket and one clock.
 *
 * Started with --av-port N [--av-size WxH]. Listens on 127.0.0.1:N, exactly as
 * the MCP bridge does, and takes one client at a time. There is no encoding,
 * no resampling and no image format anywhere in here: the client gets BGRA
 * frames and 16-bit stereo samples as the board made them, and whatever is
 * downstream (ffmpeg, usually) does the rest. --av-format nv12 is the one
 * exception: the GPU converts the picture to BT.709 limited-range NV12 before
 * it is read back (ui/av_capture.h), because a client that encodes needs YUV
 * and that conversion costs more than a CPU core at 1080p60 in swscale.
 *
 * WHY BOTH STREAMS SHARE ONE SOCKET AND ONE CLOCK. Capturing the window and
 * the speakers separately leaves audio and video on two unrelated clocks — the
 * display's and the audio device's — neither of which is the board's, so the
 * two drift and have to be lined up by ear. Here every packet is stamped with
 * the board's own 44.1 kHz sample counter (g_sound.out_total, latched once per
 * game frame in g_frame_clock), so a video frame's pts is sample / 44100 and
 * the two are in sync by construction. Nothing assumes 735 samples a frame or
 * an exact 60 Hz, and the video cadence is allowed to be irregular.
 *
 * THE WIRE FORMAT, all little-endian. A 32-byte stream header once on connect:
 *
 *   char magic[4] = "M2AV";  u16 version = 1;  u16 header_size = 32;
 *   u16 width, height;       u32 pixfmt;      // 'B','G','R','A' or 'N','V','1','2'
 *   u32 fps_num, fps_den;                     // nominal board rate, informational
 *   u32 audio_rate = 44100;  u8 channels = 2;  u8 bits = 16;  u16 reserved;
 *
 * then packets, from the next board-frame boundary, each a 24-byte header and
 * `size` bytes of payload:
 *
 *   u8 type ('V'|'A');  u8 flags;  u16 reserved;
 *   u32 size;  u64 frame;   // the counter get_status reports as "frames"
 *   u64 sample;             // A: index of the first sample in this packet
 *                           // V: samples produced when the pictured frame ended
 *
 * flags bit 0 says something of THAT stream was dropped before this packet.
 * A BGRA video payload is width*height*4 bytes, packed, top row first,
 * stride = width*4. An NV12 payload is width*height bytes of Y, top row first,
 * then width*height/2 bytes of interleaved Cb,Cr at half size both ways:
 * BT.709, limited range (Y 16-235, C 16-240), each chroma sample the mean of
 * its 2x2 block (centre-sited). An NV12 stream's width is a multiple of 4 and
 * its height is even. An audio payload is size/4 interleaved L,R int16 frames.
 * pixfmt is written as its four characters in order, so a reader can memcmp it
 * against "BGRA" or "NV12". A reader that only knows BGRA refuses an NV12
 * stream rather than misreading it, which is why the version did not change.
 *
 * WHAT MAY BE DROPPED. Video may: the timestamps make a missing frame harmless,
 * so when the queue is full or a readback is not back in time the frame is
 * dropped and the next one carries the flag. Audio may not, short of a client
 * that has stopped reading — the ring below holds about six seconds, and only a
 * consumer lapped by that much loses samples.
 *
 * THREADS. Three touch this file and none of them blocks another:
 *   - the emu thread writes audio a sample at a time, through the tap in
 *     board/sound.h (av_audio_tap);
 *   - the render thread fills a video slot and commits it (ui/av_capture.h);
 *   - the writer thread owns the socket and does every send.
 * Both rings are single-producer / single-consumer and lock-free, and the
 * writer thread is the only one of the three allowed to block. That is the
 * point: a stalled client costs dropped frames, never a stalled emulator.
 */
#ifndef AV_STREAM_H
#define AV_STREAM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* net_socket.h is the one place in the tree that opens <winsock2.h>, and on
 * Windows it has to get there before anything pulls in <windows.h>. main.c
 * includes net/ first for exactly that reason; this is stated here so the
 * dependency is visible from the file that has it. */
#include "net_socket.h"
#ifndef _WIN32
#  include <netinet/tcp.h>
#endif

#include "log.h"
#include "thread_mutex.h"
#include "sound.h"

#define AV_MAGIC              "M2AV"
#define AV_VERSION            1
#define AV_HEADER_SIZE        32
#define AV_PKT_HEADER_SIZE    24

#define AV_FLAG_DISCONTINUITY 0x01

#define AV_FPS_NUM            60       /* nominal; the stamps are what count */
#define AV_FPS_DEN            1

#define AV_MIN_DIM            16
#define AV_MAX_DIM            4096
#define AV_DEFAULT_WIDTH      1396     /* 496:384 at 1080 tall, to the nearest pixel */
#define AV_DEFAULT_HEIGHT     1080

/* CPU-side frames waiting for the socket. Three is one being filled by the
 * render thread, one on the wire and one of slack; a fourth would only buy
 * latency, because a client that cannot keep up is not helped by a deeper
 * queue — it is helped by being told a frame went missing. */
#define AV_VIDEO_SLOTS        3

/* The audio ring, in stereo frames: a power of two, and about 5.9 s. Sized by
 * how long a client may stall before it counts as dead, not by latency — the
 * writer thread drains it every millisecond. */
#define AV_AUDIO_RING         262144u
#define AV_AUDIO_MARGIN       (AV_AUDIO_RING / 8u)  /* kept clear of the write head */
#define AV_AUDIO_CHUNK        512u     /* frames before a packet goes out (~11.6 ms) */
#define AV_AUDIO_MAX_CHUNK    8192u    /* ...and the most any one packet carries */
#define AV_AUDIO_FLUSH_MS     20       /* a short tail goes out after this long */
#define AV_AUDIO_MAX_BURST    16       /* audio packets per pass (and one video frame),
                                          so neither stream holds up the other */

/* The socket send buffer. The default 64 KB means net_tcp_send_all goes round
 * its select-then-send loop ~90 times for one 1396x1080 frame, and each turn
 * waits on the reader: measured, that capped the stream at ~76 MB/s, which is
 * a quarter of what 60 fps at that size needs. A few megabytes lets a whole
 * frame go in one or two passes. */
#define AV_SEND_BUF           (8 * 1024 * 1024)

/* The picture's wire format (--av-format). */
typedef enum {
    AV_FORMAT_BGRA,
    AV_FORMAT_NV12,
} av_format_t;

/* Why a board frame did not become a video packet. Three different faults,
 * and which one it is decides what to do about it: a socket that cannot keep
 * up, a readback that was not back in time, or a renderer that never reached
 * the frame at all. */
typedef enum {
    AV_DROP_QUEUE,      /* the send queue was full - the client is behind */
    AV_DROP_READBACK,   /* the GPU had not finished with the copy */
    AV_DROP_MISSED,     /* the frame went by without the renderer seeing it */
} av_drop_t;

typedef struct {
    uint64_t frame, sample;
    uint8_t  flags;
    uint8_t *px;                  /* vbytes: BGRA, or NV12's Y then CbCr; top row first */
} av_vslot_t;

typedef struct {
    int        enabled;
    int        port;
    int        width, height;
    av_format_t format;
    uint32_t   vbytes;            /* width*height*4 (BGRA) or width*height*3/2 (NV12) */

    net_sock_t listen_sock;
    /* The writer thread owns the client socket; shutdown only shuts it down,
     * never closes it, so the handle cannot be recycled under that thread. */
    net_sock_t client;
    volatile int connected;
    volatile int alive;
#ifdef _WIN32
    HANDLE     thread;
#else
    pthread_t  thread;
#endif

    /* Video queue — render thread writes, writer thread reads. */
    av_vslot_t vq[AV_VIDEO_SLOTS];
    volatile uint32_t v_w, v_r;   /* free-running; the difference is the depth */
    int        v_gap;             /* render thread only: flag the next frame sent */
    volatile uint64_t last_frame; /* newest board frame the renderer has seen */

    /* Audio ring — emu thread writes, writer thread reads. a_w is the ABSOLUTE
     * sample index (g_sound.out_total), which is also the stream's clock, so
     * the producer never has to renumber: it overwrites, and the reader
     * notices it was lapped. */
    int16_t   *a_ring;
    volatile uint64_t a_w;
    uint64_t   a_r;               /* writer thread only */

    volatile uint64_t v_sent, v_dropped, a_sent, a_dropped;
    volatile uint64_t v_lost[3];  /* by av_drop_t */
    uint64_t   sessions;
} av_stream_t;

static av_stream_t g_av;

/* ---- Little-endian writers ----------------------------------------------- */

static inline void av__u16(uint8_t **p, uint16_t v) {
    (*p)[0] = (uint8_t)v;
    (*p)[1] = (uint8_t)(v >> 8);
    *p += 2;
}
static inline void av__u32(uint8_t **p, uint32_t v) {
    for (int i = 0; i < 4; i++) (*p)[i] = (uint8_t)(v >> (8 * i));
    *p += 4;
}
static inline void av__u64(uint8_t **p, uint64_t v) {
    for (int i = 0; i < 8; i++) (*p)[i] = (uint8_t)(v >> (8 * i));
    *p += 8;
}

static inline void av__stream_header(uint8_t *b) {
    uint8_t *p = b;
    memcpy(p, AV_MAGIC, 4);   p += 4;
    av__u16(&p, AV_VERSION);
    av__u16(&p, AV_HEADER_SIZE);
    av__u16(&p, (uint16_t)g_av.width);
    av__u16(&p, (uint16_t)g_av.height);
    memcpy(p, g_av.format == AV_FORMAT_NV12 ? "NV12" : "BGRA", 4);
    p += 4;                                /* fourcc, in memory order */
    av__u32(&p, AV_FPS_NUM);
    av__u32(&p, AV_FPS_DEN);
    av__u32(&p, SOUND_RATE);
    *p++ = 2;                              /* channels */
    *p++ = 16;                             /* bits */
    av__u16(&p, 0);
}

static inline void av__pkt_header(uint8_t *b, uint8_t type, uint8_t flags,
                                  uint32_t size, uint64_t frame, uint64_t sample) {
    uint8_t *p = b;
    *p++ = type;
    *p++ = flags;
    av__u16(&p, 0);
    av__u32(&p, size);
    av__u64(&p, frame);
    av__u64(&p, sample);
}

/* ---- The audio tap (emu thread) ------------------------------------------ */

static void av_audio_tap(int16_t l, int16_t r, uint64_t index, void *ud) {
    (void)ud;
    int16_t *ring = g_av.a_ring;
    if (!ring) return;
    /* Unconditional: the ring overwrites, and the reader detects being lapped.
     * A producer that refused to write when full would have to renumber every
     * sample behind it, and the index IS the clock. */
    uint32_t slot = (uint32_t)(index & (AV_AUDIO_RING - 1u));
    ring[slot * 2]     = l;
    ring[slot * 2 + 1] = r;
    g_av.a_w = index + 1;
}

/* ---- The video hand-off (render thread) ---------------------------------- */

static inline bool av_stream_enabled(void) { return g_av.enabled != 0; }
/* Bumped on every accept. The capture ring watches it: copies still in flight
 * when a client goes belong to that client, and must not reach the next one. */
static inline uint64_t av_stream_session(void) { return g_av.sessions; }
static inline bool av_stream_active(void)  { return g_av.enabled && g_av.connected; }
static inline int  av_stream_width(void)   { return g_av.width; }
static inline int  av_stream_height(void)  { return g_av.height; }
static inline av_format_t av_stream_format(void) { return g_av.format; }
static inline const char *av_stream_format_name(void) {
    return g_av.format == AV_FORMAT_NV12 ? "NV12" : "BGRA";
}

/* Pick the wire format. Before av_stream_start; a started stream keeps its own. */
static inline void av_stream_set_format(av_format_t f) {
    if (!g_av.enabled) g_av.format = f;
}

/* Where the next frame's pixels go, or NULL when the queue has no room (or
 * nobody is connected). A NULL is not a drop by itself — the caller decides
 * whether a board frame was lost and says so with av_stream_video_drop. */
static inline uint8_t *av_stream_video_slot(void) {
    if (!av_stream_active()) return NULL;
    if ((uint32_t)(g_av.v_w - g_av.v_r) >= AV_VIDEO_SLOTS) return NULL;
    return g_av.vq[g_av.v_w % AV_VIDEO_SLOTS].px;
}

/* Publish the slot av_stream_video_slot handed back. */
static inline void av_stream_video_commit(uint64_t frame, uint64_t sample) {
    av_vslot_t *s = &g_av.vq[g_av.v_w % AV_VIDEO_SLOTS];
    s->frame  = frame;
    s->sample = sample;
    s->flags  = g_av.v_gap ? AV_FLAG_DISCONTINUITY : 0;
    g_av.v_gap      = 0;
    g_av.last_frame = frame;
    g_av.v_w++;                 /* last: the writer thread takes the slot on this */
}

/* A board frame that will not be sent — the queue was full, its readback was
 * not back in time, or the renderer never got to it at all. */
static inline void av_stream_video_drop(uint64_t frame, av_drop_t why) {
    g_av.last_frame = frame;
    if (!av_stream_active()) return;
    g_av.v_dropped++;
    g_av.v_lost[why]++;
    g_av.v_gap = 1;
}

/* Board frames that came and went without the renderer reaching them at all —
 * an occluded window not being asked to present, a host frame that ran long.
 * Not a queue drop, but the client cares about the same thing: a gap. */
static inline void av_stream_video_missed(uint64_t n, uint64_t newest) {
    g_av.last_frame = newest;
    if (!av_stream_active() || n == 0) return;
    g_av.v_dropped += n;
    g_av.v_lost[AV_DROP_MISSED] += n;
    g_av.v_gap = 1;
}

/* ---- The writer thread --------------------------------------------------- */

static inline bool av__send(net_sock_t s, const void *p, uint32_t n) {
    /* net_tcp_send_all spins on a 5 s select, so a client that has stopped
     * reading fails here rather than pinning this thread forever. The socket is
     * put in non-blocking mode on accept for exactly that reason: a blocking
     * one would sit in send() with no deadline at all. */
    return net_tcp_send_all(s, p, n);
}

static void av__serve(net_sock_t c) {
    static int16_t chunk[AV_AUDIO_MAX_CHUNK * 2];   /* writer thread only */
    uint8_t  hdr[AV_HEADER_SIZE];
    uint8_t  ph[AV_PKT_HEADER_SIZE];
    bool     ok = true;
    int      started = 0;         /* the first video packet has set the clock */
    int      a_gap = 0;
    int      one = 1;
    uint64_t last_audio_ms;

    net_set_nonblocking(c);
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    { int sb = AV_SEND_BUF;
      setsockopt(c, SOL_SOCKET, SO_SNDBUF, (const char *)&sb, sizeof sb); }

    av__stream_header(hdr);
    if (!av__send(c, hdr, AV_HEADER_SIZE)) return;

    /* Start clean at the next board-frame boundary: whatever the queue holds is
     * from before this client existed. */
    g_av.v_r      = g_av.v_w;
    g_av.v_gap    = 0;
    g_av.a_r      = g_av.a_w;
    last_audio_ms = net_now_ms();
    g_av.sessions++;
    g_av.connected = 1;

    while (g_av.alive && ok) {
        bool did = false;

        /* The first video frame says where the audio starts, so both streams
         * begin at the same instant on the board's own clock. */
        if (!started && g_av.v_r != g_av.v_w) {
            uint64_t origin = g_av.vq[g_av.v_r % AV_VIDEO_SLOTS].sample;
            /* ...but only as far back as the ring still holds. A frame stamped
             * long before this client existed would otherwise set the read
             * head minutes in the past, and the whole session would then be
             * spent lapping through stale audio. */
            uint64_t w = g_av.a_w, span = AV_AUDIO_RING - AV_AUDIO_MARGIN;
            uint64_t oldest = w > span ? w - span : 0;
            g_av.a_r      = origin < oldest ? oldest : origin;
            started       = 1;
            last_audio_ms = net_now_ms();
        }

        /* Audio first: it is the stream that may not be dropped, and a 6 MB
         * video frame ahead of it would hold it up for a whole send. */
        for (int burst = 0; started && ok && burst < AV_AUDIO_MAX_BURST; burst++) {
            uint64_t w = g_av.a_w, avail = w - g_av.a_r;
            if (avail > AV_AUDIO_RING) {
                /* Lapped — this client has effectively stopped reading. Skip to
                 * a safe distance behind the write head rather than send samples
                 * that are being overwritten as they are copied. */
                uint64_t lost = avail - (AV_AUDIO_RING - AV_AUDIO_MARGIN);
                g_av.a_r       += lost;
                g_av.a_dropped += lost;
                a_gap = 1;
                avail = w - g_av.a_r;
                LOG_WARN("av: audio ring lapped, %llu samples lost",
                         (unsigned long long)lost);
            }
            uint64_t now = net_now_ms();
            bool due = avail >= AV_AUDIO_CHUNK ||
                       (avail > 0 && now - last_audio_ms >= AV_AUDIO_FLUSH_MS);
            if (!due) break;

            uint32_t n     = (uint32_t)(avail > AV_AUDIO_MAX_CHUNK ? AV_AUDIO_MAX_CHUNK : avail);
            uint64_t first = g_av.a_r;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t slot = (uint32_t)((first + i) & (AV_AUDIO_RING - 1u));
                chunk[i * 2]     = g_av.a_ring[slot * 2];
                chunk[i * 2 + 1] = g_av.a_ring[slot * 2 + 1];
            }
            av__pkt_header(ph, 'A', (uint8_t)(a_gap ? AV_FLAG_DISCONTINUITY : 0),
                           n * 4u, g_av.last_frame, first);
            ok = av__send(c, ph, AV_PKT_HEADER_SIZE) && av__send(c, chunk, n * 4u);
            if (!ok) break;
            a_gap          = 0;
            g_av.a_r      += n;
            g_av.a_sent   += n;
            last_audio_ms  = now;
            did = true;
        }

        /* Then ONE queued frame, and back round for the audio. Draining the
         * whole queue here starved the sound whenever the client took about as
         * long to read a frame as the renderer took to make one: at 1920x1080
         * that is ~23 ms against ~21, the queue never emptied, and measured
         * off a live tap the audio stopped for up to 1.7 s at a time while 65
         * frames went out back to back -- then arrived as nine 8192-frame
         * packets at once. A player that holds a fixed cushion of sound
         * cannot absorb that: it runs dry, then overflows and skips. */
        if (ok && g_av.v_r != g_av.v_w) {
            av_vslot_t *s = &g_av.vq[g_av.v_r % AV_VIDEO_SLOTS];
            av__pkt_header(ph, 'V', s->flags, g_av.vbytes, s->frame, s->sample);
            ok = av__send(c, ph, AV_PKT_HEADER_SIZE) && av__send(c, s->px, g_av.vbytes);
            if (!ok) break;
            g_av.v_r++;           /* last: the slot is the render thread's again */
            g_av.v_sent++;
            did = true;
        }

        if (!did) emu_sleep_ms(1);
    }
    g_av.connected = 0;
}

#ifdef _WIN32
static DWORD WINAPI av__thread_proc(LPVOID arg) {
    (void)arg;
#else
static void *av__thread_proc(void *arg) {
    (void)arg;
#endif
    while (g_av.alive) {
        struct sockaddr_in ca;
#ifdef _WIN32
        int addr_len = (int)sizeof ca;
#else
        socklen_t addr_len = sizeof ca;
#endif
        net_sock_t c = accept(g_av.listen_sock, (struct sockaddr *)&ca, &addr_len);
        if (!net_sock_valid(c)) {
            if (g_av.alive) emu_sleep_ms(10);
            continue;
        }
        g_av.client = c;
        LOG_INFO("av: client connected (%dx%d %s, %u Hz 16-bit stereo)",
                 g_av.width, g_av.height, av_stream_format_name(), SOUND_RATE);
        av__serve(c);
        g_av.client = NET_SOCK_INVALID;
        net_close(&c);
        LOG_INFO("av: client gone (%llu frames sent, %llu dropped; "
                 "%llu samples sent, %llu dropped)",
                 (unsigned long long)g_av.v_sent, (unsigned long long)g_av.v_dropped,
                 (unsigned long long)g_av.a_sent, (unsigned long long)g_av.a_dropped);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---- Public API ---------------------------------------------------------- */

static inline void av_stream_clamp_size(int *w, int *h) {
    if (*w < AV_MIN_DIM) *w = AV_MIN_DIM;
    if (*h < AV_MIN_DIM) *h = AV_MIN_DIM;
    if (*w > AV_MAX_DIM) *w = AV_MAX_DIM;
    if (*h > AV_MAX_DIM) *h = AV_MAX_DIM;
    *w &= ~1;                  /* every encoder downstream wants even dimensions */
    *h &= ~1;
    /* NV12 is converted four pixels to a texel (ui/av_capture.h), so its
     * width is a multiple of 4. 1396 and 1920 already are. */
    if (g_av.format == AV_FORMAT_NV12) *w &= ~3;
}

/*
 * The two rings are allocated once and NEVER FREED — see av_stream_shutdown.
 * Only the start path below, where no thread exists yet and the tap is not
 * installed, may hand them back.
 */
static inline void av__free_rings(void) {
    for (int i = 0; i < AV_VIDEO_SLOTS; i++) { free(g_av.vq[i].px); g_av.vq[i].px = NULL; }
    free(g_av.a_ring);
    g_av.a_ring = NULL;
}

static inline bool av_stream_start(int port, int w, int h) {
    net_sock_t s = NET_SOCK_INVALID;
    struct sockaddr_in addr;
    int opt = 1;

    if (g_av.enabled) return true;
    av_stream_clamp_size(&w, &h);
    g_av.width  = w;
    g_av.height = h;
    g_av.vbytes = g_av.format == AV_FORMAT_NV12
                ? (uint32_t)w * (uint32_t)h * 3u / 2u
                : (uint32_t)w * (uint32_t)h * 4u;

    for (int i = 0; i < AV_VIDEO_SLOTS; i++) {
        free(g_av.vq[i].px);       /* a restart at a new size; see av__free_rings */
        g_av.vq[i].px = (uint8_t *)calloc(1, g_av.vbytes);
        if (!g_av.vq[i].px) {
            LOG_ERROR("av: out of memory for a %dx%d frame slot", w, h);
            goto fail;
        }
    }
    if (!g_av.a_ring) {
        g_av.a_ring = (int16_t *)calloc(AV_AUDIO_RING * 2u, sizeof(int16_t));
        if (!g_av.a_ring) {
            LOG_ERROR("av: out of memory for the audio ring");
            goto fail;
        }
    }

    if (!net_startup()) {
        LOG_ERROR("av: socket startup failed");
        goto fail;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (!net_sock_valid(s)) {
        LOG_ERROR("av: socket() failed (%d)", net_errno());
        goto fail_net;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof opt);

    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((uint16_t)port);
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0) {
        LOG_ERROR("av: bind() failed on port %d (%d)", port, net_errno());
        net_close(&s);
        goto fail_net;
    }
    if (listen(s, 1) != 0) {
        LOG_ERROR("av: listen() failed (%d)", net_errno());
        net_close(&s);
        goto fail_net;
    }

    g_av.listen_sock = s;
    g_av.client      = NET_SOCK_INVALID;
    g_av.port        = port;
    g_av.alive       = 1;
    g_av.enabled     = 1;

    /* The tap goes in only once the ring it writes to exists. */
    sound_set_tap(av_audio_tap, NULL);

#ifdef _WIN32
    g_av.thread = CreateThread(NULL, 0, av__thread_proc, NULL, 0, NULL);
#else
    pthread_create(&g_av.thread, NULL, av__thread_proc, NULL);
#endif
    LOG_INFO("av: listening on 127.0.0.1:%d — %dx%d %s, %u Hz 16-bit stereo",
             port, g_av.width, g_av.height, av_stream_format_name(), SOUND_RATE);
    return true;

fail_net:
    net_shutdown_lib();
fail:
    av__free_rings();      /* nothing is running yet: the only safe place to */
    return false;
}

/*
 * Stop serving. THE RINGS ARE NOT FREED, deliberately: the audio tap runs on
 * the emu thread, and clearing the function pointer does not retire a call
 * already inside it, so a free here would be a use-after-free on a thread that
 * has not been stopped yet. This is the same shape as the texram crash in
 * CLAUDE.md, and the same answer — keep the allocation for the life of the
 * process. It is one allocation, made once, and the process is exiting.
 */
static inline void av_stream_shutdown(void) {
    if (!g_av.enabled) return;
    g_av.alive   = 0;
    g_av.enabled = 0;
    sound_set_tap(NULL, NULL);
    /* Both the accept and any send in flight are blocking calls on the writer
     * thread. Closing the listener ends the accept; the client socket is only
     * SHUT DOWN from here, never closed, so its handle cannot be recycled
     * under the thread that is still holding it. */
    net_close(&g_av.listen_sock);
    if (net_sock_valid(g_av.client)) {
#ifdef _WIN32
        shutdown(g_av.client, SD_BOTH);
#else
        shutdown(g_av.client, SHUT_RDWR);
#endif
    }
#ifdef _WIN32
    if (g_av.thread) {
        WaitForSingleObject(g_av.thread, 6000);
        CloseHandle(g_av.thread);
        g_av.thread = NULL;
    }
#else
    pthread_join(g_av.thread, NULL);
#endif
    net_shutdown_lib();
    LOG_INFO("av: stopped");
}

/* The "av" block of get_status. Returns the characters written. */
static inline int av_stream_status_json(char *buf, int cap) {
    if (!g_av.enabled)
        return snprintf(buf, (size_t)cap, "{\"enabled\":false}");
    return snprintf(buf, (size_t)cap,
        "{\"enabled\":true,\"port\":%d,\"width\":%d,\"height\":%d,\"format\":\"%s\","
        "\"connected\":%s,"
        "\"sessions\":%llu,\"video_sent\":%llu,\"video_dropped\":%llu,"
        "\"dropped_queue\":%llu,\"dropped_readback\":%llu,\"dropped_missed\":%llu,"
        "\"audio_sent\":%llu,\"audio_dropped\":%llu}",
        g_av.port, g_av.width, g_av.height, av_stream_format_name(),
        g_av.connected ? "true" : "false",
        (unsigned long long)g_av.sessions,
        (unsigned long long)g_av.v_sent, (unsigned long long)g_av.v_dropped,
        (unsigned long long)g_av.v_lost[AV_DROP_QUEUE],
        (unsigned long long)g_av.v_lost[AV_DROP_READBACK],
        (unsigned long long)g_av.v_lost[AV_DROP_MISSED],
        (unsigned long long)g_av.a_sent, (unsigned long long)g_av.a_dropped);
}

#endif /* AV_STREAM_H */
