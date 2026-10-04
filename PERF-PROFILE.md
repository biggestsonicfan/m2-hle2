# Where m2hle spends its time (Pinboard #399)

A `perf` profile of master after the spaghetti removal (#181, `c51bf9a`), on x86-64 Linux (the dev
container: WSL2, Ubuntu 24.04, gcc 13, `-O3`). STF, `sfight.zip`, attract. Three runs:

| run | what it is | what it shows |
|---|---|---|
| **bench** | `arc_bench --sound --frames 30000`: 30,000 frames of attract (intro, the replay fight, VS screens, several loops) as fast as it goes, one emu+render thread and the sound thread | the board, and the CPU-side renderer of the handheld / libretro builds (tiles composed on the CPU, sokol's dummy backend) |
| **desktop** | `m2hle --headless --av-port` at 60 fps for 70 s with a client draining the stream, Mesa d3d12 on the RTX 3070 | what a stream or a desktop window costs: tiles on the GPU, the GL driver, the A/V capture |
| **emu only** | `m2hle --headless` with no `--av-port` | the same board with nothing drawn |

`perf stat` over the bench: **3.19 instructions per cycle, 1.18% of branches mispredicted.** The
interpreters are not stalling on their dispatch; the cost is the work itself, so the
way to make anything here cheaper is to do less of it, not to predict it better.

## Per frame

From the bench's own timers (x86, flat out, ~1,100 frames/s): **emulation 0.52 ms, render
0.37 ms** (tiles 0.21, 3D 0.15) on the main thread, plus the sound board on its thread.
At 60 fps on the desktop run, all threads together cost **4.0 ms of CPU a frame**: main
(render + capture + GL driver) 2.3 ms, emu thread 0.95 ms, sound thread 0.71 ms. (The emu
thread costs more paced than flat out: at 60 Hz its caches are cold at every slice.)

## The bench, by subsystem

Inclusive, as a share of all samples on both threads (`perf report --children --inline` on a
`--call-graph dwarf` recording):

| | share | made of |
|---|---:|---|
| **i960 + bus + COP** (main thread) | 32% | interpreter dispatch `i960_step_core` 7.7%, bus `mem_ea` / `mem_fetch2` / `mem_read32` / `mem_write32` ~7%, the COP (`coprogram_write_cb` → `sharc_exec`) 3.5%, GEO list publish ~1% |
| run-loop overhead in `emu_slice_body` | 4.6% | timers, interrupt checks, the slice loop |
| **CPU tile compositor** (handheld/libretro only) | 18% | `s24_draw_line` 4.3%, `s24_cell_load` 2.9%, `tile_cpu_recolour` 2.1%, the colour-line `memcpy` 1.1% |
| **3D, CPU side** | 12% | `geo3d_decode_model_cached` 10.6% with the mesh cache on: the per-face work every frame (`geo3d_cached_face` 7.4%: `geo3d_board_luma` 1.5%, `geo3d_flat_depth` 1.4%, `geo3d_board_normal` 0.8%, cull 0.8%, emit ~1%) |
| **sound board** (own thread) | 31% | 68000 `m68k_step_core` 22% (of which SCSP register reads and writes ~12%), `scsp_sync` 11.6%: DSP program `scsp_dsp_prog_sfight_fvipers` 9.4%, slot PCM `scsp_slot_run_pcm` 7.3% |

## The bench, by inlined function (top 30, exclusive)

`python3 tools/perf_inline.py attract.data arc_bench` — every sample charged to the innermost inlined
function at its address, which `perf report` cannot do (it puts 31% on `emu_slice_body`):

```
  7.66%  i960_step_core                 2.09%  scsp_pcm_wave                  1.37%  geo3d_flat_depth
  6.40%  m68k_step_core                 2.05%  tile_cpu_recolour              1.36%  mem_read32
  4.87%  scsp_slot_run_pcm              2.04%  memcpy                         1.34%  hle_check_synced
  4.76%  scsp_dsp_prog_sfight_fvipers   2.00%  scsp_dsp_unpack                1.21%  emu_timers_after_step_fast
  4.31%  s24_draw_line                  1.87%  scsp_dsp_pack                  1.20%  m68k_rw
  2.91%  s24_cell_load                  1.85%  sound_run                      1.10%  m68k_fetch
  2.85%  mem_ea                         1.70%  m68k_direct                    1.04%  scsp_sync
  2.77%  emu_slice_body                 1.63%  i960_cycle_cost                1.00%  geo3d_cached_face
  2.46%  scsp_slot_env                  1.58%  scsp_ram_w                     0.94%  mem_write32
  2.30%  mem_fetch2                     1.45%  geo3d_board_luma               0.90%  m68k_wait
```

## The desktop run

The main thread is 57% of the process: our code 54% of it, the GL driver stack (NVIDIA's
user-mode driver, Mesa's gallium and d3d12) 32%, libc 11%. Of our part, `game_frame_draw`
(the 3D, the same `geo3d_cached_face` work as above) is the largest, then the A/V capture's
copy of each frame into the stream ring (`av_capture_submit`, 4.2% of the process, only while
streaming), then the tile upload (`video__compose_gpu` → `sg_update_image`, 3.8%). The emu and
sound threads look like the bench's. Loading the zip (`tinfl_decompress`, `mz_crc32`) is about
0.7 s of CPU once, at boot.

## Where to look, if something is to be made cheaper

In order of what it would save, as #399 found it (#403 below took the first two); each has to stay bit-exact
(`det_digest`, `ab-builds`, `arc_bench --draw-digest`).

1. **The 3D's per-face work, every frame** (~12% of the bench, the largest of our own costs on the
   desktop's main thread). The mesh cache keeps the decode, but luma, flat depth, the corner
   normal, the cull and the emit run per face per frame.
2. **The CPU tile compositor** (18% of the bench), for the handheld and libretro builds only.
   #211 already draws only what changed; attract scrolls, so much of it changes.
3. **The i960's per-instruction bookkeeping**: `i960_cycle_cost` 1.6%, `hle_check_synced` 1.3%,
   `emu_timers_after_step_fast` 1.2%. Together about an eighth of the i960's time, paid on every
   instruction whether or not a hook, a timer or a cost table entry has anything to say.
4. **The 68000's SCSP traffic**: register reads that make the slots catch up (`scsp_r16` →
   `scsp_latch_now`, 6%) and writes that sync the chip (`scsp_write`, 8.3%). By design (CLAUDE.md,
   "The SCSP makes its samples late"); STF's driver reads the monitor every ~5 samples.
5. **Copies**: the GEO publish (`geodl_publish`, ~1-2%, texture/polygon RAM beside each list),
   the A/V capture (4%, streaming only).

## What #403 did with it

Measured with `arc_bench` on attract, the box's other load alternated out (base, new, base, new...,
five runs each), and with `perf stat -e instructions:u,cycles:u` for a number that the load does
not move. Every change is bit-exact: `tile_test`, `arc_bench --draw-digest` over 12,000 frames,
and an FNV of the two tile layers' RGBA after every frame (12,000 frames, 4,525 different
pictures), base against new.

