# Model 2A and its TGP: what the Sega Rally recomp teaches

Notes from reading xandoxan65's `segarally95-recomp` (Pinboard #112), a static
recompilation of *Sega Rally Championship*. It lifts the i960 code to C and runs it
on a small host runtime (`lib/model2/{tgp,geo,hw,snd}`). It reaches the splash
screens, attract and a practice run, with host maths standing in for most of the
coprocessor. Its last commit is from 2026-09-26; nothing below needs re-reading
unless it moves.

Since this was first written, the `srallyc` profile landed (`profiles/srallyc.h`),
and the TGP is emulated as a CPU running the game's own program (`board/tgp.h`,
`board/model2a.h`, MAME as the oracle). So the recomp's TGP handlers are no longer a
plan to port, only a map. [SEGA-RALLY-PS2.md](SEGA-RALLY-PS2.md) covers the other
non-board reference, Sega's own C port on the PS2.

## It is Revision B; our profile is Revision C

The recomp loads MAME's `srallycb` (`epr-17888b.12` / `epr-17889b.13`, CRC
`95bce0b9` / `395c425e`). `srallyc` here is Revision C (`3d6808aa` / `f43c7802`),
the set in the container. So **an i960 address the recomp names is a Rev B
address.** The TGP upload is laid out the same in Rev C (checked: the source
`0x5F9E94` and the count pointer `0x5FB898` are the displacements at `0x4C00` /
`0x4C08`, in the loop the recomp puts at `0x4BD0`, and the count at ROM `0x5C898` is
`0x681` in both). Check any other address against the Rev C ROM before using it.

## What this project does not need from it

- **The i960 side.** The recomp lifts code rather than emulating it, and its
  `i960_fp.h` helpers hold floats as host doubles.
- **The sound board.** Its SCSP (`lib/model2/snd`) is a slot mixer with timer A
  only and no effects DSP, paced by instruction count.
- **The tilemap and the GEO.** `model2_geo_parse.c` is a port of MAME's
  `model2_v.cpp` parsers, and `sys24_tile.c` covers what `tile_render.h` does.
- **Its TGP arithmetic.** It uses `sinf`/`cosf`/`sqrtf` where the board reads the
  CPU board's table ROMs (`opr-14742a` / `opr-14743a`). `tgp.h` reads the tables, as
  `sharc_fw_*` follows the SHARC firmware. Do not take the recomp's formulas as
  bit-exact. Its own `Rules.txt` aims past MAME, not at it.

## What it is still good for

- **A disassembly of the TGP program.** `disasm/tgp/srally_tgp_program.asm`
  (1,676 lines, MAME's `mb86233d.cpp` rules) and
  `lib/model2/tgp/firmware/README.md`: the main loop at `0x70` reads a word,
  checks the doubled opcode byte (`(op << 23) | (op << 8) | op`, the SHARC's
  `COP_CMD` rule too) and calls through the table at `0xAF + op`. The rotations are
  ops `0x29`/`0x2A`/`0x2B` (STF's SHARC: `0x08`-`0x0A`). It is the TGP counterpart
  of `stf-sharc`'s `cpres1.asm`, without Sega's labels.
- **Course contact.** `copro_data` (`mpr-17754` / `mpr-17755`) holds the course's
  collision and height mesh. Op `0x52` (TGP `0x58A`) walks it for a query point and
  fills a slot; op `0x53` (`0x63C`) returns the slot as 16 floats and a flag word.
  Without the data `0x52` misses, the i960's `table_index_b` stores flag 15 and the
  car falls through the road: invisible on the desert course (Y ≈ 0), plain on the
  mountain (Y ≈ 15). That is the first thing to rule out if a car sinks here.
- **Names for the i960 code.** `symbols/functions.yaml` names about 420 routines
  (`irq_dispatcher` `0x18D8`, `game_dispatch_main` `0x3150`, `backup_sram_sync`
  `0x8F28`, ...), and `notes/play_mode_plan.md` the car-physics ones: `force_slot`
  `0x30140`, `index_refresh` `0x30320`, `contact_solve` `0x2F8A0`, `scene_prep`
  `0x34380`, `draw_4` `0x2B420`. Rev B addresses, see above.
- **Palette and luma.** `notes/palette_cgm.md` works out how the game builds
  `palram`, `lumaram` and `colorxlat` from `"CGM 1.0 "` blocks in its data ROM. We
  run that i960 code as it stands; the note is a map of that ROM if a colour bug
  shows up there.
