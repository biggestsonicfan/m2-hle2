# tools — grading the emulator against a second implementation

These are not tests. A test asserts that the emulator is right; nothing here can
do that, because the thing that would say so is the arcade board. What these do
is **measure the distance between this emulator and an independent
implementation of the same formats**, and report the number.

The second implementation is the
[Sonic The Fighters explorer](https://github.com/biggestsonicfan/noclip) — a
browser viewer that reads the same ROM set and decodes the same polygon format,
texture packing, colour tables and motion data, written from the same
reverse engineering but separately, and held to a real machine by its own
checks. It is a submodule at [`vendor/noclip`](../vendor/noclip), pinned to a
commit, so a grade is always measured against a known explorer rather than
against whatever `master` happens to be.

## The third party, and why it matters

Emulator against explorer is two ports agreeing. That is worth a lot and it is
not proof: two ports written from one set of notes can share a misreading, and
neither can see it. So where it is possible there is a third point that is
neither of them — [`ref/texram-ref.json`](ref/texram-ref.json), SHA-256 over a
MAME capture of the real board's texture RAM, luma RAM and colorxlat, copied
from the explorer's own toolkit.

That makes a texture or colour grade three-way:

| comparison | what a pass means |
|---|---|
| emulator vs explorer | the two ports agree |
| explorer vs board | the reference side is still anchored to hardware |
| emulator vs board | this emulator reproduces the machine byte for byte |

A few kilobytes of hashes stand in for 2.2 MB of the game's data and make the
same assertion — a single wrong texel still fails — without carrying any of it.
**Never regenerate that manifest from either port.** A reference built from the
thing it grades makes every check that reads it a tautology. If the explorer's
toolkit regenerates it, copy the new one over.

## Setup

```sh
git submodule update --init vendor/noclip
```

No `npm install`: nothing here has a dependency, except `grade-carpet.mjs`, which
drives a headless browser through `puppeteer-core` found outside this tree
(`$M2_PUPPETEER`, the explorer's, or `../noclip`'s). Node 18 or newer, because the
explorer's zip reader goes through `DecompressionStream`.

You supply the ROM set. Nothing here carries one and `.gitignore` refuses
`*.zip`. Drop `sfight.zip` in the repository root or in `roms/` (add
`schamp.zip` beside it for a split set), or point `$STF_ROM` at one; sibling `../stf-tools` and
`../noclip` checkouts are searched too. Both sides of every comparison read that
same file, so a grade can never be measuring two different games.

Anything a capture writes is the game's own data. It goes to a temp directory
outside the checkout by default, and that is deliberate.

## Running them

```sh
node tools/grade-models.mjs        # the one to run after touching geo3d.h
node tools/grade-pose.mjs          # ... and after touching the bone handlers
node tools/grade-all.mjs           # capture a scene, then grade models, texram and colours
node tools/grade-all.mjs --no-capture
```

Each grader launches its own emulator and kills it afterwards, headless (no
window, GPU or audio device). `$M2_WINDOW=1` shows the window. `--attach` uses
one you already have running with `--mcp` (`grade-models`, `grade-pose`,
`grade-texram`, `grade-colors`, `grade-all`, `dump-board` and `watch-var` take
it). The most recently built `m2hle` under `build/` is
launched unless `$M2_EXE` names one, and `$M2HLE_EXTRA_ARGS` is appended to
every emulator a grader starts, so a run can be graded with an option the grader
knows nothing about. The browser tools (`web-*.mjs`) need Node 22 or newer, for
its built-in `WebSocket`.

## What is here

| script | what it measures |
|---|---|
| `grade-models.mjs` | the index-array polygon decoder, over all 5103 model-table entries, against the explorer's: triangle positions, then which tile each textured face names and which coordinate each corner carries. All of that is a function of the ROM; what is *in* a tile depends on what the running game uploaded, which is `grade-texram`'s business. Needs no scene and no capture, which is what makes it the one to run after changing `geo3d.h` |
| `grade-pose.mjs` | the coprocessor's rig maths: op `0x62`, the body matrix, and op `0x6B`, the four two-bone IK chains that place twelve of a fighter's sixteen slots. Replays 328 frames of arguments captured off a real board (`stf-tools/motion-pose.csv`) through the coprocessor port and holds what comes back against the explorer's rig. Needs no scene and no capture either, which makes it the one to run after touching the bone handlers in `sharc_exec.h`. The CSV is looked for in a sibling `stf-tools` checkout (`$M2_STF_TOOLS` or `--csv <file>` override), and the grader skips cleanly without one |
| `grade-motion.mjs` | the half in front of `grade-pose`: the arguments this emulator's i960 itself sends the rig (one op `0x62` and four op `0x6B` a fighter a frame), for both fighters, against the explorer's motion decoder at the motion and motion frame the work structure names. Frames the game blends between motions are reported, not asserted on. `stf-tools/test-motion-mame.mjs` pointed at this emulator. `--capture <prefix>` grades one already taken; `--from 240 --frames 2200` is the attract intro |
| `grade-texram.mjs` | texture RAM. ~85% of the pages are compressed in ROM, so a sheet is a megabyte of output from a long run of the game's own code: a wrong bit anywhere in the i960 core, the bus or the decompressor lands in it |
| `grade-colors.mjs` | colorxlat, row group by row group, because the rows are written by four different routines at four different times. The two rows the game rotates are matched at every rotation instead, and one `frame_counter` has to explain them all at once |
| `grade-cull.mjs` | which of the arena's sixteen ground chunks are drawn. It captures a fight, replays the coprocessor's matrix to the point `ground_disp` tests from, and runs the ROM's own `clip_point_check_yoko` + `area_clip` on it (lattice and corner tables read from ROM). The chunks drawn have to be exactly that selection, in that order, at the explorer's matrix. The explorer names the chunks: its ground layer has to be the record's 16 slots |
| `grade-stage.mjs` | arena placement only, by running the explorer toolkit's own `stf-tools/verify-stage.mjs` unchanged on a `capture_dl` capture of a fight here. Needs a sibling `stf-tools` checkout (`$M2_STF_TOOLS` overrides) and skips cleanly without one. `grade-stages` is the fuller check |
| `grade-stages.mjs` | every arena — its parts, its animations, its moving world, its texture scrolls — as this emulator runs them, against the explorer's stage builder. Plays a round on each of the fifteen stages, then checks four things off each capture: that every arena draw is an explorer part on one measured clock, that a moving stage's flight is the explorer's, that the coprocessor lays the firmware's matrices into the display list, and that texture points and luma bands step as the explorer steps them. See "Stages" below |
| `match-replay.mjs` | attract mode's preprogrammed Sonic vs Bean fight, frame by frame against MAME: both fighters' whole work structures and the bufferram the coprocessor hands back outside the FIFO. The fight is an input replay, so any difference is a difference in simulation. See "match_replay" below |
| `grade-zsort.mjs` | which face wins where faces lie on faces (`geo3d_mesh_layers`), in pictures against MAME's. Plays the attract replay in MAME and here, here with the layers off and on, and counts, of the pixels the layers change, how many each puts nearer MAME. `--stage N` puts the replay on another stage in both. See "Faces lying on faces" below |
| `grade-carpet.mjs` | the explorer's Flying Carpet rug against MAME's pictures, from the board's own camera: the plate `draw_sphynx_head` lays under the rug (3332) must cover none of it, because the board sorts it behind every strip. Reads `grade-zsort --mame --stage 1`'s snapshots and cameras, renders the explorer headless (puppeteer-core, Edge) at each, and counts the rug pixels the plate changes. See "The Flying Carpet's rug" below |
| `grade-osage.mjs` | the sway chains (Fang's tail, Bean's feathers) at character select, against MAME: `Fn_osage`'s answers replayed from the board's own records, the ops that build the matrix a chain hangs from, and that matrix as this emulator hands it over. See "Sway chains (osage) at character select" below |
| `grade-reset.mjs` | the reset a netplay session starts from. Boots, runs into attract, performs the barrier's reset with no session (`board_reset` over the bridge) and holds the boot that follows against the first boot — registers and nine RAM regions, byte for byte — from two different states, the second reset on top of the first. Needs no oracle: the emulator is its own. See "The netplay reset" below |
| `bench-builds.mjs` | how fast each build runs the board, headless and unthrottled: game frames a second past the texture-load spike, builds alternated, best of each. The throughput companion of `ab-builds`; `bench-render.mjs` is the renderer's: each build headless with the A/V server up and drained, so the main thread draws every board frame on the real D3D11 device, and `get_status`'s `render` block gives the microseconds each stage (tile compose, scan, upload, 3D draw, tile quads) costs a frame |
| `ab-builds.mjs` | whether two *builds* emulate the same board. Counts frames with a breakpoint on the frame hook so both stop on the same instruction, then hashes the registers and the same nine regions `grade-reset` uses. No oracle: it answers "is this optimisation, this merge, this other compiler free?" in about ten minutes, where reasoning about it does not. What it cannot see: pixels (headless has no GPU), the GEO's and the 68000's private RAM, and anything that differs between two machines rather than two builds |
| `grade-all.mjs` | `grade-models`, `grade-texram` and `grade-colors` off one shared capture — driving the game to a scene is the slow part, and two captures minutes apart are two different moments of a running game |
| `dump-board.mjs` | takes a capture on its own: texture RAM, palette RAM, luma RAM and colorxlat, plus a `capture.json` naming the scene |
| `av-record.py` | not a grader: the reference client for `--av-port`, the emulator's raw A/V server. Reads the BGRA frames and the 16-bit samples off the socket, lays the irregular video cadence onto a constant 60 fps grid using each frame's board-sample stamp, and hands both to ffmpeg. About a hundred lines against a documented format (README.md, "Raw A/V out"); reading it is the fastest way to see how the format goes back together |
| `libretro/core-options.py` | not a grader: a fake libretro frontend, about a hundred lines of ctypes, that loads the built core and calls `retro_set_environment` alone -- no ROM, no GL context. It prints the table and checks what a frontend would quietly ignore instead of report: a `default_value` that is not one of the option's values (libretro.h: "this option will be ignored", and the row just never appears), a duplicated key, an option count that changed between two tables, and a row the update-display callback leaves visible with nothing behind it. Run it after touching the option table |
| `watch-var.mjs` | who writes this address, and what do they write? A bus watchpoint that reports the value and the IP behind it, so a variable whose owner is unknown can be traced back to its routine |
| `web-serve.mjs` | not a grader: a localhost static server for the web build (`build_web/site` by default), with the MIME types a browser insists on. `--rom <zip>` exposes one local zip at `/dev-rom.zip` without copying it into the site |
| `web-smoke.mjs` | the web build in a real headless Chrome or Edge, in real time: every console line, any exception, and the board's frame count once a second, with screenshots and timed key presses. With no ROM it is the check CI runs before deploying (`--expect-log`, `--fail-on-log`); `--expect-frames N` fails a run that did not get that far. `--mobile` emulates a touch phone at `--size` and `--taps "coin@12,b1@20:300,dpad-up-left@24"` presses the touch buttons by name, failing a tap that held nothing. `--hide T:N` covers the game with another tab at T s for N s; the board must keep counting ~60 frames a second while hidden (`bg_ticks` says the worker is what ran it). A browser gets 30 s to open its debugging port; one that exits or never opens it is reported with the tail of its stderr |
| `web-netplay.mjs` | two headless browsers, each with its own profile, playing a match through the page's own online panel: sign-up (or `--a` / `--b name:password`), host, join, start, accept, then `--seconds` of inputs. Passes when both boards reach "playing", keep advancing and never latch a desync. `--hide-a N` hides one tab mid-match (the background worker's case), `--ui-shots DIR` saves the panel at each step. Needs a local gateway and RPCN: see [web/gateway/README.md](../web/gateway/README.md), "Testing locally" |
| `web-objview.mjs` | the object viewer against the web build. See "The object viewer, in a browser" below |
| `lib/m2hle.mjs` | the MCP bridge client — the half of the toolkit that replaces MAME |
| `lib/args.mjs` | the flag parsing every tool shares; a flag that takes a value has to be declared, so a positional is never swallowed |
| `lib/board.mjs` | every board region and STF variable address the graders read, in one place |
| `lib/rom.mjs` | finds the ROM set (the search order under Setup) and decodes it the way the explorer does |
| `lib/report.mjs` | the PASS / FAIL / SKIP report and exit code every grader ends with; a skip is not a pass |
| `lib/dl.mjs` | the display list off a running emulator (`capture_dl`), in the layouts the explorer toolkit's checks expect: the fight a capture is taken in, `captureStage`, and the probes |
| `lib/capture.mjs` | pinning a scene, verifying the game actually loaded it, waiting for the upload to settle |
| `lib/noclip.mjs` | locates the explorer; `$M2_NOCLIP` overrides the submodule |
| `lib/texref.mjs` | the board digests and the exact slices they are cut at |
| `lib/cop-replay.mjs` | the coprocessor's current matrix replayed from the FIFO words, every matrix-writing command and the three matrix banks included, in the chip's float32 or in double; and a capture walked into per-frame draws |
| `lib/matrix.mjs` | row-major 4x4s in the board's convention, and an explorer op list turned into one |
| `lib/png.mjs` | just enough PNG for the graders: reads MAME's snapshots and writes RGB pictures, with node's own zlib |
| `ps3ui/` | the PS3-menu layout, font and sprite pipeline and its grader; see [ps3ui/README.md](ps3ui/README.md) |

