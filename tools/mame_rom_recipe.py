#!/usr/bin/env python3
"""mame_rom_recipe.py: a MAME model2.cpp ROM_START as rom_recipe.h parts.

    python3 tools/mame_rom_recipe.py <model2.cpp> <set> [<set> ...]

Prints, for each set, a `static const rom_part_t <set>_parts[]` and the
region sizes (`<SET>_SIZES`) in the form src/profiles/*.h use (Pinboard
#1872). Only the regions the board here loads are kept: maincpu, main_data,
copro_data, polygons, textures, audiocpu, samples, copro_tgp_tables (the
MODEL2_CPU_BOARD macro) and iocpu. A comm board's program, a Model 1 sound
board's, the default EEPROM and the DSB boards' are dropped, each named in a
comment. ROM_PARAMETER(":315_5881:key") comes out as <SET>_KEY.
"""
import re
import sys

REGIONS = {
    "maincpu": "ROM_MAINCPU", "main_data": "ROM_MAIN_DATA", "copro_data": "ROM_COPRO_DATA",
    "polygons": "ROM_POLYGONS", "textures": "ROM_TEXTURES", "audiocpu": "ROM_AUDIOCPU",
    "samples": "ROM_SAMPLES", "copro_tgp_tables": "ROM_TGP_TABLES", "iocpu": "ROM_IOCPU",
}
TGP = [("opr-14742a.45", 0x90c6b117), ("opr-14743a.46", 0xae7f446b)]

LOAD = re.compile(r'(ROM_LOAD32_WORD|ROM_LOAD16_WORD_SWAP|ROM_LOAD)\s*\(\s*"([^"]+)"\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*,'
                  r'\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*CRC\(([0-9a-fA-F]+)\)')
REGION = re.compile(r'ROM_REGION\w*\s*\(\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*("([^"]+)"|\w+)')
COPY = re.compile(r'ROM_COPY\s*\(\s*"([^"]+)"\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)')
KEY = re.compile(r'ROM_PARAMETER\s*\(\s*":315_5881:key"\s*,\s*"([0-9a-fA-F]+)"')


def rom_block(src, name):
    m = re.search(r'^ROM_START\(\s*' + re.escape(name) + r'\s*\)(.*?)^ROM_END', src, re.M | re.S)
    if not m:
        sys.exit(f"no ROM_START({name})")
    return m.group(1)


def pair_word(parts, region, pending, half):
    """A ROM_LOAD32_WORD half: held until its other half comes, then a PAIR32.
    The halves come in either order (skytargt lists the high one first)."""
    if not pending:
        return half
    if pending[1] ^ half[1] != 2:
        sys.exit(f"{pending[0]}: a half with no other half")
    lo, hi = (pending, half) if half[1] & 2 else (half, pending)
    parts.append(("PAIR32", region, lo[1], lo[0], lo[2], hi[0], hi[2]))
    return None


def load_line(parts, region, pending, m):
    kind, fname, off, crc = m.group(1), m.group(2), int(m.group(3), 0), int(m.group(5), 16)
    if kind == "ROM_LOAD32_WORD":
        return pair_word(parts, region, pending, (fname, off, crc))
    parts.append(("SWAP16" if kind == "ROM_LOAD16_WORD_SWAP" else "BYTES", region, off, fname, crc))
    return pending


def parse(block):
    parts, sizes, dropped, key = [], {}, [], None
    region = None
    pending = None   # a ROM_LOAD32_WORD half waiting for the other
    for line in block.splitlines():
        line = line.split("//")[0]
        if "MODEL2_CPU_BOARD" in line:
            sizes["ROM_TGP_TABLES"] = 0x40000
            parts.append(("PAIR32", "ROM_TGP_TABLES", 0, TGP[0][0], TGP[0][1], TGP[1][0], TGP[1][1]))
        elif m := REGION.search(line):
            tag = m.group(3) or m.group(2)
            region = REGIONS.get(tag)
            if region:
                sizes[region] = int(m.group(1), 0)
            elif tag not in ("other_data", "video_unk"):
                dropped.append(tag)
        elif m := KEY.search(line):
            key = int(m.group(1), 16)
        elif region and (m := COPY.search(line)):
            if REGIONS.get(m.group(1)):
                parts.append(("COPY", REGIONS[m.group(1)], int(m.group(2), 0), int(m.group(3), 0), int(m.group(4), 0)))
        elif region and (m := LOAD.search(line)):
            pending = load_line(parts, region, pending, m)
    return parts, sizes, dropped, key


def emit(name, parts, sizes, dropped, key):
    out = []
    if dropped:
        out.append(f"/* MAME's {', '.join(dropped)}: no board here reads them. */")
    out.append(f"static const rom_part_t {name}_parts[] = {{")
    for p in parts:
        if p[0] == "PAIR32":
            out.append(f'    ROM_PAIR32({p[1]}, 0x{p[2]:07x}, "{p[3]}", 0x{p[4]:08x}, "{p[5]}", 0x{p[6]:08x}),')
        elif p[0] == "SWAP16":
            out.append(f'    ROM_SWAP16({p[1]}, 0x{p[2]:07x}, "{p[3]}", 0x{p[4]:08x}),')
        elif p[0] == "BYTES":
            out.append(f'    ROM_BYTES({p[1]}, 0x{p[2]:07x}, "{p[3]}", 0x{p[4]:08x}),')
        else:
            out.append(f'    ROM_MIRROR({p[1]}, 0x{p[2]:07x}, 0x{p[3]:07x}, 0x{p[4]:x}),')
    out.append("};")
    order = list(REGIONS.values())
    sz = ", ".join(f"[{r}] = 0x{sizes[r]:x}" for r in order if r in sizes)
    out.append(f"#define {name.upper()}_SIZES {sz}")
    if key is not None:
        out.append(f"#define {name.upper()}_KEY 0x{key:08x}u")
    return "\n".join(out)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    for name in sys.argv[2:]:
        print(emit(name, *parse(rom_block(src, name))))
        print()


if __name__ == "__main__":
    main()
