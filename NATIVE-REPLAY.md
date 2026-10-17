# Sonic the Fighters' own replay format

Pinboard #602 kept each round of a recorded match in STF's own replay formats. #617 decoded
them in full, plays them back through the game's own replay code, and made them the default
for every replay this emulator saves: a whole local session (from the first credit back to
attract) and an online match (RPCN lockstep or GGPO) alike. The old record, the board's state
at the match's cold boot plus both players' inputs (#572), is still there as an option.

The code is `src/profiles/sfight_native.h` (the game's side) and `src/core/replay.h` (the
file). Every address below is the arcade program (`epr-19001`/`19002`, CRC32 `72E66A1D`), and
both STF profiles (`sfight`, `sfight_console`) carry the hooks.

## What the game keeps

STF has three replays of its own. None of them is a file on the board: each is a block of
RAM, laid out as below, and that is exactly what a `.m2replay` stores.

### The key bank: attract's replay fight

`0x531000`, `0x1014` bytes; `replay_bank_adr` (`0x500168`) points at it.

| Offset | Size | What |
|--------|------|------|
| `+0x00` | 4 | not written by ROUND_DSP |
| `+0x04` | 4 | `stage_x` |
| `+0x08` | 4 | not written by ROUND_DSP |
| `+0x0C` | 4 | 1P's character |
| `+0x10` | 4 | 2P's character |
| `+0x14` | `0x1000` | keys: two bytes a frame, 1P then 2P (2048 frames, ~34 s) |
| `+0xFFE` / `+0xFFF` | 1 + 1 | 2P's / 1P's rob `+0x6CC`, written at the round's start |

- ROUND_DSP writes the header at `0xD008`-`0xD084`. It starts the key_record task only when
  STAGE_ID (`0x500054`) < 9 and `not_scr_bg_move` bit 4 is clear (`0xCFF0`). So a
  sudden-death round is never keyed.
- `key_rec_disp` (`0x37CE0`) stores each rob's `+0x1200` pad byte. Bits 6 and 7 are swapped
  when the rob faces left (rob word 0, bit 1). `key_play_disp` (`0x37DC4`) swaps them back.
- The task's `+0x54` is where the next key goes. Past offset `0xFFE` it stops by storing 0 in
  its own task word.
- Frames past 2037 have already written over the `+0xFFE`/`+0xFFF` header bytes, so a long
  round loses them. `fighters.bin` keeps them.

The ROM copies its own Sonic-vs-Bean bank in only at boot (`init_adv_rep_ram` `0x7444`,
called from `0x6ABC`). After that, attract's replay fight (ADV_REPLAY) plays whatever is in
the bank.

### The round-start snapshot the bank plays from

- `0x572000`, `0x160` bytes (`key_start.bin`): the two fighters, as `save_key_replay`
  (`0x37E94`) keeps them.
- `0x573000`, `0xC80` bytes (`effect_start.bin`).

`load_key_replay`, called from `key_play_init` (`0x37D80`), puts both back.

### The ending slot: the credits' replay

`0x5A0010 + (STAGE_ID − 1) · 0x8060`, `0x8060` bytes, ten slots.

`save_ending_rep_data` (`0x38464`) writes it at the end of a round the game does not take to
sudden death, and never in a versus game. `load_ending_rep_data` (`0x385B4`) reads it back.

| Offset | Size | What |
|--------|------|------|
| `+0x00` | 4 | `stage_x` |
| `+0x04` | 4 | `dword_500434` |
| `+0x08` | 2 | the s16 at `mod_fa_obj0+6`, less `0xEF` |
| `+0x0A` | 1 | 1 when `byte_50004D` is not 0 |
| `+0x0C` / `+0x0E` | 1 + 1 | 1P's / 2P's character |
| `+0x0D` / `+0x0F` | 1 + 1 | 1P's / 2P's rounds won |
| `+0x10` | `0x44` | the `mod_fa_record` task's bytes `0x40`-`0x83` |
| `+0x60` | `0x8000` | `rep_ram` (`0x535000`): the instant replay's ring, `0x86` bytes a frame, so only about 244 frames |

## Why the bank alone does not replay a round

The bank holds 2048 frames, and its snapshot is a fraction of the fight's state. AM2 only
ever played it from attract, on a fight they had chosen. Played from attract, a recorded
round splits from the original within a few frames, for these reasons, each found by
dumping both robs every frame and comparing:

- **Most of each rob is not in `key_start`.** The motion, the hit state and the AI's
  records are all missing.
- **The coprocessor carries the fight.** The unit matrices and the collision balls, current
  and previous (DM `0x30420`, `0x1403E80`, `0x1407E80`), feed the next frame's collision
  push-out.
- **Other task data outside the robs:** `mod_fa_coli`, `mod_fa_obj0` and `mod_fa_camera`.
  The camera's angle is read by the heads, the eyes and `get_my_info_new`.
- **Each rob's motion blend RAM** (`rob+0xBD8` points at it, `0x7C8` bytes).
- **`rand` (`0x66B0`) mixes in the board's four timers**, and attract does not run the
  board as the game did. The only draw the fight's outcome depends on is `play_motion`'s
  chance step (`0x1C474`, `rand < chance`), so those draws are kept and handed back.
  Everything else that calls `rand` (the CPU's AI at `0x3BB4C`-`0x3E1AC`, the sound once a
  frame at `0x3F4E4`) either is not running in a replay or does not reach the fight.
- **The camera's player view.** `cam_mode 0x0A` (`0x6B408`) runs `cam_mode_33`, the
  player's view, only when mode and `_sub_mode` are 9 (a game's fight) and the camera
  task's `+0x41` bit 0 is up; a start button flips that bit. Attract is not mode 9, and the
  camera's angle reaches the fight, so the view each frame is kept.
