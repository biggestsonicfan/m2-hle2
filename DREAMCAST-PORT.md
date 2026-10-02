# The Dreamcast port (Pinboard #340)

The board on a Sega Dreamcast under KallistiOS, tested in Flycast's libretro
core. [dreamcast/README.md](dreamcast/README.md) has the build and the disc.
This file covers what was measured, and the traps that cost the most time.
Most of them apply to any small target, the ARC-S included.

## Where it stands (milestone D1)

STF boots under Flycast through BACKUP RAM IS BROKEN, the SEGA logo, the title
and attract, and shows FREE PLAY. Only the tile layers are drawn. There is no
3D (the PowerVR is D2), no sound board (the AICA is D3) and no netplay. The
pad is mapped but untested.

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
- **Dedupe the ROM.** `m2pack` stores each distinct page once, so mirrors,
  repeats and zero pages cost one table entry each. 21120 pages become 10005
  stored (39.1 MB).
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
