#!/usr/bin/env python3
"""The Dreamcast's model pack: the polygon and texture ROM bytes the 3D decoder
reads, laid out scene by scene (dreamcast/dc_pager.h, "The model pack").

    dc_mdlpack.py --map dreamcast/sfight.mdlmap [--groups dreamcast/sfight.mdlgroups]
                  --roms <PS3>/stf_rom --out MODELS.PAK

MAP is `det_digest --model-map`: a line per run of 64-byte lines, `region
offset length frame`, region po (polygons) or tx (textures), frame the game
frame that first read them. In the ROM a scene's meshes and UV streams lie
scattered over 32 MB; the pack puts what one scene reads next to each other,
in the order the game first reads it, so a scene takes a few pages where the
ROM took one page per model.

GROUPS (tools/dc_mdlgroups.mjs) packs like objects together, as Sonic Gems
Collection's OBJ_* files do: a stage's, a fighter's, the select screen's,
every object whole (its mesh, texture headers and UV stream, each from its
start in the model table in ROM_DATA.BIN, at 0xE0004, to the next start), not
only the lines the map saw. A matchup the map never recorded then reads its
fighters from two places in the pack, where the map's lines alone leave it
to the ROM. The groups and the objects in each go by the frame the map first
reads them; the map's lines no group holds go last, by first frame.

The bytes come from the player's own ROM_POL.BIN and ROM_TEX.BIN, placed in
the regions as dreamcast/dc_layout.h places them, so the pack holds ROM data
and is a build product, never committed.

MODELS.PAK, little-endian:
    0   'M2PK', u32 count, u32 data offset (a multiple of 2048)
    12  count entries, sorted by address: u32 address, u32 length, u32 pack
        offset. The address is in the polygons region, or 0x1000000 + an
        address in the textures region; the pack offset counts from the data.
    the data, each run 64-byte aligned.
"""
import argparse, bisect, os, struct, sys

LINE = 64
TX = 0x1000000
MODELS, MODEL_TABLE = 5103, 0xE0004   # in ROM_DATA.BIN: u32 UV, texture headers, polygons; 16 bytes an entry
SPLIT = 30        # frames: a line first read this much later than the one before starts a new run

# Where dc_layout.h puts each file in its region: (region, region offset, file, file offset, length).
LAYOUT = [
    (0,  0,        'rom_pol.bin', 0,        0x1000000),
    (TX, 0,        'rom_tex.bin', 0,        0x400000),
    (TX, 0x800000, 'rom_tex.bin', 0x400000, 0x400000),
]

def read_map(path):
    first = {}
    for line in open(path):
        if not line.strip() or line.startswith('#'):
            continue
        r, off, ln, frame = line.split()
        base = TX if r == 'tx' else 0
        for a in range(base + int(off, 16), base + int(off, 16) + int(ln, 16), LINE):
            first.setdefault(a, int(frame))
    return first

def runs(first):
    """Runs of consecutive lines read in one scene: (frame, address, length)."""
    out, ks = [], sorted(first)
    i = 0
    while i < len(ks):
        j = i
        while j + 1 < len(ks) and ks[j + 1] == ks[j] + LINE and abs(first[ks[j + 1]] - first[ks[j]]) <= SPLIT:
            j += 1
        out.append((min(first[k] for k in ks[i:j + 1]), ks[i], (j - i + 1) * LINE))
        i = j + 1
    return out

def rom_bytes(roms, addr, n):
    """n bytes of the polygons/textures regions at addr, as the board sees them."""
    out = bytearray(n)
    for base, roff, name, foff, flen in LAYOUT:
        lo, hi = max(addr, base + roff), min(addr + n, base + roff + flen)
        if lo >= hi:
            continue
        with open(roms[name], 'rb') as f:
            f.seek(foff + lo - base - roff)
            got = f.read(hi - lo)
        out[lo - addr:lo - addr + len(got)] = got
    return bytes(out)

