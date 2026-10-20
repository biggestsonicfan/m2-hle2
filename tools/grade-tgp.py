#!/usr/bin/env python3
"""grade-tgp.py - Sega Rally's TGP replies against the PS2 port's maths.

The Model 2A TGP runs the game's own program on a CPU core that follows MAME's
mb86233.cpp, so MAME cannot catch a bug in that core. The PS2 port (SLPM-62703)
has the same commands as plain C over floats, written by Sega from the TGP
program. Its maths is held here as a third point: each command the i960 sent
is replayed on a model of the PS2 functions and its replies are compared, within
float rounding, with what our TGP answered (Pinboard #661).

The capture is det_digest's --cop log on srallyc (tests/det_digest.c), which on a
Model 2A board is the TGP's conversation:

    det_digest $ROMS_DIR/srallyc.zip --profile srallyc --frames 3000 \\
        --cop 1:3000:/dev/shm/tgp.log --out /dev/shm/digest.txt
    python3 tools/grade-tgp.py /dev/shm/tgp.log

A command starts where the program takes a word at PC 0x70 or 0x77 (its main
loop), so the split needs no argument table. The PS2 functions each command is
modelled on (SLPM-62703 addresses):

    20 push 001379b0     21 pop 001379f8      25 identity 00137bb0
    27 translate 00137ac0                     28 scale
    29/32 rot X 00137bf8 2A/33 rot Y 00137cb0 2B/34 rot Z 00137d60  (by -angle)
    2C point 00137e10    2F normalise 00137a18
    42/43/44/45/48 vector slots 00138210 / 00138268 / 00138e30 / 001383d8
    54 look along 00138500                    58 distance, operands as pairs
    5C edge cross 001386e0

Matrices are col0, col1, col2, T, as the TGP's 0x26 replies them.
Prints, per command with replies, the median, 99th percentile and largest error
relative to 1 + |value|; exits 1 when one is over --limit.
"""
import argparse, collections, math, struct, sys

LOOP_PCS = (0x70, 0x77)


def f32(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]


def read_commands(path):
    """[(frame, op, args, replies)] from a --cop log."""
    cmds, cur = [], None
    with open(path) as fh:
        for line in fh:
            fr, tag, val = line.split()
            tag, val = int(tag, 16), int(val, 16)
            if tag >> 24 == 0x22:
                if (tag & 0xFFFF) in LOOP_PCS:
                    cur = [int(fr), val & 0xFF, [], []]
                    cmds.append(cur)
                elif cur:
                    cur[2].append(val)
            elif tag == 0x30000000 and cur:
                cur[3].append(val)
    return cmds


