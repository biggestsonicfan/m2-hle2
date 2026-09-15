"""Grade a sound-board capture against MAME's.

Both sides are in tools/mame/snd-capture.lua's format: MAME's from snd_capture.py,
ours from tests/snd_replay.c (MAME's MIDI stream replayed through board/sound.h
— the i960 taken out of it) or from the emulator's capture_snd bridge command.
Record timestamps are 68000 clock periods on every side (11.2896 MHz).

  python snd_compare.py <mame-prefix> <our-prefix> [seconds] [anchor-hex]

Lines the two up on the MIDI command `anchor` (default a00001ae1010, the attract
music start) and reports, over `seconds` of music:
  - key-on / key-off counts and how long voices are held
  - how many events match in order before the first difference (slot choice
    included — a race decides the first one, so read it as a horizon)
  - notes: the share of MAME's key-ons (sample address, pitch, level) that ours
    plays within 30 ms, and the timing error of those
  - voices keyed, every half second
  - with <prefix>.wav on both sides: loudness per 5 s, envelope correlation and
    band energy (DC removed; MAME's WAV is 48 kHz, the replay's 44.1 kHz)
"""
import bisect
import struct
import sys

import numpy as np

CLK = 11289600.0
A, B = sys.argv[1], sys.argv[2]
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 70.0
ANCHOR = bytes.fromhex(sys.argv[4] if len(sys.argv) > 4 else 'a00001ae1010')
TOL = 0.030