## Replacing MAME

The explorer's toolkit gets its ground truth from a MAME session driven by a Lua
script running inside the emulator. Three things that script did needed
somewhere to go here, and each became a bridge command:

| MAME's Lua did | m2-hle2 command |
|---|---|
| pace on the screen's frame notifier | `wait_frames` — blocks on the emu thread's own frame clock |
| write megabytes out from inside the emulator | `dump_memory_file` — copies a bus range to a file under the emu mutex, so it is one consistent snapshot rather than a run of reads the i960 wrote through the middle of |
| drive the front end | `set_input`, straight at the I/O port bitmask |

Plus four the MAME side did not need. `dump_model` runs the emulator's own
polygon decoder over a range of the model table and writes the triangles out.
`cop_exec` pushes raw words at the coprocessor port, exactly as the i960 does,
and `dump_tgp` reads the whole 32-slot bone table back at full precision — which
is what lets `grade-pose.mjs` replay a board capture's rig arguments without a
running fight, and what makes an argument count that is wrong by one desync here
the same way it would in a game. And `rom_loaded` on `get_status`.

That last one is small and load-bearing. A profile resolves from the ROM's
CRC32s while the regions are still being assembled, so "which game is this" and
"is its data here" are different questions. Treating the first as the second
decodes a table of zeros and reports every model empty — which reads as a
decoder that agrees about nothing rather than as a race, and did exactly that
here before it was fixed.

## Verifying the scene, and why a capture cannot skip it

`stage_num` is a byte the front end sets and `change_scene` reads. Pinning it is
how the MAME driver walks the game into a chosen arena — but **pinning is not
arriving**. `stage_num` only sets what the draw routines branch on; the scene
itself was chosen the last time `change_scene` ran, which may have been long
before the pin.

So `lib/capture.mjs` waits for the 64-word record `change_scene` copies to
`0x504800` to be the record the ROM holds for the stage that was asked for,
compared on its texture-set words — the part of the record the running game does
not go on rewriting. Every capture records the scene it identified that way, and
the graders build the explorer's answer for **that** scene rather than for the
one that was requested.

