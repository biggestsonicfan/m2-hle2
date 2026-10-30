/*
 * replay_video.h -- --replay-video FILE: play a replay with no window and
 * write it to a video file, every frame and its sound, then exit (Pinboard
 * #1925).
 *
 *     m2hle --rom sfight.zip --replay match.m2replay --replay-video match.mp4
 *
 * It is the A/V server (core/av_stream.h) with its client in the process: the
 * server listens on a free port of 127.0.0.1, and the thread below reads it as
 * tools/av-record.py does -- the picture into ffmpeg, the sound into a raw file
 * beside it, both muxed when the replay ends. ffmpeg does all the encoding; it
 * is looked for on PATH, or --ffmpeg names it.
 *
 * WHY THE BOARD WAITS. A stream keeps the board's 60 Hz and drops a frame the
 * renderer or the client cannot take; at 1396x1080 on a software GL that is
 * half of them. A file has no clock to keep, so here nothing is dropped: the
 * board waits for each frame to be drawn (g_emu_frame_gate), the renderer waits
 * for a queue slot and for its readback (av_capture_set_wait), and the queue
 * waits for ffmpeg. The replay runs as fast as the slowest of them, faster or
 * slower than real time, and the video's clock is the board's own sample count.
 *
 * WHAT IS IN THE FILE. From the first paced frame of the playback (a replay in
 * the game's own view runs unpaced through the boot first, and that is not
 * recorded) to the frame its feed ended, or left the record ("split"). The
 * sound has the board's DC offset taken out, as av-record.py does.
 */
#ifndef REPLAY_VIDEO_H
#define REPLAY_VIDEO_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#  include <signal.h>
#endif

#include "net_socket.h"
#include "log.h"
#include "emu_thread.h"
#include "replay.h"
#include "av_stream.h"
#include "av_capture.h"

#ifdef _WIN32
#  define RV_POPEN(c)  _popen((c), "wb")
#  define RV_PCLOSE(f) _pclose(f)
#else
#  define RV_POPEN(c)  popen((c), "w")
#  define RV_PCLOSE(f) pclose(f)
#endif

#define RV_FPS          60      /* the output grid, as av-record.py's */
#define RV_TAIL_MS      5000    /* after the end, how long to wait for its last packets */
#define RV_NOTE_US      5000000 /* a progress line this often */

enum { RV_OFF, RV_WAIT, RV_REC, RV_STOP, RV_DONE };

static struct {
    char path[512];             /* --replay-video FILE */
    char ffmpeg[512];           /* --ffmpeg PATH, else "ffmpeg" */
    int  phase;
    bool split;                 /* the playback left the record: the file ends there */
    int64_t next_note_us;

    /* Set by the main thread, read by the recorder. */
    volatile uint64_t start_sample, end_sample;
    volatile int start_set, end_set;
    /* Set by the recorder, read by the main thread. */
    volatile int finished, ok;
    char error[800];

#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
    bool thread_on;
} g_rv;

static inline bool replay_video_on(void) { return g_rv.phase != RV_OFF; }

/* --replay-video FILE (and --ffmpeg PATH) from the command line. */
static inline void replay_video_set(const char *path) {
    snprintf(g_rv.path, sizeof g_rv.path, "%s", path);
    g_rv.phase = RV_WAIT;
}
static inline void replay_video_set_ffmpeg(const char *exe) {
    snprintf(g_rv.ffmpeg, sizeof g_rv.ffmpeg, "%s", exe);
}

static inline void rv__fail(const char *fmt, const char *arg) {
    if (!g_rv.error[0]) snprintf(g_rv.error, sizeof g_rv.error, fmt, arg);   /* the first one counts */
}

/* A command line ffmpeg can be run with. cmd.exe strips the outer pair of
 * quotes off a /c line, so on Windows the whole line goes in one more pair. */
static inline void rv__cmd(char *out, size_t cap, const char *args) {
    const char *exe = g_rv.ffmpeg[0] ? g_rv.ffmpeg : "ffmpeg";
#ifdef _WIN32
    snprintf(out, cap, "\"\"%s\" -hide_banner -loglevel error -y %s\"", exe, args);
#else
    snprintf(out, cap, "\"%s\" -hide_banner -loglevel error -y %s", exe, args);
#endif
}

