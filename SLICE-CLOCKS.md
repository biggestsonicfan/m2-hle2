# One slice is not one frame

A proposal, not a change. The performance work on this branch left the board
exactly as it found it (`ab-builds` identical against master); this is the
accuracy bug that work walked past, written down with its measurement so it can
be fixed on its own terms.

## Status (branch `fix/slice-clocks`, 2026-09-22)

**Option A is in.** `emu_sound_slice_end` (`src/core/emu_thread.h`) charges the sound
board a frame of samples when the game's frame ends. `emu_slice_body`,
`det_digest`'s traced slice and `arc_bench`'s slice all call it, so the copies
cannot drift. There is one addition the proposal did not have: a board that never
reaches a frame edge (booting, stuck, or a profile with no frame hook) would never
run its sound. So once a frame has run `EMU_FRAME_STEPS_MAX` (4M) i960
instructions, each further slice is charged as before. The limit counts
instructions, not slices, so the rule is the same at any `--steps-per-slice`.
`NETPLAY_PROTO_REV` is now 5. `sound_status` reports `midi_holds`.

Measured with `tools/clock-state.mjs` (new; the acceptance test below as a tool):

| build | VS screen (0xB820 → 0xC34C) | attract Death Egg (0x5320C → 0x541DC) |
|---|---|---|
| master | 65 frames, 58,800 samples = **904.6**/frame | 736.6/frame |
| master `--live-timers` | 802.8/frame | — |
| branch (also with `--live-timers` and `--steps-per-slice 150000`) | **735.0**/frame | 735.0/frame |

- **`SOUND_AHEAD_MAX` left at 735.** Three MIDI bytes cross the VS screen, with no
  catch-up steps and none held back. The cap does not bind here. Raise it only if a
  state that sends a burst during a long frame shows holds.
- **`--live-timers` does not make the frames shorter.** It is still 65 frames and the
  same 9.01M instructions, so the long frames are not an artefact of frozen timers.
  Sound per slice was simply wrong, not accidentally right.
- `match-replay` against a fresh MAME reference gives the same result on master and
  the branch: the same fight over 1299 frames, the known residuals from +321, and
  byte-identical reports. (The harness itself wanders: the replay stage loads on
  frame 198, 199, 255 or 573 from run to run, on either build. Only the 198 runs
  compare.)
- `grade-reset` passes (22/22), and `det_digest`'s traced slice matches the plain one
  over 1500 frames. `mem/i960/cop/m68k/emu/net/tile` ctests pass.

**Still to do** from the list below: `grade-osage`; a MAME sound capture *through a
load*; `det_digest` in the wasm tree; and a listen for ring underruns through the VS
screen on the ARC-S. The drive.mjs warning about frame-hook breakpoints now applies
only to the timers.

---

## What is wrong

`emu_slice_body` (`src/core/emu_thread.h`) advances two board clocks once per
**slice**:

```c
emu_timers_slice_begin(ctx);        /* irqt_tick(EMU_CPU_HZ / EMU_SLICES_PER_SEC) */
...
sound_run_slice(EMU_SLICES_PER_SEC);  /* 735 samples of 68000 + SCSP */
```

That is a whole frame of i960 timer cycles (~416,667 at 25 MHz) and a whole
frame of audio, charged per slice. It is correct while one slice is one game
frame, which is the ordinary case and the case the constants were sized for
(`EMU_STEPS_PER_SLICE`, "sized to always reach the frame boundary").

It stops being correct when a frame needs more than `EMU_STEPS_PER_SLICE`
instructions. The frame then spans two or more slices, and **every one of them
charges a whole frame** of both clocks to a frame that has already been charged.

## Measured

`tools/prof-state.mjs` against STF, two states (one row per slice in
`pc.csv.frames.csv`, column 1 is the frame the slice belonged to):

