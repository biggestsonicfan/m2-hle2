# m2-hle MCP Server Guide

A new Claude instance reading this can fully operate the m2-hle Sega Model 2 emulator via the MCP tools listed below. No prior context is needed.

---

## What this is

**m2-hle** is a Sega Model 2 arcade board emulator written in C11 with an ImGui debug UI. The first (and currently only) game profile is *Sonic The Fighters* (STF). The emulator runs an Intel i960KB CPU at 25 MHz with HLE (high-level emulation) hooks for timing and hardware stubs.

The **MCP bridge** is a local TCP JSON server built into the emulator. When launched with `--mcp`, the emulator listens on `127.0.0.1:7172`. The Python MCP server (`mcp_server/server.py`) connects to that port and exposes each command as an MCP tool.

---

## Starting the emulator

**Option A — Python server launches it automatically (recommended):**
```
mcp_server\.venv\Scripts\python.exe mcp_server\server.py \
    --launch \
    --exe build\Release\m2hle.exe \
    --rom C:\path\to\sfight.zip \
    --run
```
The server launches `m2hle.exe --mcp --rom <path> --run`, waits up to 10 s for the bridge port to open, then starts serving tools.

**Option B — Launch emulator manually:**
```
build\Release\m2hle.exe --mcp --rom C:\path\to\sfight.zip --run
```
Then start the Python server separately:
```
mcp_server\.venv\Scripts\python.exe mcp_server\server.py
```

**CLI flags for m2hle.exe:**
| Flag | Meaning |
|------|---------|
| `--mcp` | Enable the TCP bridge (required for MCP) |
| `--mcp-port N` | Use port N instead of 7172 |
| `--rom <path>` | Auto-load this ROM zip on startup |
| `--run` | Start executing immediately after ROM load |
| `--match-replay` | Arm `match_replay` (below) from boot |
| `--objview [N]` | Open the object viewer at boot, optionally on model N |
| `--headless` | No window, GPU or audio device. The object-viewer tools do not work here |
| `--no-tray` | `--headless` without its notification-area icon (a service, or a Session 0 run) |
| `--overlay <path>` | Load a plugin that paints layers over the picture (`src/ui/overlay_plugin.h`) |
| `--overlay-args <s>` | Handed to that plugin verbatim; the host does not parse it |
| `--overlay-game WxH+X+Y` | Where the board sits inside the composed frame |
| `--overlay-reload` | Reload the plugin when it changes on disk |

ROM set: MAME `sfight.zip` (clone of `schamp.zip`). The emulator looks for `schamp.zip` in the same directory as `sfight.zip` for shared files.

---

## MCP tools reference

### Status and registers

**`get_status()`**
Returns: `running` (bool), `halted` (bool), `ip` (hex string), `steps_per_second` (int),
`profile` (string), `frames` (int), `rom_loaded` (bool).

`frames` is a monotonic count of completed game frames — the emulator's own frame
clock, which is what a capture should pace on rather than wall time or steps/s.

`rom_loaded` is not the same question as `profile`. The profile resolves from the
set's CRC32s while the ROM regions are still being assembled, so a tool that
treats a resolved profile as "the data is there" can read a table of zeros.
Anything that decodes out of the ROM must wait for `rom_loaded`.

`match_replay` is `off`, `armed`, `done` or `unsupported`. `match_replay_frame` is
the `frames` value at which the jump was made (0 before it).

**`match_replay()`**
Arm the jump from attract mode's intro movie straight to its preprogrammed replay
fight. In STF that is Sonic against Bean on stage 1. The jump happens at the first
frame edge where the attract step is the movie and the movie has started. It writes
the movie state a natural boot has when the replay begins, then moves on, so the
fight that follows is the natural boot's, bit for bit. It reads its addresses from
the profile's `quirks.attract_replay`; a profile without one reports
`unsupported`. Returns `match_replay` (`armed`, or `done` if it has already fired).
`tools/match-replay.mjs` grades that fight against MAME.