Without it the tool will happily dump whichever set was already resident and
label it with the stage that was pinned. That is not a capture that fails; it is
one that grades cleanly against the wrong scene. It did, before the check went
in: two captures labelled stage 15 and stage 0 turned out to be byte-identical.

## What the first runs measured

Numbers from this emulator at the commit that added these tools. They are a
starting point, not a target.

**The polygon decoder — J = 0.990154.**

```
5103 entries: 4404 carry geometry in both, 699 empty in both, 0 only here, 0 only there
PASS  every entry decodes the same way (empty or not)   5103 entries agree
PASS  triangle counts agree                             4404 of 4404 models
FAIL  geometry is identical (Jaccard = 1)               J = 0.990154 over 601690 triangles;
                                                        3996 of 4404 models exact
```

Both decoders agree on which entries carry geometry and on how many triangles
each produces — every one of 4404 — and disagree about vertex positions on 408
of them. The disagreement is clustered, worst at models 4154–4157
(J ≈ 0.63–0.72) and in a run at 4012–4018, with models 22, 658, 1146 and 2042
all at exactly 0.8140, which is one mesh repeated.

*Since closed, and it was not the connectivity rules.* Counts agreeing while
positions did not looked like the same faces built from different vertex
picks. It was the same *corners* cut into triangles along different diagonals.
Grading against a copy of the explorer with its decal cut turned off gave
J = 1.000000 on the unchanged emulator, so every one of the 408 was that rule.
A decal is a surface's own faces emitted a second time with a cut-out texture.
The explorer cuts a quad whose four corners it has already emitted along the
same diagonal as before, so the two copies are the same triangles and a
`LESS_EQUAL` depth test lands the decal on top. `geo3d.h` now does the same:

```
PASS  geometry is identical (Jaccard = 1)   J = 1.000000 over 598728 triangles;
                                            4404 of 4404 models exact
```

**Texture addressing — 2.57% of corners, since closed.** Once the geometry
matched, the same sweep could compare what each triangle's corners address:
which tile a face names and which coordinate each corner carries. The tiles
agreed. The coordinates did not: 46,089 of 1,795,005 corners exactly, 62%
even modulo the tile. The two decoders assigned the UV stream to corners in
different orders — here A,B,D,C with U and V flipped, which had been chosen by
eye, and there B,A,C,D with no flips.

The strips decide it without trusting either side. A vertex shared by two
faces of a strip carries one UV in ROM, so the right order agrees with itself
across shared corners: 96.5% for B,A,C,D, 75.5% for the old reading. After
the switch:

```
PASS  the same faces are textured                  598335 textured in both, 0 only here, 0 only there
PASS  textured faces name the same tile            598335 of 598335
PASS  every textured corner carries the same coordinate   1795005 of 1795005 corners (100.00%)
```

On screen, attract mode's hangar "CAUTION" sign had been drawing upside down
and back to front, and the bricks of the pyramid behind the Sonic-vs-Bean ring
ran diagonally.

**The rig — and one bone length of daylight, since closed.**

`grade-pose.mjs` found a real bug on its first run, which is the argument for
having built it. The two-bone IK op wrote its two output slots the wrong way
round: the forearm's matrix went to the shoulder and the upper arm's to the
elbow, and the elbow itself was stepped along the *forearm* by the forearm's own
length rather than along the upper arm by the upper arm's.

That is a mistake with a hiding place. The two edges of the triangle add to the
same point whichever order they are walked in, so the limb still ended exactly
on its IK target and still bent by the right angle — the hand and the foot
landed where they belonged. Only the joint between them moved, to the far corner
of the parallelogram, which draws a thigh from the knee down and folds the joint
backwards. The measurement was unambiguous where a screenshot would have been
arguable: 0.385 world units against the explorer, which is one arm bone.

```
before   op 0x6B — limb positions, trig held equal     3.85e-1 world units
after    op 0x6B — limb positions, trig held equal     3.52e-6 world units
```

Everything else in the two ops already agreed. With the trig conventions held
equal the body matrix comes out at 6.0e-8 and the limb rotations at 1.2e-5 over
2624 transforms — and every one of the worst of those is a limb at reach 1.000,
stretched dead straight at a target it can only just span, which is exactly
where `sqrt(1 - c*c)` loses its leading digits and float32 parts company with
the explorer's float64. A precision floor, not a rule.

**The cosine table is not settled, and the grader says so rather than guessing.**
The explorer quantises an angle to its top byte and reads a 256-entry table;
this emulator calls `cosf` on all sixteen bits. On the board's own angles that
is worth 3.0e-2 of rotation and 9.6e-3 of position, and nothing available here
can say which is the hardware's — the check that would, `stf-tools/test-head-mame.mjs`,
says of itself that it is incomplete. So the run is taken twice, once on the
board's angles and once with every input angle snapped to the table's grid, and
the difference between the two rows is the whole cost of the disagreement.
Settling it needs the board's own matrices, which means the display-list work
below.

*Since settled, by the explorer.* Its `pose.js` now reads sine and cosine out of
the coprocessor ROM by the whole 16-bit angle, as `sharc_sincos` does, and the
two rows agree to within 7.8e-8 of rotation and 1.8e-8 of position.

**Luma RAM — byte-exact, all three ways.** The emulator, the explorer and the
MAME capture agree on all 131072 bytes. This is the one place the three-way
comparison currently closes, and it says the emulator's luma ramp is the
hardware's.

**Texture RAM — the top quarter of each sheet is never written.** Against the
explorer, both sheets match exactly up to `0xC0000` and are entirely zero above
it: the emulator fills 768 KB of each 1 MB sheet and leaves the last 256 KB
untouched. The explorer's own build is byte-exact against the MAME digests, so
the reference side is sound and the gap is this emulator's.

*Since closed, and it was the CPU.* That quarter is where the mip chain lives,
and the explorer's `pageDestinations` shows how its addresses are built: each
level's coordinates are halved, then cleared to even. The i960 does the clearing
with `notand g6, 1, g6` in `sub_4C444`, and this emulator's `notand` was a copy
of `andnot`: it inverted the wrong operand. A watchpoint showed the mip pass
running, and writing to odd addresses. With the op fixed (along with `ornot` and
`notor`, which were swapped):

```
PASS  texram0: emulator vs explorer  1048576 bytes identical
PASS  texram1: emulator vs explorer  1048576 bytes identical
```

Fixing it changed attract mode's timing, and that exposed a second CPU bug.
Interrupts were delivered as plain calls, so returning from a handler did not
restore the condition code. A timer interrupt between a compare and its branch
in `unpack_lod_data` then desynced the decoder about 45 seconds in. It was
found the same way these tools work: the explorer's `texture.js` is a bit-exact
port of that routine, so the i960's per-row decoder state was diffed against
it. Every row agreed up to the crash, which put the fault inside a row, and an
instruction trace there showed the interrupt landing.

**Ground culling — an HLE hook that was skipping the cull's own data.** The
explorer draws every part of an arena and calls whatever the board left out
"culled", so it cannot say on its own whether the board left out the right
parts. For the ground chunks it can, with help: the cull is ROM arithmetic on ROM
tables, and the only input it needs is the matrix `ground_disp` tested with.

Getting that matrix took two corrections to a replay that `verify-stage.mjs` had
never needed, because it only recovers the view *relative* to the draws:

- a frame mark falls where the frame hook fires, not where `camera_init` starts,
  so the replay has to carry the matrix stack across marks;
- `Fn_base_matrix` (op 0x03) and the other ops that set the current matrix
  outright have to be applied.

Without both, the replay's camera was a half turn about Z off the coprocessor's.
With both, it matches the emulator's own current matrix at `ground_disp` to 2e-4
over 40 frames. Separately, the outcodes the i960 wrote agreed with the port run
on that matrix in 40 of 40 frames. So the port reads the ROM the way the i960
does, and the replay reproduces the coprocessor.

