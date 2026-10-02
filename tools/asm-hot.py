#!/usr/bin/env python3
"""asm-hot.py -- the compiler's own instructions for a function, each with what it cost.

What a decompiler would show you, read from the other end: the machine code
the compiler made of our C, with the count callgrind measured on every
instruction and the source line it came from. Where a C line costs more than
it reads, the reason is here: a global reloaded after every store
(-fno-strict-aliasing makes every store a possible alias), a check hoisted
into a hot loop, a call that did not inline.

    valgrind --tool=callgrind --dump-instr=yes --callgrind-out-file=cg.out \\
        build/det_digest $ROMS_DIR/sfight.zip --frames 1500 --from 999999 --no-sound-thread
    python3 tools/asm-hot.py cg.out build/det_digest                     # the hottest instructions
    python3 tools/asm-hot.py cg.out build/det_digest emu_slice_body.constprop.0 [min]

With no function it lists the hottest instructions anywhere with the function
they are in. With one it prints that function's disassembly (objdump -dl),
every instruction prefixed with its count, leaving out those under `min`.
Linux only (objdump, valgrind); perf does not run under WSL, callgrind does,
and its counts are exact, so two builds can be compared without timing noise.
"""
import collections, re, subprocess, sys


def costs(path):
    """Self cost per instruction address (calls= lines carry inclusive cost: skipped)."""
    cost = collections.Counter()
    addr = line = 0
    skip = False

    def num(tok, prev):
        if tok == '*':
            return prev
        if tok[0] in '+-':
            return prev + int(tok, 0)
        return int(tok, 0)

    with open(path) as f:
        for l in f:
            c = l[0]
            if c.isdigit() or c in '+-*':
                p = l.split()
                if len(p) < 3:
                    continue
                addr = num(p[0], addr)
                line = num(p[1], line)
                if skip:
                    skip = False
                    continue
                cost[addr] += int(p[2])
            elif l.startswith('calls='):
                skip = True
            elif l.startswith('positions:') and 'instr' not in l:
                sys.exit('no instruction positions: record with --dump-instr=yes')
    return cost


def symbols(exe):
    out = subprocess.run(['nm', '-S', '--defined-only', exe], capture_output=True, text=True).stdout
    syms = []
    for l in out.splitlines():
        p = l.split()
        if len(p) == 4 and p[2] in 'tTwW':
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    return sorted(syms)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cost = costs(sys.argv[1])
    exe = sys.argv[2]
    total = sum(cost.values())
    if len(sys.argv) < 4:
        syms = symbols(exe)
        starts = [s[0] for s in syms]
        import bisect
        for a, v in cost.most_common(60):
            i = bisect.bisect_right(starts, a) - 1
            name = syms[i][2] if i >= 0 and a < syms[i][0] + syms[i][1] else '?'
            print(f"{v:>15,} {100.0 * v / total:5.2f}%  {a:x}  {name}")
        return
    fn = sys.argv[3]
    mn = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    asm = subprocess.run(['objdump', '-d', '-l', '--no-show-raw-insn', f'--disassemble={fn}', exe],
                         capture_output=True, text=True).stdout
    shown = 0
    where = None    # the source line the next instructions came from, printed before the first shown
    for l in asm.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\t(.*)', l)
        if m:
            v = cost.get(int(m.group(1), 16), 0)
            shown += v
            if v >= mn:
                if where:
                    print(' ' * 16 + where)
                    where = None
                print(f"{v:>15,} {m.group(1)}: {m.group(2)}")
        elif re.match(r'^/.*:\d+', l):
            where = l.split('/src/')[-1]
    print(f"{shown:,} of {total:,} instructions ({100.0 * shown / total:.2f}%) in {fn}")


if __name__ == '__main__':
    main()
