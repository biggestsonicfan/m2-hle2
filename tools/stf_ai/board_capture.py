#!/usr/bin/env python3
"""Report what a real board's rng-serial capture says, field by field, and hold
it against an m2-hle2 --ai-trace of the same ROM (Pinboard #603).

  board_capture.py CAPTURE [TRACE]

CAPTURE is the raw bytes read off the RS-422 link (2,000,000 baud 8-N-1, 07 xx
per byte; ai_trace.capture_bytes strips the strobes). TRACE is a det_digest
--ai-trace of the rng-serial build; without it only the board is reported.

Two boards never match packet for packet, so this compares what can be
compared: the framing, frame_counter's step, the formula's one testable mark on
random (every step is a multiple of 16), when rand runs, and how far each timer
has counted since it was re-armed when the probe reads it.
"""
import os
import statistics
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_trace as A  # noqa: E402

M20 = 1 << 20
TIMERS = ('TIMERS_START', 'TIMER_02', 'TIMER_03', 'TIMER_04')
MAME_FRAME = 25e6 * 656 * 424 / 16e6   # 25 MHz timers, 16 MHz dot clock, 656 x 424
HLE_FRAME = 25e6 / 60


def framing(raw, pk):
    payload = len(raw) // 2
    print(f'framing: {len(raw)} bytes on the wire, {payload} payload, {len(pk)} packets'
          f' x {A.PKT_LEN} = {len(pk) * A.PKT_LEN} ({payload - len(pk) * A.PKT_LEN} bytes outside a packet)')


def frame_steps(pk):
    fcs = [p[1] for p in pk]
    steps = Counter(b - a for a, b in zip(fcs, fcs[1:]))
    print(f'frame_counter: {fcs[0]:X}..{fcs[-1]:X}, steps {sorted(steps.items())}')
    print('  a step of 2 is the probe standing aside for TIMER_04; longer ones are frames'
          ' with no idle wait (loads)')


def frame_period(pk):
    d = [((a[3][3] - b[3][3]) % M20) / (b[1] - a[1])
         for a, b in zip(pk, pk[1:]) if 1 <= b[1] - a[1] <= 2]
    d = [x for x in d if 0.9 * HLE_FRAME < x < 1.2 * HLE_FRAME]
    per = sum(d) / len(d)
    print(f'frame period: TIMER_04 falls {per:.1f} counts a frame over {len(d)} frames'
          f' = {25e6 / per:.4f} Hz at 25 MHz (MAME {MAME_FRAME:.1f}, m2-hle2 {HLE_FRAME:.1f})')


def random_steps(pk):
    ch = [(a, b) for a, b in zip(pk, pk[1:]) if a[2] != b[2]]
    odd = sum(1 for a, b in ch if (b[2] - a[2]) & 0xF)
    first = next((p[1] for p in pk if p[2]), None)
    print(f'random: {len(ch)} changes, {odd} not a multiple of 16; first non-zero at frame'
          f' {first:X}' if first is not None else 'random: never moved')
    return [b[1] for a, b in ch]


def elapsed(values, lag=0):
    return [0xFFFFF - ((v - lag) & 0xFFFFF) for v in values]


def pct(xs, q):
    return sorted(xs)[int(q * (len(xs) - 1))]


def timer_table(pk, idle, lo, hi):
    b = [p for p in pk if lo <= p[1] <= hi]
    e = [v for v in idle if lo <= v[5] <= hi]
    print(f'timers, counts since re-arm at the probe, frames {lo:X}..{hi:X}'
          f' (board {len(b)} packets, m2-hle2 {len(e)}): median [p10 p90]')
    for i, name in enumerate(TIMERS):
        bv = elapsed([p[3][i] for p in b])
        line = f'  {name:<12} board {statistics.median(bv):8.0f} [{pct(bv, .1)} {pct(bv, .9)}]'
        if e:
            ev = elapsed([v[i] for v in e], A.PROBE_LAG[i])
            line += (f'  m2-hle2 {statistics.median(ev):8.0f} [{pct(ev, .1)} {pct(ev, .9)}]'
                     f'  x{statistics.median(bv) / max(1, statistics.median(ev)):.2f}')
        print(line)
    full = Counter(p[3][1] == 0xFFFFF for p in b)
    print(f'  TIMER_02 reads 0xFFFFF in {full[True]} of {len(b)} packets')


def compare_trace(ev, pk, board_rand):
    idle = [v for k, _, v in ev if k == 'I']
    fc_at = {f: v[5] for k, f, v in ev if k == 'I'}
    emu_rand = sorted({fc_at[f] for k, f, _ in ev if k == 'R' and f in fc_at})
    print('rand runs at frame (board):  ' + ' '.join(f'{x:X}' for x in board_rand[:12]))
    print('rand runs at frame (m2-hle2):' + ' '.join(f'{x:X}' for x in emu_rand[:12]))
    print(f'CTRL_TIMER at the first packet: board {pk[0][5]:X}, m2-hle2 {idle[0][7]:X}'
          f' (first non-zero {next((v[7] for v in idle if v[7]), 0):X})')
    lo = max(board_rand[0] if board_rand else 0, emu_rand[0] if emu_rand else 0)
    hi = min(pk[-1][1], idle[-1][5])
    timer_table(pk, idle, lo, hi)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    raw = open(sys.argv[1], 'rb').read()
    pk = list(A.packets(A.capture_bytes(sys.argv[1])))
    if not pk:
        sys.exit('no 5A A5 packet in the capture')
    framing(raw, pk)
    frame_steps(pk)
    frame_period(pk)
    board_rand = random_steps(pk)
    if len(sys.argv) > 2:
        compare_trace(A.parse(sys.argv[2]), pk, board_rand)
    else:
        timer_table(pk, [], board_rand[0] if board_rand else 0, pk[-1][1])


if __name__ == '__main__':
    main()