/* Exactly n bytes off the socket; false when it closed or went quiet. */
static inline bool rv__read(net_sock_t s, uint8_t *buf, uint32_t n) {
    uint32_t got = 0;
    while (got < n) {
        int k = net_tcp_recv_timeout(s, buf + got, n - got, 30000);
        if (k <= 0) return false;
        got += (uint32_t)k;
    }
    return true;
}

static inline uint32_t rv__u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t rv__u64(const uint8_t *p) {
    return (uint64_t)rv__u32(p) | (uint64_t)rv__u32(p + 4) << 32;
}

/* The stream, from start_sample to end_sample, into the video pipe and the
 * audio file. False when the stream or ffmpeg failed first. */
static inline bool rv__pump(net_sock_t s, FILE *vp, FILE *af, uint32_t vbytes,
                            uint64_t *frames_out, uint64_t *samples_out) {
    uint8_t  hdr[AV_PKT_HEADER_SIZE];
    uint8_t *buf = (uint8_t *)malloc(vbytes > AV_AUDIO_MAX_CHUNK * 4u ? vbytes : AV_AUDIO_MAX_CHUNK * 4u);
    uint8_t *held = (uint8_t *)malloc(vbytes);
    bool have_held = false, ok = true;
    uint64_t emitted = 0, samples = 0;
    int64_t end_seen_us = 0;
    bool audio_done = false, video_done = false;
    if (!buf || !held) { free(buf); free(held); rv__fail("out of memory%s", ""); return false; }

    while (!(audio_done && video_done)) {
        if (g_rv.end_set && !end_seen_us) end_seen_us = emu_now_us();
        if (end_seen_us && emu_now_us() - end_seen_us > (int64_t)RV_TAIL_MS * 1000) break;
        if (!rv__read(s, hdr, sizeof hdr)) {
            if (!g_rv.end_set) { rv__fail("the A/V stream closed%s", ""); ok = false; }
            break;
        }
        uint32_t size   = rv__u32(hdr + 4);
        uint64_t sample = rv__u64(hdr + 16);
        if (size > (vbytes > AV_AUDIO_MAX_CHUNK * 4u ? vbytes : AV_AUDIO_MAX_CHUNK * 4u)
                || !rv__read(s, buf, size)) {
            rv__fail("the A/V stream broke off%s", "");
            ok = false;
            break;
        }
        if (!g_rv.start_set) continue;                 /* not recording yet */
        uint64_t from = g_rv.start_sample;
        uint64_t to   = g_rv.end_set ? g_rv.end_sample : UINT64_MAX;

        if (hdr[0] == 'A') {
            /* [sample, sample + n) cut to [from, to). */
            uint64_t n = size / 4u, lo = sample, hi = sample + n;
            if (lo < from) lo = from;
            if (hi > to)   hi = to;
            if (hi > lo && fwrite(buf + (lo - sample) * 4u, 4, (size_t)(hi - lo), af) != (size_t)(hi - lo)) {
                rv__fail("could not write the sound%s", "");
                ok = false;
                break;
            }
            if (hi > lo) samples += hi - lo;
            if (sample + n >= to) audio_done = true;
        } else if (size == vbytes) {
            if (sample <= from) continue;              /* a frame from before the start */
            if (sample > to) { video_done = true; continue; }
            /* A picture covers the grid up to where its sample stamp falls;
             * a gap (there should be none) holds the one before. */
            memcpy(held, buf, vbytes);
            have_held = true;
            uint64_t want = (sample - from) * RV_FPS / SOUND_RATE;
            while (emitted < want) {
                if (fwrite(held, 1, vbytes, vp) != vbytes) {
                    rv__fail("ffmpeg stopped taking the picture%s", "");
                    ok = false;
                    goto out;
                }
                emitted++;
            }
            if (sample == to) video_done = true;
        }
    }
out:
    if (ok && have_held && emitted == 0 && fwrite(held, 1, vbytes, vp) == vbytes) emitted++;
    free(buf);
    free(held);
    *frames_out  = emitted;
    *samples_out = samples;
    return ok;
}

