# The Dreamcast port (Pinboard #340)

The board on a Sega Dreamcast under KallistiOS, tested in Flycast's libretro
core. [dreamcast/README.md](dreamcast/README.md) has the build and the disc.
This file covers what was measured, and the traps that cost the most time.
Most of them apply to any small target, the ARC-S included.

## Where it stands (milestone D2, #353)

STF boots under Flycast through BACKUP RAM IS BROKEN, the SEGA logo, the FBI
picture, the title and attract, and shows FREE PLAY. The PowerVR draws the
picture (`dreamcast/dc_pvr.h`, below): both tile layers and the 3D scene from
the board's own display list. There is no netplay. Sound is Sega's console
approach, ADX cues behind the sound code (#342, below), not the sound board.
The pad is mapped but untested.

Figures from Flycast's libretro core with the HLE BIOS (emulated time, which is
approximate), per frame shown:

| | D1 (tiles only) | D2: FBI picture | D2: title | #355: title | #355: attract | #358: title | #370: attract | #370: fight |
|---|---|---|---|---|---|---|---|---|
| board fps | 7.9 | 11.4 | 2.2 | 3.1-3.4 | 5.7-6.2 | 5.1-5.9 | 10.0 | 6.4-7.7 |
| i960 slice | ~88 ms | 46 ms | 83 ms | ~80 ms | ~98 ms | ~80 ms | 53 ms | 71-79 ms |
| tile layers | ~36 ms | 1 ms | ~128 ms | ~129 ms | 1-2 ms | 5-6 ms | 16 ms | 5 ms |
| 3D decode | - | 28 ms, 745 tris | ~197-384 ms, ~2,330 tris | 46-76 ms, ~2,300 tris | ~30 ms, ~3,000 tris | 47-75 ms, ~2,300 tris | 43 ms, 168 tris | 29-39 ms, ~2,500-3,250 tris |
| depth sort | - | 1 ms | 3 ms | 3 ms | 4 ms | 3 ms | 0 ms | 3-4 ms |

The D2 title decode was not the SH-4's arithmetic: the heap was full, so 7-10
mesh builds a frame failed (malloc), and each failure cost a mesh build plus
the full decode, every frame. Only ~25 triangles a frame came through the
cache. #355 freed the heap (below); a cached triangle costs ~9 us. #358 moved
the title starfield's line scroll to the PVR (below): its tile layers went
from ~129 ms to 5-6 ms a frame.

## The picture on the PowerVR (dreamcast/dc_pvr.h)

- **KOS's PVR API, not GLdc.** geo3d.h already hands over eye-space
  triangles, and the projection is two multiplies; GLdc would transform every
  vertex again in software. SDL2 (and so `-lGL`) is still linked, for audio.
- **The board's z-sort, run backwards.** The board gives each polygon one sort
  key and fills near buckets first. The opaque list draws every face far to
  near (a radix sort on slice, key and index) with depth compare ALWAYS, which
  is the same rule, ties included. The vertex z stays the true 1/w for
  perspective-correct texturing.
- **Faces with holes go to the translucent list, not punch-through.** The PVR
  forces punch-through's compare to GEQUAL (Flycast does too), so it cannot
  draw in the board's order. The translucent list is presorted, with the
  header's compare (GEQUAL against what the opaque list wrote).
- **A texture tile becomes a twiddled 4bpp paletted PVR texture**, cut out of
  texture RAM the first time a face uses it and dropped when the game writes
  over it. The palette is a grey ramp, and texel 15 is clear in bank 1, for
  faces with holes. A face's colour ramp (luma RAM, poly luma, colorxlat) is
  fitted as base × texel + offset, the PVR's modulate with offset colour.
- **The tile layers stay on the CPU, drawn incrementally.** A full redraw is
  ~200-330 ms of SH-4 time. As `tile_compose_cpu` does, only the 8×8 blocks a
  tile RAM change reaches are drawn again (`tile_dirty_find`) and converted
  (RGB565 back, ARGB1555 front). A colour change converts everything, and only
  when a pen on screen changed: most of STF's palette writes are 3D colours.
  `s24_draw_tilemap` (tile_renderer.h, every build) now walks a run at a time,
  to the next cell edge, mask group or split, not a pixel at a time;
  `tile_test` still holds it to the original pixel for pixel.
