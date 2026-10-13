#!/usr/bin/env python3
"""The per-command SHARC costs for src/board/cop_timing.h, from a
tools/mame/cop-timing.lua capture (Pinboard #710).

  python3 cop_cost_table.py CAPTURE [--tools DIR] [--cpres1 PATH] > rows.inc

It needs cop_timing.py (tools/mame, Pinboard #607) beside it or in --tools.

Every SHARC event in the capture (taking a command word, taking an argument,
pushing a reply) is charged the work the SHARC did since its previous event,
as cop_timing.sharc_work measures it. Per command this prints the mean of
each kind, in MAME board counts x 16 (the median runs 8% short of the
measured waits, the mean within 1%, on a capture it was not taken from):
  a1 / an   the first argument, each later one
  r1 / rn   the first reply, each later one
  tail      the work before the SHARC takes the next command word
  buf       bufferram words the firmware touched (bufr + bufw), the mean
cop_timing.h scales the work by the board's 1.15 and adds 6.5 counts a
bufferram word, as cop_timing.py's replay does.

Fn_calc_coli_flag (0x3B) and Fn_parts_oidasi (0x77) take time by the
bufferram words they touch, and Fn_osage (0x4A) by the records it answers:
their rows carry the fit (busy against words, buf against replies) instead.
Fn_zanzou_reserve (0x80) is a stream: its r1 is a record's answer, and
cop_timing.h works out the terminator's from the copies it lays.
"""
import argparse
import os
import sys
from collections import defaultdict

TQ = 16
# Ops whose time follows their bufferram words: r1 = c0 + c1 * words.
BY_WORDS = (0x3B, 0x77)
STREAM = 0x80


def mean(v):
    return sum(v) / len(v) if v else 0.0


def events(sh, work):
    """(op, kind, work) per SHARC event; kind T charges a command word's work
    to the command before it."""
    cur, na, nr, held = None, 0, 0, []
    for (kind, _t, op, _x), wk in zip(sh, work):
        if kind == 1:
            if cur is not None:
                # A stream's last answer is its terminator's, not a record's.
                for w in held[:-1]:
                    yield cur, 'r1', w
                yield cur, 'tail', wk
            cur, na, nr, held = op, 0, 0, []
        elif cur is None:
            continue
        elif kind == 2:
            yield cur, 'a1' if na == 0 else 'an', wk
            na += 1
        elif cur == STREAM:
            held.append(wk)
        else:
            yield cur, 'r1' if nr == 0 else 'rn', wk
            nr += 1


def first_reply_work(sh, work):
    """The work before each command's first reply, in commands()'s order
    (None for a command that answers nothing)."""
    out, cur, r1 = [], False, None
    for (kind, _t, _op, _x), wk in zip(sh, work):
        if kind == 1:
            if cur:
                out.append(r1)
            cur, r1 = True, None
        elif cur and kind == 3 and r1 is None:
            r1 = wk
    return out


def fit_line(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if not sxx:
        return my, 0.0
    c1 = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return my - c1 * mx, c1


def row(op, kinds, cs, names):
    """One cop_cost_t initialiser: mean work per event kind, in ticks."""
    k = {kind: round(mean(v) * TQ) for kind, v in kinds.items()}
    buf = round(mean([c['bufr'] + c['bufw'] for c in cs])) if cs else 0
    extra = ''
    if op in BY_WORDS and len(cs) > 2:
        cs = [c for c in cs if c['r1'] is not None]
        c0, c1 = fit_line([c['bufr'] + c['bufw'] for c in cs], [c['r1'] for c in cs])
        k['r1'] = round(c0 * TQ)
        extra = f', .per_word = {round(c1 * TQ)}'
        buf = 0
    if op == 0x4A and len(cs) > 2:
        b0, b1 = fit_line([len(c['outs']) for c in cs], [c['bufr'] + c['bufw'] for c in cs])
        extra = f', .buf_reply = {round(b1 * TQ)}'
        buf = max(0, round(b0))
    return (f'    [0x{op:02X}] = {{ {k.get("a1", 0):6d}, {k.get("an", 0):5d}, {k.get("r1", 0):6d}, '
            f'{k.get("rn", 0):5d}, {k.get("tail", 0):6d}, {int(buf):4d}{extra} }},   '
            f'/* {names.get(op, "?")}, {len(cs)} */')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--tools', default=os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument('--cpres1', default=os.path.expanduser('~/source/repos/ai/stf-sharc/cpres1.asm'))
    a = ap.parse_args()
    sys.path.insert(0, a.tools)
    import cop_timing as ct

    recs = ct.load(a.capture)
    _i9, sh, _frames, geo = ct.walk(recs)
    shift = ct.write_shift(recs)
    work = ct.sharc_work(recs, sh, shift)
    del recs
    names = ct.op_names(a.cpres1) if os.path.exists(a.cpres1) else {}
    per = defaultdict(lambda: defaultdict(list))
    for op, kind, wk in events(sh, work):
        per[op][kind].append(wk)
    cmds = ct.commands(sh, work, geo, float('-inf'), float('inf'))
    by = defaultdict(list)
    for c, r1 in zip(cmds, first_reply_work(sh, work)):
        c['r1'] = r1
        by[c['op']].append(c)

    print(f'/* generated by tools/mame/cop_cost_table.py from {os.path.basename(a.capture)}: '
          f'{len(cmds)} commands */')
    for op in sorted(per):
        print(row(op, per[op], by.get(op, []), names))

if __name__ == '__main__':
    main()