The first run on a Flying Carpet fight:

```
FAIL  the chunks drawn are the ones area_clip selects   0 of 299 frames exact
      chunks switching on/off between frames: emulator 998, board's rule 17
```

The profile hooked `clip_point_check_yoko` to "return 0, visible". It returns
nothing: it writes one outcode byte per lattice point to `0x50E000`, which
`area_clip` ANDs four at a time. That address is scratch that `rob_spd_control`
fills with the fighters' positions every frame, so the stage chunks were being
culled on the low bytes of fighter coordinates. With the hook gone:

```
PASS  the chunks drawn are the ones area_clip selects   599 of 599 frames exact, camera-cell fallback drew in 29
      chunks switching on/off between frames: emulator 48, board's rule 48
```

Still not covered: `doom_cnt`'s backdrop segments and the `0x500288` camera mask
that `cage_clip_m` and the stage objects draw from. Both are culls against the
board's camera, and both could be graded the same way.

## match_replay

STF's first attract fight, Sonic against Bean on stage 1, is not the CPU
playing. It is an input replay: `replay_bank_init_data` (ROM `0xDC9B0`, copied
to `0x531000`) holds a byte of input per player per frame, and `key_play_disp`
feeds them in. The fight is therefore the same on every board. When two
emulators play it differently, the cause is in the simulation (an i960 flag, a
coprocessor reply, a float rounded the other way), never in the input. That
makes it the strongest whole-game check this toolkit has.

Reaching it from power-on means about 2200 frames of intro movie, which is
roughly 30 minutes of MAME under `-nodrc`. `match_replay` skips the movie. At
the movie step (`_sub_mode` 5) it writes the movie state a natural boot has when
the replay starts, then moves on to the replay step. The profile holds the
addresses and the nine state words (`quirks.attract_replay` in
`src/profiles/sfight.h`). The stage is left alone, because the poles and
barriers take part in the fight. m2hle does this with `--match-replay` or the
`match_replay` bridge command, and MAME does the same with
`tools/mame/match-replay.lua`.

```sh
node tools/match-replay.mjs --mame --frames 1400   # MAME's reference, headless (~12 min)
node tools/match-replay.mjs --frames 1400          # play it here and grade (~13 s)
```

