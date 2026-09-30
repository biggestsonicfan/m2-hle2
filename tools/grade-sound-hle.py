#!/usr/bin/env python3
"""grade-sound-hle.py -- hold the sound driver in C (board/sound_hle.h) against
the sound driver on the 68000 (the board), input by input.

Both run the same MIDI stream through tests/snd_replay (the board as is, and
with SND_HLE=1), which writes the SCSP register writes, the driver's RAM and
the audio of each. The HLE is not bit-exact by design -- the 68000's main loop
is not there to spread the work out in time -- so this grades what matters:

  notes    every key-on the board makes, matched by what it plays (pitch,
           level, pan, release, LFO: the slot registers at the key-on, not the
           slot), within 30 ms and 250 ms; the timing error of the matches
  holds    how long voices stay keyed (median, p90), each side
  audio    per 10 s window: loudness ratio, 5 ms envelope correlation at the
           best lag within +-30 ms, and band energies (dB, HLE against board)
  state    driver RAM (tracks, sequencers, controllers, globals: the same
           addresses on both sides) that differs for more than a second, outside
           the windows where the driver restarts (MIDI byte 0xFF)

Usage:
  python tools/grade-sound-hle.py <snd_replay> <out-dir> <input-prefix>... [--seconds S]
  inputs: tools/snd_stimuli.py's (bgm, sfx, sys, fuzz) or a MAME capture
  $ROMDIR as snd_replay wants it (sfight.zip and schamp.zip)

numpy only. Nothing it writes belongs in the repo.
"""
import argparse
import collections
import os
import subprocess
import sys

import numpy as np

CLK = 11289600.0
RATE = 44100
MARK = RATE * 1000 // 57524             # snd_replay's RAM dump interval, samples


def run(replay, inp, out, seconds, hle):
    env = dict(os.environ)
    if hle:
        env['SND_HLE'] = '1'
    else:
        env.pop('SND_HLE', None)
    r = subprocess.run([replay, inp, out, str(seconds)], env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'{replay} failed on {inp}:\n{r.stdout}{r.stderr}')
    if hle and 'driver not known' in r.stdout:
        sys.exit('the HLE did not take this program ROM: ' + r.stdout)


