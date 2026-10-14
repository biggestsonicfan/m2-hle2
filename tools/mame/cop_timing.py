#!/usr/bin/env python3
"""How long each COP command keeps the SHARC busy under MAME, how long the
i960 waits for it there, and how long it would wait on the board, from a
tools/mame/cop-timing.lua capture.

  python3 cop_timing.py CAPTURE [--fight | --not-fight] [--per-frame]
                        [--i960-scale K,...] [--sharc-scale S,...] [--bufferram B,...]
                        [--cpres1 PATH]

Every time is in board clocks (i960 cycles at 25 MHz; the SHARC runs at 32 MHz,
1.28 of its cycles to one of these).

MAME runs the two CPUs in timeslices, so their clocks do not sort into the
order the FIFOs saw; the capture's record order is that order. Walking it:
  - an i960 read that finds the reply FIFO empty returns 0 and MAME runs the
    same load again (i960_stall), so a run of reads ending in a good one is a
    stall, and its length is the i960's wait;
  - a write that leaves more than 16 words in the command FIFO halts the i960
    until the SHARC takes one;
  - everything else between two i960 FIFO accesses is the i960 working, and
    between two SHARC FIFO accesses the SHARC working (less any time it sat
    waiting for the word it read).

m2-hle2 answers every command at once. To say what the board loses, the walk
is replayed with the i960's work scaled by K (the board's i960 runs at ~2.7x
the cycle table) and the SHARC's by S, plus B counts for every bufferram word
it touches, under the same FIFO rules. The board's 0x3B and 0x70 round trips
(6,546 and 4,166 counts against MAME's 4,090 and 3,070, with 280 and 96
bufferram words) give S = 1.15, B = 6.5.

Per op: busy = SHARC work from its command word to its next one, reply =
last input to last reply word, bufr/bufw = bufferram words the SHARC read and
wrote, then the i960's stall on the op's replies a frame (MAME, each replay).
"""
import argparse
import bisect
import os
import re
import struct
from collections import defaultdict

REC = struct.Struct('<BdII')
FIFO_DEPTH = 16


def load(path):
    data = open(path, 'rb').read()
    return list(REC.iter_unpack(data[:len(data) - len(data) % REC.size]))


def op_names(path):
    names = {}
    if path and os.path.exists(path):
        for line in open(path, errors='replace'):
            m = re.search(r'/\*\s*\[0x([0-9a-f]+)\]\s+(\S+)', line)
            if m:
                names[int(m.group(1), 16)] = m.group(2)
    return names


class I960Walk:
    """The i960's writes and good reads: (kind, work before, stall, frame)."""

    def __init__(self):
        self.ev = []
        self.last_t = None
        self.fail_t = None      # first failed read of the current stall
        self.fail_gap = 0.0     # work before that read
        self.stall = 0.0        # stall already counted for the current read
        self.halt_end = None    # the SHARC pop that freed a halted write

    def access(self, kind, t, frame, empty):
        gap = 0.0 if self.last_t is None else t - self.last_t
        if self.fail_t is not None:
            if kind == 5:
                self.last_t = t
                if empty:
                    return False
                self.ev.append(('r', self.fail_gap, self.stall + t - self.fail_t, frame))
                self.fail_t, self.stall = None, 0.0
                return True
            # an interrupt between the retries: its accesses are work
            self.stall += self.last_t - self.fail_t
            self.fail_t = None
        if self.halt_end is not None:
            held = min(gap, max(0.0, self.halt_end - self.last_t))
            self.halt_end = None
            kd, wk, st, fr = self.ev[-1]
            self.ev[-1] = (kd, wk, st + held, fr)
            gap -= held
        self.last_t = t
        if kind == 5 and empty:
            self.fail_t, self.fail_gap = t, gap
            return False
        self.ev.append(('r' if kind == 5 else 'w', gap, 0.0, frame))
        return True