| Change | Where | Gain |
|---|---|---|
| `s24_draw_line` walks runs, not pixels: a run stays in one cell, one window-mask group and one side of the split, so the mask bit, the split and the cell key are looked at once per run | tiles | tiles −13% |
| A non-opaque pass decides a cell of the other category is dead from its tile word and never reads its graphics (most cells of the four front passes) | tiles | −1.4G instructions per 4,000 frames |
| A palette change recolours every pixel, with no test: each output pixel already holds its pen's colour, so that is the same bytes, and the per-pixel "did my pen change" branch was the mispredicted one | tiles | tiles −21% with the run walk |
| `geo3d_cached_face` looks up a face's palette colour only once it is drawn, after the cull | 3D | the geo list −23% instructions |

Together: tiles 0.36 → 0.26 ms a frame (−28%), 3D 0.136 → 0.128 ms, render 0.51 → 0.40 ms
(−21%); the whole bench (ROM load included) 55.3G → 45.7G instructions, 16.7G → 14.9G cycles.
On x86. On the handheld, see the next section: tiles 2.40 → 1.98 ms.

What was looked at and left:

- **Skipping the SCSP DSP while its input is silent.** The input (`mixs`) is zero in nearly every
  sample, but the reverb never settles: once the first sound has gone in, the delay line sits in a
  limit cycle that neither decays nor repeats (about 10,000 of its 32,768 words change every
  revolution, `efreg` stays at −1, −1). Only the samples before the first input are skippable
  exactly: 30% of attract, 55% of a scripted fight, all of it in a session's first minutes.
- **Idling the 68000.** STF's driver polls the slot monitor, which changes every sample, so its
  wait loop is never idle. `--sound-hle` already runs that driver in C.