**`get_registers()`**
Returns a full i960 CPU snapshot:
- `globals`: `g0`–`g14`, `fp` (g15, the frame pointer) — 32-bit hex strings
- `locals`: `pfp` (r0), `sp` (r1), `rip` (r2, saved return address), `r3`–`r15` — 32-bit hex strings
- `sfr`: `ip` (instruction pointer), `ac` (arithmetic controls / condition codes), `pc` (process controls), `tc` (trace controls)
- `fp_regs`: `[fp0, fp1, fp2, fp3]` as floats
- `halted`: bool
- `frame_depth`: int (call stack depth, max 16)

The **condition code** is in `ac` bits `[2:0]`:
- `0x0` = no condition (CC_NO) — used by `chkbit` when bit is 0
- `0x1` = greater than
- `0x2` = equal
- `0x4` = less than
- `0x7` = unordered

### Memory

**`read_memory(addr: str, size: int)`**
Read up to 4096 bytes from the bus. `addr` is a hex string (`"0x00500700"`). Returns `data` as a hex string (`"DEADBEEF..."`). Decoding: every 2 hex chars = 1 byte, little-endian within each 32-bit word.

**`write_memory(addr: str, data: str)`**
Write bytes to the bus. `data` is a hex string with no spaces. Returns `bytes_written`.

**`dump_memory_file(addr: str, size: int, path: str)`**
Copy a bus range straight to a file, up to 32 MB in one call. `read_memory` caps
at 4096 bytes to keep a reply inside the 128 kB buffer, so pulling a 1 MB
texture sheet through it is 256 round trips and twice that again in hex on the
wire; this is one request. The copy is made under the emu mutex, so the range is
one consistent snapshot rather than a run of reads the i960 wrote through the
middle of — which matters for anything the game is still filling.

Returns `bytes`, `nonzero` (how many of them are not zero, useful for telling a
filled region from an empty one without moving it) and `frames`.

**`dump_model(model: int, count: int, path: str)`**
Run the emulator's own index-array polygon decoder over a range of the model
table and write the triangles out. No matrix is applied and the emulator need
not be running — this decodes out of the ROM regions, not out of the running
machine — but `rom_loaded` must be true.

Geometry only: colour, texture tile and UV all depend on what the running game
has uploaded, and a decoder comparison should not be measuring that.

File format, little-endian: `"M2MD"`, u32 version=1, u32 first, u32 count, then
per model a u32 index, a u32 triangle count and that many `9 * f32` triples.
Returns `first`, `count`, `nonempty`, `tris`.

### Execution control

**`emu_run()`** — Start free-running execution (equivalent to F9 / Resume).

**`emu_stop()`** — Pause execution.

**`quit()`** — Ask the process to shut down. Not `exit()`: it raises a flag the run loop
reads, so the board, the A/V server and the netplay session come down in the same order any
other exit uses. The reply arrives first and then the socket closes because the process went
away. A `--headless --no-tray` run has no other way out, which is what this is for; a windowed
run in capture mode is allowed through the close it would otherwise swallow.

**`emu_step(count: int = 1)`** — Step `count` instructions. Emulator must be stopped. Count range: 1–1 000 000.

**`wait_frames(count: int = 1, timeout_ms: int = 30000)`**
Block until the game has advanced `count` frames. Returns `frames` (the clock),
`advanced`, `reached` (bool) and `elapsed_ms`. `reached` is false if the game
stalled or stopped instead, so a driver can tell "slow" from "stopped" rather
than assuming the frames happened.

A stopped emulator produces no frames and the call gives up on one — but only
after four seconds, because the bridge accepts a client well before a 17 MB ROM
set has finished loading and before `--run` has taken effect. Bailing out
immediately there would hand every caller `reached: false` the moment it
connected.

**`wait_for_stop(timeout_ms: int = 30000)`**
Block until the emulator stops (breakpoint hit, CPU halt, or manual pause). Returns:
- `stopped`: bool
- `reason`: `"breakpoint"` | `"halted"` | `"stopped"` | `"timeout"`
- `ip`: hex string of the IP at stop
- `elapsed_ms`: how long it waited

Typical pattern: `emu_run()` → `wait_for_stop()` → `get_registers()`.