- **The game assignments** (energy, damage type, hyper mode, barriers) are the cabinet's,
  not the round's.

So every keyed round also carries four parts in the game's own layouts, which together make
it whole.

## The parts of a round

A `.m2replay` is a zip. Each round the game played is `native/round-NN/` (NN from 01), with
one file per part and a `round.json`.

| File | Bytes | Board address | What |
|------|-------|---------------|------|
| `round.bin` | 16 | – | which round it was (below) |
| `key_bank.bin` | `0x1014` | `0x531000` | the key bank as the judge left it |
| `key_start.bin` | `0x160` | `0x572000` | the bank's fighter snapshot |
| `effect_start.bin` | `0xC80` | `0x573000` | the bank's effect snapshot |
| `ending.bin` | `0x8060` | ending slot of STAGE_ID | the credits' replay, built as `save_ending_rep_data` would build it (so a versus round has one too) |
| `keys.bin` | 2 a frame | – | every frame's key pair, start to judge (below) |
| `fighters.bin` | `0x8CA0` | – | the round's first frame (below) |
| `settings.bin` | `0x42` | `0x59C340` | the game assignments |
| `draws.bin` | 4 a draw | – | `play_motion`'s chance draws |
| `view.bin` | 1 a frame | – | the camera's view toggle |

- The first five parts are taken for every round, keyed or not. The last five exist only
  for a keyed round (`round.bin` flag bit 0).
- All of them are taken at `0xDD20`, in ROUND_DSP's judge (`ldob byte_50004D`). At that
  point the round is over, CTRL_TIMER has run out, and neither the sudden-death branch nor
  `not_sudden` has run, so nothing the ending slot reads has moved yet.

### `round.bin` (version 2)