def walk(recs):
    """i960 events, SHARC events (kind, t, op, extra), frames, and the geometry
    SHARC's (list, polygon ROM) reads since the previous command word, one per
    command word."""
    cpu = I960Walk()
    sh, frames, geo = [], [], []
    gl = gp = 0
    in_q = out_q = 0
    halted = False
    op = None
    for kind, t, w, x in recs:
        if kind == 6:
            frames.append((t, w, x >> 8))
        elif kind == 7:
            gl, gp = gl + w, gp + x
        elif kind in (1, 2):
            if kind == 1:
                op = w & 0xff
                geo.append((gl, gp))
                gl = gp = 0
            sh.append((kind, t, op, x))
            in_q -= 1
            if halted:
                cpu.halt_end, halted = t, False
        elif kind == 3:
            sh.append((3, t, op, 0))
            out_q += 1
        else:
            if cpu.access(kind, t, len(frames) - 1, out_q == 0) and kind == 5:
                out_q -= 1
            if kind == 4:
                in_q += 1
                halted = in_q > FIFO_DEPTH
    return cpu.ev, sh, frames, geo


def write_shift(recs):
    """SHARC input word n came from i960 write n + shift: the capture can open
    with words already queued. Found by value over the first 200 words."""
    ins = [r[2] for r in recs if r[0] in (1, 2)][:200]
    wv = [r[2] for r in recs if r[0] == 4][:200 + FIFO_DEPTH + 2]
    for shift in range(0, -FIFO_DEPTH - 2, -1):
        hits = sum(1 for k, v in enumerate(ins) if 0 <= k + shift < len(wv) and wv[k + shift] == v)
        if hits >= len(ins) + shift - 5:
            return shift
    raise SystemExit('cop_timing: could not line the i960 writes up with the SHARC reads')


def sharc_work(recs, sh, shift):
    """How long the SHARC worked before each of its events: from its previous
    event or from when the word it read was written, whichever came later."""
    wt = [r[1] for r in recs if r[0] == 4]
    work, prev, n = [], None, 0
    for kind, t, op, x in sh:
        start = prev if prev is not None else t
        if kind != 3:
            j = n + shift
            if 0 <= j < len(wt):
                start = max(start, wt[j])
            n += 1
        work.append(max(0.0, t - start))
        prev = t
    return work


def bufferram_cost(sh, per_word):
    """Extra board time per SHARC event for the bufferram words its command
    touched (counted between command words): charged to the command's first
    reply, or to the next command word if it has none."""
    extra = [0.0] * len(sh)
    if not per_word:
        return extra
    cmd_at, owed = None, False
    for i, (kind, t, op, x) in enumerate(sh):
        if kind == 1:
            if cmd_at is not None:
                words = (x >> 16) + (x & 0xffff)
                extra[owed if owed is not False else i] += words * per_word
            cmd_at, owed = i, False
        elif kind == 3 and owed is False:
            owed = i
    return extra


class Sharc:
    """The SHARC side of a replay: runs its events as their inputs arrive."""

    def __init__(self, sh, work, shift, s):
        self.sh, self.work, self.shift, self.s = sh, work, shift, s
        self.p, self.t = 0, 0.0
        self.wt, self.pops, self.outs, self.out_op = [], [], [], []

    def advance(self, done):
        while not done() and self.p < len(self.sh):
            kind, _, op, _ = self.sh[self.p]
            if kind != 3:
                j = len(self.pops) + self.shift
                if j >= len(self.wt):
                    return
                self.t = max(self.t, self.wt[j] if j >= 0 else 0.0)
            self.t += self.work[self.p] * self.s
            if kind == 3:
                self.outs.append(self.t)
                self.out_op.append(op)
            else:
                self.pops.append(self.t)
            self.p += 1


def replay(i9, sh, work, shift, k, s, extra):
    """The walk again with the i960's work x k and the SHARC's x s, plus its
    bufferram time. Returns the i960's stall per frame and per (frame, op)."""
    cop = Sharc(sh, [w * s + e for w, e in zip(work, extra)], shift, 1.0)
    t, nr = 0.0, 0
    per_frame, per_op = defaultdict(float), defaultdict(float)
    for kind, gap, _stall, frame in i9:
        t += gap * k
        if kind == 'w':
            need = len(cop.wt) - FIFO_DEPTH - shift     # the input that must be gone first
            if need >= 0:
                cop.advance(lambda: len(cop.pops) > need)
                if need < len(cop.pops):
                    t = max(t, cop.pops[need])
            cop.wt.append(t)
            continue
        cop.advance(lambda: len(cop.outs) > nr)
        if nr >= len(cop.outs):
            raise SystemExit('cop_timing: the replay ran out of SHARC replies')
        wait = max(0.0, cop.outs[nr] - t)
        t += wait
        per_frame[frame] += wait
        per_op[frame, cop.out_op[nr]] += wait
        nr += 1
    return per_frame, per_op


