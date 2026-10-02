# The Dreamcast port (Pinboard #340)

The board on a Sega Dreamcast under KallistiOS, tested in Flycast's libretro
core. [dreamcast/README.md](dreamcast/README.md) has the build and the disc.
This file covers what was measured, and the traps that cost the most time.
Most of them apply to any small target, the ARC-S included.

## Where it stands (milestone D1)

STF boots under Flycast through BACKUP RAM IS BROKEN, the SEGA logo, the title
and attract, and shows FREE PLAY. Only the tile layers are drawn. There is no
3D (the PowerVR is D2) and no netplay. Sound is Sega's console approach, ADX
cues behind the sound code (#342, below), not the sound board. The pad is
mapped but untested.

Figures from Flycast's libretro core with the HLE BIOS (emulated time, which is
approximate) at frame 1675:

| | |
|---|---|
| board fps | 7.9 |
| i960 slice | ~88 ms a frame |
| CPU tile compose | ~36 ms a frame |
| pager | 108 evictions, 0 ROM writes, 0 read errors |

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

- **The i960 slice (88 ms).** The interpreter on a 200 MHz SH-4 is the
  bottleneck. Options: a threaded or cached decode, or keeping the hot
  `cpu` / `bus` state in the 8 KB operand-cache RAM mode.
- **The tile compose (36 ms).** Do it on the PowerVR as textured quads (D2)
  instead of on the CPU.
- **UTLB reach.** Map the hot code pages with 64 KB pages.