- **A line-scrolled tilemap is scrolled by the PVR** (#358). The title's
  starfield is tilemap 2 with a per-line H scroll; on the CPU each scroll
  change redrew its whole line, ~2,100 of 2,976 blocks a frame. When tilemap
  2 covers the back layer alone (pair 2/3 in mode 0, no window mask bit, so
  tilemap 3 draws nowhere), all 512x512 of it sits in two textures (every
  pixel RGB565, its category 1 pixels ARGB1555) and each run of lines with
  one scroll is a strip with U starting at -scroll, cut where U wraps. Only
  cells the game changes are drawn into them, and a colour change redraws
  them only if a bank they use changed. Tilemaps 1 and 0 stay on the CPU, in
  an ARGB1555 back layer that the translucent list draws first, at the
  strips' z with GEQUAL, so the 3D hides it. Any other layout falls back to
  the full CPU layer. Since #481 tilemap 0 goes the same way when pair 0/1 is
  laid out alike (m2-sonic, below), and the window mask counts only on the
  view.
- **The 3D can draw past the board's 496 pixels.** Black bars cover x < 10 and
  x > 630 at the end of the translucent list.

## Memory: 16 MB for an 80 MB board

- **Measure the working set before designing.** `tests/rom_touch.c` maps the
  ROM `PROT_NONE` and counts the 4 KB pages a session touches, from every reader
  (bus, COP tables, 3D decode, 68000). STF touches 11.2 MB in all, and the
  biggest 60-frame window is ~2 MB. That makes demand paging viable, where
  overlays or a cut-down set would not have been.
- **Read the ROM files as they ship (#342).** The PS3 release's `stf_rom`
  files are the board's address spaces, already joined and de-interleaved
  (`dc_layout.h`), so the disc carries them unchanged: no zip on the SH-4 and
  no pack step. ROM_EP.BIN is the 1 MB mirror the program repeats 16 times;
  the layout maps it 16 times instead of storing it. What the PS3 files lack:
  texture ROM 0x700000-0x7FFFFF (the UV streams of models 4319-5102) and
  0xC00000-0xFFFFFF (never read), the copro tables (the board falls back to
  libm) and the sound CPU's program and samples (no sound board here). The
  older pack (`m2pack`, 39.1 MB deduplicated from the MAME set) is gone.
- **Size static buffers per build.** The desktop's debug rings were most of
  the 9.2 MB of BSS: `GEO_CAPTURE_SIZE` (1 MB, only the MCP bridge reads it)
  and `LOG_MAX_LINES` (256 KB) can now be overridden. A handheld build gets the
  same savings for free.
- **The 3D and the rest of the board in 16 MB (#353).** Defines the Makefile
  sets, all defaulting to the desktop's sizes: `GEO3D_MAX_TRIS` 4096,
  `GEO3D_MAX_LINES` 16, the mesh cache at 256 slots and 1 MB
  (`GEO3D_MESH_CACHE_*`), the sound board's RAM, output ring and 68000 ROM cut
  to stubs (`SOUND_RAM_SIZE`, `SOUND_OUT_FRAMES`, `M68K_ROM_SIZE`; the sound
  board never runs here), and `GEO_PUB_COPIES` 1 (memory.h): a host that draws
  each list on the emulator's thread before the next slice needs one published
  copy, not two.
- **Where the heap goes (#355).** A `--wrap=malloc` count by caller: texture
  RAM 2 MB and framebuffer 0.5 MB (`mem_init`), the page cache 1 MB, the sound
  effects ~1.1 MB (`ds_init`), the pager's tables ~110 KB, then the meshes.
  That left ~330 KB for meshes, and the title needs ~900 KB.
  `geo3d_mesh_build` had a 4096-face scratch in BSS (600 KB, faces are ~150
  bytes); it now writes the faces into their heap block directly and shrinks
  it to fit. BSS is ~9.3 MB. A mesh that does not fit stays a failed entry
  until the cache starts over, so it is not rebuilt every frame. The page
  cache gets what is free after a reserve computed from texture RAM,
  framebuffer and `GEO3D_MESH_CACHE_BYTES` plus 768 KB; that is 1 MB, the
  minimum.
- **Not a mesh pack on the disc (#355).** Prebuilt ("Ninja-style") meshes
  read through the pager would still need their pages in RAM while drawn, so
  they save the build time (paid once per mesh) but not the memory, which was
  the real limit. Worth it only if build hitches show up when scenes change.
  What the disc does carry is a pack of the raw ROM bytes, for locality
  (#456, "Sonic Gems Collection's way", below).
- **Let the host own the regions.** `g_mem_window` (memory.h) lets a host give
  the bus a window for MAIN_DATA / XTRA_DATA / VID_EXT_RAM instead of a heap
  copy. The profile skips its memcpy when the window already is the ROM. That
  saves a 16 MB copy on any target.

## Paging in software (dreamcast/dc_pager.h, dc_paged.h)

The ROM windows are paged by the program, with the SH-4's MMU off (#394 part
12, below, has why). Until then the pager ran on the MMU. That version, and
the four Flycast traps it hit, are in the history (`b35`, `pg_mmu_on`).

- **A paged region's `data` is an address that is never dereferenced**:
  `PG_VA_BASE` (0x10000000) on, 128 MB of span, naming the region's pages.
  `MEM_HOST_PAGED(data)` (dc_paged.h, put in front of every file by the
  Makefile) tells the bus which regions those are.
- **The bus page table is the TLB.** `MEM_PAGE_SHIFT` is 14 here (16 KB
  pages, `MEM_PAGES` 0x4C00) and 16 everywhere else. `mem_build_pages` leaves a
  paged region's entries empty, so the first access takes the slow path, which
  calls `MEM_HOST_PAGE_IN` (`pg_bus_in`). That brings the page in and installs
  `rd_page`, and `wr_page` only once the frame is dirty, so a first write
  still goes the slow way to mark it. Each frame remembers up to four table
  slots that point at it and clears them when it is evicted.
- **Whatever reads ROM outside the bus asks for the bytes' place**:
  `MEM_HOST_AT` / `GEO3D_ROM` → `dc_rom_at`. geo3d's model table,
  palette, material and texture-word reads, the 40-byte polygon walk, and
  `sfight_read32` at install all go through it. A pointer in a frame stays
  good for the next four calls (a ring of recent frames the CLOCK hand
  skips). Bytes that straddle two pages come back in a bounce copy.
- **`ib_build` does not cache a block it could not read.** A block built
  while its page was out comes out empty; it is built again next time.
- CLOCK eviction over 16 KB frames. Anonymous pages and written ROM pages
  are pinned (`pg_pinned`, `wr` on the overlay counts the second kind).
- **Read the disc with the GD-ROM syscalls, by PIO, and poll.** KOS's cdrom
  driver takes semaphores. Nothing else may touch the drive once the pager is
  up. With the MMU off a miss is an ordinary call, but the rule stands.
- **Find the data track through the high-density TOC.** The low-density TOC only
  names track 1, a stub. A CD-R has only the low one, so that is the fallback.

## Flycast's MMU: four things that fail without a word

These held while the pager ran on the MMU (up to b35). Real hardware was not available. Each of these made the board read zeros or
stale pages, with no error anywhere.

1. **Flycast enables full MMU emulation only if it recognizes WinCE.** It checks
   for "SH-4 Kernel" (UTF-16) at 0x8C0110A8 or 0x8C011118 when MMUCR.AT changes,
   and newer builds also check when a non-SQ UTLB entry is loaded. `pg_mmu_on`
   writes the signature there around `mmu_init`.
2. **Its fast MMU walks WinCE page tables through TTB before it raises a TLB
   miss** (`USE_WINCE_HACK`, wince.h). KOS never sets TTB, so the walk read
   whatever TTB pointed at and made up mappings. TTB now points at a table whose
   every group is zeroed, which the walk reads as "not present".
3. **An associative invalidate does not evict anything.** The fast MMU keeps
   every entry it was ever given and ignores V. Only MMUCR.TI clears it. So an
   eviction flushes with TI and then calls `mmu_init_basic()` to restore KOS's
   store-queue entries.
4. **It never raises the first-write (D bit) trap.** Writable ROM pages
   (STF writes into main_data) are checksummed on load and checked before they
   are evicted. On hardware the trap (`pg_first_write`) also catches them.

## Sound: Sega's console way (#342, dreamcast/dc_sound.h)

- **No sound board, by design.** The 68000 and the SCSP would cost more than
  the i960 already does. Sega's console DLL has the same answer: it traps
  `sound_request_special` (`0x3F268`) and plays an ADX2 cue for the code in g0
  (CLAUDE.md, "Sound board"). The port does the same with Sega's own table,
  read out of the PC DLL over the Ghidra bridge (`0x180126a70`, 123 entries;
  the eight `0xAE14xx` stop codes are a switch in `FUN_180004b80`).
- **The PS3 bank is HCA; the Dreamcast gets ADX in an AFS**, as Sega's own
  Dreamcast games shipped music (`tools/mksound.py`). HCA's loop chunk gives
  the loop in blocks; less the encoder delay it is the ADX v3 loop in samples.
  Music is resampled 48 → 44.1 kHz so the mix needs no resampler for it.
  The host test of `dc_sound.h` decodes bit for bit what ffmpeg does.
- **The audio callback must never read the disc.** The pager reads it with
  the GD-ROM syscalls from the board's slow path; a second reader would
  collide. Music is read in the main loop into a 128 KB ring (2.6 s); effects
  (1.1 MB) are loaded at boot, through KOS's driver, before the pager is up.
- **SDL2 for KOS is GPF's fork** (`dreamcastSDL2`); kos-ports only has SDL
  1.2. Its audio driver feeds KOS's `snd_stream` at any rate in S16.

## The i960 on the SH-4 (#360)

Three switches, all off for every other target, all byte-identical to them
(`det_digest --cpu`, 4000 frames of attract: HEAD, the host build and a host
build with the DC's switches give the same file). Measured as the run loop's
emulated time over the same frames 0-599 of attract, Flycast's dynarec:

| switch | frames 0-599 | |
|---|---|---|
| none | 55.9 s | |
| `MEM_COUNT=0`: no 64-bit bus tallies | 54.3 s | -2.9% |
| `MEM_LE_DIRECT=1`: aligned words in one load | 52.1 s | -6.8% |
| `IRQT_COUNT_T=int32_t`: 32-bit timer counts | 51.0 s | -8.8% |

What the SH-4 taught, which holds for any 32-bit target:

- **GCC for the SH-4 does not fold byte-wise little-endian loads into one
  load**, as it does on x86 and ARM. An instruction fetch was 40 SH-4
  instructions. **And an aligned fast path beside the byte path is not
  enough**: GCC proves both compute the same value, merges them and keeps the
  bytes. An empty `asm` on the pointer (`MEM_OPAQUE`) keeps the load.
- **64-bit arithmetic is several instructions and two stores.** A `uint64_t`
  counter bumped per access or per instruction costs more than it looks; the
  timer update was ~45 SH-4 instructions an i960 step.
- **Compare scenes, not seconds.** ns per i960 instruction ranges 640-850
  across attract's scenes, so a single screenshot's figure says nothing about
  a change. Latch a total at a fixed frame: the board is deterministic, so
  frames 0-599 are the same work in every build.
- **Flycast's timing.** Its dynarec charges issue cycles with dual issue, plus
  2 cycles (5 with the MMU on) for each of a block's first 3 memory accesses.
  Its interpreter charges external bus cycles for every access, about 10 times
  too slow, but runs one instruction at a time: a PC sampled every N
  instructions there (a local Flycast patch, not committed) is an exact
  instruction count, where the dynarec only knows blocks.

## Toward 30 fps (#370)

The goal was 30 board fps. It is not reached: attract went from 6.2-7.3 to
10.0 fps, a fight runs at 6.4-7.7, depending on the stage and the camera. What each
change did, and what a fight frame now is:

- **A decoded-block cache for the i960** (`I960_BLOCKS`, `src/core/i960_blocks.h`,
  off on every other target). Runs of plain instructions (ALU, loads and stores,
  compares, branches) are decoded once into ops that point at their registers
  and replayed. A block runs only when nothing can happen between its
  instructions that the run loop would see. The header has the rules.
  `det_digest --cpu` over 4000 frames is byte-identical with it on.
- **A frame's code does not fit the table, so evict on misses, not on the
  first one.** STF runs most of its code once a frame. Replacing a block on
  every miss rebuilt the hot ones again and again. A slot now goes to new code
  only after it has missed `IB_MISSES` (8) times in a row; until then the
  newcomer is stepped. With 2048 slots and a 16k-op pool, the pool no longer
  flushes. Attract went up ~10%.
- **No netplay on the Dreamcast** (`M2_NO_NETPLAY`, `src/net/netplay.h`). The
  calls the run loop makes are inline no-ops, and the rollback state is gone:
  676 KB of BSS, given back to the mesh cache.
- **Publish only what changed** (`geo_dirty_mark`, memory.h). Every frame copied
  all 384 KB of texture and polygon RAM into the published copy, to change a
  few hundred words. The COP writes now note the span they touched, and the
  publish copies that. That was ~3% of a fight. Bufferram's 128 KB snapshot is
  still whole: the i960 and the SHARC write it directly.
- **The serial port, even with nobody listening.** dbgio output was ~2% of a
  fight (`scif_write`). The stats are on the screen, so dbgio goes to `null`
  unless `-DDC_STATS_DBGIO=1`.
- **Sound.** Music is 44.1 kHz, so it is copied without the resampler's
  per-sample bookkeeping. The ADX decode reads its nibbles with a shift and
  copies a mono channel once per frame. Both give the same samples as
  before, bit for bit (a host harness, underruns and loops included). Sound
  was ~7.5% of a fight before them; they were not profiled on their own.
- **`floor()` on doubles in the 3D mesh key.** That is a libm call on the
  SH-4. `geo3d_round4096` gives the same value in float.

A fight frame is ~150 ms: ~75 ms of i960 (the slice) and ~60-75 ms of 3D
(decode and submit). By PC samples: the block replay and the run loop ~35%,
the PVR face path (`dp_face*`, `dp_shade`, `pvr_prim`) ~25%, the 3D decode
~10%, sound ~7.5%, memcpy ~4%. A host profile has ~32k i960 instructions really
executing a frame (110k steps, counting the idle loop's skipped spins), at
~1.5-2 us each on the SH-4.

**30 fps would need a 33 ms frame.** The i960 alone, with the 3D free, would
be ~13 fps. There is no single hot spot left; each remaining target is a
few percent. What it would take:

- **An i960 to SH-4 recompiler**, not a faster interpreter. #386 built a
  prototype and measured it: on its own it does not get there (below).
  [I960-SH4-RECOMPILER.md](I960-SH4-RECOMPILER.md) (#491) sums up what a
  real one would take and why the two CPUs make it hard.
- **Skip the 3D on alternate frames** (render every other board frame): ~9 fps
  in a fight, and smoother play rather than more frames shown.
- **The PVR face path**: the store queues for vertex submission, and `ftrv`
  for the transform.

## The recompiler prototype (#386)

`I960_JIT=1` (`make JIT=1`, off by default) compiles each decoded block to
straight SH-4 code (`src/core/i960_jit_sh4.h`; the header has the rules).
It is correct: `make JIT=1 JIT_TEST=400` runs 400 random blocks both ways on
the Dreamcast and compares every register, the condition code and memory
(400/400), and attract's frame hash matches the interpreter's. But it does not
make the board faster, and a better one would not reach 30 fps either.

**What the i960 does** (`det_digest --census F0:F1:FILE`, an `I960_BLOCKS` host
build, 3000 frames of attract, 113k steps a frame):

- 33% of the steps are the idle loop at `0x11610`, which the spin hook
  already skips without running it. 65% are instructions a block can take, 2%
  others.
- A run of block-able instructions is 1.9 long on average. Half of all steps
  are runs of one (the idle loop's other instruction), and 85% sit in runs
  under 64. There is no long straight code for a compiler to win on.
- 5.9% of instructions touch something other than plain memory, nearly all
  of it stores to `0x008xxxxx`, the COP FIFO and the GEO.

**Where an attract slice goes on Flycast**, timed with TMU2's raw count
(`EXTRA=-DIB_WHY`, 80 ns a count):

| | ms a slice |
|---|---|
| the i960 loop | 44 |
| blocks (`ib_run`) | 29 |
| of which the bus's slow path (COP / GEO: 4585 stores, 1689 loads) | 13 (not so: see #392) |
| interpreted steps | 4 |
| hooks | ~0 |
| 3D (decode + submit) | ~42 |

**The compiled code against the replay**, ns per i960 op on Flycast:

| block | `ib_run` | compiled |
|---|---|---|
| 16 register `addo`s, 5000 runs | 309 | 96 |
| the self-test's random blocks | 330-340 | 306-310 |
| attract, all blocks | 29 ms | 34 ms |

The compiled code is 3x faster on pure register work, and that work is a
small part of a real block. Real blocks are one or two instructions, so
entering and leaving one costs more than its ops. The stores call the same C
handlers as before. The 384 KB code buffer fills and flushes: by frame 1152
it had flushed 224 times, compiled 197k blocks (7 s of compiling) and made
4.7M slow-path calls. With the JIT on, the i960 took 61 ms a slice against 44.
In the same 170 s, attract reached frame 1152 with the JIT and 3299 without.

**So 30 fps is not a recompiler's to give.** A perfect one that made every
block free would save at most the ~16 ms of block work outside the bus. The
i960 would still need ~28 ms a slice and the 3D ~42 ms, against a 33 ms frame.
What would move the frame, in order:

- **The 3D**: the PVR face path (store queues, `ftrv`) or drawing every other
  board frame.
- **The COP / GEO store path**: 12 ms a slice for ~4600 stores, ~2.6 us each.
  #392 measured this again and it is not so; see below.
- **Then a recompiler**, if one is still wanted. It would have to keep
  registers in SH-4 registers across blocks, chain blocks without going back to
  the run loop, and have a bigger buffer or a cheaper flush. The prototype does
  none of these.

## The face path and the bus, measured (#392)

Attract now runs at 10-12 board fps (from ~10). A fight was not measured again.

- **Faces go straight into the store queues** (`dp_vertex`, `dp_hdr`: KOS's
  direct rendering, `pvr_dr_target` / `pvr_dr_commit`). `pvr_prim`'s call and
  copy were ~4% of a frame.
- **A texture keeps its compiled header** (`dc_tex_t.hdr`, `hdr_var`). A header
  is compiled once for each texture, list and mirror/transparent variant, not
  every time the texture changes. The untextured and checker headers are
  compiled once (`g_dp_hdr_plain`).
- **The last face's texture lookup and colour are remembered** (`g_dp_memo`).
  The two halves of a quad sit side by side in the sorted order, and a model's
  faces share textures and lights.
- **A face with no corner near the eye skips the clipper.** Its three corners
  are projected straight out, with the screen scale folded into the run's
  projection (`dp_proj_t`).
- **`DC_DRAW_EVERY=2`** (`make EXTRA=-DDC_DRAW_EVERY=2`; default 1) draws every
  other board frame. The board runs every frame, and the frames between are
  never decoded. Attract runs at 13-15 board fps then, with half of them
  shown.
- **The frame-hash line is drawn once.** It was redrawn every slice, at a
  24-row memset plus bfont each time. The slice total to frame 1200 fell 5%
  (66.9 s to 63.5 s), and the hash is unchanged.
- `geo3d_cull_code` skips a plane's tolerance when the distance is not
  negative. That gives the same codes: the tolerance is never negative, and a
  NaN fails both tests.

**The bus is not the bottleneck.** #386's 13 ms a slice for the COP / GEO
stores (2.6 us a store) was an artifact of the timer. Flycast's TMU count only
moves at the edges of its own dynarec blocks, so a short region timed with it
takes whatever block it lands in. Two measurements that do not depend on it:

- **An A/B of the slice total to frame 1200** (`HASH_FRAME=1200`, the frame
  hash identical). A fast path that took the COP and GEO stores without the
  block's sync gave 66,890 ms against 66,872 without it. Both runs repeated to
  the millisecond. It was not kept.
- **An instruction-count profile.** Flycast's interpreter was patched to sample
  the PC every 97 SH-4 instructions, over attract frames ~1200-1330. Audio
  runs in real time, so its share is overstated.

| | % of SH-4 instructions |
|---|---|
| `ib_run` (the block replay) | 11.5 |
| `adx_next` + `adx_mix` + `ds_callback` (sound) | 20.9 |
| `geo3d_decode_model_cached` | 7.5 |
| `dp_face` | 5.1 |
| `emu_slice_body` | 4.1 |
| `emit_tri_uv` | 3.4 |
| `dp_sort` | 2.3 |
| `i960_step_core` | 2.2 |
| `geo3d_flat_depth` | 2.2 |
| `s24_draw_tilemap` | 2.1 |
| `ib_lookup` | 1.8 |
| `apply_matrix` | 1.7 |
| `mem_find_region` | 1.6 |
| `cop_write` + `sharc_exec` | 2.1 |

Leaving sound out, the i960 is ~32%, the 3D ~37% and the bus and COP ~4%. The
GEO publish (`geodl_publish`: the 128 KB bufferram snapshot, about once a
frame) is ~2 ms a slice.

**So a recompiler is not worth building now.** If it made every block free, it
would save the i960's block work and not the rest: at most ~25% of a frame,
about 10 to 13 fps. Keeping the i960's registers in SH-4 registers and chaining
blocks is a large piece of work, with all of attract's state to keep exact, and
30 fps is out of its reach. 30 fps does not look reachable on this design.
What is left is a few percent at a time: the mesh decode
(`geo3d_decode_model_cached`, `emit_tri_uv`) and drawing every other frame.

## The program ROM compiled ahead of time (#394)

STF's i960 code is compiled to C on the host, built into the program, and run
in place of the interpreter (`src/core/i960_aot.h`, `tools/i960_aot.py`;
`make AOT=<PS3>/stf_rom/rom_code1.bin`). `dreamcast/sfight.aotmap` lists the
code `sfight_console` runs in attract and a scripted fight
(`det_digest --aot-map`), by address and weight only. The generator reads the
instructions out of the player's own ROM into `$(OUT)/aot_gen.h`, which holds
ROM words and is never committed. The chunks are 2^n bytes of code each: a
switch over the blocks that start there, then the blocks. The common
instructions are C with their operands decoded. The rest call the
interpreter's `i960_exec_word` out of line.

**The board cannot tell.** Each block runs only if its cycles end before the
timers' horizon and its instructions end inside the slice, as in the block
runner (`i960_blocks.h`). An access off plain memory, or an instruction that
talks to the board, first brings the interpreter's state up to date
(`aot_slow`), and stops the run if the attention word, the interrupt lines or
the horizon moved. A hook's address is never compiled. `det_digest --cpu` on
the host is identical with and without the AOT code: 6000 frames of attract
and 3200 of a scripted fight. On the Dreamcast, the frame-1500 hash is
unchanged (88ddb134).

| attract to frame 1500 | i960 slices | draws (count) | heap left |
|---|---|---|---|
| interpreter + blocks | 81.8 s | 41.1 s (1427) | 512 KB |
| AOT, `AOT_COVER=0.9` | 66.8 s | 41.2 s (1424) | |
| AOT, `AOT_COVER=0.98` (the default) | 54.6 s | 41.9 s (1421) | 128 KB |

`AOT_COVER` is the share of the map's weight compiled, hottest first. All of
it is 4.8 MB of SH-4. 0.98 is 1.0 MB more than the interpreter's build, and
0.9 is 0.4 MB more.

The room for it came out of the heap:

- **The mesh cache is one static arena** (`GEO3D_MESH_ARENA`, 768 KB),
  bump-allocated and cleared whole when full. It used to be `malloc` per mesh
  under a byte cap. With the heap short, a failed build left the slot
  `failed`, and that model was decoded in full at every draw: an AOT build's
  draws went from 41 s to over 70 s. A full arena now clears and builds again
  once.
- **The page tables cover only what lies below the framebuffer**
  (`MEM_PAGES` 0x1300, `MEM_PAGE()`; 470 KB). Nothing above 0x12C00000 is
  plain memory. The JIT keeps the full table (`#error` otherwise).
- **The block runner's pool is 2 KB with AOT** (`IB_POOL`), where the
  interpreter build keeps 16 KB.

**A build that does not fit halts at 0x77F8** ("COP self-test failed", the
i960 executing zeros at 0x77D0). This was the boot failure of builds whose
image was over ~11.3 MB, and it was not the pager: `mem_init` could not
allocate texture RAM, returned 0 with no region table, and the run went on
regardless. `main_dc.c` now stops there with "out of memory for the board's
RAM". A 1 MB pad in bss halted the interpreter's build the same way. KOS's heap
runs to 64 KB under the top of RAM (`mm_sbrk`, `THD_KERNEL_STACK_SIZE`). The
frame pool's sizing (`keep`) still stops at its 1 MB floor without checking
that the rest fits.

**Where a frame goes now** (frame 1500, AOT 0.98): 36 ms of i960 and 28 ms of
drawing for each board frame. The drawing breaks down as tile layers 6.5 ms,
the display-list scan and mesh decode 12.8 ms, the sort 1.8 ms and the PVR
submit 6.5 ms. 30 fps needs the two together under 33 ms, so neither half
alone reaches it. The draw side is where assets converted ahead of time can
help: meshes pre-decoded to strips, and textures pre-converted to `.pvr`.

### Handlers in service, and paged ROM loads (#459)

Two changes, each held by `det_digest --cpu` on the host (identical over 6000
frames of attract, with and without `--gems`) and the Dreamcast's frame-1500
hash (69890d7a since #458's audit, e746591c before it; unchanged either way):

- **An interrupt handler runs compiled too.** While a handler is in service
  the run loop took the slow path, which never entered the AOT code: 688 slow
  iterations a frame on the host, mostly VsyncScr and the timer handler.
  `aot_run` now takes a `floor`, the frame depth the handler returns to.
  `aot_x` and `aot_ret` stop the run once `frame_depth` is back at it
  (`aot_unwound`), so the run loop sees the return where it always has
  (`emu_service_sound_again`), and with a floor set `aot_slow` does not stop
  for a raised interrupt line, since none is taken in service.
  `emu_aot_in_service` lets it in only with nothing armed: no breakpoint,
  watchpoint, warn or unknown-COP trigger, no vblank or sound kick pending.
  Slow iterations went from 688 to 89 a frame.
- **A load from paged ROM is inline.** `AOT_ROMD` fell back to `aot_slow` for
  any address outside the resident regions, and on the Dreamcast all of
  program and data ROM is paged (`MEM_HOST_PAGED`). `AOT_ROMP` reads
  `bus->rd_page` directly when the page is in; a miss still goes the slow way
  and pages it in. `MEM_HOST_PAGING` is 0 on every other build, so the host's
  code is unchanged.

| f3500-3900 (400 frames) | total | i960 slices | draws |
|---|---|---|---|
| before #458 (b44) | 14948 ms | 5881 ms | 9007 ms |
| before #458, with both (b46) | 14494 ms | 5453 ms | 8981 ms |
| after #458 (b48) | 15995 ms | 6964 ms | 8963 ms |
| after #458, with both (b47) | 15519 ms | 6506 ms | 8942 ms |

The slice is 6.6-7.3% shorter. #458's audit made it ~1.1 s slower over these
400 frames on its own (the cmpr NaN fix and the Gems trap checks). What the AOT still misses is small: per host frame
~11.5k instructions compiled, 372 interpreted (a long tail, 0x19CD4 the most at
32) and 1285 in the block runner. The i960 slice is now mostly Gems C, hooks
and the COP, not interpretation. Two ideas were measured and dropped: inlining
`call` / `ret` (a few percent of the slice, but code size the heap cannot
spare) and a higher `AOT_COVER` (no memory for it).

## Flycast charges the MMU a third of the frame (#394, part 12)

Part 12's changes, each checked by the frame-1500 hash (88ddb134) and the
picture:

- **Tile RAM keeps a dirty bit per KB** (`memory.h` `tile_dirty`, set by the
  TILE and TILE_MIRROR writes; `tile_dirty_find_range`). The tile layer's
  redraw reads `bus->tile` directly and redraws only what changed, where it
  used to copy and compare a 56 KB snapshot twice.
- **MAIN_DATA loads in the AOT code read the window directly**
  (`AOT_ROMD`, `s_aot_md`), as program ROM loads already did.
- **COP FIFO writes in runs** (`cop_write_n`), a two-pass `dp_sort`, an
  unrolled outcode test (`geo3d_cull_code_d`), and `FSRRA` for 1/sqrt
  (`dc_math.h`).

Measured over frames 3500-3900 of attract (the stage-0 fight), in Flycast's
libretro core under RetroArch. `main_dc.c` draws the window's totals on
screen: i960 slices (sl), drawing (dr), and inside the drawing the tile
layers (ti), the scan and decode (sc), the sort (so) and the PVR submit (su).

| build | 400 frames | fps | sl / dr | ti / sc / so / su |
|---|---|---|---|---|
| part 11 (b33) | 27386 ms | 14.6 | 13713 / 13518 | 2136 / 9076 / 529 / 1615 |
| part 12 (b35) | 25194 ms | 15.9 | 12879 / 12162 | 688 / 9046 / 524 / 1753 |
| b35, Flycast's MMU charge at 2 cycles | 16050 ms | 24.9 | 7832 / 8150 | 438 / 6017 / 336 / 1298 |
| b36, MMU off, software paging | 17262 ms | 23.2 | 8595 / 8598 | 440 / 6438 / 344 / 1312 |

**The last row is Flycast, not the Dreamcast.** Flycast's cycle model
(`core/hw/sh4/sh4_cycles.cpp`, `countCycles`) charges the first three memory
ops of every block 5 cycles while its full MMU is on, and 2 otherwise. A real
SH-4 pays nothing extra for a UTLB hit. The full MMU is on only because
`pg_mmu_on` plants Windows CE's "SH-4 Kernel" signature, so that Flycast
translates at all. The row comes from a local Flycast patched to take the
charge from `$FLYCAST_MMU_MEMCYC` (reverted since). Everything else is
unchanged: the hash still matches, and screenshots every 40 s through
attract, select, two fights and the ranking draw as they should. (A white
frame in the first run was the title screen's flash.) The profile of b35
puts 40% in the 3D (decode_model_cached 19%, dp_frame 5%, dp_face 4.5%,
dp_tri_put 4%), 14% in AOT code, 10% in the interpreter, 9.5% in the COP and
8% in the AOT runtime.

So **in Flycast, a third of the measured time is the MMU**, on every memory
op of every block, heap and stack included. Turning MMUCR.AT off around phases
that never touch a window does not help: Flycast throws away all translated
code on every AT change (`CCN_MMUCR_write` → `ResetCache`), and checks for the
signature again.

### Page in software, with the MMU off

`tests/rom_touch.c` (now with a page-size override) over 4000 frames of
attract, in 4 KB / 64 KB pages:

| region | in 4000 frames | largest 60-frame window |
|---|---|---|
| bus MAIN_DATA | 2596 KB / 3840 KB | 1308 KB / 2176 KB |
| bus XTRA_DATA | 892 KB / 1216 KB | 464 KB / 640 KB |
| polygons | 3044 KB / 5376 KB | 768 KB / 1344 KB |
| textures (UV streams) | 1980 KB / 4800 KB | 444 KB / 1920 KB |

A steady fight window reads ~70 4 KB pages of MAIN_DATA, ~8 of XTRA_DATA
and 10-30 each of polygons and textures. The frame pool on the Dreamcast is
1792 KB, so 64 KB frames (28 of them) are too coarse. 16 KB frames would give
~110.

Who reads the windows (an audit of what `main_dc.c` compiles):

- **The i960, through the bus.** `rd_page` / `wr_page` (64 KB entries),
  the `mem_*_slow` paths, `mem_fetch2`, `ib_build`, `AOT_PG`, and `AOT_ROMD`'s
  flat `s_aot_rom` / `s_aot_md` spans. These are the bulk, and the
  page table already is a TLB: start a paged region's entries empty, let the
  slow path fault the page in and fill the entry, and clear it on eviction. A
  first write goes the slow way to mark the frame dirty and pinned. That needs
  `MEM_PAGE_SHIFT` (16 everywhere else, 14 here; tables ~150 KB at
  `MEM_PAGES` 0x13000000 >> 14) and the `& 0xFFFF` bounds tests made to follow
  it. `AOT_ROMD` falls back to `AOT_PG` (spans 0) on the Dreamcast.
- **geo3d, not through the bus.** The model table (`read_u32_le` at +0/+4/+8,
  every object every frame), `geo3d_tex_word`, and the 40-byte polygon walk
  in `geo3d_mesh_build` / `geo3d_decode_model`, which runs to an end marker in
  the data (up to 160 KB). The walk is the hard part. The plan is a converted
  asset: a tool run when the disc is built writes each model's polygon run and
  UV stream to one file with an index (made from the player's ROM, so never
  committed, like STF.AFS). A mesh build then reads one contiguous blob.
- **Boot only:** `sfight_install`'s PRCB reads, `geo3d_lookup_build`.

The disc reads stay `pg_read` (GD-ROM syscalls, polled). Without the MMU a
miss is an ordinary call on the emulator's thread, not an exception.

Expected: about the 24.9 fps of the b35 row with the charge at 2 cycles,
less the software faults. **Measured (b36): 17262 ms for the 400 frames, 23.2
fps, against 25194 ms with the MMU** (−31%). The frame-1500 hash is still
88ddb134, and the slices to frame 1500 took 22049 ms against 33700.
Screenshots every 40 s through attract (the Tails lab, select, the Death Egg,
two fights) draw as before. The pager's counters over the run: 15 disc loads
after boot, 6534 evictions, no pinned or written ROM pages, no read errors.
The frame pool came out at 1536 KB (96 frames), not 1792, because the heap
probe now leaves more free. It holds the fight's working set: the loads stop
once the stage is in.

The geo3d walk reads through `dc_rom_at`, not a converted asset. Its pages
are hot enough within a frame that the ring of recent frames is all it needs.

That is still short of 30. The rest has to come from the 3D (scan and decode
is 6438 of the 17262 ms): per-face FTRV for the normal and light, FIPR for the
dot products, and per-face attribute bits worked out once at mesh build.

### Where the software pager stands (#533, at ef71553)

Pin #533 asked for this section's plan again, from b35 (bbd7f8f). It was
already on the branch: the pager has run with the MMU off since cf18d60 (#443,
b36 above), and the 3D has read each mesh's polygon data from a file built with
the disc since STRIPS.PAK (#498). Nothing in `dreamcast/` sets MMUCR.AT any
more, so the bench was taken again and nothing was changed.

| build | 400 frames | fps | sl / dr | ti / sc / so / su | page loads |
|---|---|---|---|---|---|
| b35, MMU on, Flycast's default charge | 25194 ms | 15.9 | 12879 / 12162 | 688 / 9046 / 524 / 1753 | |
| b35, MMU on, charge at 2 cycles | 16050 ms | 24.9 | 7832 / 8150 | 438 / 6017 / 336 / 1298 | |
| ef71553, MMU off | 14049 ms | 28.5 | 5196 / 8798 | 459 / 7061 / 208 / 1056 | 567 (cd 280, da 56, sp 218) |

The ef71553 disc: full HUD, `FPS_CAP=0`, `HASH_FRAME=1500`, AOT 0.99, Gems C,
`HOST_MATH=1`, STRIPS.PAK, TEXTURES.PAK, STF.AFS, PS3 files; Flycast's libretro
core under RetroArch. The frame-1500 hash is 634d853f, as #509 and #530 had it
(b35's 88ddb134 predates the `HOST_MATH` and audit changes). With the MMU off
the two clocks the pin asked for are one number: Flycast charges a block's
first memory ops `mmu_enabled() ? 5 : 2` cycles (`sh4_cycles.cpp`), so the
penalty is never applied. The figures are Flycast's, which bills polygon bytes
(`scheduleRenderDone`), not pixels: not a Dreamcast's fps.

**What is left to 30** (13333 ms for the 400 frames, 716 ms off this run):
the draw is 8798 ms against the slice's 5196, and the 3D scan alone is 7061 ms,
half the frame. That is the transform, cull, light and per-face work over the
packed meshes, then the sort and the TA submit (1264 ms together). The pager
costs little here (567 loads, the strip pack's 218 among them), and the slice
is mostly Gems' C, the hooks and the COP. So the remainder is the 3D path's
per-face arithmetic, the next pin's work.

### The scan by function (#535, at 3e042a1)

3e042a1 is ef71553 plus a doc-only commit. The baseline of the unmodified
ef71553 disc on frames 3500-3900 was #533's to the millisecond: 14049 ms,
sl 5196 / dr 8798, ti 459 / sc 7061 / so 208 / su 1056, 567 page loads,
frame-1500 hash 634d853f. Flycast is deterministic here, so run-to-run noise
is 0 ms.

`make SCANSPLIT=1` (`dreamcast/dc_scansplit.h`, off by default) puts lap marks
at the ends of the functions inside `dp_decode`, the "sc" timer, and of the
steps of `geo3d_decode_model_cached`. A mark is one read of TMU2's count, the
clock KOS's timer runs on, and it is placed per model, never per face. The
default build (`SCANSPLIT=0`) runs the bench identically, every number and the
hash alike. The split build reads 14066 ms total (+17), sc 7057 (-4), hash
634d853f. That +17 ms is the marks' own cost. Its parts add up to 7057 ms,
the whole of its sc.

| function | ms | of the 7061 ms scan | of the 14049 ms frame |
|---|---|---|---|
| `geo3d_cached_draw_dc`'s face loop (1,216,211 faces) | 5026 | 71.2% | 35.8% |
| `geo3d_dc_verts` (projection) | 448 | 6.3% | 3.2% |
| `geo3d_decode_model` (1130 uncached draws) | 447 | 6.3% | 3.2% |
| `geo3d_cached_cull_codes` | 322 | 4.6% | 2.3% |
| the corners through the matrix (`ftrv`, 32,723 models) | 290 | 4.1% | 2.1% |
| `geo3d_mesh_for_draw` (42,401 draws) | 221 | 3.1% | 1.6% |
| `geo3d_scan_geo_list` and `dp_decode`'s setup | 210 | 3.0% | 1.5% |
| `geo3d_cached_sphere` / `_gone_carry` | 61 | 0.9% | 0.4% |
| `dp_decode`'s runs (grouping, `dp_cull_planes`) | 32 | 0.5% | 0.2% |
| `geo3d_decode_direct` | 0 | 0% | 0% |

One function is large enough: the face loop of `geo3d_cached_draw_dc` (5026 ms,
about 4.1 µs of Dreamcast time a face). A later cut of about 700 ms would be
14% of it. The face loop is the only part over 700 ms. The next largest
are 448 ms each.

### The face loop by branch (#537, at 938f675)

938f675 is #535's 3e042a1 plus the SCANSPLIT marks (PR #242, off by default).
A fresh baseline of the unmodified 938f675 disc (SCANSPLIT=0) on frames
3500-3900 is #535's to the millisecond: 14049 ms, sl 5196 / dr 8798,
ti 459 / sc 7061 / so 208 / su 1056, 567 page loads, frame-1500 hash 634d853f.

The face loop of `geo3d_cached_draw_dc` is one fused iteration, not several
passes. Each face goes through the flat key, the window test, the board cull,
the light, the diagonal and the submit in one pass, then the loop moves to the
next face. A lap mark between passes is therefore not possible, and a TMU2 read
per face would cost more than the ~800 ms being looked for. So this split is
counters only: `make FACECOUNT=1` (`dreamcast/dc_scansplit.h`, off by default)
adds one to a counter at each branch the loop already has, and prints the
totals over frames 3500-3900 on the HUD.

The default build (`FACECOUNT=0`) has the same instructions as 938f675; only
the version string in .rodata differs, and it benches the same, every number
and the hash alike. The counted build keeps hash 634d853f.
It reads 14314 ms (+265 ms, sc 7307, +246): that is the cost of the
increments. The counts do not depend on timing. Every face the loop enters
is counted in exactly one of drop-before-cull, rear, out-of-window quad and
submitted, and those four add up to the faces entered.

| counter | over 400 frames | a frame | of faces entered | of faces submitted |
|---|---|---|---|---|
| models drawn by the loop | 32,723 | 81.8 | | |
| faces entered | 1,216,211 | 3040.5 | 100% | |
| ...through the unpacked loop | 0 | 0 | 0% | |
| ...whose flat key compares corners (z mode 1/2) | 1,216,211 | 3040.5 | 100% | |
| triangle without C, skipped | 0 | 0 | 0% | |
| triangle out of the window, skipped before the cull | 47,699 | 119.2 | 3.9% | |
| dropped by the board cull (rear or link type 0) | 373,009 | 932.5 | 30.7% | |
| quad out of the window, dropped after the cull | 141,684 | 354.2 | 11.6% | |
| model wholly out (`gone`), dropped after the cull | 0 | 0 | 0% | |
| **submitted** | **653,819** | **1634.5** | **53.8%** | 100% |
| ...triangles | 156,931 | 392.3 | 12.9% | 24.0% |
| ...palette colour (`geo3d_palette_color`) | 653,819 | 1634.5 | 53.8% | 100% |
| ...specular (mode bit 0) | 509,802 | 1274.5 | 41.9% | 78.0% |
| ...quad cut the other way | 1,636 | 4.1 | 0.1% | 0.3% |
| ...colour memo missed (`dp_face_colour`, out of line) | 351,830 | 879.6 | 28.9% | 53.8% |
| ......untextured, shaded | 951 | 2.4 | | 0.1% |
| ......ramp | 350,879 | 877.2 | 28.9% | 53.7% |
| ......untextured lb<0, textured lb<0, palette | 0 | 0 | | 0% |
| ...texture key missed (`dp_tex_get`) | 32,586 | 81.5 | 2.7% | 5.0% |
| ...strip clipped by the window | 772 | 1.9 | 0.1% | 0.1% |
| ...dropped at `GEO3D_MAX_TRIS` | 0 | 0 | 0% | 0% |

The loop's steps in source order (packed loop; every face went through it):

1. prefetch the face two ahead;
2. triangle or quad, the four z sources;
3. the flat key, `geo3d_flat_z` (z mode 1/2 compares corners);
4. a triangle without C is skipped;
5. the window test, the AND of the corners' outcodes;
6. a triangle out of the window is skipped;
7. `geo3d_board_cull`: the normal (`geo3d_board_normal`), N·L and N·P, and
   the rear / link-type-0 drop;
8. out of the window or `gone`: a quad records its diagonal
   (`geo3d_split_other_way`) and is skipped;
9. `geo3d_palette_color`;
10. `geo3d_board_luma`, with specular when mode bit 0 is set;
11. the diagonal, `geo3d_split_other_way`;
12. `geo3d_dc_sface`: `dp_tex_key` (memo, else `dp_tex_get`), `dp_face_col`
    (memo, else `dp_face_colour`), `dp_strip_put` (min z, bounding box,
    clip flag, sort key);
13. the key, `geo3d_board_zkey`.

The submitted branch (steps 9-13) is the one large enough. It runs for 653,819
faces, 53.8% of those entered, and it is the only branch that does any work
after the cull. ~800 ms from it is 1.22 µs a submitted face. The average face
in the loop takes 4.13 µs. The faces the loop throws away (562,392) stop at or
before the cull. To give 800 ms they would have to cost 1.42 µs each, and rear
faces alone 2.14 µs each. These are counts, not timings: no time was read
inside the loop. Within the submitted branch, the largest sub-branch by count
is the colour memo's miss: 351,830 out-of-line calls to `dp_face_colour`, all
but 951 by the ramp path. No rare branch is large. The unpacked loop, a
triangle without C, `gone`, palette colour misses and `GEO3D_MAX_TRIS` drops
never happen, and only 772 faces are clipped.

### The face loop's tail against its prefix (#539, at 3a45608)

HEAD had moved to 3a45608: 938f675 plus #537's FACECOUNT counters (off by
default) and #244, which moves `vendor/gems-c` to the PS2 build's
`Fn_area_table_gen` (0x3A) and `Fn_outside_ball` (0x72). Those two are in the
fight's collision chain, so the attract fight plays out differently after
frame 1500 and the bench moved a little. The frame-1500 hash did not:

| build, frames 3500-3900 | total | sl / dr | ti / sc / so / su | page loads | hash |
|---|---|---|---|---|---|
| #537's table (938f675) | 14049 | 5196 / 8798 | 459 / 7061 / 208 / 1056 | 567 | 634d853f |
| 3a45608, unmodified | 14060 | 5226 / 8769 | 453 / 7040 / 217 / 1046 | 567 | 634d853f |
| 3a45608 with the gems-c of 938f675 | 14049 | 5196 / 8798 | 459 / 7061 / 208 / 1056 | 567 | 634d853f |

The second row is the baseline for the subtraction below. The third row shows
that #244 accounts for the whole move.

`make FACESTOP=1` (`GEO3D_FACESTOP`, geo3d.h, off by default) runs steps 1-8
of the loop (above) for every face, as the default build does, then stops a
face that would have gone on to step 9. No palette, luma, specular, diagonal,
`geo3d_dc_sface` (texture key, colour, strip put) or z-key. The
prefix is kept alive by a running xor of the flat key's z and the cull's N·L
and N·P, taken for every face that passes the cull, and stored to a volatile
once per model (`g_geo3d_facestop_sink`). Whether a face reaches the xor
depends on the window test and the cull, so neither can be dropped. Proof
from the disassembly (everything is inlined into `dp_decode`): the FACESTOP
build keeps the light vector's three loads and the cull's arithmetic (61
fmul, 57 fmac against the default's 63 and 48) and the `flat_prev_z` stores,
and its calls to `dp_face_colour` and `dp_tex_get` are gone. Neither build
has a timer mark (SCANSPLIT=0), so no mark cost is in either number.

| frames 3500-3900, SH-4 timer time | total | sl / dr | ti / **sc** / so / su | hash |
|---|---|---|---|---|
| 3a45608, default | 14060 | 5226 / 8769 | 453 / **7040** / 217 / 1046 | 634d853f |
| 3a45608, FACESTOP=1 | 9099 | 5216 / 3838 | 456 / **3277** / 41 / 57 | 634d853f |

**Tail (steps 9-13) = 7040 − 3277 = 3763 ms** of scan time. The fall in sort
(217 → 41) and TA submit (1046 → 57) is outside the scan: nothing was queued.
It is not counted in the tail. The FACESTOP picture is wrong, as intended. Its
frame-1500 hash stays 634d853f because that hash is board state, which the
draw never writes.
The default build with the switch present has the same instructions as
3a45608 and benches the same, every number and the hash.

The prefix is then about 1.26 s of the loop: #537's 5026 ms face loop
less 3763. That is ~1.0 µs a face for steps 1-8 over 1,216,211 faces, against
5.8 µs a submitted face for steps 9-13 over 653,819. The subtraction may
overstate the tail by a little, since a loop without the tail also leaves the
cache to the prefix. That cannot move a 3.8 s answer across the 1200 ms line.

**Verdict: the tail holds it** (3763 ms, over 1200). The prefix cannot buy 30
fps alone. The next suspect is `dp_face_colour`'s ramp miss (350,879 out-of-line
calls over these 400 frames, #537). It was not instrumented here.

### The tail split at `geo3d_dc_sface` (#540, at 3a45608)

HEAD was still 3a45608. The default baseline matched #539's default row in
every number and the hash, so the FACESTOP row below is #539's, not re-run.

`make TAILHALF=1` (`GEO3D_TAILHALF`, geo3d.h, off by default, not combined
with FACESTOP) runs steps 1-11 for every face that reaches them: the prefix,
then `geo3d_palette_color`, `geo3d_board_luma` (specular and texture LOD
included) and the diagonal (`geo3d_split_other_way`). It then stops before
step 12: no `geo3d_dc_sface` / `geo3d_dc_face`, so no colour memo or
`dp_face_colour`, texture key, strip put or z-key. The sink is FACESTOP's
running xor, now fed with the face's palette r, g, b, its luma, the texture
LOD `geo3d_board_luma` leaves in `g_geo3d_emit_texlod`, and the cut. It is
stored to the same volatile once per model, never per face. Proof from
`dp_decode`'s disassembly: the TAILHALF build still loads palette RAM,
`g_geo_rs` (texparam, coef, logram) and the light vector, and has more FP
work than the default (75 fmul, 61 fmac, against 63 and 48: the default
calls its colour and texture work out of line). Its calls to
`dp_face_colour` and `dp_tex_get` are gone. No build here has timer marks.

| frames 3500-3900, SH-4 timer time | total | sl / dr | ti / **sc** / so / su | hash |
|---|---|---|---|---|
| 3a45608, default | 14060 | 5226 / 8769 | 453 / **7040** / 217 / 1046 | 634d853f |
| 3a45608, TAILHALF=1 | 9705 | 5207 / 4454 | 463 / **3885** / 41 / 59 | 634d853f |
| 3a45608, FACESTOP=1 (#539) | 9099 | 5216 / 3838 | 456 / **3277** / 41 / 57 | 634d853f |

- **Steps 9-11** (palette, luma, specular, diagonal) = 3885 − 3277 = **608 ms**
- **Steps 12-13** (`geo3d_dc_sface`: colour, texture, strip put, z-key) =
  7040 − 3885 = **3155 ms**
- Sum 3763 ms, #539's tail to the millisecond. TAILHALF's sc is above
  FACESTOP's, so the prefix still ran.

So and su fall as with FACESTOP, because nothing was queued; that is outside
the scan and not in either half. The TAILHALF picture is wrong, as intended;
its frame-1500 hash stays 634d853f because that is board state. The default
build with the switch present has the same instructions as 3a45608 and benches
the same, every number and the hash.

**Verdict: the submit side holds it.** Steps 12-13 are 3155 ms (over 1200),
steps 9-11 608 ms (under 700). The next pin's target is
`geo3d_dc_sface` and what it calls (colour, texture, strip put, z-key) as a
whole; nothing inside it was opened here. Lighting cannot buy 30 fps alone.

### Step 12's lookups against strip put + z-key (#541, at b98a3f0)

HEAD was b98a3f0 (#540's TAILHALF commit, PR #246), unmoved. The default
baseline, every switch off, matched #540's default row in every number and
the hash.

`make STRIPSTOP=1` (`GEO3D_STRIPSTOP`, geo3d.h, off by default, not combined
with FACESTOP or TAILHALF) runs the face loop through steps 1-11 and step 12's
lookups: `dp_tex_key` (`dp_face_tex` unpacked), with `dp_tex_get` on a miss,
and `dp_face_col`, with `dp_face_colour` on a miss, ramp included
(`geo3d_dc_sface_look` / `geo3d_dc_face_look`, dc_pvr.h). It then calls no
`dp_strip_put` / `dp_tri_put` and does not run step 13 (`geo3d_board_zkey`).
Faces that returned before step 12 still do. The sink is the same running
xor: the texture pointer the key returns, the colour memo `dp_face_col` leaves
(base, offset, palette) and the cut. It is stored to the volatile once per
model. Proof from `dp_decode`'s disassembly against the default build:
`dp_face_colour` and `dp_tex_get` are still called (2 references each, as in
the default). `dp_tri_put` has no symbol at all, and the inlined
`dp_strip_put`'s stores are gone (`g_dcf_key` and `g_dcf` 1 → 0, `g_dcf_n`
5 → 1). The function goes from 13652 to 13096 bytes.

| frames 3500-3900, SH-4 timer time | total | sl / dr | ti / **sc** / so / su | hash |
|---|---|---|---|---|
| b98a3f0, default | 14060 | 5226 / 8769 | 453 / **7040** / 217 / 1046 | 634d853f |
| b98a3f0, STRIPSTOP=1 | 11164 | 5235 / 5871 | 453 / **5311** / 41 / 57 | 634d853f |
| 3a45608, TAILHALF=1 (#540) | 9705 | 5207 / 4454 | 463 / **3885** / 41 / 59 | 634d853f |

- **Lookups** (colour and texture: `dp_face_col` / `dp_face_colour`,
  `dp_tex_key` / `dp_tex_get`) = 5311 − 3885 = **1426 ms**
- **Strip put + z-key** (`dp_strip_put`, `geo3d_board_zkey`) =
  7040 − 5311 = **1729 ms**
- Sum 3155 ms, #540's steps 12-13 to the millisecond. STRIPSTOP's sc is above
  TAILHALF's, so steps 9-11 still ran, and below the default's, so the skip
  happened.

So and su fall because nothing was queued; that is outside the scan and in
neither half. The STRIPSTOP picture is wrong, as intended; the frame-1500
hash stays 634d853f because that is board state, and it proves nothing here.
The default build with the switch present is 8 bytes longer than b98a3f0's
(the layout shifts), and benches the same, every number and the hash.

**Verdict: both halves are over 700 ms.** Strip put + z-key is 1729 ms,
the lookups 1426 ms. The larger, strip put + z-key, is the next pin's target;
no function inside either half was opened here.

### Step 12's strip put against step 13's z-key (#542, at a351476)

HEAD was a351476 (#541's STRIPSTOP commit, PR #247), unmoved. The default
baseline, every switch off, matched #541's default row in every number and
the hash.

The z-key is not computed inside `dp_strip_put`. The face loop calls
`geo3d_board_zkey(z)` itself, as the key argument of `geo3d_dc_sface` /
`geo3d_dc_face`, so it runs just before the strip put rather than after it.
`dp_strip_put` (and `dp_tri_put`) only computes a key of its own when handed
a negative one, which happens only when `flat` is off. On the Dreamcast
`flat` is always on, because dc_pvr.h sets `g_geo3d_flat_list` around the
whole draw.

`make ZKEYSTOP=1` (`GEO3D_ZKEYSTOP`, geo3d.h, off by default, not combined
with the other switches) runs the face loop through `geo3d_dc_sface` /
`geo3d_dc_face` whole: the lookups, the strip put and its stores to `g_dcf` and
`g_dcf_key`. It hands them key 0 where `geo3d_board_zkey`'s key went. Faces
that returned before step 12 still do. The flat z, which only the key read,
goes into the running xor and is stored to the volatile once per model.
Proof from `dp_decode`'s disassembly against the default build:

- The `g_dcf_key` / `g_dcf` / `g_dcf_n` references are the default's, 7 and
  7. The pack `~(slice << 29 | q << 13 | 0x1FFF − t)` is stored to
  `g_dcf_key` at `8c03fd5c`, where STRIPSTOP had no store at all.
- `g_geo3d_zadjust` is touched 4 times in the default build: the per-model
  store and three inlined `geo3d_board_zkey` bodies (the packed and unpacked
  loops, and `dp_strip_put`'s fallback). ZKEYSTOP touches it twice: the store,
  and one body that sets `q = 0`, tests `flat` and branches into the body
  only when it is off. That is the fallback, never taken here. Neither loop's
  z-key is left.
- The function goes from 13652 to 13272 bytes.

`geo3d_flat_z` runs in both builds (and in STRIPSTOP's, where nothing sank
z): it stores `g_geo3d_flat_prev_z` for the next face.

| frames 3500-3900, SH-4 timer time | total | sl / dr | ti / **sc** / so / su | hash |
|---|---|---|---|---|
| a351476, default | 14060 | 5226 / 8769 | 453 / **7040** / 217 / 1046 | 634d853f |
| a351476, ZKEYSTOP=1 | 13828 | 5194 / 8568 | 456 / **6845** / 214 / 1042 | 634d853f |
| a351476, STRIPSTOP=1 (#541) | 11164 | 5235 / 5871 | 453 / **5311** / 41 / 57 | 634d853f |

- **Strip put** (`dp_strip_put` / `dp_tri_put`, with `geo3d_dc_sface`'s
  call around it) = 6845 − 5311 = **1534 ms**
- **Z-key** (`geo3d_board_zkey`) = 7040 − 6845 = **195 ms**
- Sum 1729 ms, #541's strip put + z-key to the millisecond. ZKEYSTOP's sc is
  above STRIPSTOP's, so the strip put ran (and the lookups were not deleted),
  and below the default's, so the skip happened.

The strips were queued: so / su are 214 / 1042, not the 41 / 57 of a build
that queues nothing. They are not in the subtraction. The keys are all 0, so
the picture's order is wrong, as intended. The frame-1500 hash stays 634d853f
because it is board state, and it proves nothing here.
The default build with the switch present (286b0d4) has `.text` and `.data`
byte-identical to a351476's; only the version string differs. It benches the
same, every number and the hash.

**Verdict: strip put is the larger time.** It is 1534 ms, against 195 ms for
the z-key. It was not opened here. The lookups (1426 ms, #541) can be split
by a skip; a fused strip put cannot. So the next pin splits the lookups, not
the strip put.

### Step 12's lookups: texture against colour (#544, at f52431b)

HEAD was f52431b (#542's ZKEYSTOP doc commit, PR #248), unmoved. The default
baseline, every switch off, matched the default row in every number and the
hash.

The two lookups are separate in the source. `geo3d_dc_sface` calls
`dp_tex_key(F->tex)` first, which returns the memo on a hit and calls
`dp_tex_get` on a miss. It then calls `dp_face_col`, the colour memo compare,
which calls `dp_face_colour` on a miss, and then `dp_strip_put`. `dp_face_col`
takes only whether a texture was found, not the pointer, and no colour is
worked out inside the texture call.

`make COLOURSTOP=1` (`GEO3D_COLOURSTOP`, geo3d.h, off by default, not combined
with the other switches) runs a drawn face through the texture lookup alone:
`dp_tex_key` for a packed face, and `dp_big_window` + `dp_face_tex` for an
unpacked one. It then stops: no `dp_face_col` / `dp_face_colour`, strip put or
`geo3d_board_zkey`. The face's colour, luma, texture LOD, cut and the texture
pointer the lookup returned (hit or miss) go into the running xor, which is
stored to the volatile once per model. Proof from `dp_decode`'s disassembly
against the default build:

- `dp_tex_get` is still called twice, once in each loop. The packed loop's
  memo compare (`cmp/eq` against `g_dp_memo` +24, `8c0402a2`) reads +28 on a
  hit and calls `dp_tex_get` on a miss, storing both words back. The unpacked
  loop keeps its float compares against the memo ahead of its call.
- `dp_face_colour` is not referenced in `dp_decode`, against twice in the
  default build. The one reference left in the binary is in
  `geo3d_emit_tri_uv`, which the uncached decoders call
  (`geo3d_decode_direct` / `_model`), as in the default build.
- `dp_decode` has no reference to `g_dcf` or `g_dcf_key`, against one each in
  the default build.
- The colour (r, g, b) and luma words, the LOD, the cut and the texture
  pointer are xored into the running word at `8c03f7fc`-`8c03f810`.
- The function goes from 13652 to 12864 bytes.

| frames 3500-3900, SH-4 timer time | total | sl / dr | ti / **sc** / so / su | hash |
|---|---|---|---|---|
| f52431b, default | 14060 | 5226 / 8769 | 453 / **7040** / 217 / 1046 | 634d853f |
| f52431b, COLOURSTOP=1 | 9813 | 5201 / 4563 | 461 / **3996** / 41 / 57 | 634d853f |
| a351476, STRIPSTOP=1 (#541) | 11164 | 5235 / 5871 | 453 / **5311** / 41 / 57 | 634d853f |
| 3a45608, TAILHALF=1 (#540) | 9705 | 5207 / 4454 | 463 / **3885** / 41 / 59 | 634d853f |

- **Texture lookup** (`dp_tex_key`, `dp_tex_get` on a miss) =
  3996 − 3885 = **111 ms**
- **Colour lookup** (`dp_face_col`, `dp_face_colour` on a miss) =
  5311 − 3996 = **1315 ms**
- The sum is 1426 ms, #541's lookups. Both halves are taken from the same
  COLOURSTOP figure, so the sum is exact by construction. What it does show
  is that sc landed between the bounds. 3996 is above TAILHALF's 3885, so the
  texture lookup ran and steps 9-11 were not deleted. It is below STRIPSTOP's
  5311, so neither the colour lookup, the strip put nor the z-key ran.

so / su fell to 41 / 57, as in any build that queues nothing. They are not in
the subtraction. Nothing is drawn, so the picture is wrong, as intended. The
frame-1500 hash stays 634d853f because it is board state, and it proves
nothing here. The default build with the switch present (67611ab) matches
f52431b's byte for byte in every section except `.rodata`. There, 6 bytes
differ: the version string.

**Verdict: the colour lookup is the half over 800 ms.** It is 1315 ms, against
111 ms for the texture lookup. It was not opened here. The next pin can skip
`dp_face_colour` and keep the memo hit.

### The colour lookup: memo hit against dp_face_colour (#546, at 84c31a0)

HEAD had moved from f52431b to 84c31a0: #544's COLOURSTOP commit and its two
doc commits on top. The default baseline, every switch off and built from
84c31a0, matched the default row in every number and the hash.

The memo is outside `dp_face_colour`. `dp_face_col` (dc_pvr.h) compares the
face's r, g, b, luma, palette level and two flags against `g_dp_memo` and
calls `dp_face_colour` only on a miss, before `dp_strip_put`. No function was
split for this.

`make FACECOLSTOP=1` (`GEO3D_FACECOLSTOP`, geo3d.h, off by default, not
combined with the other switches) runs a drawn face as COLOURSTOP does
(`dp_tex_key`, `dp_tex_get` on a miss) and then through the colour memo
compare (`dp_face_col_memo`, dc_pvr.h). On a hit it reads the memo's colour
(base, offset, palette). On a miss it stores the key into the memo, as
`dp_face_col` does, so the same faces hit as in the default build. It calls
no `dp_face_colour`, and so answers the key that missed. There is no strip
put or `geo3d_board_zkey`. That word, the texture pointer and TAILHALF's
colour, luma, LOD and cut words go into the running xor, which is stored to
the volatile once per model. Proof from `dp_decode`'s disassembly against the
default build:

- `dp_face_colour` is not referenced in `dp_decode`, against twice in the
  default build, and its body is not inlined: `dp_decode` shrinks from 13652
  to 12908 bytes. The one reference left in the binary is
  `geo3d_emit_tri_uv`'s, as in every build.
- The colour memo compare is in the loop: `fcmp/eq` on the four floats and
  `cmp/eq` on the two flags at `8c04005a`-`8c040086`. A hit falls through to
  `8c040088`, which reads the memo's base (+56), offset (+60) and palette byte.
  A miss branches to `8c0400d0`, which stores the key and xors its words.
- `dp_tex_get` is still referenced twice, once in each loop.
- `dp_decode` has no reference to `g_dcf` or `g_dcf_key`. Its one `g_dcf_n`
  reference is the reset at the head of the list.
- Palette and luma are still there: `g_geo_rs`, `g_light_dir`,
  `g_geo3d_emit_texlod` and `g_geo3d_board_luma` are all loaded. There are 77
  fmul and 63 fmac, against 63 and 48 in the default build.

| frames 3500-3900, SH-4 timer time | total | sl / dr | ti / **sc** / so / su | hash |
|---|---|---|---|---|
| 84c31a0, default | 14060 | 5226 / 8769 | 453 / **7040** / 217 / 1046 | 634d853f |
| FACECOLSTOP=1 | 10102 | 5200 / 4853 | 467 / **4280** / 41 / 56 | 634d853f |
| f52431b, COLOURSTOP=1 (#544) | 9813 | 5201 / 4563 | 461 / **3996** / 41 / 57 | 634d853f |
| a351476, STRIPSTOP=1 (#541) | 11164 | 5235 / 5871 | 453 / **5311** / 41 / 57 | 634d853f |

- **Memo hit** (the compare, the memo's colour read back) =
  4280 − 3996 = **284 ms**
- **Miss** (`dp_face_colour`) = 5311 − 4280 = **1031 ms**
- The halves sum to 1315 ms, #544's colour lookup. As before, they share one
  middle figure, so the sum is exact by construction. What it shows is that
  sc landed inside the bounds. 4280 is above COLOURSTOP's 3996, so the
  compare ran, and below STRIPSTOP's 5311, so `dp_face_colour` did not.

so / su fell to 41 / 56, as in any build that queues nothing. They are not in
the subtraction. The frame-1500 hash stays 634d853f because it is board state
and proves nothing here. The default build with the switch present matches
84c31a0's byte for byte in every section except `.rodata`'s version string.

**Verdict: `dp_face_colour` holds the colour lookup.** The miss is 1031 ms and
the memo hit 284 ms. `dp_face_colour` was not opened here. The next pin may
split it only if the source already has a tail a skip can cut. The ramp is
not named from this count.

### Inside dp_face_colour: the ramp walk (#547, at 940211b)

HEAD had not moved from #546's 940211b. The default baseline, every switch
off, matched in every number and in the hash: 14060 | 5226 / 8769 |
453 / 7040 / 217 / 1046 | 634d853f.

On STF a miss in `dp_face_col` is nearly always a textured face with a luma
band. `g_dp_pal_on` is set only for `any_program` homebrew, so
`dp_face_colour` gives that face to `dp_face_ramp` (dc_pvr.h). #537 counted
350,879 of the 351,830 misses on that path. `dp_face_ramp` keeps its own RAM
cache, `g_dp_ramp`: 1024 entries, direct-mapped, keyed by `gen_lut`, the
face's 5-bit colour, palette level, the trans flag and the luma base. On a
hit it reads base, offset and bank back. On a miss it walks the ramp: for
each of 16 texels it reads one luma byte and three colorxlat bytes through
`g_dp_cx` (`dp_shade`), keeps the largest drop, and picks a bank (`dp_pool_bank`
for a drop over 16, otherwise `dp_argb` ×2 and `dp_knee_bank`). Then it stores
the entry.

`make RAMPSTOP=1` (`GEO3D_RAMPSTOP`, off by default, not combined with the
other switches) runs as STRIPSTOP does: texture and colour lookups, then no
strip put and no z key. The only difference is that a `g_dp_ramp` miss does
no walk and picks no bank. It still stores the entry, with the key standing
in for base and offset and bank 0, so the same faces hit afterwards. Those
words reach the running xor through the colour memo, as STRIPSTOP's do. A
pool bank in the default build can fail its validity check later and walk
again. A stand-in never does, so that small difference lands in the piece.
Proof from the disassembly against a STRIPSTOP build from the same commit:

- `dp_decode` is instruction-identical: 0x3328 bytes in both builds. The
  only differing words are literal-pool addresses, and one of them is
  `dp_face_colour`'s own (8c023ae0 → 8c020160), which the face loop still
  calls on a memo miss. Palette, luma, light and `g_geo_rs` loads are
  untouched.
- `dp_face_colour`, with `dp_face_ramp` inlined, shrinks from 3764 to 2320
  bytes. It has 21 byte loads against 27 and 10 `shll8` against 14. The
  `g_dp_ramp` compare and store, and the `!tex` and homebrew palette paths,
  are still there. `geo3d_emit_tri_uv`'s reference is not in this path.

| frames 3500-3900 | total | sl / dr | ti / **sc** / so / su | hash |
|---|---|---|---|---|
| 940211b, default | 14060 | 5226 / 8769 | 453 / **7040** / 217 / 1046 | 634d853f |
| STRIPSTOP=1, rebuilt here | 11164 | 5235 / 5871 | 453 / **5311** / 41 / 57 | 634d853f |
| RAMPSTOP=1 | 10385 | 5197 / 5142 | 461 / **4573** / 41 / 58 | 634d853f |
| FACECOLSTOP=1 (#546) | 10102 | 5200 / 4853 | 467 / **4280** / 41 / 56 | 634d853f |

| piece skipped | ms (sc) | left of the 1031 |
|---|---|---|
| `dp_face_ramp`'s walk and bank pick on a `g_dp_ramp` miss | 5311 − 4573 = **738** | 4573 − 4280 = **293** |

The 293 ms that is left covers the call itself, `dp_face_colour`'s prelude
(c5, poly), the `g_dp_ramp` hash, compare and hit read, and the store.

sc is SH-4 time in the decode, measured on the SH-4's own timer. It is a CPU
saving, and it shows. The outer check agrees: total falls by 779 ms, dr by
729 ms, sl by 38 ms and ti rises by 8. so and su are what Flycast bills for
polygon bytes (scheduleRenderDone). Both STOP builds queue nothing, so those
stay at 41 / 57 and are not in the subtraction. A PVR or pixel saving would
not show in any of these numbers.

**Verdict: the ramp walk is 738 ms**, 72% of the 1031 and 5.2% of the
frame's 14060. That is just over the ~700 ms line. The function is
`dp_face_ramp`'s miss path. It reads luma RAM (16 bytes from the face's
luma base, a stride of 8), colorxlat (48 bytes, three channels per texel)
through the 256-byte `g_dp_cx` level table, and the pool bank table when a
drop is over 16. The result depends on luma RAM and colorxlat, which the
game writes as it plays (`gen_lut`), and on the face's colour and its
per-frame light. So a table made offline and read from GD-ROM cannot
replace it. A RAM cache of its result already exists, `g_dp_ramp`, and the
738 ms is spent on its misses. Whether a bigger or set-associative one
would replace the walk depends on whether the misses are new keys (the
light moves every frame) or conflict evictions. This pin did not count
that. Nothing was implemented, and the picture is unchanged: the default
build with the switch present matches 940211b in `.text` and `.data`,
and `.rodata` differs only in the version string.

### g_dp_ramp's misses by kind (#549, at 797dd49)

HEAD had not moved from #547's 797dd49. The default baseline, every switch
off, matched: 14060 | 5226 / 8769 | 453 / 7040 / 217 / 1046 | 634d853f.

The layout is as recorded: `g_dp_ramp` is 1024 entries of 28 bytes (28 KB),
direct-mapped, slot `((k0 * 2654435761) ^ (k1 * 40503)) >> 22 & 1023`, key
(`gen_lut | 1`, k0 = colour | level << 15 | trans << 23 | 1 << 24, k1 = luma
base). A hit also needs a pool bank to still hold its pens (`pkey`, used
this frame or the last), so a slot that still has the key can miss too.

`make RAMPCOUNT=1` (`DC_RAMP_COUNT`, off by default and compiled out, not
combined with a stop switch) counts frames 3500-3900: every lookup, and on
each miss, before the walk, which kind it is. A 96 KB set of every key
missed since F0 (calloc'd at F0, after the frame-1500 hash and the boot's
cache sizing) tells a compulsory miss (key new to the window) from a
replacement (key seen, its slot since taken by another). A seen key under
an older `gen_lut` counts as stale-lut; a slot that still holds the key
counts as pool. The walk runs as ever and the picture is untouched. The
default build matches 797dd49 in `.text`, `.data` and `.bss`, and `.rodata`
differs only in the 7-character version string.

| frames 3500-3900 | total | sl / dr | ti / sc / so / su | hash |
|---|---|---|---|---|
| 797dd49, default | 14060 | 5226 / 8769 | 453 / 7040 / 217 / 1046 | 634d853f |
| RAMPCOUNT=1 | 14094 | 5202 / 8828 | 449 / 7117 / 208 / 1040 | 634d853f |

The count build is 34 ms (0.24%) slower, which is the counter's own cost.
That cost is not taken off the 738. (A first version probed its set by the
products' low bits, built long chains and cost 269 ms. It gave the same
counts, but it was over the 1% line, so it was not used.)

| lookups | hits | misses | unique keys | compulsory | replacement | stale-lut | pool |
|---|---|---|---|---|---|---|---|
| 360,427 | 296,385 (82.2%) | 64,042 | 2,094 | 2,094 | **61,948** (96.7%) | 0 | 0 |

| the 738 ms walk, by miss kind | ms |
|---|---|
| replacement, 738 × 61,948 / 64,042 | **714** |
| compulsory, 738 × 2,094 / 64,042 | 24 |

**Verdict: the misses are evictions, not new keys.** The window uses 2,094
distinct keys, all new to it once, against `g_dp_ramp`'s 1,024 slots: twice
what the cache can hold, so no layout of 1,024 entries keeps them. Each key
is missed again about 30 times. `gen_lut` never changed inside the window,
and no pool bank went stale under a key still in its slot. A RAM cache that
holds all 2,094 without conflict, built in the frame from luma RAM and
colorxlat as this one is and not read from GD-ROM, would replace the walk
for the 61,948 replacements (714 ms) and leave the 2,094 compulsory walks
(24 ms). That means set-associative at 4,096 entries (112 KB at 28 bytes), or
a direct map big enough that 2,094 keys rarely share a slot. A
4,096-entry direct map at half load would still evict, by an amount this
pin did not count. Two things this count cannot say: how many of those keys
point at a pool bank, which a longer-lived entry could find taken (counted
as pool, with a walk), and what the bigger table's BSS does to the page
cache, whose size `dc_boot_cache_size` picks from free heap at boot.
Nothing was implemented.

### Would a retained g_dp_ramp entry have been right? (#550, at 97c1f7c)

HEAD had not moved from #549's 97c1f7c. The default baseline, every switch
off, matched: 14060 | 5226 / 8769 | 453 / 7040 / 217 / 1046 | 634d853f.

**What a hit returns** (`dp_face_ramp`): the 28-byte entry is `lut, k0, k1`
(key), `base, off` (the ramp line's base and offset, the result) and `pal`
(knee bank, pool bank or 0), plus `pkey` for a pool bank. The walk reads luma
RAM (16 texels at the luma base), colorxlat through `dp_shade` (`g_dp_cx` is
a constant table) and, for a drop over `DP_POOL_DROP`, the pool. The colour,
the face's light level and trans are in k0, and the luma base is k1, so a
change in any of them is a new key. A changed key cannot make an entry stale.
A retained entry goes stale only two ways:

- `gen_lut` moved. That is any changing write to luma RAM or to colorxlat;
  the two share one counter, so the entry cannot tell which.
- Its pool bank was taken by other pens (`key` ≠ `pkey`), or went unused for
  two frames.

`make RAMPKEEP=1` (`DC_RAMP_KEEP`, off by default and compiled out, not
combined with another switch) keeps every key's last entry from F0 on, in a
4,096-entry side set (112 KB, calloc'd at F0, after the frame-1500 hash), as
a cache with no evictions would. On a miss whose key it has seen (and whose
slot does not still hold it), it judges the kept entry before the walk by the
hit test's rules: lut, then pool bank. After the walk it checks that the
kept entry's `base, off, pal` equal what the walk gave. Then it keeps the
walk's entry. It does not stamp the pool and does not touch `g_dp_ramp`, the
walk or the picture. The default build matches 97c1f7c in `.text`, `.data`
and `.bss`; `.rodata` differs only in the 7-character version string.

| frames 3500-3900 | total | sl / dr | ti / sc / so / su | hash |
|---|---|---|---|---|
| 97c1f7c, default | 14060 | 5226 / 8769 | 453 / 7040 / 217 / 1046 | 634d853f |
| RAMPKEEP=1 | 14120 | 5216 / 8836 | 454 / 7103 / 211 / 1053 | 634d853f |

The count build is 60 ms (0.43%) slower, which is the counter's own cost. It
reproduces #549's mix exactly: 360,427 lookups, 296,385 hits, 64,042 misses,
2,094 compulsory (= unique keys), 61,948 replacement, stale-lut 0, pool 0,
and the set was never full.

| the 61,948 replacement misses | count | ms of the 738 |
|---|---|---|
| **retained-valid** (kept entry passes the hit test and equals the walk) | **61,946** | **714** |
| retained-stale, total | 2 | 0 |
| — stale-lut (`gen_lut` moved) | 0 | 0 |
| — stale-pool (bank taken or cold; the hit test refuses it) | 2 | 0 |
| — passes the hit test but gives a different result, pool involved | 0 | 0 |
| — passes the hit test but gives a different result, luma / colorxlat | 0 | 0 |

Time is 738 × count / 64,042. **Verdict: a retained entry is right in
61,946 of 61,948 cases (714 ms), above the ~700 ms line.** The two stale ones
are pool banks re-taken while the key was out of the cache; the hit test's
`pkey` check already catches them, so a bigger cache must keep that check.
Nothing passes the test with a wrong answer. Not built, as the pin says:

- **What the entry stores:** the result bytes as now, `base` and `off`
  (8 bytes), plus the bank index `pal` and `pkey`, since a pool bank can be
  re-taken. With the key that is the same 28-byte entry, not a pointer into
  the walk.
- **Slots:** 4,096. The side set is 4,096 entries, open-addressed with linear
  probing, and it held all 2,094 keys at 51% load with no eviction, so
  every replacement found its entry. A 4,096-entry direct map would still
  evict at that load. This pin did not count how many 4-way sets of 1,024
  would overflow.
- **RAM:** 4,096 × 28 = 112 KB, which is 84 KB more than `g_dp_ramp`'s 28 KB.
  What that BSS takes from the page cache (`dc_boot_cache_size`): #552,
  below; nothing measurable.

Nothing was implemented.

### What 86 KB less page cache costs (#552, at d29a411)

#550's 4,096-slot table would take 86,016 bytes more than `g_dp_ramp`. This
pin took the same RAM from the page cache and changed nothing else.
`CACHESHRINK=1` (`DC_CACHE_SHRINK`, `main_dc.c`; off by default and compiled
out) subtracts 86,016 from the budget `dc_boot` passes to `pg_init`. The pager
allocates its frame pool from that budget and nothing else. The pool is whole
16 KB frames, so it shrinks by 6 frames (96 KB). No ramp table was allocated.
`g_dp_ramp`, the walk and the picture are untouched. HEAD was still d29a411.
The default build's `.text` and `.data` are byte-identical to d29a411's, and
`.rodata` differs only in the version string. Both builds include Gems (the
`+gems` row, 83 `gfn_` symbols).

| frames 3500-3900 | total | i960 slice | draw | ti / sc / so / su | page loads (cd / da) | f1500 hash | cache budget |
|---|---|---|---|---|---|---|---|
| baseline | 14060 ms | 5226 | 8769 | 453 / 7040 / 217 / 1046 | 567 (280 / 56) | 634d853f | 1280 KB (80 frames) |
| `CACHESHRINK=1` | 14061 ms | 5205 | 8792 | 464 / 7040 / 208 / 1064 | 664 (349 / 68) | 634d853f | 1196 KB (74 frames) |

**Verdict: no measurable cost.** One run each, and +1 ms against a 1% band of
~140 ms. The pool loses 6 frames and loads 97 more pages in the window (+17%,
mostly code), but the total does not move. The i960 slice and the draw shift
by about 20 ms in opposite directions, which is noise. The hash holds. So
the 714 ms from #550 stands, and the net is 714 − 0. The 96 KB is not the
expensive part of a bigger ramp cache. The cache was not written, as the pin
says.

## Sonic Gems Collection's way (#456, GEMS-COLLECTION.md)

GEMS-COLLECTION.md, "What it means for the Dreamcast port", lists what Sega's
GameCube port does that this one did not. Measured on the bench (attract's
fight, frames 3500-3900, in the Flycast libretro core; the frame 1500 hash
e746591c in every build below).

- **Gems' C is on by default** (`GEMS=`, the Makefile). Sega's C for STF's
  trapped i960 functions and for the COP commands (`core/gems.h`) is private
  and never committed; the Makefile builds it in when it finds the directory
  beside the checkout. Its 47 trap sites are hooks the AOT must not compile
  over. 17.4 s → 14.5 s (23.0 → 27.6 fps), the i960 slice 8.7 s → 5.9 s. Its
  code and sine table take 650 KB, so the page cache is 1 MB, not 1.5 MB.
  It is not the board (CLAUDE.md, "HLE Hooks"): a build that has to match
  m2-hle2 frame for frame leaves it out.
- **Pre-decoded dispatch is not worth it here** (`OPTAB=1`, `I960_OPTABLE`).
  A handler per opcode byte and per REG opcode/function, indexed from the
  instruction word: bit-exact, but with Gems and the AOT on, the interpreter
  runs little, and it saved 38 ms of 5874 for 72 KB of text and 64 KB of heap.
  Off.
- **Sound stays Sega's console way** (ADX cues, `dc_sound.h`): no sound CPU,
  no SCSP, as on the GameCube.
- **The model pack** (`MODELS.PAK`, `tools/dc_mdlpack.py`, `dc_pager.h`).
  Gems' `OBJ_*` files are raw polygon ROM, gathered per scene. The port does
  the same from a map of what the 3D decoder reads: `det_digest --model-map`
  decodes every display list uncut, as `dp_decode` would, and records each
  64-byte line of the polygon and texture ROMs with the frame that first read
  it (`dreamcast/sfight.mdlmap`: attract, 6000 frames, and a scripted fight;
  addresses only). The tool groups lines into runs (contiguous in the ROM,
  split where the first frame jumps by more than 30) and lays them out by
  first frame: 607 runs, 4.7 MB. `dc_rom_at` finds a read in the index by
  binary search and serves it from the pack's window when one run holds all
  of it; anything else (and a disc without the file) reads the ROM.
  - On the bench, page loads 1321 → 1063: code 800 → 658 (less of the cache
    goes to scattered models, so less code is pushed out), polygons and
    textures 381 → 40, plus 250 from the pack. Flycast's disc reads cost
    nothing, so it shows no gain there (14.5 s → 15.1 s); on a GD-ROM every
    load is a seek. Not measured on hardware.
  - The 40 left are reads the map missed or that cross a run's end.
  - **Like objects together: tried and dropped** (#489, #492). A map holds
    only what its runs saw, so a matchup it never recorded reads its fighters
    from the ROM. `dc_mdlpack.py --groups` lays the pack out by object groups, as Gems' `OBJ_*` files are: one per stage,
    one per fighter (and the fighter's second-player colours), the select
    screen, the story scenes (`dreamcast/sfight.mdlgroups`, written by
    `tools/dc_mdlgroups.mjs`). The explorer says what each stage's display
    list draws, at every frame of its animations, and each fighter's parts
    and faces. Gems' files add the fighters' effects and props and the
    scenes the explorer does not reach. Each group's objects go in whole:
    10,943 runs, 20.3 MB. Its 133 KB index comes out of the frame pool
    (`pg_init`), so the heap keeps its headroom.
  - Measured by replaying the pager's model reads (`det_digest --model-map`
    traces, an LRU over 16 KB pages; the code's share of the cache left
    out), against the by-first-frame pack. The 1 MB pool is 64 frames for
    that pack and 55 for this one:

    | run | by first frame | by groups |
    |---|---|---|
    | attract, 6000 frames (in the map) | 619 | 945 |
    | the scripted fight, 9000 (in the map) | 723 | 829 |
    | another matchup, 9000 (not in the map) | 171,558 (64,891 from the ROM) | 9,146 (none) |

    What the map recorded costs a third more loads, because a group's
    pages hold objects the scene does not draw. What it did not record
    stops thrashing. Two layouts were worse and were dropped: the map's
    lines regrouped by object (3,771 / 1,266 / 106,000 + 65,000 from the
    ROM), and the map's lines first with the groups' remaining lines after
    them (753 / 920 / 185,281).
  - **In Flycast it is worse** (#492). One program (Gems, AOT 0.99), three
    discs: no pack, the by-first-frame pack, the grouped pack; the libretro
    core, HLE BIOS, the pager's counters read off the screen. Page loads on
    the bench (f3500-3900, in the map), then evictions since boot (each a
    load once the cache is full):

    | run | no pack | by first frame | by groups |
    |---|---|---|---|
    | bench loads (code / data / polygons / textures / pack) | 312 (115/25/91/81/0) | 184 (84/20/18/0/62) | 395 (145/30/90/0/130) |
    | attract, evictions to frame ~9700 | 3,772 | 2,607 | 4,378 |
    | a game, Espio vs Knuckles (not in attract), evictions over its fight, frames ~1160-3800 | 564 | 229 | 516 |

    The grouped pack needs twice the pack pages for the same scene, and
    they push code and the polygon pages the i960 reads out of the cache.
    A matchup the map never recorded does not thrash with the old pack: the
    3D decoder's mesh cache builds a mesh once (`b 0`, thousands of hits a
    second in the fight), so its reads come from the ROM once, not every
    frame as in the replay above, which decodes every display list uncut.
    So `mkdisc.sh` builds the pack by first frame; `--groups` stays in the
    tool, and the pager takes any pack's index out of the frame pool (one
    16 KB frame for the by-first-frame pack's 7 KB, as measured here).
- **Textures converted at build time do not pay here, so there is no texture
  pack.** Gems' `TEX_STG*` files look like each stage's textures, already
  decompressed and converted. The two costs that would remove, measured over
  ~490 s of attract in Flycast:
  - The decompressor: the 812 slices in which STF's loader ran took 20.8 s,
    against ~11 s for as many ordinary slices. 7.4 s of that is its rows in C
    (`m2_texload.h`), and they wrote 11.9 MB of texram. A pack of what each
    request writes would be read off the disc instead, and at a GD-ROM's
    ~1.5 MB/s 12 MB takes about as long as the rows. Trapping the request
    would also change the board's timing (the loader yields on timer 4,
    which `rand` reads).
  - The conversion: cutting and twiddling tiles for the PVR (`dp_tex_get`)
    took 2.2 s, under 0.5%.
- **Drawing from a trap at `set_obj` does not pay either.** Walking the display
  list after the frame (`geo3d_scan_geo_list`) is 216 ms of the bench's 6828 ms
  of scene; the rest is decoding and transforming each model
  (`geo3d_decode_model_cached`), which a trap would do all the same. set_obj's
  own i960 side already runs as Gems' C.
- **The AOT map is recorded with Gems on** (`det_digest --gems`). The old map
  was recorded without, so part of what it compiled was code Gems' C now runs
  in its place. With the new one the bench is the same (14.95 s against
  14.93 s, 10% of steps AOT) and `1ST_READ.BIN` is 1.75 MB instead of 2.34, so
  the pager's cache gets the room. Compiling all of it (`AOT_COVER=1.0`,
  4.1 MB) leaves too little memory to boot.
- **Host-time timers do not apply.** Gems runs its game loop on the console's
  clock; here the board's timers feed `rand` and the loader's yield, so they
  stay on the i960's cycle clock, as on every other build.

## The COP's maths on the SH-4's own instructions (#468)

`HOST_MATH=1` (the default, except with `LINK=1`) hands the COP's sine,
cosine, square root, reciprocal square root and divide to the SH-4
(`dc_math.h`): FSCA takes the board's own angle (0x10000 = 2π) and gives sin
and cos at once, FSQRT is correctly rounded, and a divide is a divide.
`sharc.h` takes them through `SHARC_HOST_SINCOS` / `SHARC_HOST_SQRTF` in place
of the firmware's table-seeded Newton steps. Gems' COP C takes them through
`GEMS_HOST_SIN` / `_COS` / `_SQRTF` in place of its 256 KB sine table and its
double-precision Newton square root, which the SH-4 (built `-m4-single`)
emulates in software. The guards are in the private Gems directory.

- **It is not the board.** The firmware's √ and ÷ differ in the low bits
  (CLAUDE.md, "The COP's arithmetic is not libm"), so the fight drifts from
  MAME's; the frame 1500 hash is a5d21d21 against 69890d7a. The picture plays
  the same fight. The `LINK=1` build, held against MAME over the serial port,
  keeps the firmware's arithmetic.
- **It saves little time.** The bench: 15.50 s → 15.43 s (−0.4%), the slice
  6.51 s → 6.46 s. The COP is 3-4 ms of the ~16 ms i960 slice, and most of
  that is the commands' own work, not their maths.
- **It saves memory.** The binary is 270 KB smaller (1.84 MB → 1.57 MB), so the
  page cache grows from 1280 KB to 1536 KB, and the bench's page loads took
  357 ms against 539 (disc reads 162 against 273). Flycast's disc is fast; on
  a GD-ROM every load is a seek.

## The board's picture at its own size (#471)

The board's 496x384 drawn pixel for pixel, centred in the 640x480 frame, and a
`FRAME512=1` build whose frame is 512x384 in the middle of the signal. The
commit (3af4093) was reverted for want of speed (below); `FRAME512=1` came back
as an option with #475 / #476, and `HUD=none` (#478) draws the board 1:1 again.

- **The default frame is the cable's 640x480**, with the board scaled to
  fill it (#479's VIEW with none set: 1.25 at (10,0)).
- **`HUD=none` (#478) draws the whole board 1:1 at (72, 48)**, black bars on
  all four sides and nothing else over it, so a screenshot cropped at
  (72, 48) is the board's 496x384 to hold against MAME's, pixel for pixel.
- **`make FRAME512=1` makes the frame itself 512x384**, set in the middle of the
  640x480 signal (`dc_video_mode`: `bitmapx` +64, `bitmapy` +48 lines, 24 a
  field interlaced), with the board 8 pixels in: the PVR renders whole
  32-pixel tiles, and 496 is not a multiple of 32. The signal and the picture
  are the same on a Dreamcast, with 192 tiles rendered instead of 300 and
  ~440 KB less framebuffer. That is not tested on hardware. The text rows are
  drawn at 0.8 in it, so that 20 fit.
- **Flycast did not show it that way** until #499. Its renderer stretched the
  TA's frame to fill its output (512x384 x 1.25) and then moved it by the
  change in `VO_STARTX`/`VO_STARTY`, so the picture came out enlarged and cut
  off on the right and at the bottom. Hence the default. Moving it back up and
  left would not have been enough: its size was wrong too.
  `dreamcast/tools/flycast-frame512.patch` (#499) fixes the libretro core's
  OpenGL output: when the framebuffer the PVR shows (`FB_R_SIZE`) is smaller
  than the screen, the output is the whole 640x480 screen and the frame goes in
  it 1:1 at the video shift, black around it, as on a Dreamcast. The stats
  rows and the fps counter all show. It also doubles an interlaced mode's
  `VO_STARTY` shift (field lines). The core in `~/build/tools/dc/flycast` has
  it, so the canary's Flycast launchers show a `FRAME512` disc whole. Composite
  (480i) and VGA put the frame at the same place. A 640x480 disc looks as
  before.
  For the RK3566 handheld (ROCKNIX's RetroArch, GLES 3),
  `~/build/tools/dc/build-flycast-arm64.sh` (#501) cross-builds the same
  patched tree as a linux-arm64 core, `build-lr-arm64/flycast_libretro.so`;
  it needs only glibc 2.38, nothing else. On the device, copy it into
  `/tmp/cores` (ROCKNIX's overlay of `/usr/lib/libretro` and `/storage/cores`):
  a file changed in `/storage/cores` directly is not seen, and the overlay then
  has to be remounted. The patch also fixes the PowerVR2 filter's VGA shader,
  which GLSL ES refused (int and float mixed): on the RK3566 a VGA disc with the
  filter on stopped at boot. This core's options are still named
  `reicast_*`; the `flycast_*` lines (ROCKNIX's own build's) are not read.
- **No time either way in Flycast** over the bench's 400 frames (f3500-3900):

| build | total | i960 slices | draws |
|---|---|---|---|
| scaled 1.25 to 640x480 (kept) | 15431 ms | 6431 ms | 8929 ms |
| 496x384 centred in 640x480 | 15433 ms | 6439 ms | 8925 ms |
| `FRAME512=1` | 15427 ms | 6436 ms | 8920 ms |

Repeat runs spread 15431-15435 ms, so all three are noise. Flycast cannot show
a difference: it charges every frame 450,000 cycles plus 100 a byte of polygon
data, whatever the resolution (`scheduleRenderDone`, `core/hw/pvr/spg.cpp`), and
the polygon data is the same. Only `FRAME512` could save time on a Dreamcast
(fewer tiles), it is untested there, and Flycast drew it enlarged and pushed
off to the right (fixed by #499's patch). The commit (3af4093) was reverted. `FRAME512=1` stays as an
option (off by default) for STF's fps disc (#475); the default frame is the
scaled 640x480 one, which #479's VIEW gives with no VIEW set.

## Only the part of the board a game uses (#479)

m2-sonic draws the Mega Drive's 320x224 at (88,80) of the board's 496x384
and leaves the rest black, so on the 640x480 frame the game was a small box
(320x224 1:1 under #471's layout, 400x280 at 1.25). `make VIEW=x,y,w,h`
(`DC_VIEW_X/Y/W/H`, `dc_pvr.h`) shows only that rectangle of the board,
scaled to fill the frame, aspect kept, centred. With no VIEW it is the whole
board: 1.25 at (10,0) in 640x480, the layout #471's revert kept.

- **m2-sonic's disc is `VIEW=88,80,320,224`**: 2.0, 640x448 at (0,16).
- **m2-pacman's disc is `VIEW=136,48,224,288`** (#487): Pac-Man's portrait
  screen at cells (17,6) of the tile plane (`PAC_COL0/ROW0`, m2-pacman's
  `pacman.c`), 5/3 to 373x480 at (133,0), the frame's full height. The scale
  is not whole, so the tiles stay bilinear. 28-31 fps in the maze, as before.
- **At a whole-number scale the tile layers are point sampled**
  (`DC_FILTER`), so the Mega Drive's pixels stay square.
- **Only the view is drawn**: the layer quads take the view's part of their
  512x512 textures (`dp_layer`), the tile conversion skips lines and columns
  outside it (`dp_tiles`), and the 3D is culled to it (`dp_decode`).

**It is cheaper, not dearer.** The PVR renders the whole 640x480 tile grid
whatever is in it, and STF's 1.25 already fills it, so the fill costs the
PVR nothing new. The SH-4 does less: the bench (full HUD,
`EXTRA=-DDC_BENCH_F0=300u -DDC_BENCH_F1=900u`, m2-sonic attract, AOT 0.95)
over the same 600 frames:

| | total ms | slice | draw | tiles |
|---|---|---|---|---|
| 1:1 (#471, the canary disc) | 34956 | 11068 | 23782 | 23364 |
| `VIEW=88,80,320,224` | 26831 | 11065 | 15691 | 15275 |

The tiles' conversion was most of the frame and the view is 38% of the
board, so a frame went from 58 ms to 45. Measured in Flycast, which does not
charge PVR fill.

## The whole frame (#503)

With no VIEW, STF's 496x384 went onto the 640x480 frame at 1.25, aspect
kept: 620x480 at (10,0), a 10-pixel black bar down each side. An arcade
monitor is 4:3 and shows the board's 496x384 across all of it, 1.29 across
and 1.25 down, and so does MAME. `make FILL=1` (`DC_FILL`, `dc_pvr.h`) does
the same: `DC_SX` and `DC_SY` scale the two axes apart (the tile quads, the
3D projection and its view clip, the checker's phase), and the board fills
the frame edge to edge. It is the default when there is no VIEW; a VIEW keeps
its shape unless FILL=1 is given with it (Pac-Man's portrait screen would
otherwise be pulled sideways), and HUD=none stays 1:1.

**It costs nothing.** The PVR renders the whole tile grid either way, and
the SH-4's work does not depend on the scale. The bench (full HUD, AOT,
Gems, fight frames 3500-3900, Flycast's libretro core):

| | total ms | slice | draw |
|---|---|---|---|
| 1.25, aspect kept (`FILL=0`) | 18805 | 10047 | 8673 |
| whole frame (`FILL=1`) | 18803 | 10000 | 8718 |

The same within noise. #479's gain for m2-sonic came from converting less of
the board, which STF cannot do: it uses all of it.

## m2-sonic's planes on the PVR, and its idle loops skipped (#481)

The view left two costs, found with the HUD's per-stage times and a
per-instruction histogram of the i960 (`det_digest --trace` with a PC count):

- **Both Mega Drive planes are line-scrolled, so the CPU redrew the whole
  view every frame** (~25 ms of tiles). m2-sonic puts plane B in tilemap 2
  and plane A in tilemap 0, each with a per-line H scroll, and its window
  layers (tilemaps 1 and 3) only round the picture, which the masks keep
  outside the view. `dp_ls_ok` now checks the mask on the view's lines and
  columns only, and when both pairs qualify tilemap 0 gets textures of its
  own too (category 0 pixels behind the 3D, ARGB1555, GEQUAL; category 1 in
  front): the CPU then converts no tile at all. A Mega Drive game cycles a
  few colours and animates a few patterns every few frames, so a cell is
  redrawn only when its entry, its char (`g_dp_chr_new`) or its palette bank
  changed; redrawing the whole tilemap on each cost another ~7 ms a frame.
- **The i960 spent a quarter of its time in the program's own vblank
  waits.** `m2_spin` knew only STF's `_idle`; a homebrew program
  (`sfight_homebrew`) now has its ROM scanned at install for the same shape,
  an absolute load and a compare-and-branch back (`m2_spin_find`), and each
  loop found is skipped on the cycle clock. The AOT leaves those addresses
  to the hook (they are in the AOT map). m2-sonic's waits are written as
  that loop (its `vbl_wait`). `det_digest --cpu` against `--spin-i960` is
  identical over 4000 frames. A `--trace` run steps one instruction at a
  time, with no room for a skip, so its histogram still shows the waits.

The same bench, 600 frames of attract:

| | total ms | slice | draw | tiles |
|---|---|---|---|---|
| `VIEW=88,80,320,224` (#479) | 26831 | 11065 | 15691 | 15275 |
| + idle loops skipped | 24085 | 8353 | 15654 | 15244 |
| + both planes on the PVR | 13437 | 8364 | 5034 | 4597 |

45 ms a frame to 22: in play the HUD reads about 20 fps (it was 15), the
i960 33-35 ms a slice. What is left of the i960 is m2-sonic's own work: the
68000 recompiler's output (`md_rc_run`, 41% of its instructions) and the
sprite and tilemap conversion (`s24_sprites`, `s24_quad`, `s24_planes`,
~14%).

## Held against MAME over the serial port (#461)

`make LINK=1` builds a disc that plays attract's replay fight (tools/README.md,
"match_replay") with the arcade profile, region Japan, and sends both fighters
over the SH-4's SCIF at every game frame edge (`dreamcast/dc_link.h`). Flycast's
libretro core carries the port to a TCP socket (`dreamcast/tools/flycast-scif.patch`),
and `tools/dc-lockstep.py` runs MAME's `match-replay.lua` on the other end. The
Dreamcast waits for the host's word after every frame, so it never runs past a
frame the host has not checked. At the first difference the host asks for the
whole frame and names the words.

A frame's record is the fight state (`+0..+0x1F8`) raw, plus a CRC-32 for every
0x100 bytes of the rest of both work structures and of the bufferram the i960
reads back: 1.4 KB, where the whole frame is 28 KB. 1299 frames take ~100 s.

What it took for the Dreamcast to compute the desktop's fight:

- **The COP's sin and cos come from the copro ROM's tables**
  (`sharc_sincos`), which the disc does not have. `SINCOS.BIN`
  (`dreamcast/tools/mksincos.py`, 512 KB from the player's arcade set) is
  `g_sharc_sincos`. libm's `sinf` / `cosf` differ in the low bit, and 418 words
  were off from the first frame.
- **`-ffp-contract=off`, as on the desktop.** sh-elf gcc fuses `a*b+c` into
  `fmac` at `-O2`, and the result is rounded once instead of twice. With the
  tables alone, 309 words were still one ulp off at +0. `LINK=1` adds the flag.
  The game build does not: on screen nobody sees an ulp, and the flag costs
  time.

With both, the fight is MAME's for all 1299 frames: motion, energy and the
whole fight state. The rest parts from MAME exactly where the desktop build does
(tools/README.md, "match_replay"):

- `P1+0x1114` at +321;
- the rig at +532;
- the sign of zero at `0x90E804` from +0.

`--peer` holds the Dreamcast to a desktop run of the same build
(`match-replay.mjs --out`). Every word the two send matches through +579. At
+580 `P1+0x1FB0`, a value decaying toward zero, reaches the denormal
`0x006047D0` on the desktop and in MAME, and 0 on the Dreamcast.

- **KOS runs the SH-4 with FPSCR.DN = 1** (`startup.S`: `0x00040000`), so a
  denormal result is flushed to zero. With DN = 0 the SH-4 traps on a denormal
  operand (the FPU error cause cannot be masked), so this is not a flag to
  flip. The i960's single/double conversions now do denormals on the bits on
  the SH-4 (`I960_SOFT_DENORMAL`, `i960_exec.h`; #478, below).

### From power-on (#478)

`dc-lockstep.py --boot` runs the same LINK disc from power-on, with no replay
jump: at every game frame edge the Dreamcast sends a CRC-32 for every 4 KB of
work RAM, RAM, bufferram, tile RAM and palette (`dc_link_boot`). MAME
(`tools/mame/boot-lockstep.lua`) and a desktop `det_digest --raw` of the same
source stream those regions raw into pipes beside it. MAME has one more frame
edge at power-on, which the host skips.

- Without the fix below, the Dreamcast was the desktop's through +3656 and
  parted at +3657, in the same word as the replay fight's +580 (P1 +0x1FB0,
  decaying through the denormals).
- **The i960 does its float↔double conversions in software on the SH-4**, for
  a denormal only: `fcnvsd` reads one as 0 there and `fcnvds` flushes one.
  The soft versions match x86's conversions bit for bit over 20 million
  inputs, half of them denormal. Elsewhere the code is as before.
- With it, **the Dreamcast is the desktop build, every block of every region,
  for all 12000 frames** (~20 minutes in Flycast): attract, the replay fight
  and on.
- Against MAME both builds part in the same places: work RAM from +0, bufferram
  from +836, tile RAM from +2491 (25 work RAM and 20 bufferram blocks by
  +12000); RAM and palette stay MAME's. The desktop build has every one of
  them, so they are the board's differences from MAME, not the Dreamcast's,
  and are not chased here.

```sh
python3 tools/dc-lockstep.py --boot --frames 12000 --gdi <disc>/m2hle2.gdi \
    --core <patched flycast_libretro.so> --retroarch-config <cfg> --det-digest <desk>/det_digest
```

```sh
cd dreamcast && make LINK=1 VENDOR=../vendor OUT=<dir>   # LINK_GEMS=1: Gems' C on
python3 tools/mksincos.py <sfight.zip> SINCOS.BIN
SINCOS=SINCOS.BIN sh mkdisc.sh <dir> <stf_rom> <disc> STF.AFS
python3 ../tools/dc-lockstep.py --gdi <disc>/m2hle2.gdi --core <patched flycast_libretro.so> \
    --retroarch-config <cfg> [--ref mame.bin] [--peer <desk>/here]
```

Without a host on the line, the hello goes unanswered for 3 s and the disc runs
the replay unlinked, so it boots on a plain emulator or a console with no cable.
On a console, a serial cable to a PC at 1.5625 Mbaud carries the same protocol.
`--listen PORT` already waits for a Dreamcast started by hand. A reader for the
serial device has not been written.

### The pictures, against MAME's (#486)

The lockstep above holds the board's state, which the Dreamcast had right; the
picture is drawn by `dc_pvr.h`, which no lockstep sees. `--boot --shots 60`
keeps both screens of the same frame, once a second (MAME's snapshot, and
RetroArch's screenshot while the Dreamcast waits on the link), and
`tools/picture-diff.py` lays them side by side and measures them. Over the
first 50 s of attract, MAME against the Dreamcast:

| | mean abs. difference | colour histogram |
|---|---|---|
| before | 13.1 | 0.813 |
| after | 9.5 | 0.853 |
| after, with #479's 1:1 board (HUD=none) | 8.5 | 0.872 |

What it took:

- **Knee ramps.** A textured face's 16 pens rarely make a line: colorxlat's
  ramps start near 88 and the shade takes 64 off first, so a dim face's dark
  texels are black and its ramp rises from some texel on. The line from texel
  0 to 15 lifted the dark half ~20 levels. Palette banks 3..54 hold the opaque
  and see-through grey ramps again with a knee every half texel, and a face
  takes the bank of its knee.
- **Palette ramps.** A few ramps fall and rise (the hut's emblem, the lab
  monitor's moon and the panel beside it): banks 55..63 are handed out by
  colour, kept across frames, and never rewritten while the PVR may still draw
  from them.
- **Textures over 256x256** (the water, a monitor's picture) are cut and
  loaded 32 KB at a time.
- **The window clip.** The board draws nothing outside a list's window; the
  PVR clipped only at the frame's edge, so at 22 s a quad meant for a small
  window covered the screen white (difference 81 → 6). A face with a corner
  outside its window is clipped in screen space; a window that is the whole
  frame needs none, as the bars round the frame hide the rest.

The cost, kept by a ramp cache keyed by the face's colours (cleared when
luma or colorxlat changes) and one header per material with the bank patched
into mode3: frames 3500-3900 of the fight take 19182 ms in Flycast against
18641 before (+2.9%; it was +36% with the ramp walked per face).

Still differing: the Tails-lab floor is grey where MAME's is pale cyan
(29-31 s, the worst frames), and at 38 s the console's lid is purple.

```sh
python3 tools/dc-lockstep.py --boot --frames 3000 --shots 60 --work <dir> --gdi <HUD=none disc>/m2hle2.gdi \
    --core <patched flycast_libretro.so> --retroarch-config <cfg> --det-digest <desk>/det_digest
python3 tools/picture-diff.py <dir> --out <dir>/diff
```

## What the console loses its time to (#495)

Every number above is Flycast's clock. On a Dreamcast the same disc (#475's
`HUD=min FRAME512=1`, the Lazyboot CD-R of #483) runs a fight at 11-15 fps,
a quarter of Flycast's 27, and Flycast's model says nothing about why: it
charges a fixed 450,000 cycles a frame for the render, nothing for a cache
miss, a store into video memory or a seek of the drive, and its disc reads
are free. The candidates, in the order the code suggests them:

- the page cache. A fight's 60-frame working set is ~3 MB at 4 KB
  granularity and the cache is 1.3-1.5 MB, so pages come back from the disc
  all through a fight (162 loads in the bench's 400 frames, 357 in Flycast's
  3500-4000 here). Each is a synchronous PIO read (`pg_read`,
  `CD_CMD_PIOREAD`, polled) and a seek on a CD-R is tens of ms. Flycast
  counts it as 0 ms (row 1's `(0 ms)`).
- stores straight into video memory: `dp_tiles_convert` writes the tile
  layers a word at a time into the PVR's textures, and every face goes out
  through the store queues.
- the caches: the SH-4 has 16 KB of data cache and 8 KB of instruction
  cache, direct mapped, and the board's state is 4 MB of bus plus 1 MB of
  mesh arena plus 0.9 MB of tile snapshots. No bench so far has had a cache
  model at all.

`HUD=prof` (dc_prof.h) measures these on the console. The SH-4's two
performance counters count the cycles the pipeline stood still for a
data-cache miss (PRFC0) and for an instruction-cache miss (PRFC1), shown
as a share of the window's cycles. KallistiOS's PVR stats give the TA's
registration and the render time of the last frame, and the vblanks. And a
sampler on TMU1 (2 kHz) takes the interrupt context's PC and looks it up in
a table of the program's symbols, generated from the linked elf by
`tools/dc_profmap.py`: the Makefile links the program twice, once with an
empty table, then with the table made from that elf; the table is data,
so the code sits at the same addresses, which the Makefile checks with `nm`.
The HUD shows each group's share of the window and the six hottest symbols.
Flycast's counters read 0 (it has none), and under it the sampler put the
fight at geo 27%, drw 20%, 960 15%, gem 12%, aot 6% (`geo3d_decode_model`
23% on its own), with the PVR rendering a frame in 7.5 ms. Row 1's disc ms
and rows 3-5's bench (the same frames as every bench above) come along on
the same disc, so a photograph of the console's screen during a fight
gives the host's split and the console's side by side.

## The models walked offline: STRIPS.PAK (#498)

The mesh cache (`geo3d_mesh_get`) walks a model's GEO stream once per key
(model, material and UV pointers) and keeps its faces in the 1 MB arena. In a
fight the arena turns over: about 115 meshes are walked again every 2 s.
STRIPS.PAK holds those meshes walked offline (`tools/dc_strips.c`, from the ROM
files by `mkdisc.sh`; `dreamcast/dc_strips.h` has the format). Each mesh has
the corners as `geo3d_mesh_build` leaves them and, per face, its corner
indices, sort and light fields, list (opaque, punch-through, translucent) and
the PVR texture key it cuts, with a tile over 256 already windowed. Each
corner's u, v is in that texture's units, in strip order. `geo3d_mesh_get`
copies a packed mesh into the arena in place of the walk; one not in the pack
is walked as before. Nothing is baked: corners stay in model space under the
matrix slot the list names, and the frame still transforms, culls, lights and
sorts. A quad without a near clip or checker goes to the TA as one 4-vertex
strip, cut ABCD or BADC as the view's diagonal says. A walked mesh's quad
still goes as two triangles.

The keys are `dreamcast/sfight.strips`, recorded like `sfight.mdlmap` with
`det_digest --strip-keys` (attract, then the scripted fight): 920 meshes,
71,923 faces, 268,614 corners, 8.3 MB. All the recorded frames draw from it;
the HUD's `b` (meshes walked) stays 0 from boot to the fight.

The bench (Flycast, PS3 files, frames 3500-3900; the frame-1500 hash is
a5d21d21 in all three):

| | all ms | slice | draw | scan | sort | submit | page loads | fight fps |
|---|---|---|---|---|---|---|---|---|
| walked (e126f7b) | 15609 | 6475 | 9074 | 6929 | 336 | 1353 | 357 | 24-26 |
| pack, `pvr_vertex_t` corners | 14303 | 6466 | 7765 | 6117 | 214 | 981 | 908 | ~28 |
| pack, u, v corners (the disc) | 14274 | 6467 | 7747 | 6110 | 211 | 980 | 428 | ~28 |

- **The corners are u, v, not whole `pvr_vertex_t`.** The first version stored
  each corner as a TA vertex, ready for a store-queue copy. That cannot be
  copied as it is: the frame writes the command word, x, y, z and both colours
  of every vertex it sends, because the corners move with the fighters'
  matrices. Only u and v come from the stored one. The full vertices made the
  pack 14.4 MB and a packed mesh larger in the arena than a walked one, which
  meant more evictions (`c` 2/127 against 1/24). The pager then loaded 908 pages
  against 357, and code pages rose from 162 to 360. Flycast's disc costs
  nothing, but on a GD-ROM every load is a seek. At 8 bytes a corner the pack
  is 8.3 MB, a packed mesh is smaller in the arena than a walked one (`c` 0/16),
  and the pager loads 428 pages: 171 from the pack against 132 + 30 for
  MODELS.PAK and the polygon ROM before. Code pages rose from 162 to 194. Not
  measured on hardware.
- **Where the time went:** scan −819 ms, the walks and the per-face attribute,
  texture-header and UV work; submit −373 ms and sort −125 ms, from a quad
  being one 4-vertex strip instead of two triangles. The slice (the board) is
  unchanged.
- **The textures stay converted at run time** (but see #502, below, for a pack
  that skips the cut when texture RAM holds what it recorded). The PVR
  textures are cut from texture RAM as the board draws, and the game's loader
  fills it at run time (see "Textures converted at build time do not pay
  here" under #456). The
  conversion is under 0.5% of attract. The key of the texture each face cuts
  is precomputed and a tile over 256 is windowed offline; one whose face
  spans more than 256 keys the whole tile, at most 1024 on a side, as
  `dp_tex_get` cuts it since #486. Each texture's
  `pvr_poly_hdr_t` was already compiled once per texture and variant
  (`dp_tex_t.hdr`), not per frame.

## Textures pre-converted: TEXTURES.PAK (#502)

The pack holds the PVR textures attract and a scripted fight cut, already
twiddled (`dreamcast/dc_texpak.h`). Texture RAM is filled by the game's loader
at run time, so an entry is keyed by the texture's key and an FNV-1a hash of
the texture-RAM words the cut reads (`dct_src_hash`). `dp_tex_get` hashes
first; a hit is one `pvr_txr_load` from the pack, a miss or another hash is
cut as before. Only textures up to 256 a side are packed. The data lies in
the order the recorded frames first drew it, and is read past the page cache
through a 64 KB window (`pg_tx_at`, `DC_TX_WINDOW`), so one read brings in
the next textures of the same scene. `det_digest --tex-pack` records it from
the ROM files; `TEXPAK=` puts it on the disc. Nothing in it is committed.

- **What it holds:** 280 textures, 2.79 MB (248 from 6000 frames of attract,
  32 more from 9000 of the scripted fight). 128x128 (62), 64x64 (50) and
  256x256 (43) are the common sizes.
- **Held against Sonic Gems Collection's banks** (`tex_banks`, Map1-Map5):
  every one of the 241 textures that is not a single index matches a Gems
  bank at the key's rectangle (correlation ratio of our 4-bit indices to the
  bank's luminance above 0.9). Attract's come from Map1, 2, 4 and 5, the
  fight's from Map3. The other 39 are one index throughout: tiles a face drew
  while the loader had not reached their rows yet. They hash as such, so a
  later frame with the real rows misses and cuts.
- **A/B in Flycast** (the same 1ST_READ.BIN, the disc with and without the
  pack, 330 s of attract from boot): the hash at f1500 is a5d21d21 on both,
  #498's, so the cut moved into `dct_cut` unchanged and the pack draws the
  same picture. Texture time up to f1500 (`tx` on the hash line) fell from
  695 ms to 37 ms. The f3500-3900 bench did not move (18800 ms against
  18791): attract cut only 5 ms of textures in that window. The bench is
  slower than #498's 14274 ms on both sides. That was put down to another
  session's Flycast sharing the CPU; it was the AOT, which had turned itself
  off (#509).
- **What it means for a console.** Flycast's disc is free, so the 658 ms are
  the SH-4's cut alone. On a GD-ROM the pack only pays if its reads are
  sequential with what the frame already reads or done at a load screen: a
  seek is ~100 ms, so a texture read on demand mid-fight costs more than the
  cut it saves. The frame-ordered layout and the 64 KB window are there for
  that; reading a scene's run of the pack during the game's own load (with
  the STRIPS.PAK run beside it) is the next step, and only hardware can say
  what it is worth.

## STRIPS.PAK by scene: groups (#504)

The question: a fight shows a stage and two fighters, so if the pack lay
by stage and by fighter, could the port read "Sonic and Bean" whole, in
one read each, and keep them in RAM while they are on screen? Fewer reads
mean fewer seeks on a GD-ROM.

`tools/dc_strips.c --groups dreamcast/sfight.mdlgroups` lays the blobs out
by object group, the same groups as MODELS.PAK's tried `--groups` (#489):
a stage, a fighter, common, the select screen, the story scenes, "other"
for a model no group names. Each group starts on a sector, the groups lie
in the order of the first frame that draws from them, and within a group
the meshes lie in the order they were first drawn. A table after the index
gives each group's place (`dcs_group_t`); `mkdisc.sh` passes `--groups`.
The bench line under the pager's loads now counts the commands sent to the
drive (`rd`), the seeks among them (`sk`: a read that does not start where
the last one ended) and the strip pack's page loads (`sp`).

**What a scene draws** (`det_digest` with every frame's mesh keys logged,
against the grouped pack):

| stretch | strip bytes drawn | their groups whole |
|---|---|---|
| attract's Sonic vs Bean, Flying Carpet (the bench, f3500-3900) | 1481 KB | 2654 KB |
| the scripted fight, Sonic vs Knuckles, South Island, per 1000 frames | 1207-1390 KB | 2380 KB |
| the scripted fight, all 9000 frames | 2697 KB | 3412 KB |

In strips the fighters are not small. In the scripted fight Sonic draws
417 KB, Knuckles 226, Amy 213; Bean draws 84 KB on the bench. A stage
draws 421 KB (South Island) to 734 KB (Flying Carpet). Another 256-491 KB of a
fight comes from the "select" group, which holds models the fight draws
too, and 86-191 KB from common. Their textures in TEXTURES.PAK (#502) are
another 50-96 KB a fighter and 640-1350 KB a stage. The port has no RAM
for that: the board's memory takes most of the 16 MB. What is left is the
frame pool (1184 KB), the mesh cache's 1 MB arena and ~700 KB of heap
headroom. The meshes a fight draws in 100 frames already fill the arena.

**Measured in Flycast** (PS3 files, the same attract from boot, f3500-3900;
the frame-1500 hash is a5d21d21 on all of them):

| disc | page loads | code | data | strip pages | drive reads | seeks |
|---|---|---|---|---|---|---|
| A: base (082fac4), pack by first frame | 686 | 340 | 74 | 259 | 749 | 671 |
| B: groups read whole into a 1 MB block, arena 256 KB | 1452 | 890 | 168 | 363 | 1530 | |
| X: pack by groups, read through the pager | 612 | 310 | 67 | 222 | 675 | 594 |
| Y: X, and a missing strip page brings in the next 3 of its group | 721 | 355 | 76 | 277 | 784 | 615 |
| the disc (X without the groups' 512 KB parts) | 616 | 305 | 69 | 229 | 679 | 605 |

- **B, whole groups in RAM, lost.** The first mesh a scene asked of a group
  read the whole group (cut into 512 KB parts) into a block of its own with
  one read; its meshes were drawn from there, and the least recently drawn
  group that neither this frame nor the last drew made room. The block came
  out of the frame pool and the arena shrank to 256 KB to pay for it. The
  bench's groups did not fit: 14 groups read (1558 KB) and 14 dropped in
  400 frames, and 1782 meshes fell back to the pager because the block was
  full of groups in use. The smaller pool doubled the code's page loads
  (890 against 340). Seeks were not counted yet, but reads doubled.
- **X, the layout alone, won.** Through the pager the same scene now takes
  fewer 16 KB pages: a page holds meshes of one group, which the scene
  draws together, not whatever the frame order put next to them. Strip
  pages fell by 14%, and with less of the pool taken, code pages fell by
  9%; seeks by 11%. The disc's pack has no parts (only the block needed
  them), which moves pages a little: 10% fewer loads and seeks than A. The difference from MODELS.PAK's groups (#489, #492),
  which lost, is what goes in: MODELS.PAK's groups held every object of a
  group (20.3 MB), STRIPS.PAK holds only meshes the recorded frames drew
  (8.3 MB), so a group's pages hold little the scene does not draw.
- **Y, read-ahead, lost.** Reading the next pages of the group in the same
  pass makes one seek of several reads, but the group's next pages are not
  what this stretch draws next; they push code out (355 against 310).
- **So the disc lays STRIPS.PAK out by groups and reads it as before.**
  Whole fighters in RAM would need the board to leave more room, and
  reading a scene's groups at a load screen into the page cache the same.
  On hardware a seek is ~100 ms; the counters say where they go (code
  first), not what they cost, and only hardware can say that.

## TEXTURES.PAK by group (#508)

The question: STRIPS.PAK by group took fewer pages (#504), so would
TEXTURES.PAK by group take fewer reads? `det_digest --tex-groups
dreamcast/sfight.mdlgroups` lays the textures out the way `dc_strips
--groups` lays the meshes: a texture goes in the group of the model that
first drew it, each group from a sector, the groups in the order of the
first frame that draws from them, and by frame within a group. The pack
keeps each entry's model past the textures (`models_off`, which the
Dreamcast never reads), so the fight's run that adds to attract's pack
knows the groups of the textures it loaded. The same 280 textures go in:
South Island 980 KB, adv 399, Flying Carpet 510, Tails' Lab 415, Aurora
Icefield 238, common 88, the fighters 8-52 KB each.

**Measured in Flycast** (one 1ST_READ.BIN, three discs that differ only in
TEXTURES.PAK, attract from boot). The window reads are `rd` on the `pk` line,
which now leads its HUD row (it ran off the screen's edge before):

| pack | window reads to f6300 (243 of 247 hits) | f3500-3900 texture reads | f1500 hash |
|---|---|---|---|
| f: by frame (#502's layout) | 160 | 0 | a5d21d21 |
| g: by group | 166 | 1 | a5d21d21 |
| h: by group, no sector per group | 166 | 1 | a5d21d21 |

The f3500-3900 bench came out the same on all three (18809 ms, `rd 680 sk
605`; this Flycast shared the CPU, as in #502).

- **By group lost, by 4%.** A scene draws the textures of its stage, two
  fighters and common together. By frame, the ones a scene draws first lie
  together, whatever group they are in: one 64 KB window holds the
  scene's next ones. By group, the scene's textures are split among its
  groups, so a scene that brings in new ones from three groups reads three
  windows where frame order read one or two. The mesh pages won by group
  because a mesh is drawn again and again from the pager's cache, so what
  counts is which meshes share a 16 KB page; a texture is read once, into
  video memory, so what counts is the order of first use.
- **The padding was not the cause:** without the sector per group (h) the
  count is the same.
- **So the disc's pack stays by frame;** `--tex-groups` stays in
  `det_digest` for a later try (a scene's textures read whole at its load
  screen, say, where whole groups are the unit).

## The AOT was off, and its map was stale (#509)

The ask was a faster ahead-of-time compiler. Two things stood before the
compiler itself, and both failed without a word.

**No disc since the master sync of #221 ran the compiled code.** `aot_check`
refuses the whole AOT when the profile has a hook at an address the generated
code compiled over, because a hook there would never run. The Makefile named
the hooks by hand, and master's CPU difficulty hook at `0x3B274` was not among
them. The refusal is a `LOG_WARN`, which a disc sends nowhere, so the program
ran interpreted and looked fine. The benches of #502, #503, #504 and #508 (a
slice of ~10000 ms) were taken that way. #498's was the last with the AOT on:
its slice, 6467 ms, is the old-map row below. Now:

- the Makefile reads the hook addresses out of the profiles' own tables
  (`PROFILE_HOOKS`, from `src/profiles/sfight.h` and `sfight_console.h`), so a
  new hook is a boundary in the next build without anyone naming it;
- `aot_check` keeps its reason (`aot_off_why()`), and the HUD's stats show
  `aot off: <reason>` when there is one. Read that row before a bench.

**The map no longer listed the hottest code.** `sfight.aotmap` was recorded
while Gems' C still ran `get_frame_dat` (`0x304C8`). Since the console profile
left that function to the i960 (`gems_trap_left_to_i960`; no longer, see
below), it and
`get_fcurve_value_f` (`0x30C28`-`0x30E04`) were three quarters of everything
the interpreter ran in a fight, and none of it was in the map. Re-recorded
(the recipe is in `dreamcast/README.md`), the scripted fight interprets
2.7 million instructions on the host where it interpreted 20.3 million. The
map has to be recorded again whenever the set of functions left to the i960
changes: a Gems trap dropped or added, a hook removed.

**Then the compiler, a little.** A store in compiled code wrote its own
address to `g_last_store_ip` (and `g_mem_last_write_ip`), which only log
lines and the COP's `ip_*` fields read. On the SH-4 each was a literal-pool
load and a store at every store site. A build without the tools
(`M2HLE_DEV_TOOLS` 0) no longer writes them (`AOT_MARK`), and a chunk keeps
the RAM base in a local (`ram_`) instead of loading `bus->ram` at each access.
That is 35 KB less code, which was worth one 256 KB step of the pager's cache.

The bench (full HUD, Gems, fight frames 3500-3900, Flycast's libretro core,
`FPS_CAP=0`); the hash at frame 1500 is a5d21d21 on every row:

| | total ms | slice | draw | text |
|---|---|---|---|---|
| before (AOT refused) | 18789 | 10018 | 8691 | 1,576,348 |
| hooks from the profiles (AOT on, old map) | 15216 | 6459 | 8693 | 1,576,036 |
| + the map re-recorded | 14342 | 5581 | 8697 | 1,842,932 |
| + no store marks, `ram_` | 14327 | 5552 | 8708 | 1,808,116 |
| the same at `AOT_COVER=0.995` (not the default) | 14136 | 5354 | 8718 | 2,146,228 |

The slice is 45% shorter and the 400 frames 24%. Against #498, the last
bench with the AOT on, the slice is 14% shorter (5552 ms against 6467). The
draw is not: 8.7 s here on every row where #498 measured 7.7 s. That came
in between #498 and this branch's base, with the AOT on or off, and was not
looked into here.

**The board cannot tell.** `det_digest --cpu --gems --profile sfight_console`
on the host is identical with and without the compiled code, 3000 frames of
attract and the 3200-frame scripted fight, for the old map, the new one and
the new one without the marks. The compiled share of the i960's instructions
rose from 43.9 to 50.8 million in attract and from 51.5 to 69.1 million in
the fight.

**Where the i960's side stands** (`HUD=prof`, PC samples in attract's fight,
frames 3572 and 4912):

| | aot | interpreter | gems | cop | geo | draw | i960 ms/slice |
|---|---|---|---|---|---|---|---|
| old map | 6% | 13% | 12% | 4% | 36% | 17% | 16 |
| new map | 11% | 7% | 14% | 4% | 35% | 16% | 13 |

The interpreter's column is the run loop too (`emu_slice_body`, 3%). Compiled
code is 774 KB for 7405 instructions, 104 bytes of SH-4 each.

**Then `get_frame_dat` went back to Gems' C.** The console profile left it
to the i960 because its head-tilt hook (`0x30608`) is inside the function and
Gems' C would have skipped it. Now the C runs the hook: its fade-out loop
loads r10-r12 as the ROM's loop holds them and calls the profile's handler
(`gems_inner`, `core/gems.h`), so all 47 traps are on for the console profile
too. The private Gems folder needs its matching `get_frame_dat.h`; one from
before does not name the hook (`GEMS_INNER_SITES`) and the function stays
with the i960, as it did.

- `--gems-verify` on every call (`M2HLE_GEMS_VERIFY_ALL=1`, `--profile
  sfight_console`) holds the C against the i960 with the hook: 5220 of 5220
  `get_frame_dat` calls exact in the scripted fight, 4618 of 4618 in 3000
  frames of attract. With the hook call taken out of a copy of the C, 278 of
  the fight's 5220 differ, so those runs do reach the head tilt.
- The map was recorded again. `get_frame_dat` and `get_fcurve_value_f` are
  gone from it, and the generator compiles 7025 instructions where it
  compiled 7405.
- A trapped function is charged as one instruction, so the timers and `rand`
  see less time: the frame-1500 hash is 634d853f now, with the compiled code
  and in a build without it. On the host, `det_digest` with and without the
  compiled code is identical again over the same two runs.

| | total ms | slice | draw | text |
|---|---|---|---|---|
| `get_frame_dat` compiled (the fourth row above) | 14327 | 5552 | 8708 | 1,808,116 |
| `get_frame_dat` in Gems' C | 13970 | 5208 | 8698 | 1,738,404 |

The slice is 6% shorter again, 48% against the refused AOT, and the code is
70 KB smaller. The profile table above was not taken again.

**What is left.**

- **A wider cover costs memory.** The map's weight is 77% idle loop, which a
  hook skips and nothing compiles, so 0.99 of it still leaves about 4% of the
  real work to the interpreter. At 0.995 the generator compiles 8983
  instructions instead of 7405 and the slice is 3.6% shorter again, for 338 KB
  more code: the pager's cache falls a step, from 1280 KB to 1024 KB, and
  448 KB of heap is left instead of 512. Flycast's disc is free, so the
  smaller cache does not show there; on a GD-ROM it is seeks. 0.99 stays the
  default. `AOT_COVER=0.995` kept the frame-1500 hash; it was not run through
  the host's `det_digest`.
- **Stores to tile and texture RAM** leave the compiled path through
  `aot_io_x` to `mem_write16`. In a load scene that helper is 10% of the
  samples and the interpreter 14%, beside the texture loader's rows (21%) and
  the tile layer (15%). Flycast runs those scenes at 87 fps, so nothing was
  done about it; an inline path for the two regions is the next thing to try
  if hardware says loads are slow.
- The i960's side is a third of a fight's frame: Gems' C 14%, compiled code
  11%, the interpreter and the run loop 7%, the COP 4%. The 3D decode and the
  draw are half of it.

## Spaghetti in the port (#522)

SPAGHETTI.md's measure (lizard's modified cyclomatic complexity, the share of
function code in functions over 20 paths) applied to the port: `tools/spaghetti.py
--tree dreamcast` measures `dreamcast/` on its own, since the port is not under
`src/`. At `cfe6b54` (the trunk after #236) the port was 169 functions over
3,249 lines, and 12 functions over 20 paths held 1,206 of them, **37.1%**
(against 4.1% in `src/`). The worst: `main` (112 paths over 438 lines),
`dp_tiles` (67 / 91), `dcs_blob` (45 / 76), `jt_run` (38 / 127), `ds_init`
(36 / 62), `pg_init` (33 / 78), and in `src/` the port's own `aot_check`
(68 / 69), which only the Dreamcast build compiles.

Three were untangled, each into named steps called in the old order:

- **`main_dc.c`'s `main`** was the boot, forty locals for the statistics, the
  run loop, the HUD's three variants, the FPS cap, the frame hash, the bench,
  the 2 s stats window and the halt screen in one body. The boot is `dc_boot`
  (with `dc_boot_sincos`, `_cache_size`, `_heap_left`, `_line`, `_calib`), the
  locals are `dc_stats_t` and the window's `dc_window_t`, and the loop calls
  `dc_hud_fps_corner`, `dc_hud_live`, `dc_fps_cap`, `dc_hash_frame`,
  `dc_bench`, `dc_stats_slice` and `dc_stats_window`, which draws the window
  through `dc_hud_cpu` / `dc_hud_window` (`DC_HUD_PROF`) or `dc_stats_ib_why` /
  `dc_stats_rows`. 112 paths to 9; the largest piece is `dc_boot` (17). Every
  row string, row number and reset is where it was, and so are the watchdog
  (`wdt_pet` first in the loop, off before the halt screen) and `dc_assert`.
- **`dp_tiles`** (dc_pvr.h) kept its last-seen generations and dirty state in
  statics and did the pen table, the line-scroll strips, the tile copy, the
  dirty extents, the CPU redraw, the recolour and the PVR show inline. That is
  `dp_tiles_state_t` and `dp_tiles_pens`, `_ls`, `_copy`, `_extents`,
  `_redraw`, `_recolour`, `_show` (which calls `dp_tiles_send`): 67 paths to
  17 over 23 lines. The state is zero-initialised and the first draw puts the
  `~0u` sentinels in: a designated initializer put the whole 4.7 KB struct
  (the dirty blocks, the row spans) into `.data`, and so into 1ST_READ.BIN.
- **`aot_check`** (src/core/i960_aot.h) is `aot_hook_known`,
  `aot_check_profile`, `aot_plain_extent`, `aot_paged_extent` and
  `aot_check_devices`: 68 paths to 9, the largest piece `aot_check_devices`
  (20). The same `AOT_OFF` messages in the same order, and `s_aot_on` is
  false before every one.

After: 200 functions over 3,306 lines; 10 over 20 paths hold 677, **20.5%**,
and nothing is over 50 (`dcs_blob` 45, `jt_run` 38, `ds_init` 36, `pg_init`
33 are the next candidates). `src/` went from 4.1% to 3.9%.

How it was checked, on `LINK=1` builds of the tree before and after:

- **The board is identical.** `tools/dc-lockstep.py --boot --no-mame --frames
  500` (Flycast against the desktop `det_digest`, every frame's CRC of work
  RAM, RAM, bufferram, tile RAM and the palette) on a disc of each build:
  `desktop: identical` in every region for all 500 frames, before and after.
- **The pictures are identical but for the HUD's own numbers.** `--shots 50`'s
  ten frames from each disc differ only in the text rows (the frame counter,
  fps and ms the host happened to measure); the heap row reads 640 KB on both.
  The first version reported 16384 KB: inlined into `dc_boot`, the probe's
  `malloc` / `free` pair was removed by gcc together with the failure test, so
  `dc_boot_heap_left` now uses the pointer in an `asm volatile`.
- **The compiler says the same things.** Both builds give the same 1,044
  warnings, line for line (`-Wall -Wextra`); the text is 1.4 KB smaller
  (1,699,280 against 1,700,704) and `.data` and `.bss` are within 200 bytes.

## Toolchain and runtime traps

- **`uint32_t` is `long` on sh-elf.** `%u` / `%x` with a `uint32_t` is a format
  mismatch there (`-Wno-format` for now). Casting, or `PRIu32`, works on every
  target.
- **KOS builds with `-m4-single`, so `double` is emulated in software.** The
  board's float paths have to stay `float` on the SH-4. On the ARC-S, double is
  cheap but halves the SIMD width.
- **Build with `-DNDEBUG`.** Without it every log line also goes to stderr,
  which on KOS is the serial port at a few KB/s. Even a `printf` that nobody
  reads costs (`scif_write`, ~2% of a fight), so `main_dc.c` points dbgio at
  `null` (#370).
- **The libretro core has no serial console.** dbgio output is lost, so the
  stats and the halt diagnostics are drawn on screen (`dc_text`). With
  `dreamcast/tools/flycast-scif.patch`, `FLYCAST_SCIF=host:port` carries the
  SCIF to a TCP socket (#461).
- **KOS's newlib has no `sched_yield` and no `FIONREAD`.** thread_mutex.h uses
  `thd_pass`, and net_socket.h stubs the ioctl.

## Next optimization targets

- **Other line-scrolled screens.** The PVR strips cover tilemap 2 alone
  behind, and tilemap 0 too when its pair is laid out the same way (#481).
  A per-line scroll on tilemap 1 or 3, or a window mask on the view, still
  redraws whole lines on the CPU. The attract's
  "REVENGE OF DR. ROBOTONIC" banner costs ~18-27 ms a frame.
- **The 3D decode (title: 46-76 ms).** Now mostly the cached path. The SH-4's
  `ftrv` for the vertex transform, and the store queues for the vertex
  submission. A smaller `geo3d_cface_t` (~150 bytes; u16 indices, integer
  texture fields) would let the mesh arena (768 KB) hold more.
- **The i960 slice (~13 ms a frame in the bench after #509).** A third of a
  fight's frame: Gems' C, the compiled code, the COP, and 7% still
  interpreted. A wider `AOT_COVER` needs memory the board does not have
  (#509), so smaller compiled code (104 bytes of SH-4 an instruction) is what
  would buy more of it. Keeping the hot `cpu` / `bus` state in the 8 KB
  operand-cache RAM mode.
- **Optional:** modifier-volume shadows; dropping SDL2 and GLdc for KOS's
  `snd_stream` directly.