def load(p):
    a = np.fromfile(p + '.bin', dtype='<u4')
    return a[:len(a) // 4 * 4].reshape(-1, 4)


def anchor_time(a):
    tag, off = a[:, 0] >> 24, a[:, 0] & 0xffffff
    idx = np.nonzero((tag == 1) & (off == 0x9c0000))[0]
    data = bytes(int(a[i, 1]) & 0xff for i in idx)
    k = data.find(ANCHOR)
    if k < 0:
        raise SystemExit(f'anchor {ANCHOR.hex()} not in the MIDI stream')
    return a[idx[k + len(ANCHOR) - 1], 2] / CLK


def events(a, t0):
    """Replay the slot register writes; key-ons and key-offs at each KYONEX strobe."""
    reg = np.zeros((32, 0x20), dtype=np.int64)
    keyed, on_t, out = [False] * 32, [0.0] * 32, []
    t = a[:, 2] / CLK
    tag, off = a[:, 0] >> 24, a[:, 0] & 0xffffff
    for i in np.nonzero((tag == 2) & (off < 0x400) & (t >= t0 - 1) & (t < t0 + SECS))[0]:
        o, d = int(off[i]), int(a[i, 1])
        mask, v = d >> 16, d & 0xffff
        s, r = o >> 5, o & 0x1e
        reg[s, r] = (reg[s, r] & ~mask) | (v & mask)
        if r or not reg[s, 0] & 0x1000:
            continue
        tt = float(t[i]) - t0
        for ss in range(32):
            w0 = int(reg[ss, 0])
            if (w0 >> 11) & 1 and not keyed[ss]:
                keyed[ss], on_t[ss] = True, tt
                pw = int(reg[ss, 0x10])
                oct_ = (pw >> 11) & 0xf
                out.append(dict(t=tt, kind='on', slot=ss, sa=((w0 & 0xf) << 16) | int(reg[ss, 2]),
                                oct=oct_ - 16 if oct_ & 8 else oct_, fns=pw & 0x3ff, tl=int(reg[ss, 0xc]) & 0xff))
            elif not (w0 >> 11) & 1 and keyed[ss]:
                keyed[ss] = False
                out.append(dict(t=tt, kind='off', slot=ss, held=tt - on_t[ss]))
        reg[s, 0] &= ~0x1000
    return [e for e in out if e['t'] >= 0]


def sig(e):
    return (e['kind'], e['slot'], e.get('sa'), e.get('oct'), e.get('fns'), e.get('tl'))


def keyed_timeline(ev, step=0.5):
    k, out, nxt = 0, [], 0.0
    for e in ev:
        while e['t'] >= nxt:
            out.append(k)
            nxt += step
        k += 1 if e['kind'] == 'on' else -1
    return out


ea, eb = load(A), load(B)
EA, EB = events(ea, anchor_time(ea)), events(eb, anchor_time(eb))
for name, ev in (('mame', EA), ('ours', EB)):
    held = [e['held'] for e in ev if e['kind'] == 'off']
    print(f"{name}: {sum(e['kind'] == 'on' for e in ev)} key-ons, {len(held)} key-offs in {SECS:.0f} s; "
          f"held median {np.median(held):.3f} s, p90 {np.percentile(held, 90):.3f} s")

sa_, sb_ = [sig(e) for e in EA], [sig(e) for e in EB]
k = 0
while k < min(len(sa_), len(sb_)) and sa_[k] == sb_[k]:
    k += 1
print(f'events identical in order: {k} (to {EA[k - 1]["t"] if k else 0:.2f} s)')

notes_b = {}
for e in EB:
    if e['kind'] == 'on':
        notes_b.setdefault((e['sa'], e['oct'], e['fns'], e['tl']), []).append(e['t'])
used = {key: [False] * len(v) for key, v in notes_b.items()}
hit, errs, total = 0, [], 0
for e in EA:
    if e['kind'] != 'on':
        continue
    total += 1
    key = (e['sa'], e['oct'], e['fns'], e['tl'])
    ts = notes_b.get(key)
    if not ts:
        continue
    j, best = bisect.bisect_left(ts, e['t'] - TOL), None
    while j < len(ts) and ts[j] <= e['t'] + TOL:
        if not used[key][j] and (best is None or abs(ts[j] - e['t']) < abs(ts[best] - e['t'])):
            best = j
        j += 1
    if best is not None:
        used[key][best] = True
        hit += 1
        errs.append(abs(ts[best] - e['t']))
print(f'notes: {hit}/{total} ({100 * hit / max(1, total):.1f}%) within {TOL * 1000:.0f} ms; '
      f'timing error median {np.median(errs) * 1000:.1f} ms, p95 {np.percentile(errs, 95) * 1000:.1f} ms')
print('voices keyed every 0.5 s, mame:', keyed_timeline(EA)[:40])
print('                          ours:', keyed_timeline(EB)[:40])


def readwav(p):
    try:
        d = open(p, 'rb').read()
    except OSError:
        return None, None
    rate, ch = struct.unpack_from('<I', d, 24)[0], struct.unpack_from('<H', d, 22)[0]
    n = (len(d) - 44) // (2 * ch)
    return rate, np.frombuffer(d[44:44 + n * 2 * ch], dtype='<i2').reshape(-1, ch).astype(np.float64) / 32768.0


ra, wa = readwav(A + '.wav')
rb, wb = readwav(B + '.wav')
if wa is None or wb is None:
    sys.exit()
W = 0.02


def env(rate, x):
    n = int(rate * W)
    k = len(x) // n
    blk = x[:k * n].reshape(k, n, x.shape[1])
    blk = blk - blk.mean(axis=1, keepdims=True)
    return np.sqrt((blk ** 2).mean(axis=(1, 2)))


xa, xb = env(ra, wa), env(rb, wb)
L = min(len(xa), len(xb))
best = max(((np.corrcoef(xa[max(0, g):L + min(0, g)], xb[max(0, -g):L - max(0, g)])[0, 1], g) for g in range(-50, 51)))
c, lag = best
xa, xb = xa[max(0, lag):L + min(0, lag)], xb[max(0, -lag):L - max(0, lag)]
print(f'audio: envelope correlation {c:.3f} (ours {lag * W * 1000:+.0f} ms from mame)')
for s0 in range(0, int(len(xa) * W) - 4, 5):
    lo, hi = int(s0 / W), int((s0 + 5) / W)
    a5, b5 = xa[lo:hi], xb[lo:hi]
    if a5.mean() < 1e-4:
        continue
    print(f'  {s0:3d}-{s0 + 5:3d} s  loudness ours/mame {b5.mean() / a5.mean():.2f}  correlation {np.corrcoef(a5, b5)[0, 1]:.3f}')