Both sides sample at `variable_diff_calc` (the write tap on `0x50D000` in
MAME, the frame hook here; `capture_dl`'s `blocks` here). Each sample holds both
fighters' whole work structures (`0x3400` bytes each) plus the bufferram the
coprocessor returns outside the FIFO: the TGP slots at `0x90E800` and the
unit-overlap table at `0x90F600`. The comparison is aligned on the frame the
stage loads. It checks, in order:

- motion and energy on every frame (the fight as it reads on screen);
- `+0..+0x1F8` bit for bit (state, position, angles, velocities, requests),
  skipping the eye blink, which draws from `rand()` and so from the board timers;
- the rig at `+0x1F8`;
- the rest of the structure;
- the bufferram ranges.

It needs `$MAME_EXE` (default `../claude_mame/mame/mame.exe`) and a
`$MAME_ROMPATH` that holds only `sfight.zip`, `schamp.zip` and `segabill.zip`
(default `tools/mame/mameroms`), as `tools/mame/cop_capture.py` does.
`--ref <file>` grades against another MAME reference and `--show N` prints the differing
words of N frames from each check's first difference (default 6).

What it found first: Bean won the mutual grab that Sonic wins on the board. The
i960's `concmpi` / `concmpo` tested the equal bit instead of the less bit, so
`get_en_info`'s facing-range test built the wrong enemy-info flags from frame
+0. On the way there, the coprocessor handlers were brought to firmware
arithmetic (Newton divide, square root and reciprocal root from the SHARC's seed
tables, and the firmware's atan2 and operation order). Before that, a fight
could drift by one bit a few hundred frames in. After both fixes:

```
PASS  the fight is the same fight (every motion and energy change on the same frame)  1399 frames
```

Still differing, with no effect on the fight so far:

- `cvtri` ties: MAME rounds half away from zero, while this emulator uses IEEE
  round-to-even (the i960's default mode), so from +321 `P1+0x18C` is `0x2AAB`
  in MAME against `0x2AAA` here;
- one bit of `P1+0x1114` from +321;
- the rig from +382;
- the sign of zero in TGP bufferram.

## Faces lying on faces (`grade-zsort`)

A depth buffer cannot tell two faces in one plane apart; the board's polygon sort
can, a polygon at a time, and MAME's software renderer sorts the way the board
does. So this grader holds pictures, not state. It plays the attract replay (above)
in MAME with snapshots at replay frames `--from`..`--to` by `--step`, and here
twice over the A/V stream at the board's 496x384, face layers off and then on.
Pictures are paired by the edge count from the jump. The `frame_counter` both
sides read there has to differ by one constant: MAME's is 640 ahead, the boot
warning this emulator skips.

```sh
node tools/grade-zsort.mjs --mame --stage 5   # MAME's snapshots (~12 min)
node tools/grade-zsort.mjs --stage 5          # here, twice, and grade (~1 min)
```

`--stage N` plays the replay on another stage in both emulators, at the same
instruction: `--match-replay-stage N` here writes N to `byte_50005B` and `stage_num`
at `0x941C` (ADV_REPLAY_INT's `call change_scene`), and `MR_STAGE` in
`tools/mame/match-replay.lua` substitutes N into the two stores just before it.
The replay is recorded for stage 1, so on another stage it drifts sooner.

- **Camera match:** a frame is graded only if the camera matches MAME's within
  `--cam-tol` (0.05 units, 64/65536 of a turn). The off and on pictures share
  their camera, so a small difference adds the same noise to both.
- **The measurement:** of the pixels where off and on differ, how many each
  puts nearer MAME's (largest channel difference, by more than `--tol`).
- **Pictures:** the frame the layers change most is written as MAME | off | on |
  changes, with the changes in green (nearer with the layers) and red (nearer
  without), plus a crop of just the changes.
- **Finding a model:** `--only-model N` (or `LO-HI`) layers only those models,
  which is how a bad result is traced to one. The emulator reports which models
  drew layered faces, and `--set k=v` passes more `set_camera` switches to the
  play with the layers on.
- **Other switches:** `--toggle NAME` A/Bs another `set_camera` switch in place
  of the layers, which stay on in both plays. `--stage 0 --toggle texclamp`
  grades the texture filter's clamp at a tile edge (issue #81) on South Island's
  sky ring.

What it found, on the way to the current rules (`CLAUDE.md`, "3D Polygon
Decoder"): a straight port of the explorer's layers put 5,588 of 5,613 changed
pixels further from MAME on Casino Night. Three departures fixed that:
- the floor emblem's base face had stopped receding (model 194);
- a glove's parallel faces had been pulled onto one plane (1813/1818);
- the order comes from the board's sort under the real camera, not a vote.

With all three, at 31 frames a stage:

```
stage 1  PASS  1469 pixels changed: 1465 nearer MAME with the layers, 3 nearer without
stage 5  PASS  808 pixels changed: 788 nearer MAME with the layers, 18 nearer without
```


## The Flying Carpet's rug (`grade-carpet`)

The explorer's fill is a depth buffer with a bounded recede, not the board's
polygon sort, and the Flying Carpet is where the two parted visibly (noclip
issue 23): the plate `draw_sphynx_head` lays at y = 0 under the rug is one quad
as wide as the rug, the rug's floor ripples a tenth of a unit either side of
that plane, and where the plate was too deep along the view to recede, the
ripple's troughs fell behind it and the rug went flat. The board sorts the
plate by its farthest corner, behind every strip, and MAME never shows it.

This grader holds the explorer to that from the board's own camera. It reuses
`grade-zsort`'s MAME run on stage 1 — the snapshots, and beside each the
`frame_counter` and the camera at `0x519E98` — so take that once:

```sh
node tools/grade-zsort.mjs --mame --stage 1        # MAME's snapshots (~12 min)
node tools/grade-carpet.mjs [--out DIR]             # the explorer at each (~30 s)
```

- **The explorer:** served from `$M2_NOCLIP` into headless Edge (SwiftShader)
  through `puppeteer-core`, resolved from `$M2_PUPPETEER`, the explorer or
  `../noclip`; `$M2_BROWSER` names another Chromium. The stage clock is held
  on MAME's `frame_counter`, the carpet is ridden so the scene is in the
  board's frame, and the camera stands at the board's eye, pitch and yaw at a
  fitted `--fov` (58).
- **The measurement:** three renders a frame — as drawn, plate hidden, rug
  alone — and the rug pixels the plate changes. The board's answer is none.
  The plate's sliver past the rug's lifted edge is the board's picture too and
  is only counted. Where the plate does cover the rug, the report gives how
  much of the rug's pattern (red at 80 or more) each render and MAME show
  there; the plate and the rug's ground are one colour, so the pattern is what
  tells them apart.
- **Pictures:** `--out DIR` writes MAME | explorer | changes, with the plate
  over the rug in magenta and past it in cyan.
- **Numbers:** noclip master at 2e4cd5d has the plate over 751,768 rug pixels
  across the 31 frames of replay 400..1300; with the plate conceding the
  bound a pixel at a time, none.

## Sway chains (osage) at character select

```
node tools/grade-osage.mjs --mame     # MAME's captures of Fang and Bean at select, headless (~15 min)
node tools/grade-osage.mjs            # capture the same here and grade (~40 s)
```

`Fn_osage` (op `0x4A`) walks typed records the i960 lays in bufferram:

- the matrix a chain hangs from;
- its limits;
- its start point;
- one record a segment.

It answers a draw matrix a segment. A chain can therefore be wrong in the port or
in its inputs, and the grader checks each:

- **The port.** `tools/mame/osage-select.lua` coins up, walks P1's cursor onto
  each fighter and takes a SHARC-side capture (`cop-capture.lua`) while the
  model turns, plus a snapshot every 15 frames. `tests/cop_replay` replays it:
  every `Fn_osage` word has to be the board's.
- **Its inputs.** The ops that build the record's matrix have to leave the
  board's matrix: `0x04` loads the camera in `osage_dsp`, and `0x45` composes
  it. Then m2hle captures the same scene (`capture_dl` with `cop: 1`, which
  writes the MAME capture's format), and the matrix each chain hangs from has
  to be as orthonormal as the board's.

`--chars 4,10` picks the fighters (4 Fang, 5 Bark, 7 Espio, 10 Bean) and
`--frames` the length of each capture; `$COP_REPLAY` names the `cop_replay`
binary if it is not the build's.

The m2hle capture boots once per fighter: after one capture the select cursor
stops answering the stick. `OSAGE=<file>` on `cop_replay` dumps every call's
records and answers.

What it found: `Fn_load_matrix` read its 12 words as a row-major, Z-negated
render matrix, the old `Fn_get_matrix` format. `osage_dsp` loads the camera
with it at select, so every chain was composed onto a sheared matrix, with
column lengths 1.21 / 0.60 / 1.38. Fang's tail came out as stretched spikes
and Bean's feathers landed on the floor. Attract never sends `0x04`, which is
why the attract captures had `Fn_osage` exact and still missed it.

`cop_replay` runs a command only when the next command word arrives. It now
holds back bufferram writes the i960 makes after the command has started
answering. Otherwise `os_set_osage_after`'s carry zeroing reached `Fn_osage`
before its own write-back did.

Two things to know before trusting a row:

- m2hle's select screen does not always run at MAME's rate. Some runs make one
  `Fn_osage` call pair every other emulator frame. A run on that path may never
  send `0x04`, so the "here" shape row can pass on broken code. The MAME-side
  `0x04` / `0x45` rows do not depend on it.
- The `Fn_get_sm_ang_f` rows still differ: an angle comes out `0xFFFF` on the
  board and `0` here. This is unrelated to the chains.

## Stages

```sh
node tools/grade-stages.mjs                                  # all fifteen, 180 frames each (~8 min)
node tools/grade-stages.mjs --stages 4 --frames 2000 --no-blocks   # a whole canyon run
node tools/grade-stages.mjs --no-capture --out <dir>         # grade captures already taken
```

Attract mode only ever fights on the Flying Carpet, so `lib/dl.mjs`
`captureStage` plays a round: coin, start and the attack buttons on a loop, as
the explorer toolkit's MAME driver does. It picks the arena where
`set_vs_cnt_and_stage_num_sel` has stored `stage_num` and is about to call
`change_scene` (`0xAFC8`), with a breakpoint that rewrites the byte. Once the
round is drawing, the live stage objects are read off `fa_object0_ram`
(`0x543100`). Each one's age (`+6`) becomes a probe beside the scene probes,
and so does each cage wall's shake index. Bufferram is snapshotted at every
mark.

Each capture is checked four ways.

- **Placement.** Every arena draw has to be C · M for some explorer part's M
  and one view matrix C a frame. This is `stf-tools/verify-stage.mjs`'s check,
  on a coprocessor replay that applies every command writing the matrix
  (`lib/cop-replay.mjs`). The explorer runs every animation off one frame
  number, and the board does not. So each part may take `frame_counter` or a
  recorded object age, and has to keep one clock for the whole capture.
- **The flight.** On the Flying Carpet, Canyon Cruise and Giant Wing, the
  position, heading, pitch and roll the object wrote are checked against
  `carpetAt`, `canyonAt` and `giantWingRoll` at that object's age.
- **What the coprocessor draws.** `Fn_put_poly` copies the current matrix into
  the display list the renderer walks. Placement grades the i960's commands and
  never sees that copy. So the emulator's own words are read out of bufferram
  and checked against a float32 replay with the chip's sine table.
- **Texture animation.** The aurora's and the Death Egg floor's texture points
  (`tpd_move`) and the sea's and river's lumabase (`transmap_change`) are found
  in the geometry program memory writes. For the texture points, some object
  also has to draw from the block that frame.
  - The aurora curtain (model 1604) is not laid down by `Fn_put_poly`. It is
    handed straight to the geometry processor as (tpa, tha, oba), and its tpa is
    `0x805000`: command 4 uploaded the scrolled points to geometrizer texture RAM
    there. So a hand-over is recognised by its mesh and header pointer, not by
    the ROM texture pointer.
  - Read back through the renderer's own decoder, the curtain's pv falls by 4 a
    frame.

What the first run found:

- **`Fn_base_3x3` (0x08001010) was a no-op in the emulator.** The firmware
  (cpres1 PM 0x20460) sets the current 3x3 to the identity and leaves T alone,
  so what comes next faces the screen. The Flying Carpet's flames and the Death
  Egg's Earth are drawn after it, and so are `kira_kira_disp`'s sparkles. Fixed
  in `sharc_exec.h`. The bufferram check now holds 720 flame draws and 180 Earth
  draws to the firmware.
- **The clocks are the board's.** Casino Night's blimp, reels and cards run on
  the pinball object's age at half its count: its mover steps `+6` a second
  time. Dynamite Plant's swing and gears, Giant Wing's clouds and roll, and the
  canyon flight run on their object's age. Everything else runs on
  `frame_counter`. Over a 2000-frame canyon run, which includes the counter's
  snap back at the end of the run, the flight stays within 1.2e-4.
- **`verify-stage` itself was misreading the emulator's captures:**
  - It applied the backdrop's drift correction on stages whose flag bit 0x1B
    is clear; Aurora's sky stands still on both sides.
  - Its arena-frame vote left the drift out of the sky segments, so the sky
    could outvote the ground.
  - A cut frame drawn at a zero matrix crashed it before its summary.
  - Its replay ignores `0x03`, `0x04`, `0x10` and the inner bank. So it could
    not follow `canyon_env_disp`, which draws from a matrix it loads, and it
    only agreed with the explorer about Giant Wing's plane because it skipped
    the load.
- **What `display.js` leaves out.** Each of these is read off the listing, and
  `grade-stages` reports when a part matches only with it:
  - `pole_disp` resets the 3x3 before each flame.
  - `draw_sphynx_head` and `giant_wing_disp` draw from inner slot 8, the arena
    frame at 1.6. So the head is at 1.6, which is not the ROM bug the explorer
    describes, and the plane's body, haze and clouds bank at 1.6.
  - `slot6_obj0_init` starts the second gear at 0x800.
  - Cage walls shake by `dword_903D0[word_50A1E8[wall]]` when a fighter hits
    them.
- `grade-cull.mjs`'s replay multiplied `Fn_mul_matrix` the wrong way round.
  `_L201EA` post-multiplies, whatever the listing's comment says. Its grade is
  unchanged: 299 of 299 frames.

Two things a pinned round does not reach by itself:

- **The Final Eggman Boss** is only ever entered from the Death Egg's Eye,
  whose transition sets bit 31 of `0x500498`. `sub_2731C` draws the hangar iris
  only with that bit set, so the capture sets it and says so.
- **Canyon Cruise's** later scenery runs and the tunnel light take a whole run
  to reach: `--frames 2000`.

A part that animates but was never drawn is listed as not exercised rather
than passed.

## The sound board

The explorer has no sound, so the sound board is graded straight against MAME,
in three steps that keep the i960 out of it:

```sh
# 1. MAME, from power-on: every MIDI byte, SCSP write, changed SCSP read and
#    interrupt, with 68000 clock-period timestamps, plus MAME's own WAV
MAME_ROMPATH=<zips> claude_mame/mcp_server/.venv/Scripts/python.exe tools/mame/snd_capture.py cap/mame 5400
# 2. MAME's MIDI stream, byte for byte at the same clock period, through board/sound.h
build/Release/snd_replay.exe cap/mame cap/ours
# 3. line them up on the music-start command and compare
python tools/mame/snd_compare.py cap/mame cap/ours 70
```

`capture_snd` (bridge) takes the same capture off a running emulator, i960
included. MAME runs about 1 frame a second once the 3D starts, so a 90-second
capture is a 25-minute wait; the slot monitor (0x408) is left out on both sides
because the driver polls it 50,000 times a second.

First full run (70 s of attract music, after the sound board rebuild): the same
3094 key-ons and 3080 key-offs as MAME; 91% of MAME's notes reproduced within
30 ms with a median timing error of 0.8 ms; events identical in order, slot for
slot, for the first 12.8 s (901 events), where a timer-A race first picks a
different slot; audio envelope correlation 0.992 and loudness within 1% in every
5-second window. Before the rebuild, the same comparison matched about half the
notes of the first five seconds and held 25-32 voices keyed where MAME holds
5-16.

### Over a long session it drifts, and 70 seconds does not show it

The 70-second run above is the whole of what had ever been graded, and the board
looks excellent over it. Taken out to 231 seconds -- MAME at about 1.7 frames a
second, so a two-hour capture -- three things appear that the short run cannot
show. Attract sends an identical command stream on every cycle (the loop is
124.3 s; `A0 00 01 AE 10 10` restarts the music), so the second pass over the
same music is a controlled repeat of the first, and it grades worse.

- **The board runs fast, and it accumulates.** Matched note-ons drift from within
  0.5 ms for the first 120 s to 7.5 ms early by 200 s. It is one timer: the
  driver reloads its timers inside its own handler, so consecutive writes to
  0x418 / 0x41A measure a period fire-to-fire, and timer A comes out 505.2467
  samples against MAME's 505.2783 -- 8.1 clock periods, 62 ppm short, every
  period, 20,000 times in 231 s. Timer B matches to 8 ppm.
- **The programmed periods are identical**, 504 and 49 samples on both sides; the
  8 clocks are interrupt latency. Timer A is level 1, the lowest, so it waits out
  the level-2 timer B/C handlers, and what it waits on is `SOUND_IPL_LEAD` --
  a fitted constant standing in for the real chip sampling IPL at the microcode
  step that loads the instruction register, a different number of cycles before
  the end of every instruction. MAME models that step (`M68000` is the
  microcode-level core in m68000.cpp, not Musashi), so there is a right answer
  and a constant is not it: sweeping it with `-DM2HLE_SOUND_IPL_LEAD=N` gives
  timer A -15.2 / -8.0 / -8.1 / +9.6 / +12.4 clocks at N = 2 / 6 / 10 / 14 / 18
  and timer B -7.3 / -3.6 / +0.1 / +4.7 / +8.3. Nothing matches both, and the
  present 10 is the best of them end to end -- 12 and 14 fix timer A and make the
  note drift three to five times worse, because timer B fires ten times as often.
- **Voice allocation diverges for good.** The two boards put every note on the
  same slot for the first 60 s; then 59% of them, 7% by 90 s, and none at all
  from 120 s on, off one timer race at 12.8 s (which is where
  `snd_compare.py`'s event horizon has always stopped). Slot choice carries pan,
  DSP send and which 8 KB
  streaming window the voice plays out of, so the mix genuinely differs after
  that: envelope correlation at 5 ms resolution, with the drift taken out
  per window, holds 0.87-0.94 to 140 s and falls to 0.65-0.75 beyond 160 s.

Two things about the harness itself came out of that run. `snd_replay` had a
4096-byte cap on the MIDI stream it would replay, which a long capture passes in
silence rather than failing; and the record's timestamp is 32 bits of 11.2896 MHz
clock, so **no capture can exceed 380.4 seconds** without folding back on itself
(board/sound.h says so where the format is defined).

`snd_compare.py`'s note figure is also not what it reads as. It keys a note on
its sample address, and in this driver the sample address is the slot's own 8 KB
window -- so a note the board played on a different slot counts as a note missed,
and widening the tolerance from 30 ms to a full second moves the count by four
points. The 91% above is largely a measure of slot agreement; match on pitch and
level alone to see timing.

### The streaming refill is not the problem (`snd_watch`)

The driver gives every one of the 32 slots an 8 KB window from 0x010000 up, loops
it, and refills the 4 KB half the chip is not playing -- it finds out which by
writing the slot to MSLC and testing CA bit 0 (`btst.b #7,0x409(a5)`, sound ROM
0x604224 / 0x60452C / 0x604544, after ten `ror.l` of settling delay). That is a
hard real-time race and losing it would sound exactly like a track that distorts
and cuts out until the game restarts it -- and it is the one register the capture
leaves out on both sides, because the driver polls it 50,000 times a second.

`{"cmd":"snd_watch","on":1}` measures it instead, against the play position the
chip actually has: every refill pass that began after the chip had already entered
the chunk being filled. Over 330 s of driven fights the answer is that the race is
never lost. Read `late_up` and not `late`: all 587 flagged passes were chunk 0 at
offset 0, with the lateness spread evenly over the 4096 samples instead of
clustered past the boundary, which is the driver giving a slot a different sample
rather than a late refill -- the window address is fixed per slot, so a reload
copies over it from offset 0 with the old sample still releasing, and no register
changes to mark it.

### Which commands reached the board (`sound_codes`)

`{"cmd":"sound_codes","since":N}` returns every command the i960 has sent the
sound UART since command N (a 512-deep ring), framed as the driver frames them:
a status byte and two data bytes, so `0xAE1004` is South Island's BGM. A lone
data byte, or a status cut short, comes back with bit 31 set. Beside them:
`sent` and `taken` (UART bytes written, and read out of the SCSP's MIDI buffer
by the 68000), `midi_drops`, `midi_holds` (bytes that waited a slice for room),
and `queue_hi`, the deepest the game's own queue has
been.

That is how the "wrong music on some stages" report was taken apart
(2026-09-22). Pinned stages showed the i960 sending the right code, and a MAME
run that fed each BGM code straight into the UART with the i960 suspended showed
the 68000 playing the right song for it: identical key-ons in all 13 songs. What
was left was the path between, and `sound_codes` in a mashed two-player session
showed commands arriving as `00 00 00` and `0D B1 A8`. The cause was the ROM's
queue overlapping other variables, exposed because the emulator took the sound
interrupt only once a slice. CLAUDE.md has it under the sound board.

## The netplay reset

A netplay session is a cold boot on both machines, so the reset at the barrier
has one job: leave nothing behind. Anything that survives it is state one player
has and the other does not, because no two players did the same thing before
they pressed Start — and the frame check would not see it, since it hashes the
i960's registers and the step count, not RAM.

`grade-reset.mjs` measures that without a session and without a second machine:

    node tools/grade-reset.mjs
    node tools/grade-reset.mjs --pre 1500,5300,9000 --frames 300

It boots and takes the board at the 120th frame boundary (a breakpoint on the
frame hook counts them; polling stops a few frames late and nothing would
match). Then it runs 1500 frames on into the attract movie, asks the bridge for
`board_reset` — `netplay_reset_board_cb`, the code the barrier runs — takes the
board at the 120th frame again, and does the same from 5300 frames further on,
inside the replay fight, on top of the first reset. Each take has to be the
first boot's, byte for byte: the registers, RAM2, RAM, bufferram, tile RAM, tile
graphics, palette, colorxlat and both texture sheets. A region that differs is
reported with its first differing address, which is usually enough to name the
owner.

Measured 2026-09-19 on master: exact, 22 of 22. It was written to rule the reset
out as the cause of a remote player's board stopping 45 frames into a session,
and it did; it stays as the check to run after adding any state a reset has to
clear. What it cannot see is anything that differs between two *machines* rather
than two boots on one, the GEO's private RAM, and the sound board, which has its
own grader.

## One state at a time

`bench-builds` times the board in steady flight, deliberately past the texture-load
spike, and `ab-builds` asks whether two builds compute the same board. Neither can
answer a complaint about one game state -- "the VS screen hitches" -- because the
state is over in a second and a ten-second average hides it completely.

Two tools do. `prof-state.mjs` needs an instrumented emulator
(`cmake -DM2HLE_PROFILE=ON`, `src/core/pc_profile.h`) and answers what the BOARD is
doing: i960 instructions per ROM address between two breakpoints, symbolicated
through the live IDA bridge, with the host microseconds and step count of every
frame and the sound board's share of them. `bench-state.mjs` runs on any build and
answers how fast: instructions a second over the same window, builds alternated,
best of each.

    cmake -S . -B build_prof -G "Visual Studio 18 2026" -A x64 -DM2HLE_PROFILE=ON
    node tools/prof-state.mjs --exe build_prof/Release/m2hle.exe --state round-mask
    node tools/bench-state.mjs build_base/Release/m2hle.exe build_opt/Release/m2hle.exe \r
         --state round-mask --rounds 5

It reports instructions a second and not milliseconds on purpose. The board has to
free-run into the state (`lib/drive.mjs` says why it cannot be driven frame by frame
off the frame hook), so it enters from a frame that wanders by a few either way: two
runs of one build differ by 15% of wall clock and by a fraction of a percent of
throughput. The window's instruction count is printed beside it, so the milliseconds
an optimisation is worth are the report's last line.

### What the first two states measured (2026-09-22)

**ROUND_MASK** (`0xB820` -> `0xC34C`, the VS screen) is a texture decompression
state: 9.01M instructions over 80 slices, 88% of them in `unpack_lod_data`,
`send_lod_data`, `send_lod_data_q_sub_norm` and the RLE fill at `sub_4BAE8`. Fourteen
of those slices ran the whole 500,000-step budget without reaching the frame hook,
and the run loop paced each of them as if the frame were over -- 45% of the state's
wall clock spent asleep. Fixing that and the step loop's own per-instruction overhead
took the state from 340 ms to 186 ms (26.4 -> 48.5 Mi/s, +83%), `ab-builds` identical
against master at 600, 1800 and 3600.

**adv_movie_egg** (`0x5320C` -> `0x541DC`, the Death Egg scene of attract) is the
opposite and worth knowing as a shape: 470 frames, 15.7M instructions, NO capped
slice, and no hot routine -- `set_obj` leads at 10%, and the rest is the ordinary
per-frame spread of `get_fcurve_value_f`, `rob_disp`, `calc_unit_mat`. **54% of its
host time is the sound board**, and the i960 half runs at 51 Mi/s. There is nothing
state-specific to optimise in it: it costs what any ordinary frame costs, and what
would move it is the 68000 + SCSP (see the sound board, below), not these routines.
The step-loop work above is worth +0.3% here, which is the same change measured
against a state where the i960 is under half the time.

### Does the sound board keep time with the frames?

`clock-state.mjs` drives to the same windows and reads the frame counter and the
SCSP's sample counter either side (plus the MIDI traffic: bytes, catch-up steps, bytes
held to a later slice). The answer should be 735 samples a frame (44100 / 60)
everywhere. It is not a timing, so one run per build is the measurement.

    node tools/clock-state.mjs buildA/m2hle.exe buildB/m2hle.exe --state round-mask
    node tools/clock-state.mjs build/m2hle.exe --state round-mask --args "--live-timers"

Before `emu_sound_slice_end` the sound board was charged a frame per *slice*, and the
VS screen's 65 frames spanned 80 slices: 904.6 samples a frame (802.8 with
`--live-timers`), the music ~23% fast through the load. It is 735.0 now, at any
`--steps-per-slice`. See `SLICE-CLOCKS.md`.

## Two builds, one board

Any "does this change the emulation?" question — an optimisation, a long-lived
branch coming back, a different compiler or architecture — is measurable, and
cheaper to measure than to argue about:

    node tools/ab-builds.mjs buildA/m2hle.exe buildB/m2hle.exe --marks 600,1800

Both builds boot the same ROM (`--rom <zip>`; the default is a path on the
development machine, so pass it anywhere else) in their own directory (so neither shares
`m2hle.log` with the other or with a running instance), and **frames are counted
with a breakpoint on the frame hook**, not `wait_frames`: a poll stops wherever
it landed and nothing would match. At each mark it hashes the registers and the
nine regions the i960 can write.

Measured 2026-09-19: `wasm` against master `089a36f`, identical at 600, 1800 and
3600, with and without `--no-mesh-cache --cpu-tiles`, at +98% headless
throughput; and the arc-s merge (the handheld's sound, the load-spike work and
the ARM parity fixes) against master `3d2ca3e`, identical at 600, 1800, 3600
and 6000 — the last two inside attract's replay fight, which is where a
one-bit difference in the board would already have grown into a different
fight. That is what says the hand-resolved conflict in the run loop
(`emu_slice_body`) resolved to the same board.

### Cross-play: the web build against the desktop build

`ab-builds` drives two `m2hle.exe` over the bridge, and the web build has none.
`tests/det_digest.c` is the board without a frontend. It runs the slice both
hosts run, from the one-zip load the page uses, with inputs keyed to game
frames, and writes one line per frame: the netplay frame check, and hashes of
work RAM, buffer RAM and the COP's memory. Build it in each tree so each side
has its own frontend's exact flags, then diff:

    cmake --build build_test --config Release --target det_digest
    cmake --build build_web --target det_digest          # emsdk on PATH
    build_test/Release/det_digest.exe merged.zip --frames 12000 --out msvc.txt
    node build_web/det_digest.js      merged.zip --frames 12000 --out wasm.txt

`--script "450:c,462:,520:s,..."` holds inputs from a frame on (the web page's
`?script=` keys; uppercase letters and `!@#$` are player 2). When the two
split, `--cop FROM:TO:FILE` logs the COP conversation of those frames and
`--trace F:FILE` every i960 instruction of one frame, with a hash of the
registers. The first differing line names the cause.

Measured 2026-09-21: identical over 12,000 frames of attract and a 10,000-frame
two-player scripted match, after two fixes. Before them the builds split at
frame 2948, on a NaN's sign and on strict aliasing (WEB-NETPLAY.md,
"Cross-play").
## The object viewer, in a browser

`web-objview.mjs` is the wasm build's answer to the desktop's MCP object viewer. The desktop
emulator has a TCP bridge an MCP server talks to; a browser has no such thing, so this drives
Chrome or Edge over the DevTools protocol, calls the page's own `window.m2hleObjview`, and
writes the PNGs it hands back -- there is no filesystem in there to write them itself.

```
node tools/web-serve.mjs --rom <merged.zip>
node tools/web-objview.mjs --url "http://localhost:8080/?rom=/dev-rom.zip"      --out shots --model 3544 --six --at-frame 1800
```

`--list FIRST:COUNT` lists triangle counts instead; `--opts '{...}'` takes any field the
viewer understands; `--show` screenshots the page with the viewer on the canvas. Full
reference, and the same commands on the desktop, in MCP_GUIDE.md.

Two things it waits for, and they are different questions. `waitReady` waits for the board to
have *drawn* 3D, which is when the texture sheets and the palette are up. `--at-frame N`
waits for a frame number, which is how you get past the other half: face colours come out of
palette RAM and the game fills that per scene, so a model whose scene attract has not reached
draws correctly shaped, correctly textured and black-faced. It is not a web-only trap, but it
bites there first -- the page starts the board the moment the ROM loads, and a script can be
asking two seconds later.

## PS3 cross-play: `ps3-audit.py`

`python tools/ps3-audit.py RPCS3.log ps3wire.log` holds two ends of a PS3 Sonic the Fighters session
against each other. One end is RPCS3's own log with `sys_net_dump` (and `Signaling`) at Trace in its
`config.yml`; the other is m2hle's wire log (`--net-ps3-wire FILE`, or `"wire"` on the bridge's
`netplay_connect`), or a second RPCS3 log. It finds every datagram one side sent in the other side's
receive log by its bytes, lays both on one clock, and reports the one-way delay, what was lost each
way, each side's RUDP packet types and flags side by side (anything only one side does is marked),
the input frames and silences of the lockstep, retransmissions, and a merged timeline. Only the time
both logs cover is judged, so a long RPCS3 log with other sessions in it is fine. A PS3 against a PS3
is the yardstick: that capture lost nothing, never retransmitted, and its longest input silence was
0.7 s.

## `tools/mame` and `tests/`

The MAME side runs under the sibling `claude_mame` checkout (its
`mcp_server/.venv` Python and `mame.exe`), with `$MAME_ROMPATH` a directory
holding only `sfight.zip`, `schamp.zip` and `segabill.zip`, and `-nodrc`:
this MAME's SHARC recompiler fails the COP self-test.

| file | what it does |
|---|---|
| `mame/cop-capture.lua` | taps the SHARC's own side of the coprocessor FIFOs: command words (told apart by the PC that read them), argument and reply words, the i960's bufferram writes and the current matrix before each command, plus per-frame probes, both fighters' TGP slots and bufferram / DM snapshots at the start. `tests/cop_replay` reads it |
| `mame/cop_capture.py` | runs attract under MAME with that tap: `cop_capture.py <outprefix> <from> <frames> <probes>` |
| `mame/match-replay.lua`, `mame/osage-select.lua` | the autoboot scripts behind `match-replay.mjs --mame` and `grade-osage.mjs --mame` (above) |
| `mame/snd-capture.lua`, `mame/snd_capture.py`, `mame/snd_compare.py` | the sound board's capture and comparison (see "The sound board") |
| `tests/cop_replay.c` | replays a coprocessor capture through `sharc_exec()`, command by command with the arguments the firmware read, and checks every word it answers: `cop_replay <prefix> [examples-per-op] [only-op-hex]`. `$COPRO_ROM` names the COP data ROM; `OSAGE=<file>` dumps every `Fn_osage` call and `DRAWS=<file>` the draws as CSV, and `RESYNC` / `STATE_EXACT` tune the matrix-state check (`STATE_EXACT`: any differing bit is a bad state, not only 1e-3) |
| `tests/snd_replay.c` | MAME's MIDI stream through `board/sound.h`: `snd_replay <mame-prefix> <out-prefix> [seconds]`, `$ROMDIR` for the zips. Run it from two builds and `cmp` the five outputs to prove a sound-board change bit-exact: `ab-builds` cannot see the SCSP |
| `tests/tile_test.c` | the tile compositor against the pixel-by-pixel original it replaced, kept verbatim as the reference: 48 random boards, every pair control mode, and the pen table against `tile_pen_lut`. A ctest; `tile_test --bench` times both compositors on one frame |
| `tests/arc_bench.c` | not a CMake target: the handheld's per-slice work (emulation, then the frame's CPU-side render on sokol's dummy backend), timed per stage with no window. `--draw-digest` and `--verify-atlas` make it a check as well as a benchmark |

The rest of `tests/` (`mem_test`, `i960_test`, `rom_test`, `emu_test`,
`boot_test`, `cop_test`, `geo_test`, `m68k_test`, `input_test`, `net_test`,
`ps3net_test`, `heat_test`, `scsp_dsp_test`, `scsp_dsp_test_masks`,
`retro_shader_test`, `sfight_settings_test`) are ctest unit tests, built with the
emulator and run by `ctest -C Release` in the build directory (or
`run_tests.ps1`). `rom_test`, `boot_test`, `geo_test` and `input_test` load the
ROM set from a fixed path under the sibling `claude_mame` checkout. `det_digest`,
`snd_bench` and `ps3ui_render` are built beside them but are tools, not ctests.

## What is not here yet

**The emulator-vs-board comparison has not actually been taken.** The board
digests describe South Island, and the scene cannot be driven to on this
emulator yet (below), so every run so far has correctly skipped that row rather
than producing a number. Until a capture of the right scene exists, the
strongest available statement about texture RAM is emulator-vs-explorer.

**Pinning a scene in attract mode does not work.** Holding `stage_num` for
2700 frames never loads the arena, and `watch-var.mjs` shows nothing reads or
writes `0x500064` during attract. Attract only fights its Flying Carpet replay,
unless the `replay_stage` hook at `0x941C` moves it (`--match-replay-stage N`,
which `grade-zsort --stage` uses). A played round does take a stage, if it is written where ROUND_INIT stores it
(`captureStage`, above). `dump-board.mjs` and `grade-all.mjs` still pin during
attract, so they grade whichever scene loaded, and say which.

**Nothing above the waist.** `grade-pose.mjs` covers the
twelve slots the body matrix and the IK chains place. The other four — the
waist's own slot, the chest, the head and the pelvis — are not arguments to
anything: the board builds them by stacking translate, `0x3F` and angle ops on
the body matrix and hands the result to op `0x67`, so grading them means
replaying that stream rather than reading a capture's columns. The explorer's
toolkit is at the same place and says why (`stf-tools/test-head-mame.mjs`: at
least ops `0x29` and `0x39` carry angles and are not decoded yet, so a replay
has already drifted before the head). The sway chains are graded against MAME at character select (`grade-osage.mjs`, above), but not yet against the explorer's `js/osage.js` or `stf-tools/osage-*.json`.

**The head aim is unsettled on both sides.** The explorer aims the head at float
object 14 and notes that the board's head block is followed by three angles that
are zero in every capture taken so far — but every one of those captures is a
stance held for the whole run, which is where head data is baked. Settling it
needs a capture of a real exchange, with the fighters apart and off their idle
motions. Worth knowing before trusting either port's head.