| state | slices | frames | slices/frame | samples run | the frames warrant |
|---|---|---|---|---|---|
| `ROUND_MASK` (VS screen, 0xB820 → 0xC34C) | 80 | 66 | **1.212** | 58,800 | 48,510 (**+21.2%**) |
| `adv_movie_egg` (attract, 0x5320C → 0x541DC) | 470 | 470 | 1.000 | 345,450 | 345,450 |

Fourteen of the VS screen's 66 frames took two slices, because both fighters'
textures decompress there and `unpack_lod_data` and its callees want about 1.1M
instructions in a frame. So across roughly a second of game time the sound board
is advanced by about 1.33 s of audio. The 68000 driver's tempo is paced by SCSP
timers counted in sample periods, so the music runs ~21% fast for the length of
the load, and the i960's own timer interrupts arrive ~21% early.

The attract row is there to show the other shape: no frame over the budget,
ratio exactly 1, nothing wrong. The bug is confined to load frames.

**Not measured, and worth measuring before deciding how much this matters:**
whether it is audible, and what MAME's sound board does across the same load.
The number above is inferred from the slice and frame counts and from the code,
not from listening or from a capture.

## Why it has not bitten

- It only happens on load frames, which are a second here and there.
- Both clocks are wrong by the *same* factor, so nothing inside the board
  disagrees with anything else: the music and the timer interrupts speed up
  together, and the fighters' own logic is driven by frames.
- It is stable across builds, so every A/B and every grader agrees with itself.
  `ab-builds` cannot see it; it compares two builds, and both are wrong alike.
- On the desktop the extra audio quietly *helps*: through a load the board is
  behind its 60 Hz deadline, and producing 21% more samples than the frames
  warrant is what keeps the host ring fed. Fixing the accuracy removes that
  accident, which is why the ring needs checking afterwards (below).

## The timer half is already fixed, behind a flag

`g_irqt_live` (`--live-timers`, from the handheld work) charges MAME's
per-opcode i960 cycle costs as they are executed and delivers timer IRQs
mid-slice. With it on, `emu_timers_slice_begin` does **not** tick a fixed frame
of cycles — it flushes what was actually executed — and `emu_timers_frame_edge`
tops the frame up to one frame's worth only if the i960 idled:

```c
int64_t idle = EMU_CPU_HZ / EMU_SLICES_PER_SEC - (cpu->cycles - s_slice_cycles0);
if (idle > 0) irqt_tick(idle);
```

A frame that ran long gets `idle <= 0` and no top-up, so it is charged the
cycles it really used. **So the timer half of this proposal is a question of
whether `--live-timers` should become the desktop default, not of new code.**
It costs 2–4% of the emu thread on the A55 (6% through a load) and nothing
measurable on x86, and it has one open question already recorded:
`--steps-per-slice` halves STF's sway-chain calls on its own, live timers or
not, which is the same "extra slices carry extra board time" disease in another
organ.

The sound board has no equivalent. Nothing ties `sound_run_slice` to anything
but the number of times it is called.

## Options for the sound half

### A. Charge the sound board at the frame edge

The whole change is to stop calling it unconditionally:

```c
/* the same `frame` the frame-edge block above already computes */
if (g_frame_done || (board_vblank && g_vblank_acked))
    sound_run_slice(EMU_SLICES_PER_SEC);
```

Mid-frame, the board still advances exactly as much as the MIDI conversation
demands, because `sound_make_midi_room` already runs it early and
`g_sound.ahead` already owes those samples back at the next edge. That machinery
exists and is the reason this option is small.

Two details to get right:

- **`SOUND_AHEAD_MAX` is 735 — "at most a slice of samples run ahead".** Under
  this change a long frame may legitimately need to run further ahead than that
  before its edge, or a MIDI byte waits a whole long frame instead of a slice.
  The cap wants to become a frame's worth of *this* frame, or simply a larger
  constant with the comment updated.
