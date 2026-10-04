# How Sonic Gems Collection runs Sonic the Fighters (Pinboard #419)

Sonic Gems Collection (GameCube 2005, PS2 Europe/Japan) carries Sonic the
Fighters. This note is what its files say about how Sega made the arcade game
run on a 485 MHz PowerPC and a 295 MHz R5900, and what of that applies to the
Dreamcast port (DREAMCAST-PORT.md, Pinboard #340).

Measured on the 123 `.CMP` files the owner supplied
(`ai/Sonic Gems Collection/CMP`, dated December 2004). The program itself
(the GameCube's `main.dol`, the PS2's `SLES_*`/`SLPM_*` ELF) is **not** among
them, so everything below comes from the data. The emulator's code is still to
read: see "Next: the executables" for what is set up for that.

## The container format: CRI CMP

Every file is `CRICMP` version 2.10: a 0x20-byte header (u32 LE decoded length
at +0x14, header length at +0x18), then a u32 BE block count and LZ blocks of
sixteen BE halfwords behind a flag halfword. A set flag bit means two literal
bytes. A clear one is either a run of the last byte (`0x0nnn`, n + 3 bytes) or a
back-reference of `(code >> 12) + 2` bytes from `code & 0xFFF` back.

**A back-reference overlaps the bytes it copies**, as in any LZ. The
community `CriCMP.c` copies with `memmove`, which breaks that case: the ROMs
come out corrupt. A forward copy a byte at a time decodes all 123 files, and
the ROMs below are then byte-identical to the arcade set.

## What is on the disc

| Files | Decoded | What it is |
|---|---|---|
| `ROM_CODE1` | 1 MB | The arcade program ROM (`epr-19001/19002`), **byte-identical** |
| `ROM_CODE2` | 2 MB + 8 | **The same program, pre-decoded**: one 8-byte record per i960 word (below) |
| `ROM_DATA` | 16 MB | Data ROM (`mpr-19005..19008`), byte-identical |
| `ROM_EP` | 920,048 | EPROM (`epr-19003/19004`), the used prefix, byte-identical |
| `ROM_POL` | 16 MB | Polygon ROM (`mpr-19009/10/12/13`), byte-identical |
| `ROM_TEX` | 8 MB | The non-empty parts of the 16 MB texture ROM, repacked |
| `OBJ_*` (74) | 23.8 MB | Per-scene model packs (each character and variant, each stage, select, ending, ...) |
| `TEXPAGE`, `FIXPAGE` | 2.0 / 4.3 MB | Pages of the data ROM, 100% raw chunks |
| `TEX_STG00..08,15`, `TEX_ROB` | 192 KB each | Generated textures, format not yet known (below) |
| `ADJUST`, `CREDITS`, `ZOOM_MODE` | 257 KB each | TIM2 images (the PS2's texture format) |
| `*_CRSDAT`, `*_CRSPAUSE`, `*_DSP_ADV`, `*_DSP_OPT` | per language E/F/G/I/S/U | The collection's own menus; `DSP_ADV` is a "DSX" file that names `_logo.gs` |

No sound ROM and no SHARC program: there is no 68000/SCSP and no TGP on the
disc. Sound must come from the collection's own sound engine, as on every
other console STF (CLAUDE.md, "Sega's own console DLL has no sound board").

## The finding: the i960 program is decoded at build time

`ROM_CODE2` holds the program a second time, transformed. The first 0x20 bytes
are copied unchanged. After that, the arcade word at offset `o` becomes the
record `(w0, w1)` at offset `2·o`, and 8 bytes of `FF` end the file. Over the
262,136 words:

| Arcade word | Records | `w0` | `w1` |
|---|---|---|---|
| CTRL (opcode 0x08–0x1F: `b`, `call`, `bx`, `be`, ...) | 18,685 | `opcode << 6` | displacement × 2 |
| COBR (0x20–0x3F: `cmpobX`, `bbs`, ...) | 23,525 | `opcode << 6`, register byte offsets in the high half | displacement × 2 |
| REG (0x58–0x7F) | 39,303 | `(opcode << 6) + function × 4`, register byte offsets in the high half | the literal or src1 operand |
| MEM (0x80–0xFF) | 59,021 | `opcode << 6` plus the addressing-mode bits | the offset |
| The second word of a two-word MEM instruction, and data | 49,618 | the word unchanged | 0 |
| Data repeated | 19,836 | the word | the word |
| Words whose top byte is 0x00–0x07 or 0x50–0x57 (not opcodes) | 52,148 | mostly put through the CTRL rule anyway | |

Each rule holds for every record of its class except one COBR word (`0x7C`, in the boot header).

What this says about the interpreter:

- **No decode at run time.** `w0`'s low halfword is an offset into a table of
  handlers: 64 bytes per opcode, 4 per REG function. Dispatch is one load and
  one indexed jump (or a computed goto). The operand fields arrive already
  scaled to byte offsets into the register file, so a handler indexes `r[]`
  without shifting or masking.
- **Branches are pre-scaled.** Each displacement is doubled, so the target is
  `pc2 + w1` in the decoded image, with no conversion back to an i960
  address on the hot path. The i960 address is `pc2 / 2`, needed only when
  `call` saves a return address or a register picks up the IP.
- **Data reads go to the untouched copy.** `ROM_CODE1` ships as well because
  the program reads tables, strings and float constants out of its own ROM;
  those loads go to the original image, and instruction fetch goes to
  `ROM_CODE2`. That is why the decoder could convert data words blindly:
  roughly 52,000 words that are not instructions came out "as branches", and
  nothing ever executes them.
- **Not a recompiler.** The program stays in i960 form, one record per
  instruction. This is the same family as the PS3/X360 ports and the PC
  console DLL (an i960 interpreter with traps), with the decode moved from
  run time to build time.

## Models, textures, and memory

The GameCube has 24 MB of main RAM and the PS2 32 MB. The board's ROMs are
over 60 MB, and the disc's own files show how it fits:

- **Models are packed per scene, not converted.** The 74 `OBJ_*` files are
  90–98% raw chunks of the arcade polygon ROM (the strip format `geo3d.h`
  decodes), with texture-header words beside them. Each pack holds what one
  scene draws: a character (in its 1, 2, R and V variants), a stage, select, the
  ending. Only the scene's pack has to be in RAM, never the 16 MB polygon ROM
  (which ships too, presumably for what the packs leave out).
- **The texture ROM is trimmed, not converted.** `ROM_TEX` is the 16 MB ROM
  without its empty regions. The i960's own decompressor (`unpack_lod_data`)
  still builds texture RAM from it, as on the board.
- **`TEX_STG*` / `TEX_ROB` are not texture RAM.** Searched as 32-byte runs
  (raw, byte-swapped, nibble-swapped, and 8×8 tiles of 4 bpp) against the
  explorer's texture sheets for all sixteen stages, they match 0%, and they
  match no arcade ROM. One file each for stages 00–08 and 15 (09–14 have none) and one for the fighters, 192 KB each: most likely each stage's
  texture RAM converted ahead of time into the console's own format (GX tiles
  or GS swizzle, with their palettes), so a stage does not run the decompressor
  or a format conversion when it loads. Confirming that needs the executable.
- `TEXPAGE` and `FIXPAGE` are data ROM pages, probably what the scene needs
  of the 16 MB data ROM gathered into one read.

## What it means for the Dreamcast port

The Dreamcast port (#340) is already further along on most of these than Gems
was: it pages the ROM in software (DREAMCAST-PORT.md, "Paging in software"),
plays Sega's own sound cue table in place of the sound board, and compiles the
hot part of the program ahead of time to SH-4 (`tools/i960_aot.py`, #394). So
Gems confirms the plan more than it changes it. What it adds:

1. **Pre-decode the instructions the AOT leaves to the interpreter.** The AOT
   compiles about a third of the code (`--cover 0.995`), and the interpreter
   decodes the rest at run time. A `ROM_CODE2`-style image (handler index
   plus pre-scaled register offsets in the first word, operand in the second)
   costs 2 MB in the pager instead of 1, and makes every interpreted
   instruction a single indexed jump. Sega kept both images and pointed data
   reads at the original, so this needs no change to the bus.
2. **Pack models per scene.** The Dreamcast notes rejected a mesh pack because
   prebuilt meshes still need their pages in RAM while drawn. Gems' packs are
   the raw ROM strips, not prebuilt meshes: their gain is locality (one read,
   pages that sit together) rather than build time. With the pager's ~1 MB
   cache that matters more than it did on a 24 MB GameCube.
3. **Convert textures at build time, per stage.** If `TEX_STG*` is what it
   appears to be, Sega paid the i960 decompressor and the format conversion
   offline. On the Dreamcast the decompressor runs on the emulated i960 and
   the conversion to PVR twiddled format on the SH-4, so both are candidates.
4. **No sound CPU, no TGP.** Both ports agree, and the port already does this.

## Next: the executables

This container is set up to read Sega's emulator once its code is here:

- **Ghidra** (`~/build/tools/ghidra`) has two user extensions in
  `~/.config/ghidra/ghidra_12.1.4_PUBLIC/Extensions/`: the GameCube loader
  (DOL/REL, Gekko) and Emotion Engine Reloaded (R5900 ELF, the PS2's VU
  and COP2 opcodes). Headless, the GameCube loader opens a symbol-map dialog and
  dies with a `HeadlessException`. Pass
  `-loader GameCubeLoader -loader-autoloadMaps true` with a `.map` file (an
  empty one works) beside the DOL.
- **Dolphin** 2609 is at `~/build/tools/dolphin` (`dolphin-emu`,
  `dolphin-tool`; the AppImage is extracted because the container has no
  FUSE). Headless debugging:
  `dolphin-emu -u <scratch>/user -b -e <game> -C Dolphin.Core.GFXBackend=Null
  -C Dolphin.General.GDBPort=<port>` (on `DISPLAY=:1`), then
  `gdb-multiarch` with `set architecture powerpc:750`, `set endian big`,
  `target remote :<port>`. `dolphin-tool extract` pulls `main.dol` out of an
  ISO/RVZ.
- **PS2** is static only (Ghidra). No PCSX2: it would need a PS2 BIOS dump.

Things to find in the executable:

- the dispatch loop and handler table that `w0` indexes, and how it reaches
  data in `ROM_CODE1`
- how the COP (TGP) commands are done in host code: the GameCube has paired
  singles and the PS2 has VU0 macro mode
- the `TEX_STG` format and its loader
- the sound-cue table and the trap that feeds it
