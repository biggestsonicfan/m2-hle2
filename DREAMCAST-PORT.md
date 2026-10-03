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
  the full CPU layer.
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
- **Let the host own the regions.** `g_mem_window` (memory.h) lets a host give
  the bus a window for MAIN_DATA / XTRA_DATA / VID_EXT_RAM instead of a heap
  copy. The profile skips its memcpy when the window already is the ROM. That
  saves a 16 MB copy on any target.

## Paging with the SH-4 MMU (dreamcast/dc_pager.h)

- The window is VA 0x10000000 + page × 4 KB. A UTLB miss goes through KOS's
  `mmu_map_set_callback` to `pg_map`, which reads the page off the disc if it is
  not resident.
- **The UTLB has 64 entries, 256 KB of reach**, against a ~2 MB window. Refills
  are cheap, but every one is an exception. Larger pages for hot code are an
  open optimization.
- **Colour the frames.** The operand cache is indexed by VA bits 13:5 and
  tagged by PA. A page whose frame has the same bits 13:12 as its window address
  is never aliased in the cache, so nothing is ever flushed. Each colour has its
  own CLOCK hand.
- **Read the disc from the exception with the GD-ROM syscalls, by PIO, and
  poll.** KOS's cdrom driver takes semaphores and must not be called there.
  Nothing else may touch the drive once the pager is up.
- **Find the data track through the high-density TOC.** The low-density TOC only
  names track 1, a stub. A CD-R has only the low one, so that is the fallback.

## Flycast's MMU: four things that fail without a word

Real hardware was not available. Each of these made the board read zeros or
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
  the GD-ROM syscalls from the TLB miss exception; a second reader would
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

- **An i960 to SH-4 recompiler**, not a faster interpreter. The block replay
  still dispatches every op through a switch and loads its operands through
  pointers. Generated SH-4 code with the i960 registers in SH-4 registers is
  the only step that is several times faster. It needs the same proof as the
  blocks (`det_digest`, `i960_test`, `i960_fuzz`).
- **Skip the 3D on alternate frames** (render every other board frame): ~9 fps
  in a fight, and smoother play rather than more frames shown.
- **The PVR face path**: the store queues for vertex submission, and `ftrv`
  for the transform.

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
  stats and the halt diagnostics are drawn on screen (`dc_text`).
- **KOS's newlib has no `sched_yield` and no `FIONREAD`.** thread_mutex.h uses
  `thd_pass`, and net_socket.h stubs the ioctl.

## Next optimization targets

- **Other line-scrolled screens.** The PVR strips cover only the title's
  case: tilemap 2 alone behind. A per-line scroll on another tilemap, or
  with a window mask, still redraws whole lines on the CPU. The attract's
  "REVENGE OF DR. ROBOTONIC" banner costs ~18-27 ms a frame.
- **The 3D decode (title: 46-76 ms).** Now mostly the cached path. The SH-4's
  `ftrv` for the vertex transform, and the store queues for the vertex
  submission. A smaller `geo3d_cface_t` (~150 bytes; u16 indices, integer
  texture fields) would let the 1 MB mesh cache hold more.
- **The i960 slice (53-79 ms a frame after #370's blocks).** The next step is
  a recompiler (Toward 30 fps, above), or keeping the hot `cpu` / `bus`
  state in the 8 KB operand-cache RAM mode.
- **The tile layer's snapshot.** Each redraw copies and compares 56 KB of tile
  RAM twice (`dc_pvr.h`); swapping two pointers would save one copy.
- **UTLB reach.** Map the hot code pages with 64 KB pages.
- **Optional:** modifier-volume shadows; dropping SDL2 and GLdc for KOS's
  `snd_stream` directly.
