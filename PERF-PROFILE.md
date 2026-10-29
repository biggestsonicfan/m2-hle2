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
   #1477 took the timer part and the `g_i960_bt_on` test (below); the cost lookup and the hook
   bit are the two inputs that really change with the instruction.
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

## What #1477 did with the per-instruction bookkeeping

Of what the fast path looked at after each instruction, only the opcode (its cost) and the IP
(the hook filter's bit) change with the instruction. The rest changes only at events:

- `g_i960_bt_on` is fixed for a slice (only daytona, srallyc and vcop set it). The loop reads it
  once and hands it to `i960_step_core`; a global was reloaded after every store.
- The timers: `pending` was a load, an add and a store an instruction, then the horizon test and
  the `intreq & intena` test two loads more. Now the loop stores the boundary and compares it with
  one precomputed cycle count, `g_irqt_due`, where pending reaches the horizon (or 0 while a
  board interrupt is up), and `pending` is brought up only when something reads it
  (memory-bus.md has the rule). The attention word is re-read only when that work ran.

Attract (`arc_bench --sound --frames 3000`, `perf stat -e instructions:u`, the whole process):
50.395G → 49.247G instructions, −2.3%, the same to 30K over two runs; the sound thread does not
change, so it is all the i960's. Bit-exact: `det_digest` frames and sound hashes on attract (3000)
and the scripted fight (4500), `ab-builds --sound` at 600/1800/3000 on sfight and at 600/1800
on the board-timing games.

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

## What the Dreamcast branch has for master (#460)

The Dreamcast port (`idea-340-dreamcast`, DREAMCAST-PORT.md there) was optimised hard for a 200 MHz
SH-4. Most of it is the SH-4's or the PowerVR's: the store queues, `ftrv`, `FSRRA`, the software
pager, the model pack, ADX sound, drawing every other frame. What is board code was measured here on
x86 with `det_digest` over 9,000 frames (attract, then a coin, a start and a punch every 30 frames
into a fight), every variant byte-identical to its base with `--cpu`:

| Dreamcast change | x86 result | for master? |
|---|---|---|
| `MEM_COUNT=0`: no 64-bit bus tallies | instructions −0.2%, cycles unchanged | no: a 64-bit add is one instruction here |
| `MEM_LE_DIRECT`, `IRQT_COUNT_T=int32_t` | GCC and clang already fold the byte loads on x86 and ARM; 64-bit adds are free | no: SH-4 only |
| `I960_BLOCKS`, the decoded-block cache | +1.1% time | no |
| `I960_OPTABLE`, handler-table dispatch | −0.3% time | no |
| `I960_AOT`, the ROM compiled to C (cover 0.99) | −1.1% time; 14% of i960 instructions compiled | no: little gain, and the generated C holds ROM words, so a CI-built core could not ship it |
| `m2_spin_timed`, the texture senders' timer-2 wait skipped | ~15,000 passes in 9,000 frames, instructions −0.007% | no: it mattered there because each pass ended a compiled run |
| tile dirty bits, publishing only the GEO state that changed | master already has both | done |
| the audit's fixes (#458): Gems traps kept off profile hooks, Gems' NaNs canonical, bufferram bounds that wrapped | correctness | yes, ported |

On x86 the i960 is not where the time goes: the 9,000 frames are 228 G instructions, most of them
the sound board and the render, so even the AOT's whole gain is about 1%. The handheld's profile
(above) has the same order, sound board and draw ahead of the i960.

## 2026-10-22: the most expensive functions, again (Pinboard #1474)

The same bench (`arc_bench --sound --frames 30000`) on master at `4075c17`, after everything above,
re-profiled to answer "what is the most expensive function, and is it the SCSP". The box was shared
(load average ~9), so wall-clock times below are rough; shares of samples and instruction counts
(`perf stat -e instructions:u`) are not.

**By thread:** main (i960, bus, COP, 3D decode, CPU tiles, render) 67.5%, the sound thread 32.5%.
Per the bench's own timers, flat out: emulation 0.40 ms and render 0.38 ms a frame on the main
thread (tiles 0.24, 3D 0.12), the sound board about a third again on its own thread. `snd_bench`
puts the sound board alone at **~49x real time** (60 s of sound in 1.22 s), i.e. ~2% of a core at
60 fps; the live thread agrees.

**By real function** (`perf report --sort symbol`, inlining folded in): `emu_slice_body` 28.4% (the
whole i960 + bus + COP + timers), **`scsp_dsp_prog_sfight_fvipers` 9.6%** (STF's reverb, the single
most expensive function of the process), `scsp_slot_run_body` 7.4%, `m68k_step_core` 6.3%, the GEO
list walk 10.4% in pieces, the CPU tile compositor 6.9%, `render_frame` 4.75%, `mem_ea` 3.6%.

**By inlined function** (`tools/perf_inline.py`, top of 39,237 samples):

```
 10.30%  i960_step_core                 2.26%  scsp_slot_env                  1.52%  hle_check_synced
  6.52%  m68k_step_core                 1.88%  s24_draw_line                  1.49%  geo3d_emit_tri_uv
  4.23%  scsp_dsp_prog_sfight_fvipers   1.82%  geo3d_board_luma               1.48%  mem_read32
  3.83%  scsp_slot_run_pcm              1.80%  scsp_pcm_wave                  1.29%  m68k_direct
  3.74%  s24_cell_load                  1.79%  scsp_dsp_pack                  1.15%  sound_step
  3.62%  memcpy                         1.74%  scsp_dsp_unpack                1.14%  tile_compose_cpu
  3.15%  emu_slice_body                 1.65%  geo3d_flat_depth               1.05%  mem_write32
  2.95%  mem_ea                         1.59%  emu_timers_after_step_fast     1.03%  geo3d_board_normal
  2.59%  mem_fetch2                     1.58%  scsp_ram_w                     0.99%  m68k_wait
```

So the hunch was right: the sound thread's 32.5% is the DSP program ~10% (the program body 4.2%,
its 23 float16 unpacks and 10 packs 3.5%, its 33 delay-line RAM accesses 1.6%, the 24-bit sign
extensions 0.7%), the slots ~8% (`scsp_slot_run_pcm` + env + wave), and the 68000 ~12.5%
(`m68k_step_core`, its bus, the two wait states, the SCSP register reads and writes). The rest is
`scsp_sync` and the thread's hand-over.

