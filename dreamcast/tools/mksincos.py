#!/usr/bin/env python3
"""mksincos.py -- SINCOS.BIN for the Dreamcast's link disc (Pinboard #461).

    dreamcast/tools/mksincos.py <sfight.zip> <out SINCOS.BIN>

The COP takes sin and cos from tables in its program ROM (sharc_sincos), and
the PS3 release's files, which the disc carries, have no copro ROM. Without
them the Dreamcast falls back to libm and the fight parts from MAME's in the
low bits from the first frame. The tables are not a formula: each entry is a
six-decimal number, off the true value by up to 2e-6, so they come from the
player's own arcade set: mpr-19015.29 (low halves) and mpr-19016.30 (high
halves) interleaved by word, and of that ROM the words 0x8000..0x17FFF (sin)
and 0x28000..0x37FFF (cos). 512 KB, little-endian, as g_sharc_sincos reads it.
"""
import struct, sys, zipfile

zipname, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(zipname) as z:
    lo = z.read('mpr-19015.29')
    hi = z.read('mpr-19016.30')
words = lambda a, b: b''.join(struct.pack('<HH', *struct.unpack_from('<H', lo, 2 * i), *struct.unpack_from('<H', hi, 2 * i))
                              for i in range(a, b))
with open(out, 'wb') as f:
    f.write(words(0x8000, 0x18000))
    f.write(words(0x28000, 0x38000))
