# The original Model 2 on MiSTer: what alphanu1's FPGA core teaches

Notes from reading alphanu1's `sega-model2-mister` (Pinboard #1479), an FPGA
recreation of the original Model 2 (i960KB, MB86234 TGP, the Model 1 sound board)
on the DE10-Nano. It runs Daytona USA (both sets), Desert Tank and Virtua Cop. Read
at commit `c6766a4` (2026-10-23). It is GPL-3.0, so we take facts from it and
never code.

Most of what it knows is in `docs/model2a-design-study.md` (37,000 lines of
numbered rounds, `R1`..`R942`) and `HANDOFF.md`. A caveat runs through both. MAME
is its oracle, as it is ours. When it says "the board", it usually means the FPGA
core on the owner's MiSTer, not an arcade PCB. Only a few entries carry evidence
from a real cabinet, and they are marked here.

## What we already do

Most of its hard-won facts are already in m2-hle2. They were checked one by one
against master (`4075c17`), and none needs work:

| Fact | Its round | Ours |
|---|---|---|
| `addc`/`subc` set carry (MAME's 32-bit wrap does not) | §2.3 | `i960_exec.h` (64-bit sum) |
| `bno` taken when AC&7 == 0 (Daytona's "cars always crash") | R242 | `i960_exec.h` |
| `cvtri` range check after rounding | R12 | `i960_exec.h` |
| long-real and transcendental ops, `flushreg` (Desert Tank, Virtua Cop) | R836, R844 | `i960_exec.h` |
| area wait table at 0xE00000 (the core and MAME ignore it) | R901 | `i960_timing.h` |
| TGP atan selector also drives gpio0 | R212 | `tgp.h` |
| cull: single-sided N·P < 0, link type 0 | R219 | `geo3d.h` |
| sort corner attr 11:10, `float_to_zval`, z_adjust | R246 | `geo3d.h` |
| translucent textured: texel 15 is a hole | R326 | `geo3d.h` |
| per-object window centre (opcode bits 30:29) | R760 | `geo3d.h` |
| windows filled last to first, the first write wins | R899 | `game_render.h` |
| checker (h0 bit 15), wrap-y | R623 | `geo3d.h` |
| window modes 1-3 ignore the row mask (Daytona's black sky band) | R755 | `tile_renderer.h` |
| per-line H scroll from the pair's even map in window modes | R761 | `tile_renderer.h` |
| videoctl bit 2 (Daytona's list buffer pick) | R765 | `irq_timer.h` |
| YM3438 at 8 MHz, only its timers and busy bit used | R820, R833 | `model1-boards.md` |
| MultiPCM 1 MB direct + 1 MB banked window | R95 | `model1_sound.h` |
| I/O board DIP-bank switch (port A bit 0), status 0x08 | R76, R839 | `model1io.h` |
| Virtua Cop's type-2 I/O board (CTC, the gun FPGA at 0x8100) | R839 | `model1io.h` (#1286) |
| 57.52 Hz frame (16 MHz / 656 / 424) | | `irq_timer.h` |

## What we could take

### 1. Desert Tank has no profile

`desert.zip` is in `$ROMS_DIR`, and nothing in `src/` loads it. The core says it
is Daytona's board: the type-1 I/O board (EPR-14869C), and the TGP program is
Daytona's upload. MAME flags it not working, but it boots and plays. A profile on
top of Daytona's install should be cheap. What the core found:

- **IN1 bit 0 high is REVERSE.** This was checked on a real cabinet. It is the
  opposite of what the core first read from MAME's input snapshots (R838).
- ADC channel 0 is HANDLE, 1 ACCEL, 2 TURRET (the cannon's elevation, centre
  0x80, lower aims up), not a brake as MAME's port name says (R837). IN1 also
  carries machine gun, cannon and shift.
- The FM is never voiced (728k YM3438 writes, no key-on), like Daytona's (R833).
- It draws at 30 Hz, a list every two vblanks.
- Its first long-real ops come at 19 s (`movrl`/`mulrl`/`cmprl`) and `divrl` at
  31 s, so a short boot test does not exercise them (R836).
- `mpr-16964`/`16965` socket suffixes are swapped against MAME's names, so match
  by CRC (`docs/rom-layout.md`).

### 2. Backup RAM starts all ones

MAME's `backup1` is `NVRAM ... DEFAULT_ALL_1` (model2.cpp), and the core copies it
(R40, `rtl/io/m2_backup.sv`). `backup_ram_open` and `mem_init` start it at zero.
The game checks a signature there and initialises the block itself. So the value
does not change what a game ends up with, but a first boot takes a different path
from MAME's, which matters for an A/B against MAME from cold. Daytona hides it
behind `daytona_settings_single`. Changing it changes what the i960 reads, so it
needs a `NETPLAY_PROTO_REV` bump. It is not done here.

### 3. Daytona over the link: FG reads take frames in MAME

`board/m2comm.h` says "FG reads take nothing (MAME's read_fg)". MAME defines
`M2COMM_SIMULATION` (m2comm.h:8), and with it `fg_r()` calls `read_fg()`. That
takes waiting frames, toggles zfg and makes a slave answer. It skips only the two
FG reads after a vblank (`m_zfg_delay = 2`). The core measured Daytona in MAME
spinning on FG until zfg flips, about 150 reads a frame on the master and 3,000 on
the slave, with all its traffic within ~70 µs of vblank (R916). Virtual-On and
Power Sled work with frames only at the pass. When Daytona gets a second cabinet
(`daytona.h` says it lacks one), expect a slave that waits a whole frame for each
flip unless FG reads drain the peer, as MAME's do.

Other Daytona link facts from R916 and `linux/m2link`:

- The settings byte after LINK ID (+0x0C) is CAR NUMBER − 1.
- A factory "TWIN MASTER" board waits at NETWORK CHECKING with or without a comm
  board fitted. SINGLE never touches the comm board, and `daytona93` never touches
  the comm window.
- Their wire format is MAME's: a TCP ring of raw `frame_size + 1` byte frames
  (3,585 for Daytona), token bytes 0xFF/0xFE/0xFC, the master waiting 0xE8 vblanks
  before ring-up. Every peer runs at the board's 57.52 Hz, and a per-frame 0xFC
  sync holds a 0.2%-fast peer in step. Ours is datagrams at 60 Hz with no 0xFC
  (`link_pace.h`). A TCP-ring transport speaking theirs would let an m2hle cabinet
  race a MiSTer or the Daytona recomp, but only at 57.52 Hz.

### 4. When the 3D is shown (unproven)

R826 argues that the real board shows a list two frames after its flip, one frame
later than MAME. MAME runs `geo_parse` at vblank start and composites at vblank
end, and Daytona pairs list k with the backdrop scroll of iteration k+2. The owner
reports MAME's attract jerks where the arcade is smooth. Delaying the 3D made the
core "better, still a bit jerky". This is inference and has not been measured on a
cabinet. We copy the list when the read pointer is written (`memory.h`), as MAME
does. The VCS capture of the owner's real 2B board (the `vcs` MCP) could settle it
for that board.

### 5. Output gamma is MAME's guess

MAME's colour output `max((i − 64) * 255 / 191, 0)` has no hardware source. The
core's owner finds photos of the cabinet brighter and bluer (R624, unresolved). The
same VCS capture is the way to measure it.

### 6. Test techniques worth borrowing

`docs/differential-testing.md` and `tools/i960-resync-diff.py`:

- **i960 trace diff across wait loops.** Compare raw PC streams against MAME. Only
  resync where both sides are in a short repeating cycle (≥ 8 skipped, ≥ 3
  iterations), report every resync with its skip counts, and stop at a divergence
  into new code. Also diff a per-loop iteration-count file, because "identical"
  otherwise hides a loop only one side ran. Past the first asynchronous interrupt,
  compare write traces, not PCs. We have this for the sound 68000
  (`m68k-trace.lua`), not for the i960.
- Run MAME from a scratch, cold nvram. A warm one once produced a false CPU-bug
  report. Our pair tests' `--no-nvram` is the same habit on our side.
- Break the model on purpose and check that the test count moves.

## What not to take

- Its TGP and 68000 cycle counts come from MAME's `mb86233`/`m68000`, which make no
  accuracy claim.
- Its "MAME is wrong" claims are mostly read from MAME's source, not seen on a PCB.
  The exceptions are R838 (Desert Tank reverse) and the arcade's sky in R755, which
  MAME gets right and the core had wrong.
- The core copies MAME's interrupt-priority mask bug (`0x1f00` for `0x1f0000`) on
  purpose, for lockstep. We clear the silicon's bits. A lockstep i960 diff against
  MAME has to allow for it across nested interrupts.