**Is the lazy chip misbehaving?** No. `det_digest` now prints the chip's counters (`sound: chip syncs,
single-slot catch-ups, lockstep samples`): attract (3000 frames, 2.2 M samples) has one full sync
per ~108 samples, 0.69 single-slot catch-ups a sample and **1** sample made in lockstep; a scripted
fight (4500 frames, 3.3 M samples) one sync per ~81 samples, 0.68 catch-ups a sample and 896
lockstep samples (FM voices). The fallback paths are not where the time goes; the cost is the
chip's work itself at 44.1 kHz.

**Where the DSP's time goes.** `perf annotate` of the program shows no hot spot: the cost is spread
evenly over ~3,000 straight-line instructions a sample (84 steps, ~36 instructions each, with the
multiply, the two clamps, the temp/mems traffic and, on 33 steps, the float16 and the RAM access).
At ~190-200 ns a sample it runs at ~3 IPC; it is throughput-bound on instruction count, so a gain
has to be fewer instructions. A silence skip is impossible (the delay line's limit cycle, SCSP.md),
the unpack LUT and the branchless unpack were measured and rejected before (`scsp.h`, above `scsp_dsp_pack`).

**What this did:** `tools/gen-scsp-dsp.py` now checks the program's delay ring against the end of
sound RAM **once a sample** (`ring_ok`: RBP, RBL and the widest address the program forms) and
makes the 33 accesses plain big-endian word reads and writes inside it (`scsp_dsp_ring_rd/wr` in
the generated header), instead of `scsp_ram_w/ww` with its mask and bounds test on each. A ring
past the end of RAM takes the checked path as before. Bit-exact: `snd_bench` hashes (out
`3704855bb1f0b17b`, ram `ee38c9db48fd2123`, efreg `3dcf915d389ad344`, dsp-state
`8caaee08bcbdff0f`), `det_digest --sound` digests and sound hashes for attract and the fight,
`scsp_dsp_test`, `scsp_dsp_test_masks` and `scsp_lazy_test` all unchanged. Gain: the DSP's own
instructions **−3.5%** a sample (12.72 G → 12.29 G over 4 M samples), `snd_bench`'s DSP time
201 → 185 ns a sample (noisy box), the whole board's instructions −0.3% over 3000 frames. Small,
as expected of a function already at its ceiling; the next real step for the sound thread would be
a different design (the 68000 in threaded code, or the driver in C, `--sound-hle`, which #173 has),
not more peephole work on the chip.

**The 68000's share** is the dispatch itself: the hottest instructions of `m68k_step_core` are the
`m68k_fast` table lookup and its indirect jump, i.e. one mispredicted jump per instruction. That is
the price of a table interpreter; the handler bodies are cheap.

**What was left alone, and why:** `scsp_slot_run_pcm` is already the slot's registers hoisted out of
the loop; its hot instructions are the pitch-LFO multiply and the stack spills of its flags, each
~1-2%, nothing to take in one piece. `scsp_eg_update` is a few adds. The i960 is not the sound
thread's problem and was covered by #403/#406.

## 2026-10-23: the sound thread, by part (Pinboard #1481)

The same attract bench (`arc_bench sfight --sound --frames 30000`, no `--i960-timing`) on
`idea-1477-i960-hoist` (#533), recorded at 800 Hz (the container's cap): 41,794 samples, the sound
thread (`m2-sound`) **34.1%** of them. No code changed; the counts per slot below come from a
throwaway build with counters in `scsp.h`.

**The sound thread by part** (`tools/perf_inline.py`, filtered to the thread with `perf script --comms
m2-sound`; % of the thread's 17,717 samples):

| part | share | what is in it |
|---|---|---|
| the SCSP | **62%** | DSP 29.5%, slots 25.8%, mix/out, timers, register and RAM-touch bookkeeping 6.7% |
| the 68000 | 31.7% | `m68k_step_core` 16.8%, `m68k_direct` 3.2%, `m68k_wait` 2.5%, `m68k_rw` 2.4%, `m68k_fetch` 1.5%, ... |
| the rest | 6% | `sound_step` / `sound_run` / `sound_m68k_write` glue 5.5%, libc |

The DSP's 29.5% is the program body 10.8%, `scsp_dsp_pack` 4.5%, `scsp_dsp_unpack` 4.3%,
`scsp_dsp_ring_rd` 3.8%, a `memcpy` inlined into the program 2.4%, `scsp_sext` 2.3%. By real
function: `scsp_dsp_prog_sfight_fvipers` 28.8% of the thread, **`scsp_slot_run_body` 24.2%**,
`m68k_step_core` 19.2%, `m68k_rw` 6.1%, `sound_run` 5.5%. The DSP program is at its ceiling (above);
the slot loop is the SCSP function left to look at, and it is nearly as big (8.2% of the process).

**What the slot loop does on attract** (30,000 frames, counted): 16.3 M runs of one slot, 398 M
slot-samples (~17 voices a sample, 24 samples a run on average). Every one of them takes the fast
path, `scsp_slot_run_pcm` (no FM, no noise, no reverse loop: the general loop ran 0 samples). All
are 8-bit PCM; PLFO / ALFO on 848 of 398 M; SDIR, STWINH, SBCTL and LPSLNK never; LPCTL 0 on 24%,
1 (forward loop) on 76%. Runs: 40% are one sample long (the single-slot catch-ups), but they are
1.6% of the samples; most samples are in runs of 16-127. The envelope: **release 50.6%, decay 2
49.4%**, attack 0.03%, decay 1 0.006%; every decay-2 sample has D2R = 0, so its level never moves.
The state changes inside a run in 0.15% of runs. 98% of the samples are non-zero after the
envelope, so there is no silence in it (and silence skip is closed anyway).

**What it costs, per slot-sample:** about 100 instructions in the loop plus 11-13 in
`scsp_eg_update`, which is not inlined (a call per sample). The work: two RAM bytes and the
interpolation (2 multiplies), the step and the loop check, the envelope (the call, an
`scsp_eg_table` load, a multiply), four gain multiplies, the sound-stack store and three
read-modify-write adds (`acc_mixs[k][isel]`, `acc_l[k]`, `acc_r[k]`). Where `perf annotate` puts the
function's samples: the sound-stack store 10.9% and the envelope's multiply 8.1% (both waiting on the
envelope), the jump back from the envelope 7.5%, the reload of `sl->active` after the call 7.4%. So
the envelope chain (call, `state` and `vol` through memory, table load, multiply) is ~35-40% of the
function. The loop's constant flags (PCM8B, PLFOS, SBCTL, STWINH, SDIR, LPCTL) and its four gains are
spilled to the stack and re-read every sample (register pressure; the flag tests alone ~10%).

**Its inputs, by how often they change:**

- *Per slice / per sync:* `s->ram`, `ram_size`, `sous_ptr` (flips only at the end of `scsp_sync`).
- *Per run (one call), always:* every slot register. A 68000 write below 0x400 is a full sync
  (`scsp_write_syncs`), so none changes inside a run: SA, LSA, LEA, PCM8B, SBCTL, LPCTL, PLFOS,
  ALFOS, SDIR, STWINH, LPSLNK, ISEL, the base step, the four gains (`g_mixs`, `g_dl`, `g_dr`,
  `g_sous`), and the envelope rates (`rr`, `d2r`, `dl`).
- *Per run, nearly always:* the envelope state (99.85% of runs). In decay 2 with D2R = 0 (half of
  all slot-samples) the envelope's gain itself, `scsp_eg_table[vol >> ...]`, is a constant of the run.
  In release `vol` falls by `rr` a sample: the table lookup is per sample, the state switch is not.
- *Per sample:* the position (`cur` / `nxt`, its fraction), the two RAM bytes, the loop wrap,
  `vol` in release, and the `k`-indexed accumulators.

So the slot loop's cost is the envelope's per-sample call and the state and level it keeps in
memory, on top of arithmetic that is already minimal. A run specialised on the state (decay-2-held,
release) with the level in a register would take the call, the reloads and the switch off almost
every sample; not tried here (this pin was measurement only). #1482 did it (below).

## 2026-10-23: the slot loop, specialised on the envelope (Pinboard #1482)

`scsp_slot_run_pcm` now hands a run to `scsp_pcm_run_eg` when the voice has no PLFO, no ALFO and
no SDIR and its envelope is in release, or in decay 2 with D2R = 0. Neither state can change inside
a run (a register write is a full sync, and the attack-to-decay link needs attack), except that
release reaching 0 stops the voice, which the loop handles itself. So: decay 2's gain
`scsp_eg_table[vol >> 16]` is read once for the run; release keeps `vol` in a register, subtracts
`rr` and looks the gain up each sample; `vol`, `cur`, `nxt` and the stop are written back once at
the end. The call to `scsp_eg_update`, its state switch and the reloads of `state`, `vol` and
`active` are gone, and the sound-stack slot for even and odd `k` is computed once. PCM8B and
release are constants at the four call sites, so each variant compiles on its own. Every other
state (attack, decay 1, decay 2 with D2R > 0), LFOs, SDIR and the non-PCM paths keep the old body.

**Cost**, `arc_bench sfight --sound --frames 3000`, `perf stat -e instructions:u`, two runs each,
built from #535 (`f5890cc`) and this branch:

| build | run 1 | run 2 |
|---|---|---|
| #535 (= #533's code) | 49,246,493,196 | 49,246,480,315 |
| #1482 | 48,270,805,086 | 48,270,759,554 |

−976 M instructions, **−1.98% of the process**; the runs agree to 50 K. Only `scsp.h`'s PCM path
changed, so all of it is the slot loop. What share of the loop that is: callgrind over the same
3000 frames counts `scsp_slot_run_body` inclusive (with `scsp_eg_update`) at 967 M → 759 M,
**−21.5% of the slot loop's instructions** (under valgrind the bench runs the sound thread less, so
only the ratio carries over; 976 M / 0.215 puts the loop at ~4.5 G of the native 49.2 G, ~9%, in
line with #1481's 8.2% of samples). `scsp_eg_update` went from 101 M to nothing.

**Proof (board-identical, no proto bump)**:

- `det_digest --sound`, #535 vs #1482: sfight attract 3000 (output `1027c032a3e9d7f5`) and the
  scripted fight 4500 (`01b322174c61fc8f`), frame digests and sound hashes identical; `--profile`
  daytona, srallyc, vcop, fvipers, dynamcop, hotd, doa and vonj at 1800, identical.
- `tools/ab-builds.mjs --sound` on sfight at 600/1800/3000: IDENTICAL.
- ctest 40/40.
- Both ends of a run in the new loop are reached on attract: a build whose release did not stop
  the voice at 0 changed the sound output hash (`6cc3eacc...`), and one whose no-loop voice did not
  stop at its end changed it too (`b45d0afd...`).

## 2026-10-23: after the envelope hoist, again (Pinboard #1487)

The same attract bench (`arc_bench sfight --sound --frames 30000`, no `--i960-timing`) on #1482's
`9ce4518`, 800 Hz, 46,126 samples; no code changed. The build is the one measured above (`perf stat`
over 3000 frames: 48,271,035,355 instructions).

**By thread:** main 68.5%, the sound thread **31.5%** (34.1% before #1482).

**By real function** (% of the process): `emu_slice_body` 26.9% (the i960, closed),
`game_render_draw_geo_list` **10.9%**, `scsp_dsp_prog_sfight_fvipers` 9.9% (closed),
`s24_draw_tilemap` 6.8%, **`scsp_slot_run_body` 6.3%** (8.2% before), `m68k_step_core` 6.3%
(closed), `render_frame` 6.0%, `mem_ea` 3.0%, `geo3d_emit_tri_uv` 2.1%, `m68k_rw` 2.0%,
`sound_run` 1.9%. In the sound thread: DSP 31%, slots 20%, 68000 core 20%.

**The most expensive open function is the GEO list walk** (10.9%), and it has no hot spot: by
source line the top is 0.5% (`geo3d_cull_code`'s five plane tests, the cached mesh's vertex
transform, `geo3d_flat_depth`); by inlined function it is luma 1.9%, flat depth 1.4%, cull codes
1.0%, normals 0.9%, cached faces 0.9%, ... It is float work per vertex and per face, already worked
by #308, #317, #339 and #403; nothing in it is a reload of something constant.

**The slot loop, what is left** (6.3% of the process; `scsp_pcm_run_eg` 4.9% and `scsp_pcm_wave`
2.4% by inlined function). `perf annotate` of the 8-bit release variant: ~75 instructions a sample,
the work the brief lists (two RAM bytes, the interpolation, the step and loop test, one
`scsp_eg_table` load, the envelope multiply, four gain multiplies, the stack store, three adds).
About **19 of the 75 are reloads of run constants from the stack**, because the loop has more live
values than x86-64 has registers: `rr`, the table base, `sa`, `step`, `ram` and `ram_size` (twice
each), the four gains, `mixs`, both sound-stack pointers, and the flags `stwinh`, `sbctl` (two
tests) and `lpctl`. They hit L1, but the loop is throughput-bound on instructions, as the DSP is.
The samples sit on the multiplies and on what follows them (the add into `acc_l`, the stack store):
the real work. The flag tests come next (`stwinh` 4.7%, the `k & 1` stack pick 5.5% in each of
the two variants, `sbctl` 1.5%), mostly skid from the multiply before.

**A cut as specific as the envelope hoist, not made** (this pin was measurement only): attract never
sets SBCTL or STWINH (#1481's counts), and sound RAM is 512 KB, so the two `a < ram_size` tests in
`scsp_pcm_wave` always pass for a voice whose whole loop lies inside it. A variant of
`scsp_pcm_run_eg` taken when `sbctl == 0 && !stwinh` and the run's reach (`sa` + max(`lea`, the
start) + 2 + `step >> 12`, below 0x80000 and with no 20-bit wrap) is inside RAM, with `lpctl` a
constant too (0 on 24% of the samples, 1 on 76%), drops the flag tests, the `sbctl` xor, both bound
tests and the `ram_size` reloads: ~13-15 instructions of ~75, and the registers they free take some
of the other spills. Eight variants (pcm8 x release x lpctl); everything else keeps today's
`scsp_pcm_run_eg`. Estimate: ~15-20% of the loop, **~1% of the process**, bit-exact by
construction (it removes tests that cannot fail). Below that, the slot loop is the arithmetic of
the voice itself.

## 2026-10-23: the slot run inside RAM (Pinboard #1488)

The cut above, made as one more case of `scsp_pcm_run_eg` (an `inside` flag beside `pcm8` and
`release`), not as eight hand-written copies; `lpctl` stays a run constant read once, as before.
`scsp_slot_run_pcm` takes it when SBCTL and STWINH are clear and `scsp_pcm_run_inside` holds: the
top position the run can read, the start (the larger of `cur` and `nxt`) plus `n - 1` steps and a
sample, scaled to bytes from `sa`, is below the RAM size and the 20-bit wrap. That bound holds for
the whole run because `cur` and `nxt` only climb, except at a forward loop's end, which takes them
back by `lea - lsa` (so the check refuses `lsa > lea` on a loop). The registers it reads are the
run's constants, the same as LFO and SDIR on this path. The inside loop drops the SBCTL xors, the
STWINH test, both wraps and both bound tests.

**Cost**: `arc_bench sfight --sound --frames 3000`, `perf stat -e instructions:u`, two runs each:
48.271G → 47.836G, **−435M (−0.90%)**, runs agree within 10K. `scsp_pcm_wave` had to be
`SCSP_FORCE_INLINE`: with an extra argument and plain `inline`, GCC stopped inlining it, and the
count went up to 48.546G.

**Who takes it**: every run on this path in STF attract, and in daytona, srallyc, vcop, fvipers,
dynamcop, hotd, doa and vonj over 1800 frames (a temporary counter, removed): none of them ever set
SBCTL or STWINH here, or ran outside RAM. The fallback is held by `scsp_fuzz` (random SBCTL, STWINH
and addresses): its file matches #536's, and changes if either guard is taken out (without the
reach test it crashes).

**Bit-exact against #536**: `det_digest --sound` on STF attract (3000) and the scripted fight
(4500), `--profile` on the eight games above (1800 each), frame digests and sound hashes identical;
`ab-builds.mjs --sound` identical at 600, 1800 and 3000; ctest 40/40.

## 2026-10-23: after the inside-RAM cut, again (Pinboard #1491)

The same attract bench (`arc_bench sfight --sound --frames 30000`, no `--i960-timing`) on #1488's
`525c36a`, 2000 Hz, 41,169 samples; no code changed. The build is the one measured above
(`perf stat` over 3000 frames, two runs: 47,836,571,484 and 47,836,590,888 instructions).

**By thread:** main 68.6%, the sound thread **31.4%** (31.5% before #1488).

**By real function** (% of the process): `emu_slice_body` 27.1% (the i960, closed),
`game_render_draw_geo_list` 10.8% (closed), `scsp_dsp_prog_sfight_fvipers` 9.9% (closed),
**`s24_draw_tilemap` 7.2%**, `m68k_step_core` 6.7% (closed), **`scsp_slot_run_body` 5.4%** (6.3%
before), `render_frame` 5.2%, `mem_ea` 3.7%, `m68k_rw` 2.2%, `geo3d_emit_tri_uv` 1.9%,
`sound_run` 1.9%, `mem_write32_slow` 1.9%. In the sound thread: DSP 32%, 68000 core 21%, slots 17%.

**The most expensive open function is the tile walk, `s24_draw_tilemap`** (7.2%; the CPU tile
compositor of the handheld / libretro builds, #211). By source line, 43% of its samples (3.4% of
the process) are on `s24_cell_load`'s graphics read, the four bytes of the cell's row at
`gfx + (entry & 0x3FFF) * 32`: by instruction, a third of the function lands on the first `shl`
that uses those bytes, the stall of a cache miss in the tile graphics ROM (one cell in 16K, picked
by the name table). The rest is the run walk: the window-mask bit test (`tile_renderer.h:227`,
10%), the name-table word (6%), the pixel stores (10%). Nothing in it is a test or reload of a
value fixed for the pass: `opaque`, `mode` and the line's split are tested once a run, and the
run is up to eight pixels.

`render_frame`'s 5.2% is mostly (2.9% of the process) `tile_compose_cpu`'s recolour of every
pixel, two 4-byte pen lookups and stores per pixel, taken when the tile RAM goes `full` or a pen
changes. #403 chose a store per pixel over a pen test per pixel on purpose; the samples sit on the
stores.

**No cut as specific as the inside-loop hoist.** The open functions left are a memory stall (the
tile graphics), stores (the recolour) and the arithmetic of the voice (the slot loop, described in
#1487's section; #1488 took its run-constant tests). Making the tile walk cheaper would mean
reading the graphics less often (caching decoded cell rows, or redrawing fewer lines), which is a
change of what is drawn when, not a hoist.

## 2026-10-27: the most expensive functions, all twelve games (Pinboard #1713)

Master at `610ed2f`, after the quiet DSP (#1681) and the cheaper voice loop (#1698). The same bench
as before (`arc_bench <set>.zip --sound`, attract, no input) on STF for 30,000 frames and on eleven
other games for 6,000 each, `perf record -F 2000` / `-F 1000 --call-graph fp`. No board code
changed. The box had a load of ~3, so the times below are rough; the shares are not.

**A correction to `tools/perf_inline.py` first.** It counted samples, but `perf record -F` gives
each thread samples of different weight: the sound thread sleeps between bursts, and its samples
stood for 0.94 M cycles each against the main thread's 2.08 M. So the inlined tables above
(#1474, #1481) put the sound thread at about twice its share; the "by thread" and "by real
function" numbers came from `perf report`, which weights by period, and stand. The script now
weights every sample by its period (and checks out against `perf report --sort comm`), and takes
`--comm NAME` for one thread's profile (`m2-sound`, or the executable's name for the main thread).

### STF

`perf stat` over 3,000 frames: **42.79 G instructions** (#1491: 47.84 G), 3.08 per cycle. The
sound thread is **24.3%** of the process (31.4% at #1491): the DSP program is down from 9.9% to
1.5%, skipped while quiet.

By real function: `emu_slice_body` 31.4% (the i960), `game_render_draw_geo_list` 11.7%,
`s24_draw_tilemap` 8.7%, `m68k_step_core` 7.5%, `render_frame` 5.2%, `scsp_slot_run_pcm` 4.8%,
`mem_ea` 3.9%, `m68k_rw` 2.4%, `sound_run` 2.2%, `geo3d_emit_tri_uv` 1.8%, `coprogram_write_cb`
1.5%, `scsp_dsp_prog_sfight_fvipers` 1.5%.

By inlined function, weighted (% of the process):

```
 13.77%  i960_step_core                 2.67%  hle_check_synced               1.57%  reg_write
  6.57%  m68k_step_core                 2.62%  s24_draw_line                  1.47%  tile_compose_cpu
  5.34%  s24_cell_load                  2.31%  emu_slice_body                 1.34%  reg_read
  3.53%  mem_fetch2                     1.98%  geo3d_flat_depth               1.33%  scsp_pcm_wave
  3.36%  memcpy                         1.97%  geo3d_board_luma               1.32%  geo3d_board_normal
  3.20%  mem_ea                         1.66%  geo3d_emit_tri_uv              1.30%  m68k_direct
  3.00%  scsp_pcm_run_eg