- **The host audio ring gets nothing for the length of a multi-slice frame.**
  On this desktop that is harmless — the board runs the VS screen at several
  times realtime and the 60 Hz pacing holds it back — but on the ARC-S a load
  frame is ~23 ms at p99, so the ring needs that much headroom or it underruns
  and clicks (see the live-stream audio note: underrun recovery is audible).

### B. Charge both clocks off the i960's cycle counter

The model the hardware has: the clocks free-run, and the i960 is not what drives
them. Samples due become a function of `cpu->cycles`
(`cycles * SOUND_RATE / EMU_CPU_HZ`, with the remainder carried the way
`slice_frac` already carries one), and the frame edge tops the cycle count up to
a whole frame if the i960 idled — exactly what `emu_timers_frame_edge` does for
the timers today.

This is the right long-term shape and it fixes both halves with one rule. It
needs `g_irqt_live` on, because `cpu->cycles` is only charged when it is, so it
inherits that flag's cost and its open question. Worth doing **after** A, if and
when live timers become the default.

### C. Raise `EMU_STEPS_PER_SLICE` until frames stop splitting

The VS screen's worst frame is ~1.1M instructions, so ~2M would do it, and
per-slice charging becomes exact again by construction with no other change.

**This is not a fix.** It hides the bug on one class of host: the cap exists to
bound how long a slice holds the emulator mutex, so raising it hands the UI a
24 ms stall on this machine and much worse elsewhere — and the handheld
deliberately *lowers* it (`--steps-per-slice 150000`), where the problem comes
straight back and multiplied. Listed so nobody rediscovers it as the easy answer.

## Recommendation

**A now, B later, never C.** A is a one-line behaviour change on top of
machinery that already exists, it is testable on its own, and it leaves the
timer half to the separate and already-half-answered question of making
`--live-timers` the default.

## What it breaks, and what has to be re-graded

This changes the board. That is the point, and it means the usual proof of a
performance change — "`ab-builds` is identical" — is the *wrong* check here;
expect it to differ, and check against MAME instead.

- **`NETPLAY_PROTO_REV`** (`src/net/netplay.h`, currently 4) **must be bumped.**
  `slice_frac` and `ahead` are board state that two peers have to agree on
  sample for sample; a mixed-version session would desync on the first load.
- **`tools/match-replay.mjs --mame`** — attract's preprogrammed replay fight has
  to still be the same fight. This is the check live timers were put through.
- **`tools/grade-osage.mjs`** — the grader that caught the last bug of exactly
  this family (the frame's idle share charged at the next slice's start, which
  ran half of STF's sway chains: 126 calls where MAME runs 242).
- **A fresh MAME sound capture taken *through a load*,** compared with
  `tools/mame/snd_compare.py`. `tests/snd_replay.c` on its own will not see any
  of this: it feeds the sound board MAME's MIDI stream directly and never
  exercises the slice structure at all.
- **`tests/det_digest.c` in both trees** (MSVC and wasm) for cross-play.
- **`tests/arc_bench.c` and `tests/det_digest.c` each keep their own copy of the
  slice loop.** Both call `sound_run_slice` and `emu_timers_slice_begin`
  themselves. If they do not move with `emu_slice_body` they will quietly go on
  measuring and digesting the old model.

## How to measure it, before and after

```
cmake -S . -B build_prof -G "Visual Studio 17 2022" -A x64 -DM2HLE_PROFILE=ON
node tools/prof-state.mjs --exe build_prof/Release/m2hle.exe --state round-mask
```

`pc.csv.frames.csv` has one row per slice with its frame number, so the
slices-per-frame ratio in the table above falls straight out of it, and the
`#sound_us` header line is the sound board's share of the window.

The acceptance test is a single ratio, and it needs no profiler build: take
`sound_status`'s `samples` and `get_status`'s `frames` either side of the VS
screen. **Today that comes out at ~891 samples a frame across the state, against
735 everywhere else. After the fix it should be 735 there too.**