def read_groups(path):
    """object -> group number, in the order the file names them."""
    group, g = {}, -1
    for line in open(path):
        line = line.split('#')[0].split()
        if not line:
            continue
        if line[0] == 'group':
            g += 1
            continue
        for w in line:
            group.setdefault(int(w, 16), g)
    return group

def object_starts(rom_data):
    """Sorted (address, object) of every mesh, texture-header and UV stream start."""
    with open(rom_data, 'rb') as f:
        f.seek(MODEL_TABLE)
        t = f.read(16 * MODELS)
    starts = set()
    for i in range(MODELS):
        uv, th, po = struct.unpack_from('<III', t, 16 * i)
        if po:
            starts.add(((po * 4 - 0x2000010) & 0xFFFFFF, i))
        for p in (uv, th):
            if p:
                starts.add((TX + p * 2, i))
    return sorted(starts)

def whole_groups(first, group, starts):
    """(address, length) in pack order: every object of every group whole, its
    mesh, texture headers and UV stream each from its start to the next start
    in the ROM. Groups by the frame that first reads them (never read: last, in
    file order), a group's objects likewise, then the map's lines that no
    group's object holds, by first frame."""
    ends = sorted({a for a, _ in starts} | {a + l for b, r, _, _, l in LAYOUT for a in [b + r]})
    lines = {}                              # object -> its lines
    for a, o in starts:
        if o not in group:
            continue
        e = ends[bisect.bisect_right(ends, a)]
        lines.setdefault(o, []).extend(range(a & ~(LINE - 1), e, LINE))
    met = lambda o: min((first[l] for l in lines[o] if l in first), default=1 << 30)
    gmet = {}
    for o in lines:
        gmet[group[o]] = min(gmet.get(group[o], 1 << 30), met(o))
    objs = sorted(lines, key=lambda o: (gmet[group[o]], group[o], met(o), o))
    out, seen = [], set()
    for o in objs:
        for l in lines[o]:
            if l in seen:
                continue
            seen.add(l)
            if out and out[-1][0] + out[-1][1] == l:
                out[-1][1] += LINE
            else:
                out.append([l, LINE])
    rest = {l: f for l, f in first.items() if l not in seen}
    return [tuple(r) for r in out] + [(a, n) for _, a, n in sorted(runs(rest))]

def find_roms(d, names=('rom_pol.bin', 'rom_tex.bin')):
    have = {n.lower(): os.path.join(d, n) for n in os.listdir(d)}
    roms = {}
    for name in names:
        if name not in have:
            sys.exit(f'dc_mdlpack: no {name} in {d}')
        roms[name] = have[name]
    return roms

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--map', required=True)
    ap.add_argument('--groups', help='dreamcast/sfight.mdlgroups: lay like objects out together')
    ap.add_argument('--roms', required=True, help='the stf_rom directory')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    roms = find_roms(a.roms, ('rom_pol.bin', 'rom_tex.bin') + (('rom_data.bin',) if a.groups else ()))
    first = read_map(a.map)
    if a.groups:
        order = whole_groups(first, read_groups(a.groups), object_starts(roms['rom_data.bin']))
    else:
        order = [(addr, n) for _, addr, n in sorted(runs(first))]   # by first frame
    index, data = [], bytearray()
    for addr, n in order:
        index.append((addr, n, len(data)))
        data += rom_bytes(roms, addr, n)
    index.sort()
    head = 12 + 12 * len(index)
    data_off = (head + 2047) // 2048 * 2048
    with open(a.out, 'wb') as f:
        f.write(b'M2PK' + struct.pack('<II', len(index), data_off))
        for e in index:
            f.write(struct.pack('<III', *e))
        f.write(b'\0' * (data_off - head))
        f.write(data)
    print(f'dc_mdlpack: {len(index)} runs, {len(data) >> 10} KB, {a.out}')

if __name__ == '__main__':
    main()