/* The recorder thread: connect, pump, encode, mux. */
static inline void rv__record(void) {
    net_sock_t s = NET_SOCK_INVALID;
    if (!net_tcp_connect(&s, "127.0.0.1", (uint16_t)av_stream_port(), 5000)) {
        rv__fail("could not reach the A/V server%s", "");
        return;
    }
    uint8_t h[AV_HEADER_SIZE];
    if (!rv__read(s, h, sizeof h) || memcmp(h, AV_MAGIC, 4)) {
        rv__fail("no A/V stream header%s", "");
        net_close(&s);
        return;
    }
    int w = h[8] | h[9] << 8, ht = h[10] | h[11] << 8;
    bool nv12 = !memcmp(h + 12, "NV12", 4);
    uint32_t vbytes = nv12 ? (uint32_t)w * (uint32_t)ht * 3u / 2u : (uint32_t)w * (uint32_t)ht * 4u;

    char vtmp[600], atmp[600], args[2048], cmd[2700];
    snprintf(vtmp, sizeof vtmp, "%s.video.mp4", g_rv.path);
    snprintf(atmp, sizeof atmp, "%s.audio.raw", g_rv.path);
    /* NV12 is BT.709 limited range already (ui/av_capture.h): tag it and pass it through. */
    snprintf(args, sizeof args,
             "-f rawvideo -pix_fmt %s %s-s %dx%d -r %d -i pipe:0 "
             "-an -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p \"%s\"",
             nv12 ? "nv12" : "bgra",
             nv12 ? "-color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv " : "",
             w, ht, RV_FPS, vtmp);
    rv__cmd(cmd, sizeof cmd, args);
    FILE *vp = RV_POPEN(cmd);
    FILE *af = fopen(atmp, "wb");
    if (!vp || !af) {
        rv__fail(!vp ? "could not run ffmpeg (%s)" : "could not write %s", !vp ? cmd : atmp);
        if (vp) RV_PCLOSE(vp);
        if (af) fclose(af);
        net_close(&s);
        remove(atmp);
        return;
    }
    LOG_INFO("replay-video: %dx%d %s into ffmpeg", w, ht, nv12 ? "NV12" : "BGRA");

    uint64_t frames = 0, samples = 0;
    bool ok = rv__pump(s, vp, af, vbytes, &frames, &samples);
    net_close(&s);                        /* the server's writer lets go of the renderer */
    fclose(af);
    int vrc = RV_PCLOSE(vp);
    if (ok && vrc != 0) { rv__fail("ffmpeg failed on the picture (%s)", vtmp); ok = false; }
    if (ok && frames == 0) { rv__fail("no frame was recorded%s", ""); ok = false; }

    if (ok) {
        LOG_INFO("replay-video: %llu frames at %d fps (%.2f s), %llu samples (%.2f s); muxing",
                 (unsigned long long)frames, RV_FPS, (double)frames / RV_FPS,
                 (unsigned long long)samples, (double)samples / SOUND_RATE);
        /* The board's output sits ~5000/32768 off zero; a cabinet's amplifier
         * is AC-coupled, so the file is too (av-record.py does the same). */
        snprintf(args, sizeof args,
                 "-i \"%s\" -f s16le -ar %u -ac 2 -i \"%s\" -af highpass=f=5 "
                 "-c:v copy -c:a aac -b:a 192k -shortest \"%s\"",
                 vtmp, (unsigned)SOUND_RATE, atmp, g_rv.path);
        rv__cmd(cmd, sizeof cmd, args);
        FILE *mp = RV_POPEN(cmd);
        if (!mp || RV_PCLOSE(mp) != 0) { rv__fail("ffmpeg could not mux %s", g_rv.path); ok = false; }
    }
    remove(vtmp);
    remove(atmp);
    g_rv.ok = ok;
}

#ifdef _WIN32
static DWORD WINAPI rv__thread_proc(LPVOID arg) {
    (void)arg;
    rv__record();
    g_rv.finished = 1;
    return 0;
}
#else
static void *rv__thread_proc(void *arg) {
    (void)arg;
    rv__record();
    g_rv.finished = 1;
    return NULL;
}
#endif

/* Before the board runs: check the file and the replay, start the recorder.
 * The A/V server must be up (on any port). False, and why in the log, if not. */
