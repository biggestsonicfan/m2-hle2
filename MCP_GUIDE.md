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

ROM set: MAME `sfight.zip` (clone of `schamp.zip`). The emulator looks for `schamp.zip` in the same directory as `sfight.zip` for shared files.

---

## MCP tools reference

### Status and registers

**`get_status()`**
Returns: `running` (bool), `halted` (bool), `ip` (hex string), `steps_per_second` (int), `profile` (string).

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

### Execution control

**`emu_run()`** — Start free-running execution (equivalent to F9 / Resume).

**`emu_stop()`** — Pause execution.

**`emu_step(count: int = 1)`** — Step `count` instructions. Emulator must be stopped. Count range: 1–1 000 000.

**`wait_for_stop(timeout_ms: int = 30000)`**
Block until the emulator stops (breakpoint hit, CPU halt, or manual pause). Returns:
- `stopped`: bool
- `reason`: `"breakpoint"` | `"halted"` | `"stopped"` | `"timeout"`
- `ip`: hex string of the IP at stop
- `elapsed_ms`: how long it waited

Typical pattern: `emu_run()` → `wait_for_stop()` → `get_registers()`.

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