```

`memcpy` is the tile compositor's colour lines (1.9%) and `sharc_sincos` (0.3%). The sound thread
by file: the 68000 52%, the SCSP 35% (the voices `scsp_pcm_run_eg` + `scsp_pcm_wave` 18%, the
DSP 4%), `sound.h` glue 9%. So **the 68000 is now the sound thread's largest cost**, and its
cost is the table dispatch (#1474).

### Every game, by subsystem

% of the process, inlined code charged to its own file. "i960" is the interpreter, the bus, the
run loop, the timers and the hook check; "board timing" is `i960_timing.h`; "coprocessor" is the
TGP (`tgp.h`, `tgpx4.h`) or the SHARC (`sharc_cpu.h`, `sharc_exec.h`, `cop.h`). Emu and tiles are
the bench's ms a frame, flat out, under perf.

| set | i960 | board timing | coprocessor | 3D | tiles | sound | emu ms | tiles ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| sfight | 39.7 | | 2.7 | 16.1 | 14.3 | 24.2 | 0.49 | 0.15 |
| fvipers | 51.9 | | 1.5 | 10.9 | 17.4 | 14.7 | 0.99 | 0.32 |
| vonj | 49.8 | | 14.3 (SHARC) | 6.4 | 15.1 | 10.8 | 2.43 | 0.54 |
| doa | 31.8 | | 26.1 (SHARC) | 7.9 | 20.3 | 11.2 | 1.90 | 0.64 |
| lastbrnx | 37.1 | | 33.4 (SHARC) | 8.9 | 9.1 | 8.6 | 2.52 | 0.29 |
| hotd | 44.5 | | 18.5 (TGPx4) | 3.9 | 2.0 | 26.2 | 1.87 | 0.05 |
| dynamcop | 49.9 | | 10.5 (TGP) | 5.3 | 1.2 | 27.3 | 1.79 | 0.03 |
| motoraid | 44.2 | | 8.8 (TGP) | 9.1 | 10.4 | 22.7 | 1.69 | 0.32 |
| vf2 | 31.3 | | 26.8 (TGP) | 5.9 | 21.1 | 12.7 | 1.76 | 0.62 |
| daytona | 21.6 | 23.7 | 10.8 (TGP) | 6.7 | 21.4 | 12.9 | 1.64 | 0.59 |
| srallyc | 35.5 | **39.4** | 9.4 (TGP) | 5.7 | 1.2 | 6.4 | 3.02 | 0.04 |
| vcop | 29.8 | **33.6** | 5.1 (TGP) | 6.1 | 3.2 | 14.4 | 2.92 | 0.11 |

(vcop's "sound" holds 7.2% of `z80.h`, which there is its I/O board's Z80, with `model1io.h` 5.4%
beside it. The rest of each row is the zip load, `model2a.h` / `model2b_cop.h` glue and libc.)

STF's emulation is the cheapest of the twelve by a factor of 2 to 6, and the reason is the next
section.

### 1. The idle loop: 42-88% of every other game's i960 instructions

A per-address count of the i960's instructions (arc_bench `--profile`, widened to work RAM for
Sega Rally; frames 1000-3000 of attract) puts most of every game's instructions on one short loop
that waits for the frame:

| set | i960 instructions a frame | the loop | its share |
|---|---:|---|---:|
| sfight | 117,605 | `0x11610`, `ldob 0x500000` + `cmpibe` (skipped, #294) | 68.2% |
| fvipers | 131,945 | `0x118DC`, the same two words | 42.0% |
| vf2 | 128,654 | `0x10F98`, the same two words | 48.5% |
| daytona | 59,157 | `0x1394`, the same two words | 57.4% |
| hotd | 167,034 | `0x18D4`, four: `ld 0x98000C`, mask, compare, `cmpibe` back | 88.2% |
| dynamcop | 166,844 | `0x71194`, the same four | 86.7% |
| motoraid | 144,256 | `0x30204`, the same four | 82.6% |
| lastbrnx | 169,725 | `0x73F4`, the same four | 83.5% |
| vcop | 121,137 | `0x9124`, the same four | 88.3% |
| srallyc | 140,398 | `0x5A379C` in work RAM, five, the same `ld 0x98000C` | 84.4% |
| doa | 167,620 | `0x27F0`, `ld 0x54B170`, `cmpibne` out, `addo`, `b` back (a counter) | 86.2% |
| vonj | 252,345 | `0x28E2C`, five: a register-based `ld`, shift, mask, `be` back | 82.4% |

0x98000C is the 2A/2B/2C video control word (MAME `videoctl_r`): six games share one library loop
that polls it for the frame flip. Only STF hooks `m2_spin_skip` (`sfight.h`; homebrew finds its
loops with `m2_spin_find`). With it, STF runs about 41,000 instructions a frame (`det_digest`:
113.7 M iterations skipped in 3,000 frames); the others run all of theirs.

**FV and VF2 are the same two words as STF, and the skip takes them as they are.** A scratch
build of `arc_bench` and `det_digest` that called `m2_spin_find` after the profile's install
(nothing else changed):

| set | instructions, 3000 frames, `--sound` | emu ms a frame | `det_digest --cpu --sound`, 3000 frames |
|---|---|---|---|
| fvipers | 71.85 G → **48.84 G (−32%)** | 0.98 → 0.57 | identical, 94.4 M iterations skipped |
| vf2 | 122.68 G → **98.52 G (−20%)** | 1.72 → 1.28 | identical, 99.1 M iterations skipped |

That is the largest gain left on the board, and for those two it is a one-line install (or the
loop's address in the profile, as STF has it). The others need more than that:

- **Daytona, Sega Rally and Virtua Cop** set `i960_board_timing`, and `m2_spin_skip` declines
  under it (a turn's cost there depends on the cache and the scoreboard, not the table). Steady in
  its loop, the cache holds the loop and a turn costs the same each time, so a skip that charges
  `k` turns of the model's cost is possible, but it is a new piece of exactness to prove.
- **The `0x98000C` loop** (six games) reads a device, not work RAM. Skipping it needs the bit's
  next flip on the cycle clock, which is where the board's frame edge already is (#253), so it is
  the same kind of skip to the horizon, with a different matcher and value.
- **DOA's** loop counts while it waits (`addo`): the skip would add `k` to the register too.
  **Virtual-On's** reads through a register. Both are small matchers of their own.

### 2. The board timing model costs more than the interpreter

On the three games that set it, `i960_timing.h` (`i960_bt_charge`, `i960_bt_data`,
`i960_bt_fetch`, `i960_bt_count`, `i960_bt_branch`) is 24-39% of the process, more than the
interpreter itself (12-18%). It is per instruction, and 57-88% of those instructions are the idle
loop above; a skip that works under it takes most of both.

### 3. The SCSP in lockstep on HotD, Dynamite Cop and Motor Raid

`det_digest --profile <set> --frames 1800` prints the chip's counters. On hotd, dynamcop and
motoraid **55-61% of all samples are made in lockstep** (759,634, 802,661 and 839,235 of ~1.38 M,
a full sync every ~1.7 samples; STF's scripted fight: 911 in 3.3 M), and the DSP is skipped on only
17-19% of samples on dynamcop and motoraid (70% on hotd, 91% on STF). A throwaway count of why the chip was coupled: every lockstep sample was "a slot reads
the DSP's delay line"; never FM, never noise. The slot is the same on each game:

```
slot 0  range [6E000,70004)  DSP ring [70000,80000)  SA 6E000 LSA 0 LEA 1FFF, 8-bit, forward loop
```

An 8 KB loop at 0x6E000-0x6FFFF, just below the ring. `scsp_slot_range` adds a margin of
`2 * adv + 3` samples past the loop end, which takes the range 4 bytes into the ring. The voice
never reads there: `scsp_pcm_run_eg` steps `cur` and `nxt` and wraps both at LEA before the next
read, so in a forward loop that `scsp_slot_loop_bounded` accepts it reads no higher than
`SA + LEA - 1` (the margin is for the pitch LFO and for positions left past LEA by a register write). A bound that is exact for a forward loop with no pitch LFO (the case here) would
uncouple the chip on all three; the sound thread is 23-27% of those games. Not changed here.

### 4. The coprocessors, the tiles and the rest

- **The SHARC interpreter** (`shc_step`, `shc_pm_rd48`, `shc_op_immmod`, ...) is 26% of DOA and 33%
  of Last Bronx, 14% of Virtual-On; the **TGP** 27% of VF2 and 11% of Daytona; the **TGPx4** 18.5%
  of HotD. These run the real copro programs; no single op stands out.
- **The interpreted SCSP DSP** (`scsp_dsp_step`, 2.6-8.4% on hotd, dynamcop, motoraid, vonj, vf2, doa):
  only STF/FV's reverb program is compiled (`scsp_dsp_known.h`). Once #3 is fixed, more of it
  will be skipped while quiet; what remains could be compiled the same way
  (`tools/gen-scsp-dsp.py`).
- **The CPU tile compositor** is 14-21% of the games with 2D layers (STF, FV, Daytona, VF2,
  DOA, Virtual-On), `s24_cell_load`'s graphics read on top (5-11%): the cache-miss stall #1491
  described. On the desktop the tiles go to the GPU.
- **The 3D's per-face work** (`game_render_draw_geo_list`, 4-16%) and the 68000 (4-8%) are as
  #1487 and #1474 left them.

### What to take, in order

1. Hook `m2_spin_skip` on FV and VF2 (their loop is STF's): −32% and −20% of the process,
   proved frame-identical above. Then a matcher for the shared `0x98000C` loop (six games, 83-88%
   of their i960 instructions), DOA's and Virtual-On's.
2. Make the skip work under the board timing model (Daytona, Sega Rally, Virtua Cop: the model is
   24-39% of the process, and most of it is spent on the idle loop).
3. Tighten `scsp_slot_range` for a forward loop, so a voice ending just below the DSP ring does not
   put the chip in lockstep (HotD, Dynamite Cop, Motor Raid).
4. Compile the other games' DSP programs, once 3 shows how much of them still runs.

### Taken, all four

- **1 and 2: every game gets the idle skip** (`m2_spin_install`, `m2_spin.h`). The skip no longer
  matches two fixed words: it decodes the loop and runs it three times on a copy of the registers,
  so a counter (DOA's `addo`, the `0x98000C` loop's) is fine, and so is a load through a register
  (Virtual-On). Sites are found by scanning the program ROM and work RAM, not listed per profile,
  and work RAM is scanned again every 64 frames (Sega Rally unpacks its loop there, Pilot Kids
  copies its own). Under the board timing model a copy of `i960_bt_t` runs along; from the second
  iteration on it must come back to the same state at the same cost, so `k` iterations charge
  the first one's cost and `k - 1` of the second's.
- **3: `scsp_slot_range` gives a forward loop longer than a step no margin** (it reads no higher
  than `max(LEA - 1, cur, nxt)`). The lockstep samples went from 1.55 M to 0 on HotD and Dynamite
  Cop, and to 49 on Motor Raid, in 3,000 frames. `scsp_lazy_test` already reaches the case: the
  bound made one sample too tight fails it on the 12th voice.
- **4: eight more DSP programs compiled** (`tools/scsp-dsp-programs.txt`): HotD (= Dynamite Cop),
  Motor Raid, VF2, Virtual-On (= von), DOA, Last Bronx, Air Walkers, Power Sled. After 3 the DSP
  still ran on 56% of HotD's samples and 88% of Dynamite Cop's and Motor Raid's.
  Sega Rally, Virtua Cop and Pilot Kids start no program in attract.

Checks: `det_digest --cpu --sound` for 3,000 frames on 18 profiles with the skip on is identical,
frame by frame, to `--spin-i960`. The old build and the new one with `--spin-i960` match too,
so the timing model's refactor (`i960_bt_step`) changes nothing. With `--scsp-exact`, the old
build and the new one match on 12 games for 3,000 frames. That holds 3 and 4 together; the
quiet-DSP skip moves by its 1 LSB when the chip uncouples. `scsp_dsp_test` (both select kinds)
and `scsp_lazy_test` pass. There is no `NETPLAY_PROTO_REV` bump: nothing the i960 can read
changes, and the frames are proved identical to the interpreter's (#294 did not bump either).

User-mode instructions, `det_digest <set> --frames 3000 --sound`, before (09f2fdf) and after:

| set | before | after | | sound thread |
|---|---:|---:|---:|---|
| sfight | 64.3 G | 63.7 G | −1% | |
| fvipers | 103.7 G | 81.0 G | −22% | |
| vf2 | 117.3 G | 96.4 G | −18% | 18.9 → 12.1 G |
| daytona | 137.7 G | 97.8 G | −29% | |
| srallyc | 226.6 G | 84.1 G | **−63%** | |
| vcop | 210.5 G | 94.7 G | **−55%** | |
| hotd | 129.3 G | 86.4 G | −33% | 26.1 → 15.6 G |
| dynamcop | 127.3 G | 79.1 G | −38% | 30.3 → 17.0 G |
| motoraid | 122.4 G | 79.0 G | −35% | 28.0 → 15.1 G |
| vonj | 137.3 G | 87.4 G | −36% | 18.7 → 10.8 G |
| doa | 115.2 G | 87.0 G | −24% | 14.5 → 10.3 G |
| lastbrnx | 129.8 G | 99.3 G | −24% | 12.4 → 12.5 G |
| airwlkrs | 97.1 G | 51.1 G | −47% | |
| powsled | 101.0 G | 53.1 G | −47% | |
| pltkids | 138.5 G | 70.2 G | −49% | |

STF was already skipped (#294). For Sega Rally and Virtua Cop the skip also takes most of the
timing model's work.


## How to take one

`perf` comes from `linux-tools-generic` (in the antigravity-dev image; `/usr/local/bin/perf`).
Build with symbols and frame pointers, at the release optimisation:

```bash
cmake -S . -B $B -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-g -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer"
cmake --build $B -j6
# arc_bench is not a CMake target; det_digest's flags build it:
gcc -g -fno-omit-frame-pointer -O3 -DNDEBUG -std=gnu11 -ffp-contract=off -fno-strict-aliasing \
    -DM2HLE_BUILD_FLAVOR=\"release\" -DM2HLE_VERSION=\"dev\" -DM2HLE_DEV_TOOLS=0 -DM2HLE_AGES \
    $(grep C_INCLUDES $B/CMakeFiles/det_digest.dir/flags.make | cut -d= -f2-) \
    tests/arc_bench.c -o $B/arc_bench $B/libminiz.a -lpthread -ldl -lm
