# Sega Rally on the PS2: what its executable says about the arcade board

Pinboard #659. The owner's Ghidra project on port 5678 holds the PS2 executable of
*Sega Ages 2500 Vol. 25: Sega Rally 2006* (SLPM-62703), which carries the 1995 arcade
game. This file records what was read out of it for the `srallyc` profile.

## What it is

**It is a C port of the arcade game, not an emulator of the board.** There is no i960,
no TGP and no FIFO. The game's i960 code was rewritten in C, and the TGP commands became
direct calls into C functions on a matrix stack (`0x42DC30`, 12 floats a slot, push
`FUN_001379B0`, pop `FUN_001379F8`) that use host maths (`sqrtf`). So, like the Sega Rally
recomp, it is a reference for **names and intent**, never for bits. `tgp.h` runs the
game's own TGP program and stays the board.

The port is faithful where it can be checked. The gear/transmission select
(`FUN_00111AA0`) scales the handle by `(h - 0x80) * -0.0003`, the same double constant the
arcade i960 uses at ROM `0x198C0` (`bal 0x19458`, `cmpibne 28`).

Ghidra answers plain HTTP at `http://192.168.65.254:5678` (`/decompile_function?address=`,
`/xrefs_to`, `/strings`, `/read_memory`). The Ghidra MCP tools point at `:8080` and fail.

## Controls

| Input | PS2 variable | Arcade scratch (written from `0x01C0001E` at ROM `0x26C0`) |
|---|---|---|
| HANDLE | `0x1E1708` | `0x202050` |
| ACCEL | `0x1E1709` | `0x202051` |
| BRAKE | `0x1E170A` | `0x202052` |
| switches | `0x1E1714` (1/2 coins, 4 test, 8 service, 0x10 start, 0x80 VR) | |
| gear | `0x3108B8` (1-4) | |
| HANDLE (DRIVE) | `0x3108B5` (in), `0x3108B4` (out) | |

The input test is `FUN_0011B618` and the pad conversion `FUN_0013BA80`.

**How far the PS2 turns the wheel.** The handle is centred at `0x80`.

- A full stick to the left gives `0x80 - 0x50` (≈ `0x31`) and to the right `0x80 + 0x40` (`0xC0`).
- The d-pad gives `0x30` / `0xC0`.
- A pedal goes `0x30 + v * 0x90 / 127`, so `0x30`..`0xC0`. Released is 0, and a button press is `0xC0`.

Our defaults (`io_5649.h`: centre `0x80`, range ±`0x58`, pedals `0x20` / `0xE0`) are wider. Both
sit inside the manual's FIG 8.4 (centre 7D-83, left < 2D, right > D3, pedals < 30 / > C0).

**What the game does with the handle.** None of this calls for a change:

- **In the race** (arcade ROM `0x28FB8`): `x = (h - 0x80) / 128.0`. A dead zone of |x| < 0.1
  gives 0, otherwise x ∓ 0.1, then `* 0.1`, accumulated. The ±0.1 is ±12.8 counts, which is
  the manual's 7D-83 centre band. No clamp was found there.
- **In the menus** (`FUN_0011A2E8`): `clamp(|h - 0x80|, 0x50) * 0.84 / 80`. So the game takes
  ±`0x50` as full lock, and our ±`0x58` reaches it.
- **Stepping through a menu** (`FUN_00127BC0`; arcade ROM `0x14780`, which runs at `0x5B3780`,
  tables `0x146E0`-`0x14770`): `h > 0x9F` steps right and `h < 0x61` steps left. It re-arms only
  once h is back inside `0x61`..`0x9F`.

## The drive board

The test menu's DRIVE BD TEST (`FUN_0011C868`) writes `7` to the drive board and, two
frames later, expects `7` back on HANDLE (DRIVE). Anything else prints DRIVE BD ERROR.
STEERING FORCE (`FUN_0011C730`) steps a level 0-8 kept at `0x1E04FF`. The PS2 drives a
Logitech wheel's force effects (`lgDev*`) in its place.

MAME wires only the write (`drive_board_w` on the 315-5649's port E) and neither do we, so
the test reports an error on both. The game does not check the drive board outside the test menu.

## The TGP's data ROM and the course

- **`cpdata.bin` is the TGP's data ROM**, loaded whole (`FUN_00137830`) and read a word at a
  time (`FUN_00137920`, the TGP's ROM port). The test menu checks it with a CRC-16
  (`FUN_00119A98`, table `0x2C7518`).
- **The ground query is a 2D point-in-polygon walk over the course data** (`FUN_00138760` /
  `FUN_00138928`). A segment holds a count, its polygons (16 words each, x/z corners at words
  0/2, 3/5, 6/8 and 9/11) and a flag word per polygon, where bit 2 marks a quad. A car's
  segment is tried, then the next and the previous (wrapping), and the hit polygon goes in the
  car's slot at `0x42DBB0`. On the board this is a TGP command, so it is the place to look if a
  car ever falls through the road.

## GEO and the models

- **The display list buffer is 128 KB** (`0x480B80`-`0x4A0B7F`). Each list ends with
  `0x07800000` (GEO command 0x0F, end of list); a full buffer halts with
  "GeoBuffer Overflow". A future srallyc capture should never get that big.
- **`cnv_m2mdl.c` turns Model 2 polygon data into PS2 packets** (`FUN_00147528`). It reads
  the same 10-word polygon records as `geo3d.h`: `attr & 3` is the link, and 0 ends the model.
  The link counts as 2 or 1, as in the iFlag table. `(attr >> 8) & 3` decides whether the face
  is emitted, and `(attr >> 4) & 0xF` (non-zero) picks the second of its two emitters. It agrees with our decoder and
  adds nothing new.
