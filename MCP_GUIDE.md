# m2-hle MCP Server Guide

A new Claude instance reading this can fully operate the m2-hle Sega Model 2 emulator via the MCP tools listed below. No prior context is needed.

---

## What this is

**m2-hle** is a Sega Model 2 arcade board emulator written in C11 with an ImGui debug UI. The first game profile is *Sonic The Fighters* (STF); *Fighting Vipers* and the m2snake homebrew have profiles too (`src/profiles/`). The emulator runs an Intel i960KB CPU at 25 MHz with HLE (high-level emulation) hooks for timing and hardware stubs.

The **MCP bridge** is a local TCP JSON server built into the emulator (`src/ui/mcp_bridge.h`). When launched with `--mcp`, the emulator listens on `127.0.0.1:7172`. The protocol is newline-delimited JSON: one `{"cmd":"...", ...}` object per line in, one reply object per line out, always carrying `ok` (and `error` when it is false). It serves **one client at a time**, one request at a time; a request line is capped at 8 kB and a reply at 128 kB.

The Python MCP server (`mcp_server/server.py`) connects to that port and exposes a subset of the commands as MCP tools: `get_status`, `get_registers`, `read_memory`, `write_memory`, `emu_run`, `emu_stop`, `emu_step`, the six breakpoint tools, `set_break_on_unknown_cop`, `get_cop_diagnostics`, `wait_for_stop`, the five `objview_*` tools, `get_geo_captures`, `wait_frames` and `set_input`. Every other command in this guide is reached by sending its JSON line to the bridge directly (the graders under `tools/` and the netplay examples below do exactly that).

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
The server launches `m2hle.exe --mcp --mcp-port <port> --rom <path> --run` (`--port N` on the server picks the port, default 7172), waits up to 10 s for the bridge port to open, then starts serving tools.

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
| `--mcp-port N` | Use port N instead of 7172. Without `--log`, the session log becomes `m2hle-N.log`, so instances started side by side keep separate logs |
| `--log <path>` | Write the session log here instead of `m2hle.log`; `--log off` writes no file (the log window still has it) |
| `--log-level SPEC` | Drop lines below a level: `warn`, or per channel (the `mem:` / `netplay:` / `sound:` tag a line starts with), e.g. `warn,mem=error,netplay=debug`. Levels are `debug`, `info`, `warn`, `error`, `off`. A dropped line is dropped everywhere: the file, the log window and the MCP log. The file takes 64 MB whatever the level, then only errors (1 MB more) |
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
`profile` (string), `frames` (int), `rom_loaded` (bool), `match_replay`,
`match_replay_frame`, and two objects: `av` (the `--av-port` server's state) and
`overlay` (whether an `--overlay` plugin is loaded and running, its `path`, and
the `swap` block that `overlay_swap` below moves along).

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
the profile's `quirks.attract_replay`; on a profile without one the command
fails (`ok: false`, "this game profile has no attract replay"). Returns
`match_replay` (`armed`, or `done` if it has already fired).
`tools/match-replay.mjs` grades that fight against MAME.

**`get_registers()`**
Returns a full i960 CPU snapshot:
- `globals`: `g0`–`g15` (`g15` is the frame pointer, `fp`) — 32-bit hex strings
- `locals`: `pfp` (r0), `sp` (r1), `rip` (r2, saved return address), `r3`–`r15` — 32-bit hex strings
- `sfr`: `ip` (instruction pointer), `ac` (arithmetic controls / condition codes), `pc` (process controls), `tc` (trace controls)
- `fp_regs`: `[fp0, fp1, fp2, fp3]` as floats
- `halted`: bool
- `frame_depth`: int (call stack depth, max 64 — `FRAME_STACK_DEPTH`)

The **condition code** is in `ac` bits `[2:0]`:
- `0x0` = unordered / none (`CC_NO`) — also what `chkbit` sets when the bit is 0
- `0x1` = greater than
- `0x2` = equal
- `0x4` = less than

(`0x7`, `CC_O` in `constants.h`, is a branch mask — "ordered" — not a code the CPU sets.)

### Memory

**`read_memory(addr: str, size: int)`**
Read up to 4096 bytes from the bus (a larger `size` is clamped). `addr` is a hex string (`"0x00500700"`). Returns `addr` and `data` as a hex string (`"DEADBEEF..."`). Decoding: every 2 hex chars = 1 byte, little-endian within each 32-bit word.

**`write_memory(addr: str, data: str)`**
Write bytes to the bus. `data` is a hex string with no spaces. Returns `bytes_written`.
Both are done under the emu mutex, so a write lands in one piece.

**`dump_memory_file(addr: str, size: int, path: str)`**
Copy a bus range straight to a file, up to 32 MB in one call. `read_memory` caps
at 4096 bytes to keep a reply inside the 128 kB buffer, so pulling a 1 MB
texture sheet through it is 256 round trips and twice that again in hex on the
wire; this is one request. The copy is made under the emu mutex, so the range is
one consistent snapshot rather than a run of reads the i960 wrote through the
middle of — which matters for anything the game is still filling.

Returns `addr`, `bytes`, `nonzero` (how many of them are not zero, useful for telling a
filled region from an empty one without moving it) and `frames`.

**`dump_model(model: int, count: int, path: str)`**
Run the emulator's own index-array polygon decoder over a range of the model
table and write the triangles out. No matrix is applied and the emulator need
not be running — this decodes out of the ROM regions, not out of the running
machine — but `rom_loaded` must be true.

Positions, the UV each corner carries, the texture tile rectangle and the face's
fill flags — all a pure function of the ROM. Which *texels* sit in that tile
depends on what the running game has uploaded, and none of that is written.

File format, little-endian: `"M2MD"`, u32 version=2, u32 first, u32 count, then
per model a u32 index, a u32 triangle count and that many `20 * f32` records:
`(x,y,z)*3`, `(u,v)*3`, tile `x,y,w,h` (`w = 0` untextured), `GEO3D_FACE_*` flags.
`model` (default 0) is the first entry and `count` (default 1) is clamped to the
table. Returns `first`, `count`, `nonempty`, `tris`.

### Execution control

**`emu_run()`** — Start free-running execution (equivalent to F9 / Resume).

**`emu_stop()`** — Pause execution.

**`quit()`** — Ask the process to shut down. Not `exit()`: it raises a flag the run loop
reads, so the board, the A/V server and the netplay session come down in the same order any
other exit uses. The reply arrives first and then the socket closes because the process went
away. A `--headless --no-tray` run has no other way out, which is what this is for; a windowed
run in capture mode is allowed through the close it would otherwise swallow.

**`emu_step(count: int = 1)`** — Step `count` instructions. Emulator must be stopped. Count range: 1–1 000 000. Returns `steps`.

**`wait_frames(count: int = 1, timeout_ms: int = 30000)`**
(The bridge's default is 30000 and its cap 300000; the Python tool passes 10000
unless told otherwise.)
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
Block until the emulator stops (breakpoint hit, watchpoint hit, CPU halt, unknown COP command, or manual pause); `timeout_ms` is capped at 300000. Returns:
- `stopped`: bool
- `reason`: `"timeout"` | `"halted"` | `"watchpoint"` | `"breakpoint"` | `"cop_unknown"` | `"stopped"` (checked in that order)
- `ip`: hex string of the IP at stop
- `elapsed_ms`: how long it waited
- `cop_cmd`, `cop_ip`: the unknown COP command and where it was sent (see `set_break_on_unknown_cop`)
- `wp_addr`, `wp_val`, `wp_ip`, `wp_write`: the last watchpoint hit

Typical pattern: `emu_run()` → `wait_for_stop()` → `get_registers()`.

**`board_reset()`** — the cold boot a netplay session performs at the barrier,
with no session: re-installs the ROM set and resets both CPUs, the sound board,
the interrupt controller, the input latch, the frame clock and the step count.
The run state is left alone, so a stopped board stays stopped, at the reset
vector. Returns `resets`, the number performed so far. Refused with no ROM set
loaded, and while a netplay session is at the barrier or playing -- there it
would reset one board of two. `tools/grade-reset.mjs` is built on it.

### Input

**`set_input(held: str = "0x0")`**
Set the active-high held mask the game's input read is served, in the
`0x500700` bit layout (see the table under "STF memory map"). `"0x1000"` holds
P1 Down, `"0x0"` releases everything. It replaces the whole mask
(`g_input.held`, the same one the window's keyboard drives). Returns `held`. This is the only safe way to drive a
netplay session (see below).

### Breakpoints

**`set_breakpoint(addr: str, label: str = "")`**
Add a breakpoint. The emulator stops when IP reaches this address. `label` is optional and shown in the debug UI. Returns `addr`.

**`clear_breakpoint(addr: str)`**
Remove all breakpoints at `addr`. Returns `removed`.

**`enable_breakpoint(addr: str)`**
Re-enable a disabled breakpoint without removing it. Returns `updated`.

**`disable_breakpoint(addr: str)`**
Mute a breakpoint (keeps it in the list, won't trigger). Returns `updated`.

**`clear_all_breakpoints()`**
Remove every breakpoint. Returns `removed`.

**`list_breakpoints()`**
Returns `breakpoints`, an array of `{addr, label, enabled}` for all active breakpoints.

**`set_watchpoint(addr, size = 1, type = "w", label = "")`** *(bridge only)*
Stop when the i960 touches `[addr, addr+size)`. `type` is `"w"`, `"r"` or `"rw"`.
Returns `addr`, `size`, `slot` (`ok: false` and `slot: -1` when the table is full).
`wait_for_stop` reports the hit as `reason: "watchpoint"` with `wp_addr`, `wp_val`,
`wp_ip` and `wp_write`.

**`clear_watchpoint(addr)`** *(bridge only)* — returns `removed`.
**`clear_all_watchpoints()`** *(bridge only)*.
**`list_watchpoints()`** *(bridge only)* — returns `watchpoints`, an array of
`{lo, hi, w, r, label}`.

**`set_break_on_unknown_cop(enable: bool = True)`**
Stop on the first COP command that has no handler. Clears the previous trigger.
Returns `break_on_unknown_cop`. After `emu_run()`, `wait_for_stop()` reports
`reason: "cop_unknown"` with `cop_cmd` and `cop_ip`.

**`get_cop_diagnostics()`**
COP counters and the unknown-command log: `writes`, `reads`, `transforms`,
`matrix_reads`, `unknown_cmds`, `unknown_unique`, `break_on_unknown`,
`unknown_triggered`, `trigger_cmd`, `trigger_ip`, `cmd_ips` (the last IP to send
each of a handful of common commands) and `unknown_log`, an array of
`{cmd, first_ip, count}` per distinct unknown opcode.

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

### Stream overlay

**`overlay_swap(path, args, title, note, card, announce_s = 3, hold_s = 1.5)`** — put a
new build of the overlay plugin on a live stream without taking it down. `path` must be a
**new file**, because Windows locks the running DLL. The stream shows a **FLY UPDATE** toast for
`announce_s`, then a **PLEASE STAND FLY** card over the game while the old plugin is swapped
for the new one, and the game comes back `hold_s` after the card went up. Returns
`{"ok":true,"queued":true}` at once. It returns `ok:false` with `error` if the file is missing
or a swap is already running, and then nothing shows on the stream. Follow it with
`get_status`: `overlay.swap.state` goes `queued` → `announce` → `standby` → `hold` → `idle`,
then `overlay.swap.last` is `ok`, `rolled_back` (the new file would not load and the old one is
running again) or `failed`. `README.md`, "Putting a new overlay build on a live stream", has
the details. JSON-escape the path: `"C:\\fly\\flyoverlay.dll"`.

### Netplay (RPCN)

The netplay window's buttons, as bridge commands — enough to hold a lobby open,
notice a challenger and accept a match without anybody at the keyboard. All of
them post onto the same mutex-guarded queue the UI uses, so the bridge thread is
one more UI thread as far as `src/net/` is concerned; none of them touch the emu
mutex or a socket.

Sign in **once**, by hand, before scripting anything: the Twitch device flow is
a browser dance that happens once ever, and the login token it yields is stored
in the per-user settings file (`%APPDATA%\m2hle2\netplay.cfg`; `--net-config` names another).
Every copy of the emulator on the machine reads that one file, so every later `netplay_connect`,
from any of them, needs no arguments at all. Do not run the device flow again from another copy:
the server keeps one token per account, and a new one retires the old.

```
m2hle --rom sfight.zip --run --netplay --net-server rpcn.sonicthefighte.rs --net-twitch
```

**`netplay_status(log: int = 12, rooms: int = 0)`**
Everything the published snapshot holds. `state` is the text
(`off` / `connecting` / `online` / `in a room` / `waiting at the barrier` /
`playing` / `failed`) with `state_num` beside it, plus `room_id` (a **string** —
it is 64-bit), `com_id`, `frame`, `stalls`, `generation`, `seed`,
`desync_frame` (null while the two boards agree), `error`, `stage`, `is_host`,
`player`, `room_flags`, and a `twitch` object (`state`, `signed_in`, `npid`,
`user_code`, `uri`, `error`). `rooms: 1` adds `search_pending` and `rooms`, the
last search's results as `{room_id, owner, members, max, password, flags}`
(`flags` bits 6-7 are the build family: 1 is a browser-build room, which this
build can join now that cross-play is on); `log` is how many lines of
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
| `addr` | the address the server gave for them |

`ready` is the whole reason these commands exist. RPCN has no "ready" message;
the barrier releases when both peers announce the same generation, which is what
pressing Start does. A peer who has pressed it while this end has not is
announcing a session we are not in — `lockstep_on_peer_announce` drops exactly
those, so `netplay.h` latches them separately. It is a **freshness window**, not
a flag: a challenger who gives up stops announcing and `ready` goes false about
two seconds later, rather than leaving a challenge standing that nobody is at.

**`netplay_connect(server, port, user, pass, token, fingerprint, twitch, delay, browse_yamp, p2p_port)`**
Sign in. Every field is optional and defaults to whatever is stored, so
`{"cmd":"netplay_connect"}` means "as whoever signed in last". It fails with no
server (none passed or stored) or, without `twitch`, with no account. `twitch: 1`
signs in with Twitch instead: with a good token already stored that is an
ordinary login with the token, and only without one does it run the device
flow — which signs in *and connects itself*, so it replaces the connect rather
than preceding it. Passing `pass` logs in with the password; the stored Twitch
token is kept, not cleared.

Every command from here down answers at once with `queued` (the verb) and the
current `state`; poll `netplay_status` for the result.

**`netplay_host(delay, room_pass, max_players)`** — take a room for 2..8 (default 2).
In a room of more than two, two fight and the rest wait in line and watch; the
winner stays on and the loser goes to the back (`net/room.h`). The room's owner
starts the first match once every player has pressed `netplay_start`, and after
that the room rolls on by itself on a countdown.
**`netplay_join(room_id, room_pass)`** — join one. `room_id` is a string.
Straight after sign-in, both wait until RPCN's signaling helper has answered
(at most 4 s; the log says "waiting for the server to learn this machine's
address"), because a room copies its members' addresses once, when it is
taken. A script that hosts the moment `state` reads `online` is expected to see
that line.
**`netplay_search(browse_yamp)`** — fill `rooms` in the status.
**`netplay_stop()`** — leave the match, keep the room, so the next challenger
has one to join.
**`netplay_disconnect()`** — give the room back and drop the session.
**`netplay_leave()`** — leave the room and stay signed in.
**`netplay_entry(entry)`** — 0 either side, 1 "1P Entry", 2 "2P Entry": jump the
line for that side.
**`netplay_watch(watch)`** — 1 sits out (never picked to fight), 0 comes back.
**`netplay_force_start()`** — the room's owner only: start the next match now.

`netplay_status` carries a `room` object: `phase` ("lobby"/"match"), `match`,
`fighters` (member ids on 1P and 2P), `last_result` (0 = 1P won), `auto_start_s`,
`max`, `me`, and `members` in line order, each with `id`, `npid`, `line`,
`side` (0/1, -1 when not fighting), `ready`, `watch`, `entry`, `games`, `wins`,
`points` and whether we hear them. `state` is "watching" while this board runs
somebody else's match; `player` is then 2.

**`netplay_start()`** — **accept.** Begin (or restart) a lockstepped session.
Refused until this end is in a room.

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

### Captures and diagnostics

`get_geo_captures` is an MCP tool; the rest are bridge-only JSON commands, used
mostly by the graders under `tools/`. Paths are resolved by the emulator
process, so pass absolute ones.

**`get_geo_captures()`**
The models the board drew in the last frame: `count` and `captures`, each with
`idx` (what `objview_set`'s `capture` takes), `model`, `mesh`, `pos`, `ang`,
`xyz`, per-column `scale`, `up`, the clip window (`clip`, `cx`, `cy`, `cw`, `ch`),
`bone`, `vs`, `win`, `vp`, `gp`, `tpa`, `tha`, `matptr` and `m`, the 12-word matrix.

**`capture_dl(path, frames = 60, probes, max_words, timeout_ms = 120000, lo, hi, tgp, slots, unit, blocks, cop)`**
Record every write to the geometry processor and the coprocessor for `frames`
whole frames (capped at 3600), in the explorer toolkit's MAME capture format:
`<path>.bin` (u32 address, u32 value per write) and `<path>.json` (`words`,
`frames`, `overflow`, `probes`, `marks`). `probes` is `"hexaddr:size,..."` read at
each frame edge; `lo`/`hi` narrow the recorded window; `tgp:1`, `slots:1`,
`unit:1` and `blocks:"hexaddr:hexlen,..."` add `<path>.tgp.bin`, `.slots.bin`,
`.unit.bin` and `.blocks.bin` per mark; `cop:1` records the coprocessor
conversation in a MAME SHARC-side capture's format with `.bufram.bin` and
`.dm.bin` beside it (`tests/cop_replay`). Blocks until done. Returns `words`,
`frames`, `complete`, `overflow`.

**`capture_snd(path, frames = 600, async = 0, timeout_ms = 600000)`**
The sound board's side of the next `frames` game frames in
`tools/mame/snd-capture.lua`'s format: `<path>.bin`, `.ram.bin`, `.regs.bin` and
the `.json` index. With `async: 1` it only arms (returns `armed`), so a driver
can arm before `emu_run` and catch power-on; **`capture_snd_finish(timeout_ms)`**
then waits for it and writes the index. Returns `records`, `frames`.

**`sound_codes(since = 0)`** — every command the i960 has sent the sound board,
oldest first, framed, from command number `since` on (`codes`, with `next` to pass
as `since` to read on and `lost` for any that left the ring). `sent` and `taken`
are UART bytes the i960 wrote and bytes the 68000 read back out of the SCSP's MIDI
buffer: a gap that stays is bytes lost. `queue_hi` is the high-water mark of the
ROM's command queue, and `midi_hi` / `midi_holds` / `midi_drops` the MIDI buffer's.

**`prof(on = 1)`**, **`prof_dump(path)`** — the i960 address profiler
(`tools/prof-state.mjs`): `prof` arms it (clearing it) or disarms it with `on: 0`,
and `prof_dump` writes `addr,count` to `path` with a `.frames.csv` companion. Only
an `M2HLE_PROFILE` build counts anything; any other answers `ok: false`.

**`dump_geo_list(path)`** — the GEO display list the renderer walks, as last
published: u32 read pointer, u32 publish count, u16 H-sync, u16 V-sync, then
bufferram's words. Returns `read_start`, `seq`, `hsync`, `vsync`.

**`dump_geo_stream()`** — this frame's captured COP command stream inline:
`total` and `cmds`, each `{c, a}` (command word and up to 8 arguments).

**`set_geo_isolate(index = -1, from, to, dump_tex)`** — draw only captured
object `index` (-1 for all), or only captures in `[from, to]` (`to < from` turns
the range off). Returns `isolate`.

**`set_camera(cam_x, cam_y, cam_z, rot_x, rot_y, fov, test, lines_only, zsort, zrecede)`**
Live-tune the 3D renderer's camera and switches; values travel as strings and
an omitted one keeps its value. `zsort: 0` turns off the board's polygon z-sort.
Returns `cam`, `rot`, `fov`, `lines`, `tris`, `test`.

**`set_shadow_floor(y)`** — the shadow floor height. Returns `shadow_floor_y`.

**`dump_bones()`** — the current position and a four-slot summary of P1's bone
slots (`rot_cache_T`, `tgp_bone_T`, `rot_cache_R`), at three decimals.

**`dump_tgp()`** — the whole 32-slot bone table (`tgp`, P1 on 0..15, P2 on
16..31, each a column-major 3x4) with the current `pos` and `rot`, at full
precision for differencing.

**`cop_exec(words, reset = 0)`** — hand the coprocessor a stream of 32-bit
words (`words`: 8 hex chars each, no spaces) through the i960's own MMIO path,
argument counting and all; `reset: 1` clears COP and SHARC state first. Read the
result back with `dump_tgp`. Returns `words` (how many went in).

**`dump_face_uv()`** / **`dump_tex_stats()`** — texture-decode debug counters
from the renderer.

**`sound_status()`** — the sound board at a glance: the 68000's `m68k_pc` /
`m68k_sr` / `cycles`, `samples`, `irqs`, the SCSP interrupt state (`scieb`,
`scipd`, `lines`, `levels`, `timers`), the `keyed` and `active` slot masks,
`dsp_steps`, the host ring (`out_fill`, `out_dropped`) and the MIDI input
(`midi_writes`, `midi_fifo`, `midi_drops`, `midi_hi`, `midi_drains`).

**`snd_watch(on)`** — the streaming watchdog: `on: 1` arms it and clears the
counters, `on: 0` disarms, no `on` reads it. `late_up` of `refills` is the
headline; `by_slot` is `[refills, late, worst]` per slot and `events` the first
few late refills with the 68000 PC that wrote them.

**`reset_sound(restart = 1, midi)`** — reboot the 68000 and SCSP, and/or push
`midi` (hex, 2 chars a byte) at the MIDI input; `restart: 0` just sends the
bytes. The recovery path for a driver that has gone silent. Returns `restarted`,
`midi_bytes`, `m68k_pc`, `midi_drops`, `midi_hi`.

**`dump_midi_log()`** — the bytes the i960 sent the sound board: `count` and
`bytes`, each `{v, ip}`.

**`read_wave(addr, len)`** / **`read_comm(addr, len)`** — up to 256 bytes of
sound RAM, or of the SCSP registers (no read side effects), as a decimal array `b`.

---

## STF memory map (key addresses)

### Input (built by the game's own `read_sw` from the I/O ports at `0x01C00000`)
| Address | Description |
|---------|-------------|
| `0x00500700` | P1+P2 held buttons bitmask (rebuilt every vblank) |
| `0x00500704` | P1+P2 momentary buttons |
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
| `0x00000001` | P1 Coin | `0x00000002` | P2 Coin |
| `0x00000004` | Service | | |

### HLE hook addresses (all STF-specific)
| Address | Function | What the hook does |
|---------|----------|--------------------|
| `0x00000F3C` | `cop_initialize_l1` | Sets COP-ready bit to unblock boot |
| `0x0004A55C` | `check_timer_4` | Skips timer spin loop (returns 0) |
| `0x0004A58C` | `check_timer_4_spin` | Writes `0x01` to `0x50008C` to unblock |
| `0x00001768` | `interrupt_wait` | Runs `VsyncScr` (`0x0C40`), then skips the spin loop |
| `0x00011580` | `interrupt_wait_b` | Clears `RAM_BASE` so `_idle`'s hook is reached |
| `0x00011610` | `_idle` | Runs `VsyncScr` on first entry, then lets the loop exit |
| `0x00007264` | `_700000_loop` | Zeroes r3 to exit sound-init delay |
| `0x00011A04` | `frame_pace` | Sets `g_frame_done` for 60 Hz pacing |
| `0x000077F8` | `co_processor_error_hang` | Halts the CPU and logs the COP self-test error code |

There is deliberately no `read_sw` (`0x17CC`) hook: inputs reach the game through
its I/O ports. These are the boot and pacing hooks; the table in
`src/profiles/sfight.h` (`SFIGHT_BASE_HOOKS`) also holds the region and DAMAGE
defaults (`0x62688`, `0x62674`), the versus result (`0xDC3C`), the VS rematch
(`0xE584`), the attract replay's stage (`0x941C`) and the PS3 cross-play hooks
(`xplay_*`), and `sfight_console.h` adds the Console profile's.

### Memory bus regions (board-level, all Model 2 games)
| Base address | Size | Region |
|-------------|------|--------|
| `0x00000000` | loaded size (STF 2 MB) | ROM (maincpu) |
| `0x00200000` | ~830 KB | RAM2 |
| `0x00500000` | 1 MB | RAM (work RAM; the input words above live here) |
| `0x00880000` | 128 KB | COPROGRAM (the COP FIFO) |
| `0x00900000` | 128 KB | BUFF_RAM (bufferram, the GEO display list) |
| `0x01000000` | 512 KB | TILE (64 KB tile RAM, mirrored at `0x01010000`) |
| `0x01800000` | 16 KB | PALETTE |
| `0x01C00000` | 0x44 bytes | IO (input ports) |
| `0x02000000` | 32 MB | MAIN_DATA |
| `0x06000000` | 16 MB | XTRA_DATA (STF: mirror of main_data+0x1000000) |
| `0x11000000` / `0x11200000` | 1 MB each | TEXRAM0 / TEXRAM1 |

The full, ordered table is `mem_init` in `src/board/memory.h`.

---

## Common debugging workflows

### Inspect state at a known function
```python
set_breakpoint("0x00011A04", "variable_diff_calc")   # STF: runs once per game frame
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

**Call/ret**: `call` aligns SP to 64 bytes, zeros new locals, saves pfp/sp/rip. `ret` restores all locals from the frame stack. The emulator maintains `frame_depth` (max 64 deep).

---

## Architecture notes

- **Board-layer bugs**: If something breaks, assume it is a board-level i960 / memory / COP issue affecting all Model 2 games, not STF-specific.
- **HLE hooks fire instead of executing the real function** when `return 0`; when `return 1` the real function executes normally (hook just patched state beforehand).
- **The emu thread runs on a separate thread** — the bridge reads CPU state through a mutex snapshot. Never assume `get_registers()` is cycle-accurate to the exact moment of the call; it returns the last completed snapshot.
- **`wait_for_stop` polls at 10 ms granularity** on the C side. The TCP connection stays open while waiting; don't call other tools concurrently while a `wait_for_stop` is in flight.
