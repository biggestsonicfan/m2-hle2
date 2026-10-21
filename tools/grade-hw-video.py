#!/usr/bin/env python3
"""grade-hw-video.py -- hold m2hle's STF attract against a video of the real board.

The capture (Pinboard #1360, uploads "2026-10-20 15-33-37.mp4") is a Model 2B
running Sonic the Fighters, Japan, recorded with OBS: 448x346 at 60 fps, AAC
48 kHz, 104 s from the middle of the WARNING screen through the intro, the Tails
lab, the title, the attract fight (Sonic vs Bean, an input replay) and the
winner. Colour and luminosity are not graded: each frame is a luma pattern with
its own mean and contrast taken out.

  python3 tools/grade-hw-video.py --video <mp4> --exe <m2hle> [--rom sfight.zip]
                                  [--work DIR] [--secs 112] [--keep] [--emu-args '...']

It runs m2hle headless (--profile sfight --region japan --nowarnskip
--no-nvram) for --secs of wall time, reads its raw A/V stream (--av-port,
av_stream.h), and measures:

  pictures  the board's frames registered onto the capture (REG below, fitted
            once on this capture), DTW-aligned; the median residual
  rate      board frames a second the real board shows: its 60 Hz capture
            repeats a frame about every 24 (57.52 Hz), and in the replayed
            fight it advances as many board frames a second as m2hle does
  loads     how many board frames m2hle is ahead of the board, by hw time
            (reported, not graded: open, see sound-board.md "The real board")
  pitch     the frequency scale between the two spectra (Welch, log f)
  tempo     emu audio time against hw time, from a log-band spectrogram match
            per second, as a slope per segment
  sync      where each frame's sound is against the frame (a-v), from the same
            match and the stream's sample stamps

Needs ffmpeg, numpy and scipy. Writes to --work (a temp dir, deleted unless
--keep): the decoded capture is ~200 MB.
"""
import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np
from scipy import ndimage, signal

BOARD_HZ = 25e6 / 434600                 # irq_timer.h IRQT_FRAME_CYCLES
EMU_W, EMU_H = 496, 384                  # --av-size
HW_W, HW_H = 224, 173                    # the capture at half size
# emu pixel (496x384) -> capture pixel (448x346): x = sx*u + dx, y = sy*v + dy
REG = (0.85232, 0.84136, 1.16696, 16.25196)
SEGMENTS = ((13, 51), (63, 74), (78, 96))   # hw seconds: intro + lab, fight, fight's end
FIGHT = (60, 95)

LIMITS = {                                # what passes
    'pictures': 0.35,                     # median DTW residual
    'shown': 0.15,                        # |the capture's frame rate - the board's| Hz
    'rate': 0.002,                        # |emu/hw frames - 1| in the fight
    'pitch': 0.002,                       # |scale - 1|
    'tempo': 0.015,                       # |emu/hw audio time - 1|, each segment
    'sync': 0.35,                         # |a-v| seconds, 13 s to the end of the fight
}


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def decode_capture(video, work):
    gray = os.path.join(work, 'hw.gray')
    pcm = os.path.join(work, 'hw.f32')
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', video, '-vf', f'fps=60,scale={HW_W}:{HW_H},format=gray',
                    '-f', 'rawvideo', gray], check=True)
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', video, '-vn', '-ac', '1', '-ar', '48000',
                    '-f', 'f32le', pcm], check=True)
    hw = np.fromfile(gray, np.uint8).reshape(-1, HW_H, HW_W)
    return hw, np.fromfile(pcm, np.float32)


