"""Hold the sound 68000's instruction timing against MAME's cycle-level core.

  python tools/mame/m68k_timing_compare.py <mame-prefix> <ours.bin> [sound-rom.bin]

<mame-prefix>.tr and .fetch.bin come from tools/mame/m68k-trace.lua; <ours.bin>
from snd_replay with $SND_TRACE (records of [u32 pc][u64 clock] per instruction,
$SND_TRACE_FROM / $SND_TRACE_TO in seconds: MAME's frames 1700-1800 are about
29.5-31.3 s). The sound ROM (the 68000's 512 KB, big-endian) is only for the
disassembly, through capstone; without it the report is addresses.

MAME's clock for an instruction is the fetch of its opcode word, which the 68000
prefetches during the instruction before -- so a single instruction's time
cannot be read off it, but a run of instructions can: the report pairs every
sequence of PCs between two anchor instructions that both boards executed
identically and compares the clocks each took. With the timing right they
agree to a few clocks over fifty instructions; a wrong table entry shows as a
constant gap in every sequence that holds the instruction, and a different
interrupt sample point as the boards taking a handler from different
instructions (the sequences themselves differ, and the "shared" count drops).

The anchors default to STF's timer B handler: its reload write to the C reload
that follows (0x603d28 -> 0x603d4e). Any pair of PCs can be given.
"""
import collections
import re
import sys

import numpy as np

if len(sys.argv) < 3:
    raise SystemExit(__doc__)
MAME, OURS = sys.argv[1], sys.argv[2]
ROM = sys.argv[3] if len(sys.argv) > 3 else None
ANCHORS = [(0x603d28, 0x603d4e, 'timer B reload -> timer C reload'),
           (0x603d42, 0x603d46, 'the sequencer tick (jsr 604096)')]

dis = lambda pc: ''
if ROM:
    try:
        from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000
        md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
        rom = open(ROM, 'rb').read()

        def dis(pc):
            for i in md.disasm(rom[pc - 0x600000:pc - 0x600000 + 10], pc):
                return f'{i.mnemonic} {i.op_str}'
            return '?'
    except ImportError:
        print('(pip install capstone for the disassembly)')

pcs = []
with open(MAME + '.tr') as f:
    for line in f:
        m = re.match(r'([0-9A-F]{6}):', line)
        if m:
            pcs.append(int(m.group(1), 16))
mp = np.array(pcs, dtype=np.int64)
fe = np.fromfile(MAME + '.fetch.bin', dtype='<u4').reshape(-1, 2)
fa, ft = fe[:, 0].astype(np.int64), fe[:, 1].astype(np.int64)
T = np.empty(len(mp), dtype=np.int64)
j = 0
for i in range(len(mp)):
    k = j
    while fa[k] != mp[i]:
        k += 1
    T[i] = ft[k]
    j = k + 1
o = np.fromfile(OURS, dtype='<u4').reshape(-1, 3)
opc = o[:, 0].astype(np.int64)
oc = o[:, 1].astype(np.int64) | (o[:, 2].astype(np.int64) << 32)
print(f'mame: {len(mp)} instructions over {(T[-1] - T[0]) / 11289600:.2f} s; ours: {len(opc)} over {(oc[-1] - oc[0]) / 11289600:.2f} s')


def paths(p, t, a, b):
    s = np.nonzero(p == a)[0]
    e = np.nonzero(p == b)[0]
    out = collections.defaultdict(list)
    for i in s:
        k = np.searchsorted(e, i)
        if k < len(e) and e[k] - i < 400:
            out[tuple(int(x) for x in p[i:e[k] + 1])].append(int(t[e[k]] - t[i]))
    return out


for a, b, label in ANCHORS:
    pm, po = paths(mp, T, a, b), paths(opc, oc, a, b)
    common = [(k, pm[k], po[k]) for k in set(pm) & set(po)]
    common.sort(key=lambda x: -min(len(x[1]), len(x[2])))
    print(f'== {label}: {len(pm)} distinct sequences in mame, {len(po)} in ours, {len(common)} shared')
    for k, dm, do in common[:10]:
        cm, co = collections.Counter(dm).most_common(2), collections.Counter(do).most_common(2)
        print(f'   {len(k):3d} instructions: mame {cm} ours {co}')
    if common:
        k = common[0][0]
        print('   the commonest shared sequence:')
        for pc, c in sorted(collections.Counter(k).items()):
            print(f'      {pc:06x} x{c:3d} {dis(pc)}')