# a Ninja tree has no flags.make; the include list is in build.ninja instead:
#   grep -A6 'build CMakeFiles/det_digest.dir/tests/det_digest.c.o' $B/build.ninja | grep ' INCLUDES = ' | sed 's/^ *INCLUDES = //'
```

`snd_bench` and `snd_replay` want a MAME sound capture (`tools/mame/snd_capture.py`, tools/README.md).
In the dev container the shared MAME build makes one in a few minutes: `MAME_EXE=~/build/mame-bin/mame-shared/shared`,
`MAME_ROMPATH=$ROMS_DIR`, `MAME_EXTRA_ARGS="-video none -debugger none"`, `SDL_VIDEODRIVER=dummy`, and
`BRIDGE_PORT=<free port>` when the bridge's 6510 is taken. Hold any change to the chip against its hashes
(`snd_bench <cap> 60 400000`) and against `det_digest --sound` before and after.

Then, from a scratch directory (never the ROM folder):

```bash
perf record -F 2000 --call-graph fp -o attract.data -- $B/arc_bench $ROMS_DIR/sfight.zip --sound --frames 30000
python3 tools/perf_inline.py attract.data $B/arc_bench --top 40          # by inlined function
python3 tools/perf_inline.py attract.data $B/arc_bench --comm m2-sound   # one thread (the sound board)
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