def record(args, work):
    """m2hle's frames (124x96 gray, 4x4 mean), their frame and sample stamps, and its audio."""
    port = free_port()
    with open(os.path.join(work, 'net.cfg'), 'w'):
        pass                              # never the per-user netplay.cfg (it holds a Twitch login)
    cmd = [args.exe, '--headless', '--net-config', os.path.join(work, 'net.cfg'), '--rom', args.rom,
           '--profile', 'sfight', '--region', 'japan', '--nowarnskip', '--no-nvram', '--run',
           '--av-port', str(port), '--av-size', f'{EMU_W}x{EMU_H}'] + args.emu_args.split()
    log = open(os.path.join(work, 'emu.log'), 'w')
    proc = subprocess.Popen(cmd, cwd=work, stdout=log, stderr=subprocess.STDOUT)
    try:
        for _ in range(300):
            try:
                s = socket.create_connection(('127.0.0.1', port), timeout=15)
                break
            except OSError:
                time.sleep(0.1)
        else:
            sys.exit('no A/V stream from m2hle (see emu.log)')
        s.settimeout(15)

        def rd(n):
            b = bytearray(n)
            v = memoryview(b)
            g = 0
            while g < n:
                k = s.recv_into(v[g:], n - g)
                if not k:
                    raise EOFError
                g += k
            return bytes(b)

        h = rd(32)
        w, ht = struct.unpack_from('<HH', h, 8)
        frames, samples, pics, audio = [], [], [], []
        t0 = time.time()
        while time.time() - t0 < args.secs:
            typ, _f, _r, size, frame, sample = struct.unpack('<BBHIQQ', rd(24))
            p = rd(size)
            if typ == ord('A'):
                audio.append((sample, np.frombuffer(p, np.int16).reshape(-1, 2)))
            elif typ == ord('V'):
                g = np.frombuffer(p, np.uint8).reshape(ht, w, 4)[:, :, :3].mean(2)
                g = g[:ht // 4 * 4, :w // 4 * 4].reshape(ht // 4, 4, w // 4, 4).mean((1, 3))
                frames.append(frame)
                samples.append(sample)
                pics.append(g.astype(np.float32))
        s.close()
    finally:
        proc.kill()
        proc.wait()
        log.close()
    # Laid out on the board's sample counter from 0, so a frame's sound is at
    # its stamp whenever the stream was joined (a gap stays silent).
    pcm = np.zeros(audio[-1][0] + len(audio[-1][1]), np.float32)
    for at, a in audio:
        pcm[at:at + len(a)] = a.astype(np.float32).mean(1) / 32768.0
    return np.array(frames), np.array(samples), np.array(pics), pcm


def features(a):
    a = a[:, 4:-4, 4:-4]
    a = a.reshape(len(a), 44, 2, 58, 2).mean((2, 4)).reshape(len(a), -1)
    a = a - a.mean(1, keepdims=True)
    return (a / np.maximum(a.std(1, keepdims=True), 3.0)).astype(np.float32)


def align(hw, pics):
    """Each capture frame's emu frame: a monotone DTW over registered luma patterns."""
    sx, sy, dx, dy = REG
    v, u = np.mgrid[0:EMU_H // 4, 0:EMU_W // 4].astype(np.float32)
    hx = (sx * (u * 4 + 2) + dx) / 2
    hy = (sy * (v * 4 + 2) + dy) / 2
    hr = np.stack([ndimage.map_coordinates(f.astype(np.float32), [hy, hx], order=1) for f in hw])
    H, E = features(hr), features(pics)
    C = ((H ** 2).sum(1)[:, None] + (E ** 2).sum(1)[None, :] - 2 * H @ E.T) / H.shape[1]
    n, m = C.shape
    D = C[0].copy()
    P = np.zeros((n, m), np.int8)
    for i in range(1, n):
        b = np.concatenate(([np.inf], D[:-1]))
        c = np.concatenate(([np.inf, np.inf], D[:-2]))
        st = np.stack([D + 0.05, b, c + 0.05])
        k = st.argmin(0)
        D = C[i] + st[k, np.arange(m)]
        P[i] = k
    j = int(D.argmin())
    path = np.zeros(n, int)
    for i in range(n - 1, -1, -1):
        path[i] = j
        j -= int(P[i, j])
    return path, C[np.arange(n), path]


def bands(x):
    f, _t, S = signal.stft(x, 48000, nperseg=2048, noverlap=2048 - 480)   # 10 ms hop
    S = np.abs(S)
    edges = np.geomspace(150, 12000, 33)
    B = np.stack([S[(f >= edges[i]) & (f < edges[i + 1])].mean(0) for i in range(32)])
    return np.log(B + 1e-3 * S.mean())


def audio_match(hpcm, epcm, esamples, path, hw_secs):
    """Per second of capture: the emu audio time whose spectrogram matches it best,
    searched +-5 s around where the matched frame's sound is."""
    He, Ee = bands(hpcm), bands(epcm)

    def z(a):
        a = a - a.mean(1, keepdims=True)
        return a / (a.std() + 1e-9)

    W, out = 400, []
    for t0 in np.arange(9, min(99, hw_secs - 5), 1.0):
        i = int(t0 * 100)
        w = z(He[:, i:i + W])
        pred = esamples[path[int(t0 * 60)]] / 44100.0
        cs = np.full(1001, -1.0)
        for k, lag in enumerate(range(-500, 501)):
            j = int(pred * 100) + lag
            if 0 <= j and j + W <= Ee.shape[1]:
                cs[k] = (w * z(Ee[:, j:j + W])).mean()
        k = int(cs.argmax())
        m = cs.copy()
        m[max(0, k - 20):k + 21] = -1
        out.append((t0, pred, (int(pred * 100) + k - 500) / 100, cs[k], m.max()))
    return np.array(out)


def pitch_scale(h, e):
    """The frequency scale that best lays e's spectrum on h's (1.0 = same pitch)."""
    f, Ph = signal.welch(h, 48000, nperseg=16384)
    _, Pe = signal.welch(e, 48000, nperseg=16384)
    lf = np.linspace(np.log(200), np.log(8000), 8000)
    a = np.log(np.interp(lf, np.log(f[1:]), Ph[1:]) + 1e-12)
    b = np.log(np.interp(lf, np.log(f[1:]), Pe[1:]) + 1e-12)
    a, b = a - a.mean(), b - b.mean()
    step = lf[1] - lf[0]
    best = (-2.0, 1.0)
    for s in range(-60, 61):            # +-60 steps of 0.046%: +-2.8%
        x, y = (a[s:], b[:len(b) - s]) if s >= 0 else (a[:s], b[-s:])
        c = float((x * y).sum() / np.sqrt((x * x).sum() * (y * y).sum()))
        if c > best[0]:
            best = (c, float(np.exp(s * step)))
    return best[1], best[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--video', required=True)
    ap.add_argument('--exe', required=True)
    ap.add_argument('--rom', default=os.path.join(os.environ.get('ROMS_DIR', ''), 'sfight.zip'))
    ap.add_argument('--work')
    ap.add_argument('--secs', type=float, default=112.0)
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--emu-args', default='', help="more m2hle options (e.g. '--i960-timing table')")
    ap.add_argument('--reuse', action='store_true', help="grade --work's last recording again (rec.npz)")
    args = ap.parse_args()
    work = args.work or tempfile.mkdtemp(prefix='grade-hw-video-')
    os.makedirs(work, exist_ok=True)
    fails = []

    def grade(name, ok, text):
        print(f'  {"PASS" if ok else "FAIL"}  {name:9s} {text}')
        if not ok:
            fails.append(name)

    try:
        hw, hpcm = decode_capture(args.video, work)
        rec = os.path.join(work, 'rec.npz')
        if args.reuse and os.path.exists(rec):
            z = np.load(rec)
            frames, samples, pics, epcm = z['frames'], z['samples'], z['pics'], z['pcm']
        else:
            frames, samples, pics, epcm = record(args, work)
            if args.keep or args.work:
                np.savez(rec, frames=frames, samples=samples, pics=pics, pcm=epcm)
        epcm48 = signal.resample_poly(epcm, 160, 147).astype(np.float32)
        hw_secs = len(hw) / 60.0
        print(f'capture {len(hw)} frames ({hw_secs:.1f} s); m2hle {len(frames)} frames '
              f'{frames[0]}..{frames[-1]}, {samples[-1] / max(1, frames[-1]):.2f} samples a frame')

        path, cost = align(hw, pics)
        t = np.arange(len(path)) / 60.0
        live = t < hw_secs - 6                     # the capture's last seconds are the winner's fade
        grade('pictures', np.median(cost[live]) < LIMITS['pictures'],
              f'median residual {np.median(cost[live]):.3f} (luma-normalised; limit {LIMITS["pictures"]})')

        # The capture's repeats: a frame no different from the one before, between
        # two that move. Still scenes and 30 Hz animation hide some and fake a
        # few, so the rate comes from the spacing of the repeats, not their count.
        d = np.abs(np.diff(hw.astype(np.int16), axis=0)).mean((1, 2))
        i = np.arange(1, len(d) - 1)
        rep = i[(d[i - 1] > 1) & (d[i + 1] > 1) & (d[i] < 0.3 * np.minimum(d[i - 1], d[i + 1])) & (d[i] < 0.3)]
        period = 60.0 / (60.0 - BOARD_HZ)
        span = n_rep = 0
        for g in np.diff(rep):
            n = round(g / period)
            if n >= 1 and abs(g - n * period) < 3:
                span += g
                n_rep += n
        shown = 60.0 - 60.0 * n_rep / span if span else 0.0
        grade('rate', abs(shown - BOARD_HZ) < LIMITS['shown'],
              f'the capture repeats a frame every {span / max(1, n_rep):.2f}: the board shows '
              f'{shown:.2f} Hz (m2hle runs {BOARD_HZ:.3f})')
        ef = frames[path] / BOARD_HZ
        m = (t > FIGHT[0]) & (t < FIGHT[1])
        slope = float(np.polyfit(t[m], ef[m], 1)[0])
        grade('rate', abs(slope - 1) < LIMITS['rate'],
              f'replayed fight: m2hle board frames / the board\'s {slope:.4f}')

        off = (ef - t) * BOARD_HZ
        off -= off[9 * 60]
        print('  info  loads     m2hle frames ahead of the board (-) since 9 s: ' +
              ' '.join(f'{s}s:{off[s * 60]:+.0f}' for s in range(15, int(hw_secs) - 6, 6)))

        al = audio_match(hpcm, epcm48, samples, path, hw_secs)
        good = al[:, 3] - al[:, 4] > 0.1
        for a, b in SEGMENTS:
            k = good & (al[:, 0] >= a) & (al[:, 0] <= b)
            if k.sum() < 4:
                grade('tempo', False, f'{a}-{b} s: too few matches ({k.sum()})')
                continue
            sl = float(np.polyfit(al[k, 0], al[k, 2], 1)[0])
            grade('tempo', abs(sl - 1) < LIMITS['tempo'],
                  f'{a}-{b} s: emu/hw audio time {sl:.4f} ({(1 / sl - 1) * 100:+.2f}% tempo; n={k.sum()})')
        # Over a still picture (the title) any of its frames matches, so where
        # its sound is says nothing: only windows that move count.
        moves = np.array([np.median(d[int(s * 60):int(s * 60) + 240]) > 0.5 for s in al[:, 0]])
        k = good & moves & (al[:, 0] >= 13) & (al[:, 0] <= FIGHT[1])
        av = al[k, 2] - al[k, 1]
        if os.environ.get('GRADE_HW_DEBUG'):
            for r in al:
                print(f'    hw {r[0]:5.1f}  a-v {r[2] - r[1]:+.2f}  corr {r[3]:.2f}/{r[4]:.2f}')
        grade('sync', np.abs(av).max() < LIMITS['sync'],
              f'sound against its frame {av.min():+.2f}..{av.max():+.2f} s over 13-{FIGHT[1]} s, '
              f'{k.sum()} moving windows '
              f'(drift {np.polyfit(al[k, 0], av, 1)[0] * 1000:+.1f} ms/s)')

        h = hpcm[13 * 48000:51 * 48000]
        e0 = int(al[np.argmin(np.abs(al[:, 0] - 13)), 2] * 48000)
        sc, c = pitch_scale(h, epcm48[e0:e0 + len(h)])
        grade('pitch', abs(sc - 1) < LIMITS['pitch'], f'spectrum scale {sc:.4f} (corr {c:.3f}), 13-51 s')
    finally:
        if not args.keep and not args.work:
            shutil.rmtree(work, ignore_errors=True)
    print('PASS' if not fails else 'FAIL: ' + ', '.join(sorted(set(fails))))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
