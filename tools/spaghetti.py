#!/usr/bin/env python3
"""How tangled is src/? (Pinboard #321; SPAGHETTI.md has the reading.)

Runs lizard (pip install lizard) over src/ and prints the share of
hand-written function code that sits in functions too branchy to follow.

  python3 tools/spaghetti.py [--threshold 20] [--top 25]

Branchiness is lizard's *modified* cyclomatic complexity (-m: a whole switch
counts as one decision, so a flat dispatch table is not penalised for its
cases). Generated files are left out, and so are the instruction
interpreters and command dispatchers, whose size is the size of the thing
they decode (DISPATCH below); both are reported on their own lines.
"""
import argparse, csv, io, os, subprocess, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..'))

# "GENERATED ... Do not edit" in their headers.
GENERATED = ('scsp_dsp_known.h', 'ps3ui_layout.h', 'ps3ui_fonts.h')

# One case per opcode / command / option: long because the instruction set or
# the command line is, not because the logic is knotted.
DISPATCH = {
    'i960_step_core', 'm68k_step_core', 'm68k_ea_read', 'm68k_time_of',
    'sharc_exec', 'sharc_args_for_cmd', 'scsp_dsp_decode',
    'mcp_dispatch', 'objview_cmd_apply', 'parse_args', 'sokol_main',
}


def lizard(*extra):
    cmd = [sys.executable, '-m', 'lizard', '-l', 'c', '-l', 'cpp', *extra, 'src']
    try:
        out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        sys.exit('lizard failed; install it with: python3 -m pip install lizard')
    return out


def functions():
    plain = {r[5]: r for r in csv.reader(io.StringIO(lizard('--csv')))}
    modified = {r[5]: int(r[1]) for r in csv.reader(io.StringIO(lizard('--csv', '-m')))}
    for key, r in plain.items():
        yield dict(nloc=int(r[0]), ccn=int(r[1]), mccn=modified[key], params=int(r[3]),
                   file=r[6], fn=r[7], line=int(r[9]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--threshold', type=int, default=20, help='modified CCN above which a function counts (20)')
    ap.add_argument('--top', type=int, default=25, help='list this many of the worst')
    a = ap.parse_args()

    every = list(functions())
    gen = [f for f in every if f['file'].endswith(GENERATED)]
    hand = [f for f in every if not f['file'].endswith(GENERATED)]
    disp = [f for f in hand if f['fn'] in DISPATCH]
    total = sum(f['nloc'] for f in hand)

    def share(fs):
        n = sum(f['nloc'] for f in fs)
        return f'{len(fs):5d} functions {n:7d} lines {100 * n / total:5.1f}%'

    print(f'hand-written: {len(hand)} functions, {total} lines of code in functions '
          f'(generated, left out: {sum(f["nloc"] for f in gen)} lines)')
    print(f'dispatchers           {share(disp)}')
    for th in sorted({15, 20, 30, 50, a.threshold}):
        knots = [f for f in hand if f['mccn'] > th and f['fn'] not in DISPATCH]
        print(f'modified CCN > {th:<3d}     {share(knots)}' + ('   <- the headline' if th == a.threshold else ''))

    print(f'\nby directory, share of lines in functions over {a.threshold} (dispatchers excluded):')
    by = {}
    for f in hand:
        d = f['file'].split('/')[1] if f['file'].count('/') > 1 else 'main*.c'
        t = by.setdefault(d, [0, 0])
        t[0] += f['nloc']
        t[1] += f['nloc'] if f['mccn'] > a.threshold and f['fn'] not in DISPATCH else 0
    for d, (n, k) in sorted(by.items(), key=lambda x: -x[1][1] / x[1][0]):
        print(f'  {d:10s} {n:6d} lines {100 * k / n:5.1f}%')

    print(f'\nworst {a.top} (modified CCN, lines, params):')
    for f in sorted((f for f in hand if f['fn'] not in DISPATCH), key=lambda f: -f['mccn'])[:a.top]:
        print(f'  {f["mccn"]:4d} {f["nloc"]:5d} {f["params"]:3d}  {f["file"]}:{f["line"]} {f["fn"]}')


if __name__ == '__main__':
    main()
