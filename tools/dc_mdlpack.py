#!/usr/bin/env python3
"""The Dreamcast's model pack: the polygon and texture ROM bytes the 3D decoder
reads, laid out scene by scene (dreamcast/dc_pager.h, "The model pack").

    dc_mdlpack.py --map dreamcast/sfight.mdlmap --roms <PS3>/stf_rom --out MODELS.PAK

MAP is `det_digest --model-map`: a line per run of 64-byte lines, `region
offset length frame`, region po (polygons) or tx (textures), frame the game
frame that first read them. In the ROM a scene's meshes and UV streams lie
scattered over 32 MB; the pack puts what one scene reads next to each other,
in the order the game first reads it, so a scene takes a few pages where the
ROM took one page per model.

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
import argparse, os, struct, sys

LINE = 64
TX = 0x1000000
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

def find_roms(d):
    have = {n.lower(): os.path.join(d, n) for n in os.listdir(d)}
    roms = {}
    for name in ('rom_pol.bin', 'rom_tex.bin'):
        if name not in have:
            sys.exit(f'dc_mdlpack: no {name} in {d}')
        roms[name] = have[name]
    return roms

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--map', required=True)
    ap.add_argument('--roms', required=True, help='the stf_rom directory')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    roms = find_roms(a.roms)
    rs = runs(read_map(a.map))
    order = sorted(rs)                      # by first frame, then address
    place, data, off = {}, bytearray(), 0
    for frame, addr, n in order:
        place[addr] = off
        data += rom_bytes(roms, addr, n)
        off += n
    index = sorted((addr, n, place[addr]) for _, addr, n in rs)
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
