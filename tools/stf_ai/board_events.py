#!/usr/bin/env python3
"""Split the board's frame by STF event and hold m2-hle2's to it (Pinboard #1126).

  board_events.py CAPTURE TRACE [--stretch LO:HI] [--all]

The rng-events probe (stfdisasm rng-events-1126) sends the rng-serial packet
(5A A5, 28 bytes) and, when the frame has room, the game's own event profile
after it: 5A A7 and 30 x 3 bytes. event_loop re-arms TIMER_02 before each event
and stores what it has counted at 0x50C010 + 0x20 * event once it returns: the
event's own time, the "now" column of the game's event debug screen. The frame
then splits into

  vblank->main_loop   TIMER_03 - TIMERS_START at the probe (the interrupt
                      handler, the end of interrupt_wait_b, bookkeep_all_clock,
                      variable_diff_calc)
  main_loop other     TIMERS_START - TIMER_02 - events 0..28: main_loop's calls
                      up to event_loop and event_loop's own step per event
  fa_<event>          the event's word (an idle one is the call and its ret)
  tail                TIMER_02 - fa_pol_test's word: set_end_mark,
                      test_sw_chk, unp_send_tex_para, interrupt_wait_b to _idle

which add up to the frame's busy time (TIMER_03 at the probe). TIMER_02 at the
probe (the rand window) is fa_pol_test plus the tail.

On m2-hle2 and MAME the timers don't burst, so event_loop's ldl TIMER_02 reads
TIMER_02 twice. If the board's words come out rising and as large as the time
from the vblank, its ldl read TIMER_03 into the second word instead: the
script says so and splits on the differences.

CAPTURE is the board's raw serial capture (serial_capture.py). TRACE is
det_digest --ai-trace of the same EPROMs (its T lines are the bytes the probe
sent), run from a folder holding sfight.zip and sfight/epr-19001.15 + .16:

  det_digest sfight.zip --frames 6000 --region japan --profile sfight \\
      --i960-timing board --spin-i960 --texload-i960 --ai-trace ai.csv --out dd.txt

Both sides are read from the probe's own packets, so the probe's lag is the
same on both and cancels. Frames pair on frame_counter: the board's runs 0x280
ahead of det_digest's (its warning screen).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_trace as A  # noqa: E402

FC_LAG = 0x280
BASE_LEN, EV_N = 28, 30
EV_LEN = 2 + 3 * EV_N
EVENTS = ('nameentry', 'sel_disp', 'key_play', 'play', 'win0', 'win1', 'enemy0', 'enemy1',
          'control0', 'record', 'rob0', 'rob1', 'tobi', 'coli', 'key_record', 'sampling',
          'game_info', 'object0', 'camera', 'user', 'game_disp', 'effect', 'sound',
          'pol_string', 'stage_efc', 'bunri', 'kill_osage', 'osage0', 'osage1', 'pol_test')
SEGS = ('vblank->main_loop', 'main_loop other') + tuple('fa_' + e for e in EVENTS) + ('tail',)


def le(b, o, w):
    return sum(b[o + j] << (8 * j) for j in range(w))


def elapsed(v):
    return 0xFFFFF - (v & 0xFFFFF)


def frames(stream):
    """{frame_counter: [TIMERS_START, TIMER_02, TIMER_03 elapsed, words or None]}.
    A frame's event section follows its own base packet."""
    out, i, cur = {}, 0, None
    while i + 2 <= len(stream):
        if stream[i] == 0x5A and stream[i + 1] == 0xA5 and i + BASE_LEN <= len(stream):
            b = stream[i:i + BASE_LEN]
            cur = le(b, 2, 4)
            if cur not in out:
                out[cur] = [elapsed(le(b, 10, 3)), elapsed(le(b, 13, 3)), elapsed(le(b, 16, 3)), None]
            else:
                cur = None
            i += BASE_LEN
        elif stream[i] == 0x5A and stream[i + 1] == 0xA7 and i + EV_LEN <= len(stream):
            if cur is not None and out[cur][3] is None:
                out[cur][3] = [le(stream, i + 2 + 3 * k, 3) for k in range(EV_N)]
            cur = None
            i += EV_LEN
        else:
            i += 1
    return out


