#!/usr/bin/env python3
"""Read the rng-bench probe's benchmark packets (stfdisasm rng-bench, Pinboard
#607) off a real board's RS-422 capture, an m2-hle2 --ai-trace of the same
ROM, or both, and say what each benchmark costs in timer counts against the
cycles m2-hle2's i960 table charges for it.

  board_bench.py [--capture CAPTURE] [--trace TRACE]

Packet, after the 28-byte RNG packet: 5A A6, slot, then TIMER_04 before the
bench less TIMER_04 after it (3 bytes, little-endian). Slot = frame_counter
& 63 (& 31 before the probe had 32 benches; the first 16 did not change):
bench slot >> 1, run n times for an even slot and 2n for an odd one.
The two sizes share every fixed cost, so (count at 2n - count at n) / n is one
turn of the loop alone, and count at n - n x turn is what is left over: the
second timer read, the loop's entry and exit, and the first pass's cache
misses. In m2-hle2 a count is exactly 4 + n x the table's turn (5 for the
jumps, which branch into their aligned loop after the first read).
"""
import argparse
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_trace as A  # noqa: E402

# name, n, table cycles a turn (m2-hle2 i960_cycle_table), what it is
BENCHES = (
    ('reg', 64, 12, '7 addo + subo + cmpobne, in the instruction cache'),
    ('ram ld', 32, 37, '8 ld from work RAM 0x500100, in order'),
    ('rom ld', 32, 37, '8 ld from program ROM'),
    ('ram st', 64, 21, '8 st to the stack, in order'),
    ('ram ldq', 64, 19, '2 ldq from work RAM'),
    ('line', 1, 1024, '4 KB of straight-line addo a pass (cache misses)'),
    ('call', 64, 21, 'call + ret of a leaf'),
    ('mulo', 16, 77, '4 mulo'),
    ('timer ld', 32, 37, '8 ld from the timers 0xF00000'),
    ('ram ldob', 32, 37, '8 ldob from work RAM'),
    ('branch', 64, 13, '8 taken b'),
    ('tile ld', 32, 37, '8 ld from tile RAM 0x1000000'),
    ('ld+use', 48, 25, '4 x (ld, then addo of it)'),
    ('ram stq', 64, 15, '2 stq to the stack'),
    ('far ld', 32, 37, '8 ld from work RAM 512 bytes apart'),
    ('none', 8, 0, 'the two timer reads alone'),
    ('buf ld', 32, 37, '8 ld from bufferram 0x91FF00'),
    ('buf st', 32, 21, '8 st to bufferram 0x91FF00 (what is there)'),
    ('tile st', 8, 21, '8 st to tile RAM 0x100E000 (what is there)'),
    ('cop', 4, 18, 'Fn_get_3d_len through the COP FIFO and its reply, used'),
    ('cop x2', 4, 30, 'two Fn_get_3d_len back to back, then both replies'),
    ('cop stat', 16, 37, '8 ld of the COP FIFO status 0x980004'),
    ('jump', 4, 36, '32 jumps to line starts 128 bytes apart (4 KB)'),
    ('jump end', 4, 36, 'the same to the last word of each line'),
    ('jump in', 64, 12, '8 jumps 32 bytes apart over 256 bytes (cached)'),
    ('st+3', 32, 45, '8 x (st to the stack, 3 addo)'),
    ('ld+1+use', 48, 25, '4 x (ld, ld, addo, addo): a use one later'),
    ('line 436', 8, 112, '436 bytes of straight-line addo a pass'),
    ('line 1012', 2, 256, '1012 bytes of straight-line addo a pass'),
    ('buf stq', 32, 15, '2 stq to bufferram 0x91FF00 (what is there)'),
    ('tile stq', 8, 15, '2 stq to tile RAM 0x100E000 (what is there)'),
    ('none', 8, 0, 'the two timer reads alone, again'),
)


# Benches that branch into their aligned loop between the two timer reads
# count one cycle more in m2-hle2.
ENTRY_B = ('jump', 'jump end', 'jump in')


def bench_packets(stream):
    """Yield (slot, count) for every 5A A6 packet in the byte stream."""
    i = 0
    while i + 6 <= len(stream):
        if stream[i] == 0x5A and stream[i + 1] == 0xA5:
            i += A.PKT_LEN
        elif stream[i] == 0x5A and stream[i + 1] == 0xA6 and stream[i + 2] < 2 * len(BENCHES):
            yield stream[i + 2], stream[i + 3] | stream[i + 4] << 8 | stream[i + 5] << 16
            i += 6
        else:
            i += 1


def by_slot(stream):
    res = {}
    for slot, count in bench_packets(stream):
        res.setdefault(slot, []).append(count)
    return res


def fit(slots, b):
    """Median count at n and 2n, the turn and the rest, for bench b."""
    lo, hi = slots.get(2 * b), slots.get(2 * b + 1)
    if not lo or not hi:
        return None
    n = BENCHES[b][1]
    m1, m2 = statistics.median(lo), statistics.median(hi)
    turn = (m2 - m1) / n
    spread = max(max(lo) - min(lo), max(hi) - min(hi))
    return m1, m2, turn, m1 - n * turn, spread, len(lo) + len(hi)


def report(name, slots, out=sys.stdout):
    print(f'{name}: {sum(len(v) for v in slots.values())} bench packets', file=out)
    print(f'  {"bench":9} {"n":>4} {"table":>5} {"at n":>9} {"at 2n":>9} {"turn":>9}'
          f' {"x table":>7} {"rest":>7} {"spread":>6} {"pkts":>5}', file=out)
    for b, (bn, n, cyc, _) in enumerate(BENCHES):
        f = fit(slots, b)
        if f is None:
            print(f'  {bn:9} {n:4d} {cyc:5d}  (no packets at both sizes)', file=out)
            continue
        m1, m2, turn, rest, spread, k = f
        ratio = f'{turn / cyc:7.3f}' if cyc else '      -'
        print(f'  {bn:9} {n:4d} {cyc:5d} {m1:9.1f} {m2:9.1f} {turn:9.2f} {ratio}'
              f' {rest:7.1f} {spread:6d} {k:5d}', file=out)


def check_trace(slots, out=sys.stdout):
    """m2-hle2 must count exactly 4 + n x table (+ 1 for ENTRY_B) for every packet."""
    bad = 0
    for slot, counts in sorted(slots.items()):
        bn, n, cyc, _ = BENCHES[slot >> 1]
        want = 4 + (n << (slot & 1)) * cyc + (bn in ENTRY_B)
        for c in counts:
            if c != want:
                bad += 1
                if bad <= 5:
                    print(f'  slot {slot} ({bn}): count {c}, the table says {want}', file=out)
    print(f'  trace: {"every count is the table" if not bad else f"{bad} counts off the table"}',
          file=out)
    return bad == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--capture', help="raw byte capture off a board's RS-422 link")
    ap.add_argument('--trace', help='det_digest --ai-trace of the rng-bench build')
    a = ap.parse_args()
    if not a.capture and not a.trace:
        ap.error('give --capture, --trace or both')
    ok = True
    if a.trace:
        ev = A.parse(a.trace)
        slots = by_slot([v[0] for k, _, v in ev if k == 'T'])
        report('m2-hle2', slots)
        ok = check_trace(slots)
    if a.capture:
        report('board', by_slot(A.capture_bytes(a.capture)))
    for b, (bn, _, _, what) in enumerate(BENCHES):
        print(f'  {b:2d} {bn:9} {what}')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