**`board_reset()`** — the cold boot a netplay session performs at the barrier,
with no session: re-installs the ROM set and resets both CPUs, the sound board,
the interrupt controller, the input latch, the frame clock and the step count.
The run state is left alone, so a stopped board stays stopped, at the reset
vector. Returns `resets`, the number performed so far. Refused with no ROM set
loaded, and while a netplay session is at the barrier or playing -- there it
would reset one board of two. `tools/grade-reset.mjs` is built on it.

### Breakpoints

**`set_breakpoint(addr: str, label: str = "")`**
Add a breakpoint. The emulator stops when IP reaches this address. `label` is optional and shown in the debug UI.

**`clear_breakpoint(addr: str)`**
Remove all breakpoints at `addr`.

**`enable_breakpoint(addr: str)`**
Re-enable a disabled breakpoint without removing it.

**`disable_breakpoint(addr: str)`**
Mute a breakpoint (keeps it in the list, won't trigger).

**`clear_all_breakpoints()`**
Remove every breakpoint.

**`list_breakpoints()`**
Returns an array of `{addr, label, enabled}` for all active breakpoints.

### Object viewer (screenshots of one model, from any angle)

Chasing a visual artifact through the running game means steering the emulator into the scene
that draws the object and then fighting the game for the camera. The object viewer draws one
model **by itself**, offscreen, against a flat background, from wherever you put the camera —
and takes several angles inside a single host frame, so a sweep around an object is one call.

What it draws is the emulator's own decoder and the emulator's own fill shader, the same ones
the frame uses. An artifact that shows here is an artifact the frame has.

**It needs a windowed emulator.** `--headless` has no renderer and these tools say so rather
than hanging. A minimised window may get no frames from the OS, which looks the same as a
stall; the timeout message names both causes.

**Paths are resolved by the emulator process**, whose working directory is not the caller's.
Pass an absolute path to `objview_shot` or the write fails with `cannot open ... for writing`.

---

**`objview_wait_ready(timeout_ms=60000)`**

Block until the game has built the 3D state the viewer needs. The model table is ROM and
readable the moment a set loads, but what the object is *made of* is not: the texture sheets
are filled by the game's own decompressor and the face palette by its colour setup, both
during boot. A model decoded before then has the right shape with no texels and no colours —
which reads as an artifact, and is not one. In STF that lands as attract mode starts.

This measures the state rather than counting frames, so it holds for other games too. The
decisive field is `saw_3d` -- the board having *drawn* 3D at least once, latched, because by
the time it submits its first object everything a model is made of is up. A weaker "texture
RAM is not all zero" test is not enough: five frames into an STF boot it is 8% full, and a
model decoded there comes back correctly shaped and entirely black. The reply also carries
`tex_pct` and `pal_pct` if you want to watch boot progress.

One thing readiness does **not** cover: face colours come out of palette RAM, which the game
fills per scene. A model whose scene attract has not reached yet draws with the right shape,
the right texels and black faces. If that is what you are looking at, run the game on
(`wait_frames`) rather than hunting a bug -- in STF's attract, Sonic's palette is in by frame
~1800.

Call `emu_run()` first. `ok=False` means the timeout ran out.

**`objview_list(first=0, count=64, nonempty_only=True)`**

How many triangles each model-table entry decodes to. Most of the table is empty in any given
game, and an empty entry looks exactly like a broken one from a screenshot. `count` is capped
at 4096 per call; `table_count` in the reply is the whole table's size.

**`objview_status()`**

The viewer's whole state, the readiness probe and the last decode's result. Every other
objview tool answers with these same fields, so you rarely need to call it on its own.

**`objview_set(...)`** — select the object, place it, aim the camera

Every argument is optional; an omitted one keeps its value, so you can nudge one angle at a
time. The call waits up to `settle_ms` (default 1500) for one render pass, so the reply
carries *this* object's triangle count, bounds and auto-fit distance rather than the previous
object's. `ok=False` means the object did not draw, and `last_error` says why.

| Group | Arguments |
|-------|-----------|
| Object | `model` (model-table index) **or** `capture` (an index from `get_geo_captures`), `use_capture_matrix` |
| Placement | `pos_x/y/z` world units, `rot_x/y/z` degrees (Rz·Ry·Rx), `scale` |
| Camera | `yaw`, `pitch`, `dist`, `fov` (vertical degrees), `autofit`, `fit_margin`, `target_x/y/z` |
| Image | `width`, `height` (32–2048), `bg_r/g/b` (0–1), `wireframe`, `textured`, `cull` (0 none / 1 CW / 2 CCW) |
| UI | `active` (render at all), `window` (open the viewer's panel in the emulator's window) |

- The camera is an **orbit about a target**. At yaw 0 / pitch 0 it stands on +Z looking down
  −Z. A model's own facing is whatever the ROM gave it — in STF a head faces along its X — so
  the angles name where the camera is, not which side of the object you get.
- `autofit` is on by default: it centres the orbit on the model and pulls back far enough to
  hold its bounding sphere, with `fit_margin` loosening (>1) or tightening (<1) the framing.
  Naming `dist` or any `target_*` turns autofit off, because leaving it on would overwrite
  what you just set on the very next pass — which would read as the setting being ignored.
- `use_capture_matrix` places the model exactly as the board did this frame. It **replaces**
  the placement, so `pos`/`rot`/`scale` are not applied while it is on.
- `textured=False` draws flat face colour only, which separates a texturing artifact from a
  geometry one. `wireframe=True` overlays the decoder's edges, which is how you see seams and
  degenerate faces.

**`objview_shot(path, ...)`** — render and write PNGs

`path` must be absolute. A single shot writes exactly that file; for several it is the stem
and the shots land at `<stem>-000.png`, `<stem>-001.png` and so on. Angles come from one of
three spellings, checked in this order:

| Spelling | Angles |
|----------|--------|
| `six=True` | the six camera stations: +Z, +X, −Z, −X, +Y (looking down), −Y (looking up) |
| `count=N` | a turntable from `yaw0` in steps of `yaw_step`; with no step named, the shots spread evenly over a full turn |
| neither | one shot at the viewer's current yaw and pitch |

`N` is capped at 64, and a whole batch renders inside one host frame — six 384×384 angles of a
3400-triangle model take about 5 ms.

Any setting `objview_set` takes may be passed here too, so one call can select the object,
place it and shoot it.

Each shot reports `coverage` (the fraction of the image that is not the background) and `box`
(the pixel rectangle the object drew into). That is enough to tell an off-screen or hair-thin
result from a well-framed one **without opening the file** — worth reading first, because an
image that is all background costs a round trip to discover by eye. Coverage 0 with autofit on
usually means the model decoded to nothing; check `tris` in the same reply.

---

**Workflow — an artifact you can see on screen**

```python
emu_run()
objview_wait_ready()                       # attract has started; textures are in

caps = get_geo_captures()                  # what the board drew this frame
# pick the idx whose model/pos matches the thing that looks wrong

objview_set(window=True, capture=7, use_capture_matrix=True)
# reply carries drawn_model, tris, bmin/bmax — confirm it is the right object

objview_shot(path=r"C:\tmp\susp.png", six=True, width=512, height=512)
# then read the six PNGs; coverage in the reply says which are worth opening
```

**Workflow — sweeping a model from the table**

```python
objview_list(first=3500, count=64)         # find the non-empty entries
objview_shot(path=r"C:\tmp\turn.png", model=3545, count=8,
             pitch0=15, width=384, height=384)   # 8 angles, one full turn
objview_shot(path=r"C:\tmp\wire.png", model=3545, wireframe=True, textured=False)
```

The viewer's panel is also in the emulator's **Debug → Object viewer** menu, with the same
controls and a live preview of the very render target the shots are read out of; `--objview`
(optionally `--objview N`) opens it at boot. Both drive the same state, so an object found by
eye can be handed to a sweep without retyping the numbers.

#### In the browser

The same viewer, the same commands, the same PNGs — in the wasm build. What changes is how
you reach it, because a browser has no TCP bridge to connect an MCP server to and no
filesystem to write a file into.

There are two ways in.

**From a shell, like the desktop bridge.** `tools/web-objview.mjs` drives Chrome or Edge over
the DevTools protocol and writes the PNGs to disk:

```
node tools/web-serve.mjs --rom <merged.zip>            # in one shell
node tools/web-objview.mjs --url "http://localhost:8080/?rom=/dev-rom.zip" \
     --out shots --model 3544 --six --at-frame 1800
```

`--list FIRST:COUNT` lists triangle counts instead of shooting; `--opts '{...}'` passes any
field `objview_set` takes, so nothing needs a flag of its own; `--show` leaves the viewer on
the canvas and screenshots the page; `--headful` runs with a window so you can watch.

**From the page itself**, which is what that script is driving: `window.m2hleObjview`, in the
browser console or over any automation that can evaluate JavaScript.

```js
await m2hleObjview.waitReady();
const r = await m2hleObjview.shot({ model: 3544, six: true, width: 512 });
r.shots[0].url        // a blob: URL to open, or .bytes for the raw PNG
await m2hleObjview.set({ model: 3545, yaw: 200, pitch: 12 });
m2hleObjview.show(true);   // the viewer on the canvas, in place of the game
```

`?objview=3545` in the address bar does the last two: it waits for the game, selects the
model and shows it on the canvas. Drag to orbit, wheel to zoom, Escape to go back to the
game — which never stopped running behind it.

**Three differences worth knowing:**

- **Every call is async.** The thread asking is the thread drawing, so nothing may block: a
  request is armed and the answer collected on a later animation frame. A whole shot batch
  still renders inside one emulator frame, so `await shot({count: 8})` costs a frame or two
  of wall time, not eight.
- **Nothing is written.** There is no filesystem, so each PNG stays in the emulator's heap
  until the next batch replaces it and JavaScript copies it out. `path` is ignored;
  `web-objview.mjs` is what turns the bytes into files.
- **Readiness and frame numbers are not the same question.** `waitReady` returns once the
  board has *drawn* 3D, which is when the texture sheets and the palette are up. But face
  colours come out of palette RAM, which the game fills **per scene**: a model whose scene
  attract has not reached yet draws with the right shape, the right texels and black faces.
  That is why `--at-frame` exists. It is not a web-only trap — the desktop's `wait_frames`
  is the same knob — but it bites here first, because the page starts the board the moment
  the ROM loads and a script can be asking within two seconds. For STF's attract, Sonic's
  palette is in by frame ~1800.

The geometry path is bit-identical to the desktop's: the same six angles of model 3544 came
back with the same coverage and the same pixel boxes on D3D11 and on WebGL2.

### Netplay (RPCN)

The netplay window's buttons, as bridge commands — enough to hold a lobby open,
notice a challenger and accept a match without anybody at the keyboard. All of
them post onto the same mutex-guarded queue the UI uses, so the bridge thread is
one more UI thread as far as `src/net/` is concerned; none of them touch the emu
mutex or a socket.

Sign in **once**, by hand, before scripting anything: the Twitch device flow is
a browser dance that happens once ever, and the login token it yields is stored
in `m2hle_netplay.cfg` beside the executable. Every later `netplay_connect` then
needs no arguments at all.

```
m2hle --rom sfight.zip --run --netplay --net-server rpcn.sonicthefighte.rs --net-twitch
```

**`netplay_status(log: int = 12, rooms: int = 0)`**
Everything the published snapshot holds. `state` is the text
(`off` / `connecting` / `online` / `in a room` / `waiting at the barrier` /
`playing` / `failed`) with `state_num` beside it, plus `room_id` (a **string** —
it is 64-bit), `com_id`, `frame`, `stalls`, `generation`, `seed`,
`desync_frame` (null while the two boards agree), `error`, and a `twitch`
object. `rooms: 1` adds the last search's results; `log` is how many lines of
the emulator's own netplay log to return, with `log_count` beside it so a
poller can tell "nothing happened" from "I missed some".

`peer` is the interesting one:

| field | means |
|---|---|
| `npid` | the challenger's RPCN account name, empty for an empty room |
| `known` | the server has told us their address |
| `heard` | a datagram has actually arrived from them |
| **`ready`** | **they have joined AND pressed Start — this is a challenge** |
| `ready_gen` | the session generation they announced |

`ready` is the whole reason these commands exist. RPCN has no "ready" message;
the barrier releases when both peers announce the same generation, which is what
pressing Start does. A peer who has pressed it while this end has not is
announcing a session we are not in — `lockstep_on_peer_announce` drops exactly
those, so `netplay.h` latches them separately. It is a **freshness window**, not
a flag: a challenger who gives up stops announcing and `ready` goes false about
two seconds later, rather than leaving a challenge standing that nobody is at.

**`netplay_connect(server, port, user, pass, token, fingerprint, twitch, delay, browse_yamp, p2p_port)`**
Sign in. Every field is optional and defaults to whatever is stored, so
`{"cmd":"netplay_connect"}` means "as whoever signed in last". `twitch: 1` runs
the device flow instead — which signs in *and connects itself*, so it replaces
the connect rather than preceding it. Passing `pass` means the password and
clears any stored Twitch token for this attempt.

**`netplay_host(delay, room_pass)`** — take a room. 2 slots; host is always P1.
**`netplay_join(room_id, room_pass)`** — join one. `room_id` is a string.
**`netplay_search(browse_yamp)`** — fill `rooms` in the status.
**`netplay_stop()`** — leave the match, keep the room, so the next challenger
has one to join.
**`netplay_disconnect()`** — give the room back and drop the session.

**`netplay_start()`** — **accept.** Begin (or restart) a lockstepped session.

Two things it is important to have read before calling it:

* **The board is about to cold-boot.** A session starts from power-on on both
  machines, because with no savestates that is the only state two copies are
  certain to share. Anything the bridge had set up — a fight in progress, a
  character written into a fighter record, credits poked into RAM — is gone the
  moment the barrier releases. Getting back to a round is `set_input`'s job:
  coin, START, the select cursor, confirm.

* **`write_memory` is a desync.** While a session is playing, a write changes
  one of the two boards and not the other, which is precisely what the frame
  check exists to catch. Reads are free and unaffected. Inputs are fine and
  need no new command — `set_input` writes `g_input.held`, which is exactly what
  `netplay_sample_local` reads and transmits, so what the bridge presses goes
  out on the wire and comes back applied to both boards.

Halting the board is also a stall on the other machine, and m2-hle2 drops a
session that stalls for fifteen seconds — so no `emu_stop` between frames while
a session is running.

A lobby that holds itself open, in full:

```jsonc
{"cmd":"netplay_connect"}                  // the stored login
{"cmd":"netplay_status"}                   // poll until state == "online"
{"cmd":"netplay_host","delay":2}           // poll until state == "in a room"
{"cmd":"netplay_status"}                   // poll peer.ready
{"cmd":"netplay_start"}                    // when it is true: accept
                                           // both boards reset; play with set_input
{"cmd":"netplay_stop"}                     // match over — the room stays open
```

`flystf/rpcn.py` in the [stf-fly](../stf-fly) sibling is that loop with a fruit
fly behind it.

---

## STF memory map (key addresses)

### Input (written by the UI thread each frame)
| Address | Description |
|---------|-------------|
| `0x00500700` | P1+P2 held buttons bitmask (written every frame) |
| `0x00500704` | P1+P2 momentary buttons (one-shot, OR'd in, cleared after read) |
| `0x0059C388` | P1 credits byte |
| `0x0059C38C` | P2 credits byte |

**Input bitmask layout (held/momentary at 0x500700/0x500704):**
| Bit | P1 action | Bit | P2 action |
|-----|-----------|-----|-----------|
| `0x00002000` | P1 Up | `0x00200000` | P2 Up |
| `0x00001000` | P1 Down | `0x00100000` | P2 Down |
| `0x00008000` | P1 Left | `0x00800000` | P2 Left |
| `0x00004000` | P1 Right | `0x00400000` | P2 Right |
| `0x00000100` | P1 B1 (Z) | `0x00010000` | P2 B1 |
| `0x00000200` | P1 B2 (X) | `0x00020000` | P2 B2 |
| `0x00000400` | P1 B3 (C) | `0x00040000` | P2 B3 |
| `0x00000010` | P1 Start | `0x00000020` | P2 Start |
| `0x00000004` | Service | | |

### HLE hook addresses (all STF-specific)
| Address | Function | What the hook does |
|---------|----------|--------------------|
| `0x00000F3C` | `cop_initialize_l1` | Sets COP-ready bit to unblock boot |
| `0x000074E4` | `CoProcessorErr` | Simulates `ret` to bypass COP self-test hang |
| `0x0004A55C` | `check_timer_4` | Skips timer spin loop |
| `0x0004A58C` | `check_timer_4_spin` | Writes `0x01` to `0x50008C` to unblock |
| `0x00001768` | `interrupt_wait` | Writes to `RAM_BASE` to unblock |
| `0x00011580` | `interrupt_wait_b` | Increments `RAM_BASE` |
| `0x00011610` | `_idle` | Increments `RAM_BASE` |
| `0x00007264` | `_700000_loop` | Zeroes r3 to exit sound-init delay |
| `0x00011A04` | `frame_pace` | Sets `g_frame_done` for 60 Hz pacing |
| `0x000017CC` | `read_sw` | Zeroes held buffer before real input read |

### Memory bus regions (board-level, all Model 2 games)
| Base address | Size | Region |
|-------------|------|--------|
| `0x00000000` | 2 MB | ROM (maincpu) |
| `0x00200000` | 8 MB | RAM |
| `0x00500000` | 4 MB | IO / registers |
| `0x01000000` | 4 MB | TILE VRAM |
| `0x02000000` | 32 MB | MAIN_DATA |
| `0x04000000` | 8 MB | COPRO data |
| `0x05000000` | 8 MB | GEO capture buffer |
| `0x06000000` | 16 MB | XTRA_DATA (STF: mirror of main_data+0x1000000) |

---

## Common debugging workflows

### Inspect state at a known function
```python
set_breakpoint("0x000074E4", "CoProcessorErr")
emu_run()
r = wait_for_stop(timeout_ms=15000)
# r["reason"] should be "breakpoint"
regs = get_registers()
# regs["locals"]["rip"] is the return address
```

### Read the current input state
```python
result = read_memory("0x00500700", 8)
# result["data"] = 8 bytes = held (u32 LE) + momentary (u32 LE)
```

### Inject a coin for P1
```python
# Read current credit count
m = read_memory("0x0059C388", 1)
credits = int(m["data"], 16)
# Write credits + 1
write_memory("0x0059C388", format(credits + 1, "02X"))
```

### Step through a function and watch registers
```python
emu_stop()
for _ in range(20):
    emu_step(1)
    r = get_registers()
    print(r["sfr"]["ip"], r["locals"])
```

### Run until halt (e.g. to catch a crash)
```python
emu_run()
r = wait_for_stop(timeout_ms=60000)
if r["reason"] == "halted":
    get_registers()   # ip points to the fault
    read_memory(r["ip"], 16)  # inspect the instruction stream
```

---

## i960 register conventions

- `g0`–`g7` — global scratch (caller-saved)
- `g8`–`g13` — global callee-saved
- `g14` — return-value register (also used as link register in leaf functions)
- `g15` (`fp`) — frame pointer; points to base of current call frame
- `pfp` (`r0`) — previous frame pointer (saved on `call`, restored on `ret`)
- `sp` (`r1`) — stack pointer (aligned to 64 bytes on call)
- `rip` (`r2`) — return instruction pointer (where `ret` will jump)
- `r3`–`r15` — local scratch within the current frame

**Call/ret**: `call` aligns SP to 64 bytes, zeros new locals, saves pfp/sp/rip. `ret` restores all locals from the frame stack. The emulator maintains `frame_depth` (max 16 deep).

---

## Architecture notes

- **Board-layer bugs**: If something breaks, assume it is a board-level i960 / memory / COP issue affecting all Model 2 games, not STF-specific.
- **HLE hooks fire instead of executing the real function** when `return 0`; when `return 1` the real function executes normally (hook just patched state beforehand).
- **The emu thread runs on a separate thread** — the bridge reads CPU state through a mutex snapshot. Never assume `get_registers()` is cycle-accurate to the exact moment of the call; it returns the last completed snapshot.
- **`wait_for_stop` polls at 10 ms granularity** on the C side. The TCP connection stays open while waiting; don't call other tools concurrently while a `wait_for_stop` is in flight.
