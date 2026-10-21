# Virtua Fighter 2 and its decomp: what each side teaches the other

Notes from Pinboard #1393. celsowm's
[vf2-decomp](https://github.com/celsowm/vf2-decomp) is a clean-room C decompilation
of VF2 Version 2.1 (the `vf2` set) with its own small Model 2A runtime, under
BSD-3. This emulator runs the same program on an emulated board
([profiles/vf2.h](src/profiles/vf2.h)). The two describe one machine from opposite
ends, so where they disagree one of them is wrong, and where they agree they hold
each other up. This file says which is which, as of vf2-decomp `fc5dcc98`
(2026-10).

## What vf2-decomp gives us: Sega's names

vf2-decomp's `decomp/i960/original_symbols.csv` holds 301 of Sega's own symbols
for the 2.1 program, from the symbol table Sega left in the DLL of a later
Windows port (its `docs/ORIGINAL_SYMBOLS.md`). The ones at addresses this emulator uses:

| 2.1 address | Sega's name | used here |
|---|---|---|
| `0x94D0` | `rand` | the random numbers that part our attract from MAME's (vf2.h); vf2-decomp calls it `timer_fuse` |
| `0x7C60` | `set_obj` | the draw at `0x7CB8` that indexes the model table |
| `0x1FCC0` | `change_scene` | writes `0x500064`, the stage byte (vf2-decomp: "display profile") |
| `0x6DD4C` | `check_sram_all` | the boot's backup-RAM check, `0x6DDC8`..`0x6E1A4` (settings_loaded) |
| `0x6010C` | `sram_clear_for_game_assign` | the factory defaults; country_default at `0x6029C` |
| `0x60330` | `sram_clear_for_coin_assign` | |
| `0xA804` | `WARNING_INT` | the "only in Japan" screen |
| `0x1610` | `check_credit` | holds `0x21F4`, where USA changes the attract |
| `0x2B4C` | `chg_scr_color_req` | |

The names are in vf2.h's comments. A name is a hint about intent, not a measurement.

## What we give vf2-decomp: the TGP's command table

vf2-decomp read the i960's TGP FIFO words as Model 2 geometrizer commands
(`0x01` "object", `0x02` "direct", `0x3f` "end") and framed them by header words
(`decomp/i960/notes/mame_3d_frame_geometry_format.md`). On a Model 2A board they
are calls into the TGP program the game uploads: a header
`(op << 23) | (op << 8) | op`, then a fixed number of arguments. Our TGP runs that
program ([MODEL2A-TGP.md](MODEL2A-TGP.md), `board/tgp.h`), and det_digest's
`--cop` log records each word the program takes with the PC that takes it, so the
split is the program's own:

    det_digest $ROMS_DIR/vf2.zip --profile vf2 --region japan --frames 3000 \
        --cop 1:3000:/dev/shm/tgp.log --out /dev/shm/digest.txt
    python3 tools/tgp-ops.py /dev/shm/tgp.log --names commands.csv

`commands.csv` is vf2-decomp's `decomp/tgp/commands.csv`. VF2's main loop reads
headers at TGP PC `0x35`. Over 3000 attract frames the game sent 72 ops, 2,086,897
commands. Every op took one fixed argument count, and every count equals the one in
Sega's PS2 port's table (Sega Ages 2500 Vol.16, SLPM-62547; ages-decomp
`VF2/COP/INDEX.md`). The one reply that varies is `0x4a` `Fn_osage`, 44..190
words (the sway chains). So `0x01`/`0x02` are push/pop matrix, `0x3f` is
`Fn_zyx_rot` with 3 arguments, `0x29`/`0x37`/`0x39` are fixed 3/2/3, and `0x13`
`Fn_add` takes 2, not 0: one of them is nearly always `0x00000000` (0.0), which
passes the header test as op `0x00`. 36,696 of the 36,698 calls carry one, which is
why header framing cannot work.

That table is now vf2-decomp's `vf2_tgp_command_payload_words`,
`vf2_tgp_command_reply_words` and `vf2_tgp_command_name` (branch
`idea-1393-m2hle2` on biggestsonicfan/vf2-decomp, for upstream review).

## Where the two agree

- The board timers at `0xF00000..0xF0000C` count down at 25 MHz and read `0xFFFFF`
  idle; `rand` mixes all four into `0x500098`. Both sides found it alone, so it
  holds: an attract that parts from MAME's is a cycle-count difference, not a
  logic bug.
- The TGP lengths vf2-decomp did pin by self-consistent framing (`0x0b` = 12,
  `0x34` = 1, `0x01`/`0x02` = 0) are the measured ones.

## Where vf2-decomp's runtime is behind the board (for its authors)

Its runtime is a host for decompiled functions, not a board, and says so. These are
the places it stops short of what this emulator and MAME do, which explain the
symptoms it records:

- **The frame.** It injects vector 12 after four visits to the frame-wait poll; the
  board's frame is 434,600 i960 cycles (57.52 Hz, `IRQT_FRAME_CYCLES`,
  docs/invariants/memory-bus.md). Only vector 12 is injected; MAME also raises 14
  (timer) and 15 (sound).
- **The TGP.** Its attract stalls in the TGP self-test, `0x804000` reads all ones
  and `0x980004` always reads empty. A TGP that runs the uploaded program (ours,
  or MAME's mb86233) answers the self-test; until then a reply table from the
  table above is the cheapest stand-in.

## Re-checking

Rerun the two commands above. `tgp-ops.py --names` exits 1 if any op's count
moves off the PS2 table's, which would mean either the TGP core or the table is
wrong. The log is the game's data: keep it in `/dev/shm`, out of both repos.
