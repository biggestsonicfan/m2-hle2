#!/usr/bin/env python3
"""Read the rng-bench probe's benchmark packets (stfdisasm rng-bench, Pinboard
#607) off a real board's RS-422 capture, an m2-hle2 --ai-trace of the same
ROM, or both, and say what each benchmark costs in timer counts against the
cycles m2-hle2's i960 table charges for it.

  board_bench.py [--probe 611|613|634|656|670|754|766|773] [--capture CAPTURE] [--trace TRACE]

Packet: 5A A6, slot, then TIMER_04 before the bench less TIMER_04 after it
(3 bytes, little-endian). Up to rng-bench 9ed4ef2 it followed the 28-byte RNG
packet (5A A5 ...); from 03e8189 (Pinboard #610) it is all the probe sends. Slot = frame_counter
& 127 from Pinboard #611's second probe (& 63 with 32 benches, & 31 with 16;
the benches already there never changed):
bench slot >> 1, run n times for an even slot and 2n for an odd one.
The two sizes share every fixed cost, so (count at 2n - count at n) / n is one
turn of the loop alone, and count at n - n x turn is what is left over: the
second timer read, the loop's entry and exit, and the first pass's cache
misses. In m2-hle2 a count is exactly 4 + n x the table's turn (5 for the
jumps and the cache-way benches, which branch into their aligned loop after
the first read).

--probe 613 reads Pinboard #613's probe instead: eight benches, slot =
frame_counter & 15 (PROBE_613). Its COP benches run once and twice, and its
tile benches only on frames whose TIMER_03 says the store lands in the
active picture or in the vblank; a frame that fails its bench's gate sends
nothing. Benches that share a name are pooled.

--probe 656 is #634's probe with bench 4 reading 0x802008 only (no GEO
stores).

--probe 670 is the same table: the COP benches' TIMER_03 limit went from
300,000 to 385,000 counts, because the board's attract fight reaches the
probe 370,000-420,000 counts past the vblank and the 656 capture had no COP
packets.

--probe 634 is the same table on Pinboard #634's build of that probe, which
sends a packet's head (5A A6 slot) before its bench and the count after it,
and runs the COP benches only in a fight. A capture that ends on a head
with no count stopped inside that bench, and the report says which.

--probe 754 is Pinboard #754's: #738's fetch benches (A1, A2, B1-B3) with
#611's jump, line 1012 and branch as controls, on that packet.

--probe 773 is Pinboard #773's: #754's eight benches as they were, slot =
frame_counter & 31, with two more in slots 16-31: C1, landings that run
48 bytes, and C2, a cached landing that runs on into three lines it has
to fetch, with no branch just before them.

--probe 766 is Pinboard #766's wait-bench (stfdisasm wait-bench-766): each
bench writes one word of the wait table at 0xE00000 (ROM, download RAM or
work RAM) for its own length, slot = frame_counter & 63. After the table
the report prints what one wait costs a ROM or RAM access, the DRAM bit's
cost, stores against loads, and code fetched from RAM against ROM.
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
    ('drom ld', 32, 37, '8 ld from data ROM 0x20E0000, in order'),
    ('drom2 ld', 32, 37, '8 ld from data ROM 0x2CE0000, in order'),
    ('drom far', 32, 37, '8 ld from data ROM 512 bytes apart'),
    ('drom 4k', 32, 37, '8 ld from data ROM 4 KB apart'),
    ('drom ldob', 32, 37, '8 ldob from data ROM'),
    ('drom ldq', 64, 19, '2 ldq from data ROM'),
    ('drom use', 48, 25, '4 x (ld from data ROM, then addo of it)'),
    ('rom far', 32, 37, '8 ld from program ROM 512 bytes apart'),
    ('geo ld', 16, 37, '8 ld of the GEO read pointer 0x803008'),
    ('tile far', 32, 37, '8 ld from tile RAM 512 bytes apart'),
    ('ram 4k', 32, 37, '8 ld from work RAM 4 KB apart'),
    ('ram ldl', 64, 25, '4 ldl from work RAM'),
    ('ram ldt', 64, 29, '4 ldt from work RAM'),
    ('ram stl', 64, 17, '4 stl to the stack'),
    ('ram stt', 64, 13, '2 stt to the stack'),
    ('ram ldos', 32, 37, '8 ldos from work RAM'),
    ('ld idx', 32, 37, '8 ld (r6)[r9*4] from work RAM'),
    ('ld abs', 32, 37, '8 ld from absolute work RAM addresses (2 words each)'),
    ('st+ld', 32, 29, '4 x (st to the stack, ld it straight back)'),
    ('ram/drom', 32, 37, '8 ld from work RAM and data ROM in turn'),
    ('assoc', 32, 66, '2 x 128 bytes of code 512 apart (one set if direct-mapped)'),
    ('assoc ctl', 32, 66, 'the same 256 apart'),
    ('assoc 3', 32, 98, '3 x 128 bytes of code 512 apart'),
    ('jump mid', 4, 36, 'the jumps to the third word of each line'),
    ('cobr nt', 32, 37, '8 cmpobe not taken'),
    ('bcc t', 64, 13, '4 x (cmpo, be taken)'),
    ('bcc nt', 64, 13, '4 x (cmpo, bne not taken)'),
    ('buf far', 32, 37, '8 ld from bufferram 0x900000 512 bytes apart'),
    ('drom 16', 32, 37, '8 ld from data ROM 16 bytes apart (a line each)'),
    ('ram 16', 32, 37, '8 ld from work RAM 16 bytes apart'),
    ('none', 8, 0, 'the two timer reads alone, a third time'),
    ('reg', 64, 12, 'the reg bench again'),
)


# Pinboard #613: the fight frame's COP round trips, TIMER_02's GEO stores and
# tile RAM in and out of the vblank, with two controls.
PROBE_613 = (
    ('reg', 64, 12, '7 addo + subo + cmpobne, in the instruction cache'),
    ('ram st', 64, 21, '8 st to the stack, in order'),
    ('cop 70', 1, 112, 'Fn_area_coli round trip: 6 st, 22 ld, use the last'),
    ('cop 3b', 1, 40, 'Fn_calc_coli_flag round trip: 8 st, 4 ld, use the last'),
    ('geo wb', 8, 29, '4 x (ld 0x802008, st 0x801008): the GEO write pointer'),
    ('tile act', 8, 21, '8 st to tile RAM 0x100E000, 45,000+ counts past vblank'),
    ('tile vbl', 8, 21, 'the same inside the vblank (< 24,000 counts)'),
    ('tile vbl', 8, 21, 'the same, its second pair of slots'),
)
# Pinboard #656: bench 4 only reads the GEO's write position. On the board
# 0x802008 is where the GEO has got to, not the start register MAME answers,
# so #613's write-back moved the next list's start and the board stopped.
PROBE_656 = PROBE_613[:4] + (
    ('geo rd', 8, 21, '4 x ld 0x802008: the GEO write position, nothing stored'),
) + PROBE_613[5:]
# Pinboard #754: #738's instruction fetch benches (PR #318 comment
# 6140364322) and three of #611's as controls. Every landing in A1 and A2
# is on a 32-byte boundary of a 4 KB footprint, so it misses; a 16-byte
# fill reads A1's 32 bytes in two fills, A2's 64 in four.
PROBE_754 = (
    ('A1', 8, 259, '32 blocks 128 B apart, 32 B straight each (4 KB)'),
    ('A2', 8, 259, '16 blocks 256 B apart, 64 B straight each (4 KB)'),
    ('B1', 32, 67, '256 B straight: 62 addo + subo + cmpobne'),
    ('B2', 32, 67, 'the same 256 B, a b .+4 every 16 B'),
    ('B3', 32, 52, '15 x (addo, b .+8 over one word, addo), 2 addo'),
    ('jump', 4, 36, "#611's 32 jumps to line starts 128 bytes apart (4 KB)"),
    ('line 1012', 2, 256, "#611's 1012 bytes of straight-line addo a pass"),
    ('branch', 64, 13, "#611's 8 taken b"),
)
# Pinboard #773: #754's benches and two to confirm its reading of A1/A2
# (PR #318 comment 6155175491). C1 is A2 with each landing one 16-byte line
# shorter. C2's head (32 B) runs twice, first landing as a miss and then
# cached, and the second pass falls through into a 48 B tail it fetches.
# Slots 16-31 hold C1 and C2 four times each; the report pools them.
PROBE_773 = PROBE_754 + (
    ('C1', 8, 195, '16 blocks 256 B apart, 48 B straight each (4 KB)'),
    ('C2', 8, 546, '16 blocks 256 B apart: 32 B run twice, then 48 B'),
) * 4
# Pinboard #766: the wait table at 0xE00000. A bench's name says the access,
# the area (rom, wram = work RAM 0x580000, which the game sets 1 wait + DRAM,
# dram = download RAM 0x210000, 1 wait, no DRAM bit) and the waits it runs
# at. The data loops are cached, so only their accesses go to the bus.
PROBE_766 = (
    ('ld rom2', 32, 37, '8 ld from program ROM, 2 waits (the game\'s)'),
    ('ld rom3', 32, 37, 'the same at 3 waits'),
    ('ld rom4', 32, 37, 'the same at 4 waits'),
    ('ldq rom2', 32, 19, '2 ldq from program ROM, 2 waits'),
    ('ldq rom4', 32, 19, 'the same at 4 waits'),
    ('ld wram1', 32, 37, '8 ld from work RAM, 1 wait + DRAM (the game\'s)'),
    ('ld wram2', 32, 37, 'the same at 2 waits + DRAM'),
    ('ld wram3', 32, 37, 'the same at 3 waits + DRAM'),
    ('ldq wram1', 32, 19, '2 ldq from work RAM, 1 wait + DRAM'),
    ('ldq wram3', 32, 19, 'the same at 3 waits'),
    ('st wram1', 32, 21, '8 st to work RAM, 1 wait + DRAM'),
    ('st wram3', 32, 21, 'the same at 3 waits'),
    ('stq wram1', 32, 15, '2 stq to work RAM, 1 wait + DRAM'),
    ('stq wram3', 32, 15, 'the same at 3 waits'),
    ('ld dram1', 32, 37, '8 ld from download RAM, 1 wait, no DRAM bit (the game\'s)'),
    ('ld dram3', 32, 37, 'the same at 3 waits'),
    ('ldq dram1', 32, 19, '2 ldq from download RAM, 1 wait'),
    ('st dram1', 32, 21, '8 st to download RAM, 1 wait'),
    ('st dram3', 32, 21, 'the same at 3 waits'),
    ('stq dram1', 32, 15, '2 stq to download RAM, 1 wait'),
    ('line rom2', 2, 256, "#611's line 1012 in ROM, 2 waits"),
    ('line rom3', 2, 256, 'the same at 3 waits'),
    ('line rom4', 2, 256, 'the same at 4 waits'),
    ('line wram1', 2, 256, 'the same code copied into work RAM, 1 wait + DRAM'),
    ('line wram3', 2, 256, 'in work RAM at 3 waits'),
    ('line dram1', 2, 256, 'copied into download RAM, 1 wait'),
    ('line dram3', 2, 256, 'in download RAM at 3 waits'),
    ('jump rom2', 4, 36, "#611's jumps, ROM at 2 waits"),
    ('jump rom4', 4, 36, 'the same at 4 waits'),
    ('reg rom2', 64, 21, '16 addo + subo + cmpobne, cached; ROM at 2 waits'),
    ('reg rom4', 64, 21, 'the same at 4 waits: must not move'),
    ('st wram2', 32, 21, '8 st to work RAM, 2 waits + DRAM'),
)
# (what, bench at fewer waits, bench at more, accesses a turn, waits apart)
WAITS_766 = (
    ('ld ROM, a wait', 'ld rom2', 'ld rom4', 8, 2),
    ('ld ROM, 2 to 3', 'ld rom2', 'ld rom3', 8, 1),
    ('ldq ROM, a wait', 'ldq rom2', 'ldq rom4', 2, 2),
    ('ld work RAM, a wait', 'ld wram1', 'ld wram3', 8, 2),
    ('ld work RAM, 1 to 2', 'ld wram1', 'ld wram2', 8, 1),
    ('ldq work RAM, a wait', 'ldq wram1', 'ldq wram3', 2, 2),
    ('st work RAM, a wait', 'st wram1', 'st wram3', 8, 2),
    ('st work RAM, 1 to 2', 'st wram1', 'st wram2', 8, 1),
    ('stq work RAM, a wait', 'stq wram1', 'stq wram3', 2, 2),
    ('ld download RAM, a wait', 'ld dram1', 'ld dram3', 8, 2),
    ('st download RAM, a wait', 'st dram1', 'st dram3', 8, 2),
    ('DRAM bit, ld (wram1 - dram1)', 'ld dram1', 'ld wram1', 8, 1),
    ('DRAM bit, ldq', 'ldq dram1', 'ldq wram1', 2, 1),
    ('DRAM bit, st', 'st dram1', 'st wram1', 8, 1),
    ('DRAM bit, stq', 'stq dram1', 'stq wram1', 2, 1),
    ('line in ROM, a wait (a pass)', 'line rom2', 'line rom4', 1, 2),
    ('line in work RAM, a wait', 'line wram1', 'line wram3', 1, 2),
    ('line in download RAM, a wait', 'line dram1', 'line dram3', 1, 2),
    ('line, work RAM 1 against ROM 2', 'line wram1', 'line rom2', 1, 1),
    ('line, download RAM 1 against ROM 2', 'line dram1', 'line rom2', 1, 1),
    ('jump, a ROM wait', 'jump rom2', 'jump rom4', 1, 2),
    ('reg, a ROM wait (cached: 0)', 'reg rom2', 'reg rom4', 1, 2),
)
PROBES = {611: BENCHES, 613: PROBE_613, 634: PROBE_613, 656: PROBE_656,
          670: PROBE_656, 754: PROBE_754, 766: PROBE_766,
          773: PROBE_773}


# Benches that branch into their aligned loop between the two timer reads
# count one cycle more in m2-hle2.
ENTRY_B = ('jump', 'jump end', 'jump in', 'assoc', 'assoc ctl', 'assoc 3',
           'jump mid', 'A1', 'A2', 'B1', 'B2', 'B3', 'jump rom2', 'jump rom4',
           'C1', 'C2')


def bench_packets(stream, benches=BENCHES):
    """Yield (slot, count) for every 5A A6 packet in the byte stream."""
    i = 0
    while i + 6 <= len(stream):
        if stream[i] == 0x5A and stream[i + 1] == 0xA5:
            i += A.PKT_LEN
        elif stream[i] == 0x5A and stream[i + 1] == 0xA6 and stream[i + 2] < 2 * len(benches):
            yield stream[i + 2], stream[i + 3] | stream[i + 4] << 8 | stream[i + 5] << 16
            i += 6
        else:
            i += 1


def trailing_head(stream, benches=BENCHES):
    """The slot of a 5A A6 head the stream ends on with no full count, else None."""
    for i in range(max(0, len(stream) - 5), len(stream) - 2):
        if stream[i] == 0x5A and stream[i + 1] == 0xA6 and stream[i + 2] < 2 * len(benches):
            return stream[i + 2]
    return None


def by_slot(stream, benches=BENCHES):
    res = {}
    for slot, count in bench_packets(stream, benches):
        res.setdefault(slot, []).append(count)
    return res


def pooled(slots, benches):
    """(name, n, cycles, counts at n, counts at 2n) for each bench name."""
    out = {}
    for b, (bn, n, cyc, _) in enumerate(benches):
        row = out.setdefault(bn, (bn, n, cyc, [], []))
        row[3].extend(slots.get(2 * b, ()))
        row[4].extend(slots.get(2 * b + 1, ()))
    return list(out.values())


def fit(n, lo, hi):
    """Median count at n and 2n, the turn and the rest."""
    if not lo or not hi:
        return None
    m1, m2 = statistics.median(lo), statistics.median(hi)
    turn = (m2 - m1) / n
    spread = max(max(lo) - min(lo), max(hi) - min(hi))
    return m1, m2, turn, m1 - n * turn, spread, len(lo) + len(hi)


def report(name, slots, benches=BENCHES, out=sys.stdout):
    print(f'{name}: {sum(len(v) for v in slots.values())} bench packets', file=out)
    print(f'  {"bench":9} {"n":>4} {"table":>5} {"at n":>9} {"at 2n":>9} {"turn":>9}'
          f' {"x table":>7} {"rest":>7} {"spread":>6} {"pkts":>5}', file=out)
    for bn, n, cyc, lo, hi in pooled(slots, benches):
        f = fit(n, lo, hi)
        if f is None:
            print(f'  {bn:9} {n:4d} {cyc:5d}  (no packets at both sizes)', file=out)
            continue
        m1, m2, turn, rest, spread, k = f
        ratio = f'{turn / cyc:7.3f}' if cyc else '      -'
        print(f'  {bn:9} {n:4d} {cyc:5d} {m1:9.1f} {m2:9.1f} {turn:9.2f} {ratio}'
              f' {rest:7.1f} {spread:6d} {k:5d}', file=out)


def waits_report(slots, benches, out=sys.stdout):
    """Pinboard #766: each pair's turn difference per access per wait."""
    turns = {bn: fit(n, lo, hi) for bn, n, _, lo, hi in pooled(slots, benches)}
    print('  per access, counts (line: a pass; jump, reg: a turn):', file=out)
    for what, a, b, acc, w in WAITS_766:
        fa, fb = turns.get(a), turns.get(b)
        if fa is None or fb is None:
            print(f'    {what:36} (no packets)', file=out)
            continue
        print(f'    {what:36} {(fb[2] - fa[2]) / acc / w:8.3f}', file=out)


