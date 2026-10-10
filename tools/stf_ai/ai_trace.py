#!/usr/bin/env python3
"""Read an STF AI trace (det_digest --ai-trace, sfight_ai_trace.h) and work out
the random number generator from what it logged, without being told it.

  solve    : find the advance random += sum(Tn << sn) and the answer
             (random >> k) & mask that fit every R line, by search over shifts
  packets  : decode the rng-serial probe's RS-422 packets (T lines, or a raw
             byte capture from a board with --capture) and hold them against
             the trace's own I and R lines
  frames   : per frame, the draws and what each CPU fighter did with them

Packet (stfdisasm rng-serial, all little-endian): 5A A5, frame_counter & 0xFF,
random (4), TIMERS_START, TIMER_02, TIMER_03, TIMER_04 (3 each) = 19 bytes.
The probe loads TIMER_04 16 cycles after _idle's entry and the other three at
28, 32 and 36; a timer counts down a cycle at a time, so on the board each one
reads that much less than the I line's.
"""
import argparse
import itertools
import sys

PKT_LEN = 19
PROBE_LAG = (28, 32, 36, 16)   # cycles from _idle's entry to each timer's load


def parse(path):
    ev = []
    with open(path) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            p = line.rstrip().split(',')
            kind, frame = p[0], int(p[1])
            if kind == 'I':
                vals = [int(x, 16) for x in p[2:6]] + [int(p[6])]
            else:
                vals = [int(x, 16) for x in p[2:]]
            ev.append((kind, frame, vals))
    return ev


def rand_lines(ev):
    # R,frame,site,old,new,value,t0,t1,t2,t3
    return [v for k, _, v in ev if k == 'R']


def advance_fits(v, sh):
    """Does random += sum(Tn << sn) take this R line's old state to its new?"""
    return ((v[1] + sum(t << s for t, s in zip(v[4:8], sh))) & 0xFFFFFFFF) == v[2]


def fit_advance(rs):
    """Every shift set (s0..s3) that fits all the calls: filter on the first
    64, then hold the survivors to the rest."""
    probe = rs[:64]
    cands = [sh for sh in itertools.product(range(24), repeat=4)
             if all(advance_fits(v, sh) for v in probe)]
    return [sh for sh in cands if all(advance_fits(v, sh) for v in rs)]


def fit_answer(rs):
    """Every (k, mask) with value == (new >> k) & mask on all the calls."""
    return [(k, m) for k in range(28) for m in (0xFF, 0xFFFF, 0xFFFFFF, 0xFFFFFFFF)
            if all(((v[2] >> k) & m) == v[3] for v in rs)]


def solve(ev, out=sys.stdout):
    rs = rand_lines(ev)
    if not rs:
        print('solve: no R lines', file=out)
        return None
    good, outs = fit_advance(rs), fit_answer(rs)
    print(f'solve: {len(rs)} calls', file=out)
    for sh in good:
        terms = ' + '.join(f'(T{i} << {s})' for i, s in enumerate(sh))
        print(f'  advance: random += {terms}   (holds for all {len(rs)})', file=out)
    if not good:
        print('  no shift set fits every call: the advance is not a shifted sum', file=out)
    if outs:
        print(f'  answer : (random >> {outs[0][0]}) & 0x{outs[0][1]:X}', file=out)
    else:
        print('  no (random >> k) & mask fits the answers', file=out)
    return good, outs


def capture_bytes(path):
    """A board capture: raw bytes as read off the link. The m2-sdk sends each
    byte as 07 xx; strip the strobes if every other byte is one."""
    data = open(path, 'rb').read()
    if len(data) >= 4 and all(b == 0x07 for b in data[0::2][:64]):
        data = data[1::2]
    return list(data)


def packets(stream):
    """Yield (index, frame, random, [t0, t1, t2, t3]) from a byte stream."""
    i, n = 0, 0
    while i + PKT_LEN <= len(stream):
        if stream[i] != 0x5A or stream[i + 1] != 0xA5:
            i += 1
            continue
        b = stream[i:i + PKT_LEN]
        le = lambda o, w: sum(b[o + j] << (8 * j) for j in range(w))
        yield n, b[2], le(3, 4), [le(7, 3), le(10, 3), le(13, 3), le(16, 3)]
        n += 1
        i += PKT_LEN


def trace_probes(ev):
    """For each I line, the random the probe would read and its frame."""
    rnd, res = 0, []
    for k, frame, v in ev:
        if k == 'S':
            rnd = v[0]
        elif k == 'R':
            rnd = v[2]
        elif k == 'I':
            res.append((frame, rnd, v[:4]))
    return res


def check_packets(ev, stream, out=sys.stdout):
    pk = list(packets(stream))
    tp = trace_probes(ev)
    print(f'packets: {len(pk)} decoded, {len(tp)} idle entries in the trace', file=out)
    # The probe skips an entry when TIMER_04 is close; line them up on random
    # and the frame byte, walking forward.
    j, bad, skipped = 0, 0, 0
    for _, fb, rnd, tim in pk:
        while j < len(tp):
            frame, trnd, ttim = tp[j]
            want = [(t - lag) & 0xFFFFF if t != 0xFFFFF else t for t, lag in zip(ttim, PROBE_LAG)]
            if trnd == rnd and want == tim:
                break
            j += 1
            skipped += 1
        if j == len(tp):
            bad += 1
            if bad <= 5:
                print(f'  no idle entry matches packet frame byte {fb:02X} random {rnd:08X}'
                      f' timers {" ".join(f"{t:05X}" for t in tim)}', file=out)
            j = 0
            continue
        j += 1
    print(f'  {len(pk) - bad} packets match an idle entry exactly (timers and random),'
          f' {skipped} entries had none (probe skipped), {bad} unmatched', file=out)
    return bad == 0


def frames(ev, first, last, out=sys.stdout):
    by = {}
    for k, frame, v in ev:
        if first <= frame <= last and k in 'RA':
            by.setdefault(frame, []).append((k, v))
    for frame in sorted(by):
        draws = [f'{v[0]:X}:{v[3]:04X}' for k, v in by[frame] if k == 'R']
        acts = [f'P{v[0]} waza {v[1]:X} lv {v[2]:X} rnd {v[3]:02X}' for k, v in by[frame] if k == 'A']
        print(f'{frame:6d} draws [{" ".join(draws)}]' + (f'  -> {"; ".join(acts)}' if acts else ''),
              file=out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('trace')
    ap.add_argument('--capture', help='raw byte capture off a board\'s RS-422 link')
    ap.add_argument('--frames', help='FIRST:LAST, print per-frame draws and decisions')
    a = ap.parse_args()
    ev = parse(a.trace)
    solve(ev)
    stream = capture_bytes(a.capture) if a.capture else [v[0] for k, _, v in ev if k == 'T']
    ok = True
    if stream:
        ok = check_packets(ev, stream)
    if a.frames:
        f0, f1 = (int(x) for x in a.frames.split(':'))
        frames(ev, f0, f1)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
