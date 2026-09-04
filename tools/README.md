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

No `npm install`: nothing here has a dependency. Node 18 or newer, because the
explorer's zip reader goes through `DecompressionStream`.

You supply the ROM set. Nothing here carries one and `.gitignore` refuses
`*.zip`. Drop `sfight.zip` in the repository root (add `schamp.zip` beside it
for a split set), or point `$STF_ROM` at one; sibling `../stf-tools` and
`../noclip` checkouts are searched too. Both sides of every comparison read that
same file, so a grade can never be measuring two different games.

Anything a capture writes is the game's own data. It goes to a temp directory
outside the checkout by default, and that is deliberate.

## Running them

```sh
node tools/grade-models.mjs        # the one to run after touching geo3d.h
node tools/grade-all.mjs           # capture a scene, then grade everything
node tools/grade-all.mjs --no-capture
```

Each grader launches its own emulator and kills it afterwards. `--attach` uses
one you already have running with `--mcp`.

## What is here

| script | what it measures |
|---|---|
| `grade-models.mjs` | the index-array polygon decoder, over all 5103 model-table entries, against the explorer's. Geometry only — colour and UV depend on what the running game uploaded, and a decoder grade should not be measuring that. Needs no scene and no capture, which is what makes it the one to run after changing `geo3d.h` |
| `grade-texram.mjs` | texture RAM. ~85% of the pages are compressed in ROM, so a sheet is a megabyte of output from a long run of the game's own code: a wrong bit anywhere in the i960 core, the bus or the decompressor lands in it |
| `grade-colors.mjs` | colorxlat, row group by row group, because the rows are written by four different routines at four different times. The two rows the game rotates are matched at every rotation instead, and one `frame_counter` has to explain them all at once |
| `grade-all.mjs` | all of the above off one shared capture — driving the game to a scene is the slow part, and two captures minutes apart are two different moments of a running game |
| `dump-board.mjs` | takes a capture on its own: texture RAM, palette RAM, luma RAM and colorxlat, plus a `capture.json` naming the scene |
| `watch-var.mjs` | who writes this address, and what do they write? A bus watchpoint that reports the value and the IP behind it, so a variable whose owner is unknown can be traced back to its routine |
| `lib/m2hle.mjs` | the MCP bridge client — the half of the toolkit that replaces MAME |
| `lib/capture.mjs` | pinning a scene, verifying the game actually loaded it, waiting for the upload to settle |
| `lib/noclip.mjs` | locates the explorer; `$M2_NOCLIP` overrides the submodule |
| `lib/texref.mjs` | the board digests and the exact slices they are cut at |

## Replacing MAME

The explorer's toolkit gets its ground truth from a MAME session driven by a Lua
script running inside the emulator. Three things that script did needed
somewhere to go here, and each became a bridge command:

| MAME's Lua did | m2-hle2 command |
|---|---|
| pace on the screen's frame notifier | `wait_frames` — blocks on the emu thread's own frame clock |
| write megabytes out from inside the emulator | `dump_memory_file` — copies a bus range to a file under the emu mutex, so it is one consistent snapshot rather than a run of reads the i960 wrote through the middle of |
| drive the front end | `set_input`, straight at the I/O port bitmask |

Plus two the MAME side did not need: `dump_model`, which runs the emulator's own
polygon decoder over a range of the model table and writes the triangles out,
and `rom_loaded` on `get_status`.

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
of them. Counts agreeing while positions do not points at the connectivity rules
rather than at the face loop: the same faces are being built from different
vertex picks. The disagreement is clustered, worst at models 4154–4157
(J ≈ 0.63–0.72) and in a run at 4012–4018, with models 22, 658, 1146 and 2042
all at exactly 0.8140, which is one mesh repeated.

**Luma RAM — byte-exact, all three ways.** The emulator, the explorer and the
MAME capture agree on all 131072 bytes. This is the one place the three-way
comparison currently closes, and it says the emulator's luma ramp is the
hardware's.

**Texture RAM — the top quarter of each sheet is never written.** Against the
explorer, both sheets match exactly up to `0xC0000` and are entirely zero above
it: the emulator fills 768 KB of each 1 MB sheet and leaves the last 256 KB
untouched. The explorer's own build is byte-exact against the MAME digests, so
the reference side is sound and the gap is this emulator's.

## What is not here yet

**The emulator-vs-board comparison has not actually been taken.** The board
digests describe South Island, and the scene cannot be driven to on this
emulator yet (below), so every run so far has correctly skipped that row rather
than producing a number. Until a capture of the right scene exists, the
strongest available statement about texture RAM is emulator-vs-explorer.

**Pinning a scene does not work on this emulator.** Holding `stage_num` at a
value for 2700 frames never gets the requested arena loaded, and `watch-var.mjs`
says why: **nothing in m2-hle2 reads or writes `0x500064` at all** during
attract, over 25 seconds of watching, either direction. The MAME driver pins the
same byte successfully, so this is a difference between this emulator and the
board worth chasing on its own — it is not a limitation of the tooling. The
capture path handles it correctly in the meantime: it reports the scene it
actually got and grades against that.

**No display-list grader.** The explorer's toolkit replays a captured display
list into (model, matrix) draws and holds every arena draw against the stage it
builds from the ROM tables — the check that would grade the COP matrix pipeline
end to end. It needs a raw capture of both FIFO ports in write order; m2-hle2's
`geo_capture` ring records the coprocessor port only, and the geometry
processor's `0x804000` writes are handled separately for clip windows. Adding a
unified dual-port capture is the next piece of bridge work.

**No motion, pose or osage graders.** The explorer has the rig, the IK chains and
the sway chains, and its checks hold them against real captures. Grading this
emulator's COP bone handlers against them is the natural next step and needs
nothing new from the bridge beyond what `dump_bones` already gives.