def records(prefix):
    a = np.fromfile(prefix + '.bin', dtype='<u4')
    return a[:len(a) // 4 * 4].reshape(-1, 4)


def keyons(prefix):
    """(time, what it plays) of each key-on, and (key-on time, held) of each note."""
    a = records(prefix)
    tag, off = a[:, 0] >> 24, a[:, 0] & 0xFFFFFF
    reg = np.zeros((32, 16), dtype=np.int64)
    on = [None] * 32
    ons, holds = [], []
    for i in np.nonzero((tag == 2) & (off < 0x400))[0]:
        o, d = int(off[i]), int(a[i, 1])
        mask, v = d >> 16, d & 0xFFFF
        s, w = o >> 5, (o & 0x1F) >> 1
        reg[s, w] = (reg[s, w] & ~mask) | (v & mask)
        if w or not reg[s, 0] & 0x1000:
            continue
        t = a[i, 2] / CLK
        for k in range(32):
            if reg[k, 0] & 0x800 and on[k] is None:
                on[k] = t
                ons.append((t, (int(reg[k, 8]) & 0x7FFF, int(reg[k, 6]) & 0xFF, int(reg[k, 0xB]) >> 8,
                                int(reg[k, 5]) & 0x1F, int(reg[k, 9]))))
            elif not reg[k, 0] & 0x800 and on[k] is not None:
                holds.append(t - on[k])
                on[k] = None
        reg[s, 0] &= ~0x1000
    return ons, holds


def match(a, b, tol):
    byk = collections.defaultdict(list)
    for j, (t, k) in enumerate(b):
        byk[k].append((t, j))
    used, err = set(), []
    for t, k in a:
        best = None
        for tb, j in byk.get(k, ()):
            if j not in used and abs(tb - t) <= tol and (best is None or abs(tb - t) < abs(best[0] - t)):
                best = (tb, j)
        if best:
            used.add(best[1])
            err.append(best[0] - t)
    return np.array(err) * 1000


def wav(prefix):
    x = np.fromfile(prefix + '.wav', dtype='<i2')[22:].astype(np.float64)
    return x[:len(x) // 2 * 2].reshape(-1, 2).mean(1)


BANDS = [(0, 250), (250, 1000), (1000, 4000), (4000, 8000), (8000, 16000)]


def audio(a, b, win=10.0):
    n = min(len(a), len(b))
    hop = RATE // 200

    def env(x):
        m = len(x) // hop * hop
        return np.sqrt((x[:m].reshape(-1, hop) ** 2).mean(1))

    def bands(x):
        f = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
        fr = np.fft.rfftfreq(len(x), 1 / RATE)
        return np.array([f[(fr >= lo) & (fr < hi)].sum() for lo, hi in BANDS])

    rows = []
    for t0 in np.arange(0, n / RATE - win + 1e-9, win):
        s = slice(int(t0 * RATE), int((t0 + win) * RATE))
        x, y = a[s], b[s]
        if np.sqrt((x ** 2).mean()) < 30:
            continue
        ea, eb = env(x), env(y)
        best = -2.0
        for lag in range(-6, 7):
            u = ea[max(0, lag):len(ea) + min(0, lag)]
            v = eb[max(0, -lag):len(eb) + min(0, -lag)]
            best = max(best, np.corrcoef(u, v)[0, 1])
        rows.append((np.sqrt((y ** 2).mean() / (x ** 2).mean()), best,
                     10 * np.log10((bands(y) + 1e-9) / (bands(x) + 1e-9))))
    return rows


def restarts(inp):
    a = records(inp)
    tag = a[:, 0] >> 24
    return [a[i, 2] / CLK for i in np.nonzero(tag == 1)[0] if (a[i, 1] & 0xFF) == 0xFF]


def state(pa, pb, skip):
    A = np.fromfile(pa + '.ram.bin', dtype=np.uint8).reshape(-1, 0x4000)
    B = np.fromfile(pb + '.ram.bin', dtype=np.uint8).reshape(-1, 0x4000)
    n, hold = min(len(A), len(B)), int(1.0 * 57.524)
    # what carries meaning: not the queue's indices or the note set-up scratch
    # (0x2400-0x243F), which move with any timing difference, nor a
    # sequencer's countdown and position (+2..+7), which are its phase. The
    # HLE applies an event's effects when it takes it, where the board may be
    # busy with it for a while (a song's preload holds its track set-up 0.3 s),
    # so a difference counts only when it lasts a second.
    regions = {'globals': [0x141E, 0x141F, 0x1420, 0x1421, 0x142E, 0x142F, 0x1430, 0x1431, 0x1440, 0x1441],
               'sequencers': [0x2000 + s * 16 + b for s in range(8) for b in range(16) if not 2 <= b <= 7],
               'tracks': list(range(0x3000, 0x3140)),
               'controllers': list(range(0x3200, 0x3250)) + list(range(0x3270, 0x3290))}   # not SC08's TL save
    out = []
    for name, cols in regions.items():
        cols = np.array(cols)
        D = A[:n][:, cols] != B[:n][:, cols]
        for j, col_addr in enumerate(cols):
            col, k = D[:, j], 0
            while k < n:
                if not col[k]:
                    k += 1
                    continue
                e = k
                while e < n and col[e]:
                    e += 1
                t = k * MARK / RATE
                if e - k >= hold and not any(s - 0.1 <= t <= s + 3.1 for s in skip) and t > 3.2:
                    out.append(f'{name} 0x{0x1000 + col_addr:04X} {t:.2f}-{e * MARK / RATE:.2f} s: '
                               f'board {A[k, col_addr]:02X} HLE {B[k, col_addr]:02X}')
                k = e
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('replay')
    ap.add_argument('out')
    ap.add_argument('inputs', nargs='+')
    ap.add_argument('--seconds', type=float, default=120)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for inp in args.inputs:
        name = os.path.basename(inp)
        pa, pb = os.path.join(args.out, name + '.board'), os.path.join(args.out, name + '.hle')
        run(args.replay, inp, pa, args.seconds, False)
        run(args.replay, inp, pb, args.seconds, True)
        (oa, ha), (ob, hb) = keyons(pa), keyons(pb)
        e30, e250 = match(oa, ob, 0.030), match(oa, ob, 0.250)
        print(f'== {name}: {len(oa)} key-ons on the board, {len(ob)} in C')
        print(f'   notes: {100 * len(e30) / max(1, len(oa)):.1f}% within 30 ms, '
              f'{100 * len(e250) / max(1, len(oa)):.1f}% within 250 ms; '
              f'timing (C - board) median {np.median(e250):+.2f} ms, p95 |err| {np.percentile(abs(e250), 95):.1f} ms')
        print(f'   holds: board median {np.median(ha):.3f} s p90 {np.percentile(ha, 90):.3f} s; '
              f'C median {np.median(hb):.3f} s p90 {np.percentile(hb, 90):.3f} s')
        rows = audio(wav(pa), wav(pb))
        if rows:
            loud = np.array([r[0] for r in rows]); corr = np.array([r[1] for r in rows])
            db = np.array([r[2] for r in rows])
            print(f'   audio: {len(rows)} windows; loudness C/board {loud.min():.3f}-{loud.max():.3f}; '
                  f'envelope correlation median {np.median(corr):.3f}, worst {corr.min():.3f}; '
                  'bands (dB) ' + ' '.join(f'{np.median(db[:, i]):+.2f}' for i in range(len(BANDS))))
        diffs = state(pa, pb, restarts(inp))
        print(f'   state: {len(diffs)} driver RAM differences lasting over a second')
        for d in diffs[:8]:
            print('     ' + d)


if __name__ == '__main__':
    main()
