#!/usr/bin/env python3
"""wav_compare.py -- hold snd_lockstep's WAVs against a reference (MAME's).

    python tools/scsp_mednafen/wav_compare.py <ref.wav> <a.wav> [<b.wav> ...] [--seconds N]

Each WAV is compared with the reference after four steps: both are aligned
by cross-correlating their 50 ms loudness envelopes (MAME's WAV and a replay
need not start on the same sample), both have their own DC removed (the
board's output carries ~5000 of it, MAME's too), each is scaled by the one
gain that best maps it onto the reference, and then per 5 s:

  env corr   correlation of the 50 ms loudness envelopes (the figure
             tools/README.md quotes for the board against MAME: 0.992)
  gain dB    the RMS level against the reference
  residual   what the best gain leaves, as a share of the reference

Stereo is averaged to mono. numpy only.
"""
import struct
import sys

import numpy as np

RATE = 44100
ENV = RATE // 20                        # 50 ms


def load(path):
    with open(path, "rb") as f:
        d = f.read()
    i = d.find(b"data")
    ch = struct.unpack_from("<H", d, 22)[0]
    x = np.frombuffer(d[i + 8:], dtype=np.int16).astype(np.float64)
    return x.reshape(-1, ch).mean(axis=1)


def env(x):
    n = len(x) // ENV
    b = x[:n * ENV].reshape(n, ENV)
    return np.sqrt(((b - b.mean(axis=1, keepdims=True)) ** 2).mean(axis=1))


def align(ref, x, max_s=5.0):
    """Samples to drop from the start of x (negative: from ref)."""
    er, ex = env(ref), env(x)
    n = min(len(er), len(ex))
    er, ex = er[:n] - er[:n].mean(), ex[:n] - ex[:n].mean()
    m = int(max_s * RATE / ENV)
    best, lag = -2.0, 0
    for k in range(-m, m + 1):
        a, b = (er[:n - k], ex[k:]) if k >= 0 else (er[-k:], ex[:n + k])
        if len(a) < 20 or a.std() == 0 or b.std() == 0:
            continue
        c = float(np.corrcoef(a, b)[0, 1])
        if c > best:
            best, lag = c, k
    return lag * ENV


def main(argv):
    secs = None
    if "--seconds" in argv:
        k = argv.index("--seconds")
        secs = float(argv[k + 1])
        del argv[k:k + 2]
    if len(argv) < 2:
        print(__doc__)
        return 2
    ref = load(argv[0])
    for path in argv[1:]:
        x = load(path)
        lag = align(ref, x)
        r, y = (ref, x[lag:]) if lag >= 0 else (ref[-lag:], x)
        n = min(len(r), len(y))
        if secs:
            n = min(n, int(secs * RATE))
        r, y = r[:n], y[:n]
        print("%s against %s: aligned by %+d samples (%+.3f s)" % (path, argv[0], lag, lag / RATE))
        print("%8s %9s %8s %9s" % ("seconds", "env corr", "gain dB", "residual"))
        w = 5 * RATE
        for s in range(0, n - w + 1, w):
            a, b = r[s:s + w] - r[s:s + w].mean(), y[s:s + w] - y[s:s + w].mean()
            if a.std() == 0 or b.std() == 0:
                continue
            ea, eb = env(a), env(b)
            ec = float(np.corrcoef(ea, eb)[0, 1]) if ea.std() and eb.std() else 0.0
            g = float(np.dot(a, b) / np.dot(b, b))           # b scaled onto a
            res = float(np.linalg.norm(a - g * b) / np.linalg.norm(a))
            level = 20 * np.log10(b.std() / a.std())          # RMS, not g: g is ~0 once the timing drifts
            print("%8d %9.3f %+8.1f %9.3f" % ((s + w) // RATE, ec, level, res))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