def trace_stream(path):
    return [int(line.split(',')[2], 16) for line in open(path) if line.startswith('T,')]


def cumulative(fs):
    """True when the words are TIMER_03 stamps (rising, past the main_loop start)
    rather than each event's own time."""
    rows = [f for f in fs.values() if f[3]]
    if not rows:
        return False
    hits = sum(1 for ts, t02, t03, w in rows
               if all(b >= a for a, b in zip(w, w[1:])) and w[0] >= t03 - ts)
    return hits > len(rows) // 2


def segments(f, cum):
    ts, t02, t03, w = f
    if w is None:
        return None
    if cum:
        if any(b < a for a, b in zip(w, w[1:])) or w[-1] > t03:
            return None
        pre = t03 - ts
        ev = [w[0] - pre] + [b - a for a, b in zip(w, w[1:])]
        return [pre, 0] + ev + [t03 - w[-1]]
    tail = t02 - w[-1]
    other = ts - t02 - sum(w[:-1])
    if tail < 0 or other < 0:
        return None
    return [t03 - ts, other] + w + [tail]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('capture')
    ap.add_argument('trace')
    ap.add_argument('--stretch', default='381:C80', help='board frame_counter range, hex (attract to the fight)')
    ap.add_argument('--all', action='store_true', help='every segment, not only those over 100 counts')
    a = ap.parse_args()
    lo, hi = (int(x, 16) for x in a.stretch.split(':'))
    board = frames(A.capture_bytes(a.capture))
    emu = {fc + FC_LAG: v for fc, v in frames(trace_stream(a.trace)).items()}
    cb, ce = cumulative(board), cumulative(emu)
    print(f'board: {len(board)} frames, {sum(1 for v in board.values() if v[3])} with the event profile; '
          f'm2-hle2: {len(emu)} frames, {sum(1 for v in emu.values() if v[3])} with it')
    for name, c in (('board', cb), ('m2-hle2', ce)):
        if c:
            print(f'{name}: the event words rise from the vblank: its ldl TIMER_02 read TIMER_03 '
                  f'into the second word (the timers burst); split on the differences')
    rows = []
    for fc in sorted(set(board) & set(emu)):
        if not lo <= fc <= hi:
            continue
        sb, se = segments(board[fc], cb), segments(emu[fc], ce)
        if sb and se:
            rows.append((fc, board[fc], emu[fc], sb, se))
    if not rows:
        sys.exit(f'no frame in {a.stretch} has the event profile on both sides')
    n = len(rows)
    busy_b = sum(r[1][2] for r in rows) / n
    busy_e = sum(r[2][2] for r in rows) / n
    gap = busy_e - busy_b
    print(f'frames {lo:X}..{hi:X} paired with both profiles: {n}')
    print(f'busy (TIMER_03 at the probe): board {busy_b:9.0f}  m2-hle2 {busy_e:9.0f}  '
          f'{gap:+8.0f} ({busy_e / busy_b - 1:+.2%})')
    t2b = sum(r[1][1] for r in rows) / n
    t2e = sum(r[2][1] for r in rows) / n
    print(f'TIMER_02 window: board {t2b:9.0f}  m2-hle2 {t2e:9.0f}  {t2e - t2b:+8.0f} ({t2e / t2b - 1:+.2%})')
    print(f'\n{"segment":<20} {"board":>9} {"m2-hle2":>9} {"diff":>8} {"diff %":>8} {"of gap":>7} {"active":>6}')
    for k, name in enumerate(SEGS):
        b = sum(r[3][k] for r in rows) / n
        e = sum(r[4][k] for r in rows) / n
        if not a.all and max(b, e) < 100:
            continue
        act = sum(1 for r in rows if r[3][k] >= 100)
        pct = f'{e / b - 1:+.2%}' if b else '-'
        share = f'{(e - b) / gap:+.0%}' if gap else '-'
        print(f'{name:<20} {b:9.0f} {e:9.0f} {e - b:+8.0f} {pct:>8} {share:>7} {act:6d}')


if __name__ == '__main__':
    main()
