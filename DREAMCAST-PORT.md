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

## Tried and reverted: the board's picture at its own size (#471)

The board's 496x384 drawn pixel for pixel, centred in the 640x480 frame, and a
`FRAME512=1` build whose frame was 512x384 in the middle of the signal (192
PVR tiles rendered instead of 300). Neither was faster over the bench's 400
frames (f3500-3900, Flycast):

| build | total | i960 slices | draws |
|---|---|---|---|
| scaled 1.25 to 640x480 (kept) | 15431 ms | 6431 ms | 8929 ms |
| 496x384 centred in 640x480 | 15433 ms | 6439 ms | 8925 ms |
| `FRAME512=1` | 15427 ms | 6436 ms | 8920 ms |

Repeat runs spread 15431-15435 ms, so all three are noise. Flycast cannot show
a difference: it charges every frame 450,000 cycles plus 100 a byte of polygon
data, whatever the resolution (`scheduleRenderDone`, `core/hw/pvr/spg.cpp`), and
the polygon data is the same. Only `FRAME512` could save time on a Dreamcast
(fewer tiles), it is untested there, and Flycast draws it enlarged and pushed
off to the right. The commit (3af4093) was reverted. `FRAME512=1` stays as an
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
  flip. It has not reached the fight in this replay.

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
- **The i960 slice (~14 ms a frame after #459).** The AOT covers nearly all
  of it; what is left is Gems C, hooks and the COP. Keeping the hot `cpu` /
  `bus` state in the 8 KB operand-cache RAM mode.
- **Optional:** modifier-volume shadows; dropping SDL2 and GLdc for KOS's
  `snd_stream` directly.
