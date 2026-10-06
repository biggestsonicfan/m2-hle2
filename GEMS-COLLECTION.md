# How Sonic Gems Collection runs Sonic the Fighters (Pinboard #419)

Sonic Gems Collection (GameCube 2005, PS2 Europe/Japan) carries Sonic the
Fighters. This note is what its files say about how Sega made the arcade game
run on a 485 MHz PowerPC and a 295 MHz R5900, and what of that applies to the
Dreamcast port (DREAMCAST-PORT.md, Pinboard #340).

Measured on the 123 `.CMP` files the owner supplied
(`ai/Sonic Gems Collection/CMP`, dated December 2004), and on the GameCube
executable that runs them (`stf.elf`, Pinboard #451; "The GameCube
executable" below), and on the PS2 one (`SLPM-66074`, Pinboard #488; "The
PS2 executable" below).

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
- **Mostly not a recompiler.** The program stays in i960 form, one record
  per instruction. This is the same family as the PS3/X360 ports and the PC
  console DLL (an i960 interpreter with traps), with the decode moved from
  run time to build time. The exception is about 70 hot functions, which the
  executable replaces with C translated from the i960 code ("The GameCube
  executable").

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
   cache that matters more than it did on a 24 MB GameCube. Tried: a
   MODELS.PAK grouped as the `OBJ_*` files are, a stage and a fighter at a
   time, loaded more pages in Flycast than the pack by first frame, even for
   a matchup the map never recorded, so the disc keeps the latter (#489,
   #492, DREAMCAST-PORT.md "Like objects together").
3. **Convert textures at build time, per stage.** If `TEX_STG*` is what it
   appears to be, Sega paid the i960 decompressor and the format conversion
   offline. On the Dreamcast the decompressor runs on the emulated i960 and
   the conversion to PVR twiddled format on the SH-4, so both are candidates.
4. **No sound CPU, no TGP.** Both ports agree, and the port already does this.

## The GameCube executable

`stf.elf`, loaded in the owner's Windows Ghidra with the GameCube loader
(reachable from the container at `http://192.168.65.254:5678`, the
GhidraMCP HTTP server). It has no symbols: about 3,280 functions, plus
MetroTRK and CRI's ROFS, ADXT and sound libraries. The i960 names below come
from the stfdecomp symbol table (`stfdecomp/temp/rom_code1.out`), which lines
up with every trap address checked.

### The interpreter

- **One record, one handler.** The handler table at `0x8013AF10` has 4,096
  entries, 16 per i960 opcode; 300 are distinct and the rest point at one
  default (`0x8002C2B0`). A record's first halfword (`& ~3`) picks the entry.
  The handler returns how far to move the record pointer, 8 for the next
  instruction or a pre-scaled branch displacement, so a branch is an add.
- **Memory ops are specialised by addressing mode**: the 16 entries of a MEM
  opcode are 16 handlers, one per mode, so no handler decodes its mode.
- **Only what STF uses is there.** No handler exists for opcode 0x66
  (`calls`, `modpc`, ...) or for most floating-point ops.
- **The loop runs in bursts of 12 instructions**, then reads the host clock
  (`0x8002DD0C` / `0x8002DDAC`, picked at `0x8002DCD0`). The i960's four
  timers advance by host time elapsed (`0x8002DBBC`). There is no cycle count.
  The second loop also stops after a wall-clock budget.
- **The bus is a page table** at `0x800F5850`: 0x40 bytes a page, 16 function
  pointers (read and write, by width). Addresses under `0x3000000` index by
  1 MB page; the rest by their top nibble.

### The traps

Opcode 0 is the trap. Its handler calls one switch of 161 cases
(`0x80035ED8`). A table at `0x8013F6A8` (640 entries of
`{i960 address, flags, trap × 4}`, ended by `0xFFFFFFFF`) patches them into
the decoded program at load; `ROM_CODE2` on the disc has none. 158 trap
numbers are used:

- **Waits and the frame loop**: `main_loop`, `interrupt_wait`, `_idle`,
  `mode_control`. The same places this emulator once hooked, and has since
  taken out (CLAUDE.md, "HLE Hooks").
- **Attract, select, continue and VS flow**: about 60 traps in `ADV_*`,
  `SEL_*`, `player_entry`, `vs_game_continue_check_ex` (`0xE584`, the VS
  rematch site the PC DLL also traps), and the select screen's hidden
  characters. Most are console behaviour, not speed.
- **`set_obj`, at every one of its 467 call sites** (trap 0x38). The native
  code writes the object command into the GEO display list itself.
  `set_obj_common`, `_go`, `_thd`, `_tpd`, `_fifo` and `set_window` are traps
  too.
- **Sound**: `sound_request_special` (`0x3F268`), as in the PC DLL, plus
  `sound_queue_output` and the sound init. The disc's music is ADX
  (`bgm00`–`bgm18.adx`), so no sound board is emulated.
- **94 whole functions in C, translated from the i960.** 47 are trap
  entries (0x65–0x93): `calc_unit_mat`, `get_frame_dat`,
  `calc_rob_angle_cont`, `set_coli_ball_data`, `rob_ball_data_make`,
  `coli_cont_cop`, `osage_dsp`, `area_check`, `ground_disp`, `cage_disp`,
  `mirror_rob_disp`, `rob_kage_disp_test`, `dented_cnt`, `doom_cnt`,
  `pendulum_3axis_cnt`, `select_enemy_command`, `rand`, the text and number
  drawers, two blocks inside `rob_disp` and one inside `ring_tobitiri`. The
  other 47 are what those call, translated too, so a whole call tree stays
  in C: `calc_unit_1`; `get_fcurve_value_f`, `get_start_value`,
  `get_end_value`, `set_mirror`, `rear_smooth_int`; the collision chain
  (`calc_attack_flag`, `area_coli`, `decide_coli_kind`, `unit_to_ball`,
  `decide_dir`, `coli_recalc_pos`); every `rob_disp` effect
  (`spin_attack_cnt_*_dsp`, `efc_*`, `tails_tail_disp`, `kosi_nobi_put`);
  the sway chains (`os_set_matrix`, `os_set_tsukene`, `os_set_coli`,
  `os_set_osage`, `osage_copro`, `os_set_osage_after`, `calc_kaze`); the
  ground and cage draws; and `set_obj` itself. The names were matched by
  call order and callee counts against the i960 program in IDA.

  The C is mechanical, a static recompiler's output. The i960 register file
  stays in memory and every function works on it. Main RAM is read and
  written directly (byte-swapped); anything else goes through the bus's page
  table, so `rand` still reads the timers at `0xF00000`. COP words go
  through the same FIFO calls the interpreter uses. Each function takes a
  flag: a trap entry passes 1 and the function ends by popping the i960
  frame, as `ret` would; a call from another translated function passes 0
  and just returns. It covers only the code that costs the most: the
  skeleton, motion, collision and drawing. The decompilation itself (and
  the COP's, below) is kept off the repo.

### The COP

The TGP is C, not an emulated SHARC: Sega rewrote the coprocessor's command
set as one C function per command and kept the firmware's own `Fn_*` names.
Ghidra decompiles it cleanly. The handlers are now named in the Windows
Ghidra project (`Fn_*`, `cop_stub_NN` for the empty ones, `cop_op_NN` where
the table has no name).

**The command table** starts at `0x8013A68C`: 16 bytes per opcode
0x00–0x87, `{handler, args, reply bytes, name}`. (Part 2 read it from
`0x8013A690`, one word late, and so paired each name with the next opcode's
handler. Its list of stubs was wrong.) The argument counts agree with the
firmware's, and with this emulator's wherever the emulator knows the
command. The word feeder (`0x8002298C`) collects the arguments, calls the
handler and copies the reply out. Both the i960 bus (writes to `0x880000`)
and the translated functions reach it.

**31 handlers are a bare `blr`:** `Fn_coli_dist` (0x33), `Fn_coli_sink`
(0x3C), `Fn_calc_unit` (0x40), 0x4B–0x4E, `Fn_base_zy` / `_yz` / `_zyx_ang` /
`_zyx` (0x4F–0x53, but not 0x51), 0x5F, `Fn_calc_unit_2` / `_1` (0x60, 0x61),
`Fn_2d_coli_*` (0x64–0x66), `Fn_x/y/z_rot_e` and `Fn_trans_e` (0x6C–0x6F),
`Fn_ball_to_unit` (0x71), `Fn_get_glo_ang_zyx` (0x76), `Fn_ziku_rot` (0x79),
`Fn_mul_matrix3` (0x7A), `Fn_scrn_clip` (0x7B), `Fn_load_inner_3x3` /
`Fn_store_inner_3x3` / `Fn_mul_matrix_inner3` (0x7C–0x7E), and
`Fn_zanzou_load_matrix_inner` / `_get_matrix_inner` (0x83, 0x87). Most of
these have zero arguments in Gems' table, so STF never sends them. Every
command this emulator found STF using has a real handler:
`Fn_fcurve_spl`, `Fn_osage` (it walks the record stream, echoes each type and
dispatches through a type table at `0x8014FE48`, as the firmware does),
`Fn_area_coli`, `Fn_calc_coli_flag`, `Fn_calc_unit_2_fast` (the two-bone IK),
`Fn_get_glo_ang`, `Fn_get_sm_ang_f` / `_r`, `Fn_parts_oidasi` and the zanzou
engine.

**It keeps the firmware's memory map.** The matrix stack is the current
slot plus a depth counter capped at 7. `Fn_parts_oidasi` reads the world
balls at DM `0x3E80` / `0x7E80`, the addresses `sharc_coli.h` uses.

**Its arithmetic is not the firmware's.** The constants are in `.sdata2`
(`r2` = `0x801F0520`), which part 2 did not search. The spline's span/30,
`0x3D08882F`, is there. What differs:

| Operation | SHARC firmware (what `sharc.h` ports) | Gems |
|---|---|---|
| √ | `rsqrts` 8-bit seed, three Newton steps | Gekko `frsqrte`, three Newton steps in double, then rounded to single |
| atan2 | Analog Devices' routine, two range reductions | its own rational P(x²)/Q(x²), constants `5.7310`, `0.17442`, `11.5545`, `22.9397`, `29.7767`, `20.5109` |
| asin | `_L20332` | libm's double `asin` (`0x800AEA98`) |
| angle word | `floor(rad · 0x4622F983) & 0xFFFF` | wrap to [0, 2π), then `fctiwz(65536 · a / 2π)` |
| spline (`0x32`) | float Horner, plain multiply and add | the same Horner order, with fused `fmadds` / `fmsubs` |
| `Fn_parts_oidasi` push | `1 − 2·d/(R+r)` (the `f11` bug) | `1 − d/(R+r)`, the intent, so half the arcade's push |

So Gems' TGP is Sega's reading of what each command means, not a copy of the
chip. It cannot replace the firmware as a reference. `stf-sharc` is the
firmware itself, reassembled bit for bit, and `sharc_exec.h` is held to it
and to MAME. A fight on Gems drifts from the arcade the way the PS3 port's
does.

**What it is good for:**
- **A readable second opinion on what a command means.** Where the firmware
  listing is hard to follow, the C handler with the same name says what Sega
  meant it to do. When the two disagree (the push factor), the firmware wins.
- **Arguments for commands STF never sends.** Gems' table and the firmware
  agree on 10 commands that take arguments and that `sharc_args_for_cmd`
  does not list, so it answers 0 for them: `Fn_sqr_r` 0x19 (1),
  `Fn_put_c` / `_add_c` / `_sub_c` / `_mul_c` / `_div_c` 0x1B, 0x1D–0x20 (1 each),
  `Fn_tri_shin` 0x28 (3), `Fn_get_inner` 0x2A (6), `Fn_mul_matrix_rev` 0x47 (12)
  and `Fn_sub3` 0x5D (6). A game that sends one would desynchronise the FIFO
  (the "unknown cmd" floats in CLAUDE.md). That is worth fixing before a
  second game needs it.
- **The shape of a fast COP for the Dreamcast** (below).

### What it adds for the Dreamcast port

1. **Translate the hot functions whole, not the hot blocks.** Gems
   interprets the program and compiles about 70 functions, chosen by name.
   They still hand their COP commands to the C TGP, but through the word
   feeder directly, not over the i960 bus. The port's AOT (`i960_aot.py`)
   compiles by coverage; the Gems list is a cross-check of what is hot.
2. **Draw from a trap at `set_obj`.** One trap at 467 call sites turns the
   game's draw into a native call. On the Dreamcast that is the place to
   hand an object to the PVR path without the i960 ever building the list.
3. **Clock the timers from host time and run in short bursts.** Gems keeps no
   cycle count. The port has to stay deterministic for netplay, so this only
   fits an offline build.
4. **Trap the VS rematch and the sound request** at the same addresses as the
   PC DLL (`0xE584`, `0x3F268`). The port already does the sound one.
5. **Write the COP as one C function per command, as Gems does,** with
   arguments read straight off the FIFO and no SHARC underneath. Sega's own
   console port shows the command set fits that shape, and each handler is a
   few dozen float operations. The SH-4 has the instructions for the same
   shortcuts Gems takes: `fsrra` for 1/√, `fsca` for sine and cosine, `ftrv`
   for the 4×4 multiply. The cost is the same as Gems': a fight then drifts
   from the arcade's. If the port must play netplay against m2-hle2 or the
   arcade, it has to keep `sharc.h`'s firmware arithmetic instead (single
   precision, so it fits the SH-4's FPU, only slower than the shortcuts).

Not yet read: the `TEX_STG` loader (the format string is at `0x8013F5D4`),
and how the GEO list becomes GX calls. The PS2 answers the first (below).

## The PS2 executable

`SLPM-66074` (the Japanese Gems Collection), loaded in the owner's Windows
Ghidra with Emotion Engine Reloaded (Pinboard #488, same address as the
GameCube one). No symbols: 3,155 functions in `0x100000`–`0x29067F`, only the
SDK's syscalls named. Ghidra has no function at many of the addresses below
(the COP handlers, the load callbacks); they were read with capstone (MIPS64,
little-endian) on a dump of the section instead.

### The same port, a different back end

The interpreter, the traps and the COP are the GameCube's, compiled for the
R5900. What changes is everything under them: the arithmetic, the drawing and
what is loaded from disc.

- **Handler table** at `0x2396B0`: 4,096 entries, 300 distinct, as on the
  GameCube.
- **Trap table** at `0x2882B0`: the same 12-byte
  `{i960 address, flags, trap × 4}` records. 639 entries and 157 trap
  numbers against the GameCube's 640 and 158; the one missing is trap 0x48
  at `0x3B654` (`enemy_control`). The 47 translated functions are the same
  traps (0x65–0x93) at the same i960 addresses.
- **COP table** at `0x23F0A0`, the same `{handler, args, reply, name}` layout
  with the `Fn_*` names at `0x23E700` on. The argument counts match, and the
  same 31 handlers are empty (`jr ra`). The handler state sits off `$gp`:
  argument pointer at `-0x7E2C`, reply pointer `-0x7E38`, reply count
  `-0x7E3C`, current matrix `-0x7E54`.
- **The i960 and the EE are both little-endian,** so main RAM needs none of
  the GameCube's byte swapping. (Not checked in the interpreter itself.)

### The COP on the R5900

All single precision, using what the EE has:

| Operation | GameCube | PS2 |
|---|---|---|
| √ (`Fn_sqr`, `0x15E140`) | `frsqrte`, three Newton steps in double | `sqrt.s` (`0x165300`), with a zero check |
| 1/√ (`Fn_sqr_r`, `0x15E180`) | | one `rsqrt.s` (`0x165340`), no Newton step |
| sin / cos (`0x165400` / `0x1653C0`) | | a 65,536-float table at `0x242360` (256 KB): `T[a & 0xFFFF]`, cos `T[(a + 0x4000) & 0xFFFF]` |
| atan2 (`Fn_atan`, `0x165500` → `0x164D40`) | the rational P(x²)/Q(x²) | the same constants, in single, on the R5900's accumulator (`adda.s`, `madd.s`, `msub.s`) |
| asin (`Fn_asin`, `0x165540`) | libm's double `asin` | ±1 special-cased, then libm (`0x1161C0`, single) |
| tan (`Fn_tan`, `0x165680`) | | `0x4000` / `0xC000` special-cased, then libm (`0x116140`) |
| angle word | wrap, `fctiwz(65536 · a / 2π)` | the same wrap, `cvt.w.s`, with a fix-up for results ≥ 2³¹ |
| 4×4 multiply (`Fn_mul_matrix`, `0x15D480` → `0x164A00`) | FPU | VU0 macro mode: the current matrix lives in `vf4`–`vf7`, each row is one `vmulax` / `vmadday` / `vmaddz`, 12 VU0 instructions in all (`Fn_mul_matrix_rev` → `0x164980`) |

The sine table is not the arcade's. The COP ROM's (`mpr-19015/19016`,
interleaved; `sharc_sincos` reads sine at word `0x10000` + angle) matches it in
1,773 of 65,536 entries. The PS2 table is within 6.5e-7 of the true sine but
only 16,141 entries are its exact float32. And the R5900's FPU is not IEEE 754
(no infinities or denormals, truncating rounding), so even the formulas the two
consoles share give other bits. A fight on the PS2 drifts from the arcade and
from the GameCube.

Across the game code there are 526 VU0 macro instructions, 127 `lqc2`/`sqc2`,
331 MMI and 5,368 FPU instructions. The densest functions are the COP's matrix
code (`0x15C980`, `0x15E340` around `Fn_calc_unit_2_fast`, `0x15EDC0`,
`0x164580`) and `glo_to_loc` (`0x1657C0`).

### Drawing: a geometrizer on VU1

The PS2 keeps the arcade's display list. `M2EPOL`, a native emulation of the
Model 2 geometrizer, walks the GEO list the i960 builds (`0x18C780`, command
`(w >> 23) & 0x1F`, list capped at 0x8000 words, bit 31 marks a jump):

| Command | What the PS2 does |
|---|---|
| 1 / 0x11 object, 2 / 0x12 direct | draw (`0x18D180`) |
| 3 window | up to 8 |
| 4 `WRT_TEXTURE` | into a copy of texture-parameter RAM (16-bit, up to 0x10000 entries, address `0x80xxxx`) |
| 5 `WRT_OBJECT` | into a copy of polygon RAM (up to 0x8000 words) |
| 6 material, 10 light data | uploaded to VU1 memory (light data at 0x20) |
| 0xB matrix, 0xC translate | uploaded to VU1 `0x3C4` (four quadwords, w = 1.0) and `0x3C7` |
| 7 mode, 8 focal, 9 light, 0x16 LOD | state |
| 0xE, 0x14 | skipped |
| 0xF end | kicks the double-buffered DMA chain (`0x18D4C0`) |

Uploads are a DMA tag plus a VIF `UNPACK V4-32` (`0x1FC7C0`); objects are
DMA `call` tags into prebuilt packets (`0x1FCA00`). The VU1 microcode belongs
to Sega's `GFX2 Ver.0.958R PS2MCW` library (27 May 2005): 1,664 instructions
in MPG blocks at `0x22DD84`–`0x230DB4` (about 13 of VU1's 16 KB), plus a
97-instruction program at `0x22D50C`, loaded through the chain at `0x22DCD0`.

**Objects are converted once, at scene load, not per frame:**

- The pack loader (`0x1922C0`) walks the scene's `obj_*` pack and converts
  each object to a VU1 packet (`0x192400`), storing its address in a table at
  `0x1A42680` indexed by object number (below 0x13F0). The converter carries
  hand-written fixes for single objects by number (0x1C7, 0x1CC, 0x1CB, 0x244,
  0x642, and 0xA65, which gets bits `0x7C0000` forced across 200 polygons).
- The native `set_obj` (`0x192A80`, `0x192B80`, `0x192C80`) looks the object
  up in that table and writes the GEO command with the packet's address in
  place of the ROM address, so command 1 is a DMA call. Objects 0x22B, 0xA01,
  0xA00, 0x9FF, 0x3B4 and 0x4A get flag 4.
- Only objects in the copy of polygon RAM (bit 31) are converted per frame,
  into a buffer per slot (`0x18DEC0`).
- The next scene's pack is streamed over several frames (`0x1939C0`): it
  clears the table entries the old pack set, then queues `tex_stg%02d.cmp` and
  the pack with their callbacks.

### Loading: no 16 MB ROMs

The executable names `rom_code1.cmp`, `rom_code2.cmp`, `rom_ep.cmp`,
`fixpage.cmp`, `tex_rob.cmp`, `tex_stg%02d.cmp` and 73 `obj_*` packs. It never
names `ROM_DATA`, `ROM_POL`, `ROM_TEX` or `TEXPAGE`. The PS2 runs without
them:

- **The data ROM is `FIXPAGE`, mapped in 4 KB pages.** A table of 4,096 page
  pointers at `0x1A3CE10` covers `0x2000000`–`0x2FFFFFF`. At load
  (`0x199340`) the callback walks a range list at `0x28ABD0` and points each
  page at its place in `FIXPAGE` (loaded at `0x1A78680`):

  | i960 range | Size |
  |---|---|
  | `0x20C0000`–`0x20D7FFF` | 96 KB |
  | `0x20E0000`–`0x20F3FFF` | 80 KB |
  | `0x2100000`–`0x210EFFF` | 60 KB |
  | `0x2120000`–`0x2120FFF` | 4 KB |
  | `0x2300000`–`0x2359FFF` | 360 KB |
  | `0x2800000`–`0x29F3FFF` | 2,000 KB |
  | `0x2B00000`–`0x2B21FFF` | 136 KB |
  | `0x2C00000`–`0x2D6FFFF` | 1,472 KB |

  4,308,992 bytes, exactly `FIXPAGE`'s size. Decoded, `FIXPAGE` is those
  ranges of `ROM_DATA` concatenated, byte for byte, except its last 3,710
  bytes (`0x2D6F182`–`0x2D6FFFF`): floats in both, different ones, not yet
  identified. So this is STF's whole data-ROM working set, 26% of the ROM,
  fixed for the game and not per scene.
- **Reads go through one lookup** (`0x187A00`, from six readers at
  `0x1593C0`–`0x159640`; `0x187980` from `0x1990CC`). It first maps
  `0x6400000`–`0x64FFFFF` and `0x6C00000`–`0x6CFFFFF` both onto the EPROM
  (`ROM_EP`, at `0x1E94680`, `0x199400`), then the page table; an unmapped page
  gives NULL.
- **Textures are two prebuilt sheets.** `tex_stg%02d` and `tex_rob`
  (192 KB each) are copied row by row (`0x1564C0`) into two 256 KB sheets at
  `0x14D5CC0`, sheet 0 for the stage and 1 for the fighters, 0x180 bytes into
  each 0x200-byte row for 0x200 rows. A flag at `0x14CBC8C` is then set,
  presumably to upload them. There is no conversion in the copy, so the files
  are already in the sheet's format. This settles "Models, textures, and
  memory" above: they are generated texture sheets, and with no `ROM_TEX`
  loaded the i960's texture decompressor has nothing to unpack. The texel format (depth, palette) is not yet read.

Where it sits in the EE's 32 MB: the sheets at `0x14D5CC0` (512 KB), the
page table at `0x1A3CE10` (16 KB), the object table at `0x1A42680`, the
`tex_rob` buffer at `0x1A47680`, `FIXPAGE` at `0x1A78680`–`0x1E9467F`
(4.1 MB) and `ROM_EP` right after it.

### Everything else

- **Sound**: IOP modules from `/STF/` (`sio2man`, `mcman`, `mcserv`,
  `xpadman`, `libsd`, `modhsyn`, `modsesq` and Sega's own `soundstf.irx`),
  banks `bgm_adx.sp2`, `voice_adx.sp2`, `se_1`–`se_3.sp2`,
  `se_ps2_common.sp2`, music `bgm00`–`bgm18.adx`. CRI's ADX, ADXF, SJ and ROFS
  libraries, as on the GameCube.
- **Platform**: `CROS/PS2 Ver.0.726` (27 May 2005), `STF/XMODULES.MRG`, HDD
  (pfs) support, and `cdrom0:\MC2.ELF` to return to the collection's menu.
- **Its own options**: HYPERMODE, BARRIER, ENERGY MAX and others.

### What it adds for the Dreamcast port

1. **The data-ROM working set is known: 4.1 MB in eight ranges.** Sega
   measured it and shipped only that. The port's pager can ship those pages
   and nothing else of `ROM_DATA`, or keep the hottest ranges resident.
   Before relying on it, check what the 3,710 changed bytes are.
2. **Convert models at scene load into native packets, indexed by object
   number,** as the PS2 does for VU1: on the Dreamcast, PVR vertex lists.
   `set_obj` then costs a table lookup, and only polygon-RAM objects are
   converted each frame. The PS2's converter also shows which objects need
   fixes by hand.
3. **Keep the GEO list and parse it natively** rather than trapping every
   draw: the PS2 shows a geometrizer front end is cheap enough on a slower
   CPU than the Gekko.
4. **Textures as prebuilt sheets per stage**, as the GameCube files
   suggested; the PS2 confirms it and that nothing runs the ROM decompressor.
5. **The cheap COP arithmetic is what the SH-4 has too**: one `fsrra` like
   `rsqrt.s`, `fsca` in place of the 256 KB sine table, `ftrv` like VU0's
   multiply. The netplay caveat above applies the same way.

Not yet read on the PS2: the interpreter's loop and bus (burst size, timers),
the `TEX_STG` texel format and its GS upload, `soundstf.irx`, and the threads.

## Tools in the container

This container is set up to read the PS2 build, or to run the GameCube one
under Dolphin:

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
- **PS2** is static only (Ghidra, plus capstone for the code Ghidra has no
  function for). No PCSX2: it would need a PS2 BIOS dump.