def mame_waits(i9, sh):
    """MAME's own stalls, per frame and per (frame, op owning the reply)."""
    out_op = [op for kind, t, op, x in sh if kind == 3]
    per_frame, per_op = defaultdict(float), defaultdict(float)
    nr = 0
    for kind, gap, stall, frame in i9:
        per_frame[frame] += stall
        if kind == 'r':
            if nr < len(out_op):
                per_op[frame, out_op[nr]] += stall
            nr += 1
    return per_frame, per_op


def commands(sh, work, geo, lo, hi):
    """Per command: op, input and output times, SHARC busy, bufferram words,
    and the geometry SHARC's display-list reads while it ran."""
    cmds, cur, n = [], None, 0
    for (kind, t, op, x), wk in zip(sh, work):
        if kind == 1:
            if cur is not None:
                cur['busy'] += wk       # the handler ran until the SHARC read this
                cur['bufr'], cur['bufw'] = x >> 16, x & 0xffff
                cur['geol'] = geo[n][0] if n < len(geo) else 0
                cmds.append(cur)
            n += 1
            cur = {'op': op, 't': t, 'ins': [t], 'outs': [], 'busy': 0.0}
            continue
        if cur is None:
            continue
        cur['busy'] += wk
        (cur['outs'] if kind == 3 else cur['ins']).append(t)
    return [c for c in cmds if lo <= c['t'] < hi]


