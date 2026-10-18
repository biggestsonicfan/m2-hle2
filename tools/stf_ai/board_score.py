#!/usr/bin/env python3
"""Score m2-hle2's i960 timing frame by frame against a real board's
rng-serial capture (Pinboard #1119).

  board_score.py CAPTURE TRACE [TRACE ...]

CAPTURE is the board's raw rng-serial capture (uploads/603-rng-capture.bin).
TRACE is det_digest --ai-trace of the same EPROMs (canary model2/stf-rng-serial
as sfight/epr-19001.15 and .16 beside sfight.zip, run from that folder):

  det_digest sfight.zip --frames 6000 --region japan --profile sfight \
      --i960-timing board --spin-i960 --texload-i960 --ai-trace ai.csv --out dd.txt

Each frame's TIMERS_START count at the probe (counts since the vblank re-armed
it, so the frame's busy time) is paired with the board's for the same game
frame: the board's frame_counter runs 0x280 ahead (its warning screen). Unlike
board_capture.py's medians over the whole run, this holds the model to the
board frame for frame, by stretch.
"""
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_trace as A  # noqa: E402

FC_LAG = 0x280
STRETCHES = ((0x381, 0xC80, 'attract to the fight'), (0xCA0, 0x1080, 'fight'))


def elapsed(v, lag=0):
    return 0xFFFFF - ((v - lag) & 0xFFFFF)


def corr(xs, ys):
    mx, my = st.mean(xs), st.mean(ys)
    sx = sum((x - mx) ** 2 for x in xs) ** .5
    sy = sum((y - my) ** 2 for y in ys) ** .5
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sx / sy


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    board = {}
    for p in A.packets(A.capture_bytes(sys.argv[1])):
        board.setdefault(p[1], p[3])
    for path in sys.argv[2:]:
        emu = {}
        for k, _, v in A.parse(path):
            if k == 'I':
                emu.setdefault(v[5], v)
        rows = sorted((fc + FC_LAG, elapsed(board[fc + FC_LAG][0]), elapsed(v[0], A.PROBE_LAG[0]),
                       elapsed(board[fc + FC_LAG][1]), elapsed(v[1], A.PROBE_LAG[1]))
                      for fc, v in emu.items() if fc + FC_LAG in board)
        print(f'{path}: {len(rows)} frames paired')
        for lo, hi, name in STRETCHES:
            r = [x for x in rows if lo <= x[0] <= hi]
            if len(r) < 2:
                continue
            b, e = [x[1] for x in r], [x[2] for x in r]
            print(f'  {name:<22} frames {lo:X}..{hi:X} ({len(r)}): board {st.mean(b):9.0f}'
                  f'  m2-hle2 {st.mean(e):9.0f}  {st.mean(e) - st.mean(b):+7.0f}'
                  f' ({sum(e) / sum(b) - 1:+.2%})  corr {corr(b, e):.4f}')
        t2 = [(x[3], x[4]) for x in rows if x[3]]
        print(f'  TIMER_02 window        board {st.median(a for a, _ in t2):9.0f}'
              f'  m2-hle2 {st.median(c for _, c in t2):9.0f} (medians)')


if __name__ == '__main__':
    main()
