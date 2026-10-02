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

| | D1 (tiles only) | D2: FBI picture | D2: title | #355: title | #355: attract | #358: title |
|---|---|---|---|---|---|---|
| board fps | 7.9 | 11.4 | 2.2 | 3.1-3.4 | 5.7-6.2 | 5.1-5.9 |
| i960 slice | ~88 ms | 46 ms | 83 ms | ~80 ms | ~98 ms | ~80 ms |
| tile layers | ~36 ms | 1 ms | ~128 ms | ~129 ms | 1-2 ms | 5-6 ms |
| 3D decode | - | 28 ms, 745 tris | ~197-384 ms, ~2,330 tris | 46-76 ms, ~2,300 tris | ~30 ms, ~3,000 tris | 47-75 ms, ~2,300 tris |
| depth sort | - | 1 ms | 3 ms | 3 ms | 4 ms | 3 ms |

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

## Toolchain and runtime traps

- **`uint32_t` is `long` on sh-elf.** `%u` / `%x` with a `uint32_t` is a format
  mismatch there (`-Wno-format` for now). Casting, or `PRIu32`, works on every
  target.
- **KOS builds with `-m4-single`, so `double` is emulated in software.** The
  board's float paths have to stay `float` on the SH-4. On the ARC-S, double is
  cheap but halves the SIMD width.
- **Build with `-DNDEBUG`.** Without it every log line also goes to stderr,
  which on KOS is the serial port at a few KB/s.
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
- **The i960 slice (46-88 ms).** The interpreter on a 200 MHz SH-4. Options:
  a threaded or cached decode, or keeping the hot `cpu` / `bus` state in the
  8 KB operand-cache RAM mode.
- **UTLB reach.** Map the hot code pages with 64 KB pages.
- **Optional:** modifier-volume shadows; dropping SDL2 and GLdc for KOS's
  `snd_stream` directly.