class Ps2Tgp:
    IDENT = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]

    def __init__(self):
        self.stack = [self.IDENT[:]]
        self.slots = {}

    @property
    def m(self):
        return self.stack[-1]

    def rot(self, a, b, c, s):
        """Columns a, b: a' = c*a + s*b, b' = c*b - s*a."""
        m = self.m
        ca, cb = m[a * 3:a * 3 + 3], m[b * 3:b * 3 + 3]
        m[a * 3:a * 3 + 3] = [c * x + s * y for x, y in zip(ca, cb)]
        m[b * 3:b * 3 + 3] = [c * y - s * x for x, y in zip(ca, cb)]

    def point(self, x, y, z):
        m = self.m
        return [m[i] * x + m[3 + i] * y + m[6 + i] * z + m[9 + i] for i in range(3)]

    def rotate(self, op, words, fa):
        if op >= 0x32:                      # halfword angle, 0x8000 = pi
            h = words[0] & 0xFFFF
            ang = (h - 0x10000 if h & 0x8000 else h) * math.pi / 32768
        else:
            ang = fa[0]
        c, s = math.cos(ang), math.sin(ang)
        cols = {0: (1, 2), 1: (2, 0), 2: (0, 1)}[(op - (0x32 if op >= 0x32 else 0x29))]
        self.rot(cols[0], cols[1], c, -s)

    def look_along(self, x, y, z):
        h, l = math.hypot(x, z), math.sqrt(x * x + y * y + z * z)
        if h and l:
            self.rot(1, 2, h / l, y / l)
            self.rot(0, 2, z / h, x / h)

    def slot(self, words, fa, op):
        k, v = words[0], self.slots.get(words[0], [0, 0, 0])
        if op == 0x42:
            self.slots[k] = fa[1:4]
        elif op == 0x44:
            self.slots[k] = [p + q for p, q in zip(v, fa[1:4])]
        elif op == 0x45:
            self.slots[k] = [p - q for p, q in zip(v, fa[1:4])]
        elif op == 0x48:
            self.slots[k] = self.point(*v)
        return v if op == 0x43 else None

    def matrix(self, op, fa):
        if op == 0x20:
            self.stack.append(self.m[:])
        elif op == 0x21 and len(self.stack) > 1:
            self.stack.pop()
        elif op == 0x25:
            self.stack[-1] = self.IDENT[:]
        elif op == 0x22:
            self.stack[-1] = fa[:12]
        elif op == 0x27:
            self.m[9:12] = self.point(*fa[:3])
        elif op == 0x28:
            for k in range(3):
                self.m[k * 3:k * 3 + 3] = [e * fa[k] for e in self.m[k * 3:k * 3 + 3]]
        elif op == 0x54:
            self.look_along(*fa[:3])

    def run(self, op, words):
        """The replies the PS2's maths gives, or None for a command it does not grade."""
        fa = [f32(w) for w in words]
        if op in (0x20, 0x21, 0x22, 0x25, 0x27, 0x28, 0x54):
            self.matrix(op, fa)
        elif op in (0x29, 0x2A, 0x2B, 0x32, 0x33, 0x34):
            self.rotate(op, words, fa)
        elif op == 0x26:
            return self.m[:]
        elif op == 0x2C:
            return self.point(*fa[:3])
        elif op == 0x2F:
            n = math.sqrt(sum(x * x for x in fa[:3]))
            return [x / n for x in fa[:3]] if n else [0, 0, 0]
        elif op in (0x42, 0x43, 0x44, 0x45, 0x48):
            return self.slot(words, fa, op)
        elif op == 0x58:
            return [math.dist(fa[0:6:2], fa[1:6:2])]
        elif op == 0x5C:
            oz, iz, jx, ox, ix, jz = fa[:6]
            return [(jx - ix) * (oz - iz) - (jz - iz) * (ox - ix)]
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('log', help="det_digest --cop log of srallyc")
    ap.add_argument('--limit', type=float, default=1e-3, help="largest error allowed (default 1e-3)")
    ap.add_argument('--show', metavar='OP', help="print the worst replies of one command (hex)")
    args = ap.parse_args()

    tgp, errs = Ps2Tgp(), collections.defaultdict(list)
    for fr, op, words, replies in read_commands(args.log):
        want = tgp.run(op, words)
        if want is None or not replies:
            continue
        got = [f32(r) for r in replies]
        e = max(abs(g - w) / (1 + abs(w)) for g, w in zip(got, want))
        errs[op].append((e, fr, got, want))

    bad = False
    for op in sorted(errs):
        es = sorted(x[0] for x in errs[op])
        n = len(es)
        over = es[-1] > args.limit
        bad |= over
        print('op %02X  n=%6d  median %.3g  p99 %.3g  max %.3g%s'
              % (op, n, es[n // 2], es[int(n * .99)], es[-1], '  OVER' if over else ''))
    if args.show:
        for e, fr, got, want in sorted(errs[int(args.show, 16)], reverse=True)[:12]:
            print('frame %d  err %.3g  got %s  want %s' % (fr, e, ['%.7g' % x for x in got], ['%.7g' % x for x in want]))
    if not errs:
        print('no graded commands: is this a Model 2A --cop log?')
        return 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
