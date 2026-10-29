# Tool-assisted play (`core/tas.h`, `ui/tas_window.h`, Pinboard #1462)

Part of m2-hle2's load-bearing invariants, indexed in [CLAUDE.md](../../CLAUDE.md): facts reverse-engineered or debugged into the emulator, in no datasheet. Do not re-derive them.

The desktop build's TAS menu and window. A movie is its start (a savestate zip held in memory, the *anchor*) and one input per board frame after it: the held word and, on a gun game, the guns' GUN payload (`lightgun_feed_put`, as follow.h's). The board runs on the movie through the same latch a follower or a replay uses (`g_input.net_held` + `use_net`), so the board itself is unchanged: no `NETPLAY_PROTO_REV` bump, and with TAS off every hook is a test of `tas_live()`. `tas_live()` is a constant false without `M2HLE_DEV_TOOLS`, so the web, SDL3 and libretro builds carry the code but never run it.

Using it:

- TAS > *Start a movie here* takes the board as it is; *from power-on* resets it first (the start waits for the reset: `start_req` in `tas_pump`). *Open movie...* / *Save movie...* read and write `.m2tas` files.
- F10 advances a frame (pauses while it runs). F11 rewinds a frame and repeats while held. Shift+F11 undoes the rewind, going forward to where the run of rewinds began. F12 turns recording on and off.
- The virtual pad: a click holds a button until clicked again; a right-click holds it for the next frame only. *Keys and pads too* records the host's own buttons alongside. On a gun game, the aim box sets where each gun points (left button P1, right P2), and *Off-screen border* how near the edge an aim counts as off the screen (MAME's 5 % down to none, Pinboard #1493); each frame records the border in its GUN payload, so changing it is an edit like any other.

- Branches 1-9 keep whole movies (a best run so far). *Undo* / *Redo* take back an edit, a take that wrote over the movie, or a branch load. In the piano roll, a click on a cell toggles that button in that frame, and a click on a frame number goes there.

The rules that keep it exact:

- **Frame f's input is read while the board runs frame f; the cursor is the frame about to run.** `tas_slice_begin` latches it at every slice, a capped (mid-frame) slice included, and `tas_slice_end` advances the cursor only on a slice that reached the vblank. A slice that stopped short sets `mid`. A mid-frame board is never stored in the greenzone, and a seek from one always loads a state.
- **A seek always runs at least one frame** (it loads the state at or before `target - 1`). The picture on screen is then the target's own, not a stale one from the state's frame. Seeks run unpaced and muted (`tas_unpaced`, `g_sound_mute` set after `sound_settle`), and stop by `frame_budget`. Pausing during a seek leaves the board where it got to.
- **The greenzone is rollback.h's flat in-process copy** (`rollback_save_to` / `rollback_load` plus the latches, as GGPO loads). It is not the zip savestate: about 6 MB and a memcpy, not a deflate. Only the anchor is a zip, so a movie file loads on another build that reads its savestates. States are taken between slices (`tas_pump`) every `gz_every` frames, and at every stop, so frame advance always has the frame behind it. Over budget, the off-grid states farthest from the cursor go first, then the grid's step doubles. Frame 0 always stays.
- **An input that changes behind the cursor makes the board stale** (`tas__dirty` sets `stale`, drops the states after the change and clears the frame checks). The next seek must load a state even when it goes forward. Without that, a branch load ran the new inputs on from a board that had played the old ones (the test caught it).
- **Every frame keeps the frame check it first ended on** (`netplay_frame_check`). A frame that ends on another check with the same inputs is a desync, the emulator's bug: `desync` names the first one, logged at WARN and shown in red in the window.
- **After a seek, the pad takes the movie's input at the cursor** (`tas__pad_from_movie`). Otherwise frame advance after a rewind recorded whatever the pad last held, and, with *A new take cuts the rest* on, dropped the rest of the movie. Now nothing changes until a button does.
- **A seek asked for and not yet taken up plays the movie's frame, never the pad's** (`tas__frame_input`). The window and the bridge open a movie under the mutex, between two slices, and the run loop takes the seek up only in `tas_pump` before the next one. A board that was running ran a slice first, recorded frame 0 from the pad, and the cut dropped the other 92907 frames of the movie (Pinboard #1640).
- **A take that differs from the movie counts one rerecord**, and its undo point is pushed once per take (`take_saved`). With *cut* on (the default), the old rest of the movie goes; it is still in Undo and in any branch.
- **Anything the movie cannot carry stops TAS** (`tas_break`, beside every `follow_lead_break`): a reset, a state load, the idle hold, a comm-link role reset, another ROM, a netplay session. The movie is kept, to save or to take up again (*Take the movie up again* goes back to its anchor).
- **TAS refuses a board that is not the player's alone** (`tas__busy`): netplay or GGPO, a replay playing, a follower, a comm link, SKY EYE.

The `.m2tas` file is a zip:

- `tas.json`: format 1, profile id, frames, rerecords, `warning_skip`, the eight `follow_settings` words, and the branches' `index:frame` list;
- `start.sta`: the anchor;
- `inputs.bin`: `M2TASIN1`, u32 frames, u32 bytes per frame (28), then each frame's held word and GUN payload, little-endian;
- `branch-N.bin` for each branch kept, in the same layout.

Loading a movie restores the settings, loads the anchor and plays the movie to its end. It refuses a movie of another profile.

Tests: the bridge's `tas` command (`ui/mcp_bridge.h`: start, seek, rewind, undo_rewind, edit, branch_save/load, save/load, aim, and `from`/`count` for the held words and checks). Record 320 frames of sfight, then rewind, replay, edit, undo, branch, insert/delete, and save and reload the file. Every frame check comes back identical. The same holds for a hotd movie whose gun aim moves.
