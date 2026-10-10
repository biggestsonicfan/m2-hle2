# Model 2A and its TGP: what the Sega Rally recomp teaches

Notes from reading xandoxan65's `segarally95-recomp` (Pinboard #112), a static
recompilation of *Sega Rally Championship* (srallyc). It lifts the i960 code to C
and runs it on a small host runtime (`lib/model2/{tgp,geo,hw,snd}`). It is early
work: it reaches the splash screens, attract and a practice run on the desert
course, with host maths standing in for most of the coprocessor.

## What this project does not need from it

- **The i960 side.** The recomp lifts code rather than emulating it, and its
  `i960_fp.h` helpers hold floats as host doubles. Our core runs the board's own
  instruction semantics and is graded against MAME. There is nothing to take here.
- **The sound board.** Its SCSP (`lib/model2/snd`) is a compact slot mixer with
  timer A only and no effects DSP, paced by instruction count. Ours runs in
  lockstep with the 68000 and is graded against MAME: see "Sound board" in
  CLAUDE.md.
- **The tilemap and the GEO.** `model2_geo_parse.c` is a port of MAME's
  `model2_v.cpp` parsers through a Python tool, and `sys24_tile.c` covers what
  `tile_render.h` already does. Our polygon decoder, fill and z-sort are further
  along and graded.
- **Palette and luma (CGM).** Its `notes/palette_cgm.md` works out by hand how
  Sega Rally builds `palram`, `lumaram` and `colorxlat` from `"CGM 1.0 "` blocks
  in its data ROM. We run that i960 code as it stands, so we get the tables for
  free. The note is still a useful map of that ROM if a colour bug shows up there.

## What it teaches about Model 2A

Sega Rally is a **Model 2A** game (MAME `model2a_state`). The game-profile enum
used to call it 2C, which is now fixed in `game_profile.h`. A 2A board has a
Fujitsu **TGP (MB86234)** where our 2B games have a SHARC. Supporting any 2A game
(VF2, Sega Rally, Virtua Cop 2, Manx TT, Dynamite Cop) means a second
coprocessor, and the recomp shows what that job involves:

- **The TGP's program comes from the i960, as the SHARC's does.** The i960 sets
  bit 31 of `0x980000` (`COPRO_CONTROL1_BASE`), writes the program word by word
  to the FIFO at `0x884000`, then clears the bit. Sega Rally sends `0x681`
  words from ROM `0x5AE94` (the count is at `0x5C898`). Our `COPRO_CTL` and
  `COPROGRAM` regions already take the SHARC's upload, so a TGP can be fed the
  same way.
- **The command words use the same formula.** `0x29005252` is op `0x52`:
  `(op << 23) | (op << 8) | op`, the `COP_CMD` rule our SHARC dispatch uses.
  The TGP's main loop reads a word, checks the doubled opcode byte, and calls
  through a jump table at `0xAF + op`. So an unknown command can be logged and
  its argument count read the same way as on the SHARC.
- **The opcode numbers do not match the SHARC's.** In Sega Rally the rotations
  are `0x29`/`0x2A`/`0x2B`. In STF they are `0x08`/`0x09`/`0x0A`. Each game's
  program has to be read on its own.
- **The TGP program can be disassembled.** The recomp has Sega Rally's program
  (`disasm/tgp/srally_tgp_program.asm`, 1,676 lines, disassembled with MAME's
  `mb86233d.cpp` rules) and a handler table for some opcodes. That is the TGP
  counterpart of `stf-sharc`'s `cpres1.asm`, and it is where a port starts.
- **The TGP's arithmetic comes from table ROMs** (`opr-14742a` / `opr-14743a`:
  sine, reciprocal and inverse square root). The recomp uses `sinf`/`cosf`
  instead. Our SHARC work showed that host maths drifts a replay off MAME within
  a few hundred frames. A 2A port should seed from those tables the way
  `sharc_fw_sqrt` and the others follow the SHARC firmware, and should not take
  the recomp's formulas as bit-exact.
- **A 2A racing game has a coprocessor data ROM.** Sega Rally's `copro_data`
  (`mpr-17754` / `mpr-17755`) holds the course's collision and height mesh.
  The TGP reads it at bank `0x800000`. Op `0x52` walks it for a query point and
  fills a slot, and op `0x53` returns that slot as 16 floats plus a flag word.
  The i960's road-contact code (`table_index_a` / `_b`, ROM `0x2AC2C` /
  `0x2ACC0`) reads the reply. `notes/play_mode_plan.md` and the firmware README
  in the recomp describe the handlers. Our memory map has no such region yet.

## If 2A support is ever started

1. Add a srallyc profile on the board layer, with `BOARD_MODEL2A_CRX`, and let
   the unknown-command warnings list the opcodes it sends.
2. Take the TGP program out of the upload, disassemble it (the recomp's
   `srally_tgp_program.asm` is a head start), and port handlers the way
   `sharc_exec.h` does, citing TGP program addresses.
3. Use the recomp's handler notes as a map, not as ground truth. Its own
   `Rules.txt` says it aims past MAME, not at it, and its play-mode plan lists
   host workarounds it still has to remove. MAME stays the oracle.