static inline bool replay_video_start(void) {
    if (strchr(g_rv.path, '"') || strchr(g_rv.ffmpeg, '"')) {
        LOG_ERROR("--replay-video: a path with a \" in it cannot be handed to ffmpeg");
        return false;
    }
    if (!replay_playing()) {
        LOG_ERROR("--replay-video: no replay is playing (give it one with --replay FILE)");
        return false;
    }
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);   /* an ffmpeg that dies is an error, not the end of us */
#endif
#ifdef _WIN32
    g_rv.thread = CreateThread(NULL, 0, rv__thread_proc, NULL, 0, NULL);
    g_rv.thread_on = g_rv.thread != NULL;
#else
    g_rv.thread_on = pthread_create(&g_rv.thread, NULL, rv__thread_proc, NULL) == 0;
#endif
    if (!g_rv.thread_on) { LOG_ERROR("--replay-video: could not start the recorder"); return false; }
    g_rv.next_note_us = emu_now_us() + RV_NOTE_US;
    return true;
}

/* Where the playback is: (frame, frames, ended, split). */
static inline replay__pos_t rv__pos(void) {
    return replay__pos(&g_replay_play, g_replay_play.on && g_replay_play.native_on);
}

/* The end: no more frames wanted, the board back on its clock. The frames
 * still in the readback ring go out first: nothing later will push them. */
static inline void rv__end(void) {
    av_capture_flush();
    g_rv.end_sample = g_frame_clock.sample;
    g_rv.end_set    = 1;
    g_emu_frame_gate = 0;
    g_rv.phase = RV_STOP;
}

/* Once per pass of the headless loop. True when the file is done (or failed)
 * and the process should exit. */
static inline bool replay_video_pump(void) {
    switch (g_rv.phase) {
    case RV_WAIT:
        if (g_rv.finished) { g_rv.phase = RV_DONE; return true; }   /* the recorder gave up */
        if (!replay_playing()) {
            rv__fail("the replay stopped before it played%s", "");
            rv__end();
            return false;
        }
        if (replay_play_unpaced() || !av_stream_active()) return false;
        /* The first paced frame: record from here, every frame. */
        av_capture_set_wait(true);
        g_rv.start_sample = g_frame_clock.sample;
        g_rv.start_set    = 1;
        g_emu_frame_drawn = g_frame_clock.frame;
        g_emu_frame_gate  = 1;
        g_rv.phase = RV_REC;
        LOG_INFO("replay-video: recording %s", g_rv.path);
        return false;
    case RV_REC: {
        replay__pos_t p = rv__pos();
        if (g_rv.finished) { rv__end(); return false; }
        if (emu_now_us() >= g_rv.next_note_us) {
            g_rv.next_note_us = emu_now_us() + RV_NOTE_US;
            LOG_INFO("replay-video: frame %u of %u", p.frame, p.frames);
        }
        /* Ended, once the frame it ended on is drawn (the board waits on it). */
        if ((!replay_playing() || p.ended || p.split) && av_capture_seen() >= g_frame_clock.frame) {
            g_rv.split = p.split;
            if (p.split)
                LOG_WARN("replay-video: the playback left the record at frame %u; the video ends there", p.frame);
            rv__end();
        }
        return false;
    }
    case RV_STOP:
        if (!g_rv.finished) return false;
        av_capture_set_wait(false);
        g_rv.phase = RV_DONE;
        return true;
    default:
        return g_rv.phase == RV_DONE;
    }
}

/* After the loop: wait for the recorder, and say how it went. The exit code. */
static inline int replay_video_finish(void) {
    g_emu_frame_gate = 0;
    av_capture_set_wait(false);
    if (g_rv.thread_on) {
        if (!g_rv.end_set) {              /* a quit from the tray or the bridge: keep what there is */
            g_rv.end_sample = g_frame_clock.sample;
            g_rv.end_set    = 1;
        }
#ifdef _WIN32
        WaitForSingleObject(g_rv.thread, INFINITE);
        CloseHandle(g_rv.thread);
#else
        pthread_join(g_rv.thread, NULL);
#endif
        g_rv.thread_on = false;
    }
    if (!g_rv.ok || g_rv.error[0]) {
        LOG_ERROR("--replay-video %s: %s", g_rv.path, g_rv.error[0] ? g_rv.error : "not written");
        return 1;
    }
    LOG_INFO("replay-video: wrote %s%s", g_rv.path, g_rv.split ? " (up to where the playback split)" : "");
    return g_rv.split ? 4 : 0;
}

#endif /* REPLAY_VIDEO_H */