| Offset | Size | What |
|--------|------|------|
| `+0` | 4 | `"STFR"` |
| `+4` | 1 | version: 2 (1 is #602's) |
| `+5` | 1 | `stage_num` (`0x500064`) |
| `+6` | 1 | STAGE_ID (`0x500054`) |
| `+7` | 1 | flags: bit 0 keyed, bit 1 ran past the bank's 2048 frames, bit 2 sudden death follows |
| `+8` | 2 | key frames, u16 LE (`keys.bin`'s length / 2) |
| `+10` | 1 | `curr_round_num` (`0x500055`) |
| `+11` | 1 | `gameprogram` (`0x50004C`) |
| `+12` | 1 | `byte_50004D` |

Sudden death follows when `byte_50004D == 2`, the two win counts are equal, and
`num_rounds_to_win` (`0x500052`) ≤ 1P's wins. By `0xDD20` the win counts have already been
updated.

### `keys.bin`

Two bytes a frame, 1P then 2P, as `key_rec_disp` lays them out, from the round's first frame
to the judge.

- The first 2048 are read at `key_rec_disp`'s entry: frame = (`+0x54` − bank − `0x14`) / 2.
- Once the bank is full the task stops. The rest are taken at the event loop's visit to the
  key_record task (`0x11418`, `ld 0(g13), r4`, with g13 = the task word at `0x50084C`), the
  same point in the frame where `key_rec_disp` would have run.
- Frames are capped at `0x4000` (about 4½ minutes; a round's timer runs out long before).
- **Off by one:** the key recorded on frame f + 1 is the one played on frame f.

### `fighters.bin` (version 2)

| Offset | Size | What |
|--------|------|------|
| `+0x00` | 4 | `"STFF"` |
| `+0x04` | 1 | version: 2 |
| `+0x06` | 2 | `start_yang` (`0x500432`), u16 LE |
| `+0x08` / `+0x09` | 1 + 1 | the bank's `+0xFFE` / `+0xFFF` |
| `+0x0C` | `0x14` | the bank's first `0x14` bytes |
| `+0x20` | `0x3400` | rob 0 (`*0x500804`) |
| `+0x3420` | `0x3400` | rob 1 (`*0x500808`) |
| `+0x6820` | `0xC00` | COP DM words, LE: `0x30420` ×`0x180` (unit matrices), `0x1403E80` ×`0xC0`, `0x1407E80` ×`0xC0` (balls) |
| `+0x7420` | 2 × `0x7C8` | each rob's motion blend RAM (`*(rob+0xBD8)`) |
| `+0x83B0` | `0x8F0` | RAM: `0x519510` ×`0x3F0` (`mod_fa_coli`), `0x519E00` ×`0x80` (`mod_fa_obj0`), `0x519E80` ×`0x480` (`mod_fa_camera`) |

- The header is taken at `key_rec_disp`'s first frame.
- The rest is taken at rob 0's first task visit after that frame. In a game the key task
  runs before the robs; in attract's replay `key_play_disp` runs after them. Taken at
  `key_rec_disp`, the snapshot sits one rob run behind the point playback writes it, and
  every per-frame counter in the robs plays one frame behind.
- Version 1 (#602) ends at `+0x6820`. It still plays, without the COP, motion and RAM parts.

### `settings.bin`

`0x42` bytes at `0x59C340`, the work copy of the game assignments the game reads. Playback
writes it for the round and puts the cabinet's back afterwards.

### `draws.bin`

Per draw, 4 bytes: the key frame it fell on (u16 LE) and the value `rand` returned (u16 LE).
They are taken at `0x1C478`, right after `play_motion` calls `rand`, with the result in g0.
Playback hands them back in order at the same instruction.

### `view.bin`

One byte a key frame, read at `cam_mode 0x0A`'s `cmpo 0, r5` (`0x6B478`, where r5 is the
view bit):
- bit 0: the player's view ran.
- bit 1: `gameprogram` was set, so `cam_mode_33` swapped the robs.

### `round.json`

The profile's summary, with where each part goes:

```json
{"game":"stf","round":1,"stage":3,"stage_id":4,"keyed":1,"key_frames":1042,"sudden":0,
 "p1":0,"p2":26,"p1_wins":1,"p2_wins":0,"energy":[10,0],
 "parts":[{"file":"round.bin","addr":0,"len":16},{"file":"key_bank.bin","addr":5443584,"len":4116}, ...]}
```

- `energy` is both fighters' energy (rob `+0x1AC`) at the judge. Playback checks the fight
  it played against it.
- `addr` 0 means the part has no home on the board.

## The file around the rounds

`replay.json` sits at the zip's root. It holds the match's label: players, characters,
stage, date, winner, rounds and the room's settings. It also has two fields that say what
the file holds:

- `"content"`: `"native"` (the rounds only) or `"inputs"` (also `state.sta` and
  `inputs.feed`, #572's record).
- `"native_rounds"`: how many `native/round-NN/` there are.

A native-only file is a few KB a round (most of it the `0x8060`-byte ending slot, which
deflates well). An inputs file adds the ~6.7 MB cold-boot state, deflated to ~140 KB.

What gets recorded:
- **An online match:** from the session's cold boot to its result (+240 frames).
- **A local game:** from the moment the profile says a game is on (`in_play`: main mode
  4-19, not 16-17) back to attract. The CPU's rounds and continues are in the same file.

A native-format match none of whose rounds the game kept is not saved.

### Choosing the format

| | Save | Play |
|-|------|------|
| Desktop | Replays → Save as (kept in `replay.cfg` as `format=`) | Replays → Play as |
| Command line | `--replay-format native\|inputs` | `--replay-view native\|inputs\|auto` |
| Bridge | `{"cmd":"replay","format":...}` | `{"cmd":"replay","view":...}` |
| Web page | Replay tab, "Save the buttons from power-on" | Replay tab, "Play a replay's buttons" |

- The default is native for both.
- The "auto" view plays natively when the file has a keyed round, and otherwise follows the
  inputs.

## Playback

The keyed rounds go to the profile (`sfight_native_play`). The board boots with the battery
detached, and attract is sent straight to its replay fight (`g_match_replay`). Each round
then plays through the game's own code:

1. **ADV_REPLAY_INT (`0x8F80`)**
   - `key_bank.bin`, `key_start.bin`, `effect_start.bin` and the ending slot go in.
   - The round's stage is pinned (`g_replay_stage_pin`, so attract's stage pick takes it).
   - The bank's first `0x14` bytes and `+0xFFE`/`+0xFFF` come back from `fighters.bin`.
2. **`key_play_disp` (`0x37DC4`)**, which `key_play_init` falls into once
   - On the round's first frame it writes the rest of `fighters.bin`, `start_yang` and
     `settings.bin`.
   - Attract's steps 8 and 9 still run the replay fight's set-up (the camera at `0x1FA44`,
     `get_frame_dat` at `0x30838`). So until `dword_500034` (1 << step) leaves `0x300`, the
     first frame is written again and held.
   - On every frame after that it puts the next key pair at the bank's `+0x14` and points
     the task's `+0x54` there.
3. **`play_motion`'s draw (`0x1C478`)** takes `draws.bin`'s next value into g0.
4. **`cam_mode 0x0A` (`0x6B408`)** jumps to `cam_mode_33` past its rob swap (`0x6DCF8`), with
   g7/g8 set to the robs, on each frame whose `view.bin` byte has bit 0.
5. **ADV_REPLAY_DSP (`0x97B8`)**
   - It holds attract's timer at 2 until the keys run out, so neither a KO nor attract's
     timer ends the replay early.
   - Then it checks both energies against `round.json`. The log says "as recorded" or
     "NOT as recorded".
   - It puts the cabinet's settings back and lets the routine's own count end the round.
6. **ADV_REPLAY_PIC2 (`0x98B8`)** goes back to ADV_REPLAY_PIC for the next round instead of
   on to the CPU battle.

After the last round, attract carries on and the replay ends.

To put a single round back for attract to play on its own, use `replay_native_install`
(bridge `{"cmd":"replay","native":N}`). That writes only the game's own parts.

### Seeking and GGPO

- **Seeking:** back is a restart, run unpaced to the frame. Forward is the run alone.
- **GGPO:** a rollback cuts the native rounds back with the board (`replay_ggpo_mark` /
  `replay_ggpo_rewind`). The recorder's counters are board state (`savestate_extra`
  `SFN_REC`).

## How exact it is

- Measured for #617: all 7 rounds of a local session and 16 of the 17 keyed rounds of six
  GGPO matches play to the recorded energies.
- The one that does not (a Bean-vs-Bark round 2) has P2 pressing `0x05` on the round's second
  key frame. The key reaches rob +0x1200 as on the board, but the fighter answers it
  differently: +0x19C becomes `0x07` on the board and `0x29` in playback, and the fight goes
  its own way from there. So some state the press depends on is not in the snapshot.
  Candidates are a flag outside the robs, or rob +0x614's top byte, which differs on frame 0
  of every round. This is open. The inputs view of the same file is exact.
- Against the inputs record (a follower of the same match), both fighters were compared
  word for word every frame. What still differs is bookkeeping:
  - pointers into the frame's texture-point area (rob `+0x2058`/`+0x205C`);
  - the sign of a zero;
  - two accumulators at `+0xB30`/`+0xB38`, which grow without bound. Nothing in the
    rounds checked depended on them (every one ended on the recorded energies), but what reads
    them has not been traced.
- It is not the board, and cannot be made to be. Attract runs a different set of tasks,
  timers and sound from a game, so the frame counts and everything outside the fight
  differ. The inputs record is the exact one.

## Sega Rally

Sega Rally (`srallyc`, Pinboard #997, `profiles/srallyc_native.h`) has a replay of its own
too: the championship ending's staff credits play a clip of each course the player raced.
Its native rounds are those clips.

### What the game keeps

- **The clips.** Each course has a buffer of `0x9AB0` bytes at `0x556420 + course * 0x9AB0`
  (forest, desert, mountain, lakeside), and its count of frames at `0x217260[course]`.
  - A record is the `0x58`-byte car block (`0x213B40` is the player's), taken every 4 frames.
    Records 0-224 are the player's car, 225-449 the nearest rival's.
  - `0x5E50E8` clears the count at the race start. At each of the course's three points
    (the table at `0x5E5010`) the game picks the nearest car and records 250 frames of both,
    so a course holds up to 750 frames.
  - `0x5E53C0` (course, frame, side) reads a clip back, the player's frames from 0 and the
    rival's from 900, interpolating between records.
- **The ending.** Sub-state 5 (the championship) states 5 and 6 (`0x5E7CB0`, `0x5E78B0`),
  reached when the lakeside is won. Its own state is `0x218C50`: the podium, then state 3,
  the credits over each course's clip, forest to lakeside (`0x5E7E40`, which moves the course
  on), then the name entry. Each course plays for 679 frames. A course never raced plays as
  an empty road, as on the board.

### The parts of a round

A round is a race, taken at the championship's race end (`0x5BAE40`, sub-state 5 state 4),
which every race reaches, won or timed out. Practice (sub-state 7) is not recorded.

| Part | Board address | |
|------|---------------|-|
| `clip.bin` | `0x556420 + course * 0x9AB0` | the course's buffer, `0x9AB0` bytes |
| `count.bin` | `0x217260 + course * 4` | its count of frames |

`round.json` is `{"game":"srallyc","course","passed","car","clip_frames","keyed","key_frames"}`.
A round is keyed when its clip has 1-750 frames; `key_frames` is then 680, so a round plays
for 679 frames of the credits.

### Playback

The rounds are grouped into championships, each a run of rounds whose courses climb (a new
game starts again at the forest). The board cold-boots; at the main loop's mode dispatch
(`0x5A2200`), on attract's first frame:

1. The four counts are zeroed and the championship's clips and counts go back in.
2. The game is put where the lakeside's race end puts it: mode 3 (a player's game), sub-state
   5, state 5, course 3. The ending runs from there on the game's own code.
3. When the ending reaches its name entry, the board is sent back to its boot (mode 0, no name
   asked), and at attract the next championship goes in. After the last one the replay ends.

`g_native_play` counts the credits' frames over a recorded course. The recording hook only
reads, and playback writes only while a replay plays, so a board not playing one is unchanged.
