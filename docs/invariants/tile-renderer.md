# Tile Renderer (board-level)

Part of m2-hle2's load-bearing invariants, indexed in [CLAUDE.md](../../CLAUDE.md): facts reverse-engineered or debugged into the emulator, in no datasheet. Do not re-derive them.

- **16-bit byteswap on pixel bytes**: indices `[0,1,2,3]` are read as `[1,0,3,2]` (XOR low bit of byte index). Within each swapped word, high nibble = left pixel, low nibble = right.
- **Tilemap entry** (MAME `segaic24` tile_info): bit15=priority, bits[14:7]=pal_bank (**8-bit**, `(entry >> 7) & 0xFF` — bit 14 is a palette bit, *not* h_flip; `change_bg_color` uses it), char = low bits. Full tile index = `entry & 0x3FFF`. Palette LUT index = `pal_bank * 16 + color_idx` (stride=16 entries = 32 bytes per bank). Verified: CG87 palette written to pal+0x660 = bank 51×32; tile entry pal_bank=(0x9980>>7)&0xFF=51; pal+51×32=0x660 ✓.
- **Color index 0 is transparent on foreground layers only**; background layers fully opaque (pass `NULL` for `alpha_out`).
- **Four tilemaps, each with its own scroll, and a window mask per pair** (MAME `segaic24` draw_common, `model2_v.cpp` screen_update). Tilemap t sits at tile RAM word `0x1000*t`, H scroll `0x5000+t`, V scroll `0x5004+t` (bit 15 disables), and samples at `(x − hscroll, y + vscroll)`. Pairs 0/1 and 2/3 share a control word (`0x5004` / `0x5006`, bits 14:13) and a mask (`0x6000` / `0x6800`, four words a line, one bit per 8 px):
  - control 0: the even tilemap draws where the mask bit is 0, the odd one where it is 1;
  - control 1: split at line `−vscroll`;
  - control 2/3: split at column `hscroll`.

  Behind the 3D go tilemaps 3 and 2 opaque, then 1 and 0 with tile bit 15 clear. In front go 3, 2, 1 and 0 with bit 15 set.
  - *Symptom that surfaced this in STF:* NEXT MATCH draws each fighter's art as a top half in tilemap 2 and a bottom half in tilemap 3, stitched by a control-1 split. The renderer drew only the even tilemap of each pair, so both fighters were cut off at mid-screen. It also showed leftover "WAITING FOR CHALLENGER" tiles that the split hides.