def check_trace(slots, benches=BENCHES, out=sys.stdout):
    """m2-hle2 must count exactly 4 + n x table (+ 1 for ENTRY_B) for every packet."""
    bad = 0
    for slot, counts in sorted(slots.items()):
        bn, n, cyc, _ = benches[slot >> 1]
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
    ap.add_argument('--probe', type=int, choices=sorted(PROBES), default=611,
                    help="which probe ROM's bench table (default 611)")
    a = ap.parse_args()
    benches = PROBES[a.probe]
    if not a.capture and not a.trace:
        ap.error('give --capture, --trace or both')
    ok = True
    if a.trace:
        ev = A.parse(a.trace)
        slots = by_slot([v[0] for k, _, v in ev if k == 'T'], benches)
        report('m2-hle2', slots, benches)
        if a.probe == 766:
            waits_report(slots, benches)
        ok = check_trace(slots, benches)
    if a.capture:
        cap = A.capture_bytes(a.capture)
        report('board', by_slot(cap, benches), benches)
        if a.probe == 766:
            waits_report(by_slot(cap, benches), benches)
        slot = trailing_head(cap, benches)
        if slot is not None:
            print(f'  the board stopped inside slot {slot} ({benches[slot >> 1][0]}):'
                  ' its head went out and its count never did')
    for b, (bn, _, _, what) in enumerate(benches):
        print(f'  {b:2d} {bn:9} {what}')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