def med(v):
    v = sorted(v)
    return v[len(v) // 2] if v else None


def per_op_table(cmds, names, nframes, waits):
    by = defaultdict(list)
    for c in cmds:
        by[c['op']].append(c)
    head = ' '.join(f'{lbl:>9}' for lbl, _ in waits)
    print(f'{"op":>4} {"name":<26} {"calls/fr":>8} {"in":>3} {"out":>3} {"busy":>6} {"reply":>6} '
          f'{"bufr":>5} {"bufw":>5} {"geo":>5} {head}')
    for op, cs in sorted(by.items(), key=lambda kv: -sum(c['busy'] for c in kv[1])):
        reply = med([c['outs'][-1] - c['ins'][-1] for c in cs if c['outs']])
        ws = ' '.join(f'{w.get(op, 0.0) / nframes:9.0f}' for _, w in waits)
        print(f'{op:#04x} {names.get(op, "?")[:26]:<26} {len(cs) / nframes:8.2f} {len(cs[0]["ins"]) - 1:3d} '
              f'{len(cs[0]["outs"]):3d} {med([c["busy"] for c in cs]):6.0f} {reply or 0:6.0f} '
              f'{med([c["bufr"] for c in cs]):5d} {med([c["bufw"] for c in cs]):5d} '
              f'{sum(c["geol"] for c in cs) / len(cs):5.1f} {ws}')


def floats(text):
    return [float(v) for v in text.split(',')]


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('capture')
    ap.add_argument('--cpres1', default=os.path.expanduser('~/source/repos/ai/stf-sharc/cpres1.asm'))
    ap.add_argument('--fight', action='store_true', help='only frames whose also_sub_mode is 0x0A (a fight)')
    ap.add_argument('--not-fight', action='store_true', help='only frames whose also_sub_mode is not 0x0A')
    ap.add_argument('--per-frame', action='store_true')
    ap.add_argument('--i960-scale', type=floats, default=[2.7])
    ap.add_argument('--sharc-scale', type=floats, default=[1.0, 1.15])
    ap.add_argument('--bufferram', type=floats, default=[0.0, 6.5],
                    help='board counts a SHARC bufferram word costs on top of MAME (6.5 with S 1.15 fits '
                         'the board\'s 0x3B and 0x70 round trips)')
    return ap.parse_args()


def all_waits(a, i9, sh, work, shift):
    """MAME's own wait, then one replay per (B, K, S)."""
    waits = [('MAME', mame_waits(i9, sh))]
    for b in a.bufferram:
        extra = bufferram_cost(sh, b)
        for k in a.i960_scale:
            for s in a.sharc_scale:
                waits.append((f'k{k:g}s{s:g}b{b:g}', replay(i9, sh, work, shift, k, s, extra)))
    return waits


def op_totals(waits, sel):
    out = []
    for lbl, (pf, pop) in waits:
        tot = defaultdict(float)
        for (f, op), v in pop.items():
            if f in sel:
                tot[op] += v
        out.append((lbl, tot))
    return out


def wait_summary(waits, frames, keep):
    print('i960 wait a frame (median / mean / max):')
    for lbl, (pf, pop) in waits:
        w = [pf.get(f, 0.0) for f in keep]
        print(f'  {lbl:>14}: {med(w):8.0f} {sum(w) / len(w):8.0f} {max(w):8.0f}')
    modes = defaultdict(list)
    for f in keep:
        modes[frames[f][2]].append(f)
    print('median wait a frame by also_sub_mode:')
    print(f'{"sub":>5} {"frames":>6} ' + ' '.join(f'{lbl:>14}' for lbl, _ in waits))
    for sub, fs in sorted(modes.items()):
        print(f'{sub:#5x} {len(fs):6d} ' + ' '.join(f'{med([pf.get(f, 0.0) for f in fs]):14.0f}'
                                               for _, (pf, pop) in waits))


def geo_frames(recs, frames, keep):
    """The geometry SHARC's display-list and polygon ROM words a frame."""
    starts = [fr[0] for fr in frames]
    rows = defaultdict(lambda: [0, 0])
    for kind, t, w, x in recs:
        if kind == 7:
            r = rows[bisect.bisect_right(starts, t) - 1]
            r[0] += w
            r[1] += x
    if not rows:
        print('no geometry SHARC records (a capture from before record 7)')
        return
    modes = defaultdict(list)
    for f in keep:
        modes[frames[f][2]].append(rows[f])
    print('geometry SHARC a frame (median): display-list words, polygon ROM words')
    for sub, rs in sorted(modes.items()):
        print(f'{sub:#5x} {len(rs):6d} {med([r[0] for r in rs]):8d} {med([r[1] for r in rs]):8d}')


def kept_frames(a, frames):
    def wanted(sub):
        return not (a.fight and sub != 0x0A) and not (a.not_fight and sub == 0x0A)
    keep = [f for f in range(len(frames) - 1) if wanted(frames[f][2])]
    if not keep:
        raise SystemExit('cop_timing: no frames')
    return keep


def print_per_frame(waits, frames, keep):
    print('frame   sub ' + ' '.join(f'{lbl:>14}' for lbl, _ in waits))
    for f in keep:
        print(f'{frames[f][1]:#06x} {frames[f][2]:#04x} ' +
              ' '.join(f'{pf.get(f, 0.0):14.0f}' for _, (pf, pop) in waits))


def main():
    a = parse_args()
    recs = load(a.capture)
    i9, sh, frames, geo = walk(recs)
    shift = write_shift(recs)
    work = sharc_work(recs, sh, shift)
    keep = kept_frames(a, frames)
    waits = all_waits(a, i9, sh, work, shift)
    sel = set(keep)
    starts = [fr[0] for fr in frames]
    cmds = [c for c in commands(sh, work, geo, starts[0], starts[-1])
            if bisect.bisect_right(starts, c['t']) - 1 in sel]
    print(f'{len(keep)} frames (fc {frames[keep[0]][1]:#x}..{frames[keep[-1]][1]:#x}), {len(cmds)} commands, '
          f'shift {shift}')
    work_fr = defaultdict(float)
    for kind, gap, stall, frame in i9:
        work_fr[frame] += gap
    print(f'MAME i960 work between COP accesses: median {med([work_fr[f] for f in keep]):.0f} a frame')
    per_op_table(cmds, op_names(a.cpres1), len(keep), op_totals(waits, sel))
    wait_summary(waits, frames, keep)
    geo_frames(recs, frames, keep)
    if a.per_frame:
        print_per_frame(waits, frames, keep)

if __name__ == '__main__':
    main()