- **The i960's per-instruction bookkeeping.** `i960_cycle_cost` is one table load and
  `hle_check_synced` one bitmap test; what is left is the interpreter itself, which #187 and
  #294 have been over. The texture loader and the spin skip read the cycle accounting, so a
  change there is a board change, not a host one.
- **The zkey of a culled face.** `geo3d_flat_depth` has to run for every face (a culled face
  still sets the previous z), and the key itself is a few integer operations.

## What #406/#408 did with it, and the handheld

**The GEO publish copies only what changed** (`geodl_publish`, `geo_raster_publish` in
`memory.h`). Each published list used to be a copy of all 512 KB of bufferram, and texture,
polygon and log RAM were copied whole whenever a list had written any of them: STF rewrites the
eyes' texture points and the material slots every frame, so that was ~290 KB a frame for a few
hundred bytes of change. Now the list walk that applies the state copies the words it visits
(the renderer's walk takes the same steps and reads nothing else), and a write marks its
256-byte block dirty for each published copy. A profile in the homebrew list format
(`quirks.geo_displaylist`) still gets all of bufferram. `dump_geo_list` writes the published
snapshot, so outside the list's own words it now holds older frames' bytes.

Exact: the published texture, polygon and log RAM against live after every publish, and the
renderer's walk over the partial copy against a full one, 0 mismatches over 12,000 frames;
`--draw-digest` identical over 12,000 frames of STF and 8,000 of Fighting Vipers; `det_digest
--cpu` identical over 9,000. x86: 2.4% fewer cycles a frame.

**On the RG ARC-S** (RK3566, Cortex-A55), `arc_bench` cross-built (`-mcpu=cortex-a55`), 3,000
frames of attract, the clock pinned at 1416 MHz so throttling cannot pick a winner, the three
builds alternated four times, each run started below 57 C. The four runs of a build agreed to
0.02 ms:

| Build | emu ms | render ms | tiles ms | 3D ms | frame ms |
|---|---|---|---|---|---|
| master before #403 (d641b75) | 3.36 | 3.86 | 2.40 | 1.35 | 7.22 |
| #403 (3c0428e) | 3.36 | 3.42 | 1.98 | 1.34 | 6.78 |
| #403 + GEO publish | 3.12 | 3.42 | 1.98 | 1.34 | 6.54 |

So on the device #403's tile work is −17.5% tiles and −11% render (it was −28% / −21% on x86),
its 3D change does not show, and the GEO publish is −7% of the emu thread. Together −9.4% of a
frame's CPU, 138 → 152 frames a second flat out.

## How to take one

`perf` comes from `linux-tools-generic` (in the antigravity-dev image; `/usr/local/bin/perf`).
Build with symbols and frame pointers, at the release optimisation:

```bash
cmake -S . -B $B -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-g -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer"
cmake --build $B -j6
# arc_bench is not a CMake target; det_digest's flags build it:
gcc -g -fno-omit-frame-pointer -O3 -DNDEBUG -std=gnu11 -ffp-contract=off -fno-strict-aliasing \
    -DM2HLE_BUILD_FLAVOR=\"release\" -DM2HLE_VERSION=\"dev\" -DM2HLE_DEV_TOOLS=0 \
    $(grep C_INCLUDES $B/CMakeFiles/det_digest.dir/flags.make | cut -d= -f2-) \
    tests/arc_bench.c -o $B/arc_bench $B/libminiz.a -lpthread -ldl -lm
```

Then, from a scratch directory (never the ROM folder):

```bash
perf record -F 2000 --call-graph fp -o attract.data -- $B/arc_bench $ROMS_DIR/sfight.zip --sound --frames 30000
python3 tools/perf_inline.py attract.data $B/arc_bench --top 40          # by inlined function
perf report -i attract.data --no-children --sort srcline --stdio -g none   # by source line
# inclusive, through the inlining: a dwarf unwind (big: 170 MB for 12,000 frames at 500 Hz)
perf record -F 500 --call-graph dwarf,16384 -o dwarf.data -- $B/arc_bench ... --frames 12000
perf report -i dwarf.data --children --inline --sort symbol --stdio -g none
python3 tools/perf_inline.py dwarf.data $B/arc_bench --callers __memcpy_avx_unaligned_erms
```

The desktop path: `GALLIUM_DRIVER=d3d12 perf record ... -- timeout -s INT 70 $B/Release/m2hle
--headless --rom $ROMS_DIR/sfight.zip --profile sfight --run --no-nvram --av-port 39399
--av-size 496x384`, with a client reading the port (any socket that reads and discards);
without a client nothing is drawn.
