"""
m2-hle MCP server — bridges Claude to the running emulator via TCP.

Typical usage (launch emulator automatically):
    .venv\\Scripts\\python server.py --launch --exe ..\\build\\Release\\m2hle.exe --rom sfight.zip [--run]

Or connect to an already-running emulator:
    .venv\\Scripts\\python server.py [--port 7172]

The emulator must have been started with --mcp for the bridge to be active.
"""

import argparse
import json
import socket
import subprocess
import sys
import threading
import time
from typing import Optional

from mcp.server.fastmcp import FastMCP

# ---- CLI args (parsed at module load so they're available before mcp.run()) -

_parser = argparse.ArgumentParser(add_help=False)
_parser.add_argument("--launch",  action="store_true",
                     help="Launch the emulator as a subprocess")
_parser.add_argument("--exe",     default=r"..\build\Release\m2hle.exe",
                     help="Path to m2hle.exe (only used with --launch)")
_parser.add_argument("--rom",     default="",
                     help="ROM zip path passed to --rom (only used with --launch)")
_parser.add_argument("--run",     action="store_true",
                     help="Pass --run to emulator so it starts executing immediately")
_parser.add_argument("--port",    type=int, default=7172,
                     help="MCP bridge TCP port (default 7172)")
_args, _unknown = _parser.parse_known_args()

EMU_HOST = "127.0.0.1"
EMU_PORT = _args.port

# ---- Emulator subprocess management ----------------------------------------

_emu_proc: Optional[subprocess.Popen] = None


def _launch_emulator() -> None:
    global _emu_proc
    cmd = [_args.exe, "--mcp", "--mcp-port", str(EMU_PORT)]
    if _args.rom:
        cmd += ["--rom", _args.rom]
    if _args.run:
        cmd.append("--run")
    print(f"[m2-hle] launching: {' '.join(cmd)}", file=sys.stderr)
    _emu_proc = subprocess.Popen(cmd)

    # Poll until the bridge port is open (up to 10 s)
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        try:
            s = socket.create_connection((EMU_HOST, EMU_PORT), timeout=0.5)
            s.close()
            print(f"[m2-hle] bridge ready on port {EMU_PORT}", file=sys.stderr)
            return
        except OSError:
            time.sleep(0.2)
    raise RuntimeError(f"m2hle.exe bridge did not open on port {EMU_PORT} within 10 s")


# ---- Emulator connection ----------------------------------------------------

_lock = threading.Lock()
_sock:   Optional[socket.socket]    = None
_fh:     Optional[socket.SocketIO]  = None


def _connect() -> None:
    global _sock, _fh
    if _sock is not None:
        return
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect((EMU_HOST, EMU_PORT))
    s.settimeout(None)
    _sock = s
    _fh   = s.makefile("r", encoding="utf-8")


def _send(cmd: dict) -> dict:
    """Send a JSON command and return the parsed response. Thread-safe."""
    with _lock:
        _connect()
        assert _sock is not None and _fh is not None
        _sock.sendall((json.dumps(cmd) + "\n").encode())
        line = _fh.readline()
        if not line:
            raise ConnectionResetError("emulator closed connection")
        return json.loads(line)


# ---- MCP server ------------------------------------------------------------

mcp = FastMCP("m2-hle")


@mcp.tool()
def get_status() -> dict:
    """Return emulator run state: running/stopped/halted, current IP, steps/sec, active profile."""
    return _send({"cmd": "get_status"})


@mcp.tool()
def get_registers() -> dict:
    """Return a full snapshot of all i960 CPU registers: globals (g0-g15/fp),
    locals (pfp/sp/rip/r3-r15), SFRs (ip/ac/pc/tc), FP regs, frame depth."""
    return _send({"cmd": "get_registers"})


@mcp.tool()
def read_memory(addr: str, size: int) -> dict:
    """Read `size` bytes (max 4096) from the emulator's memory bus at `addr`.
    `addr` is a hex string like '0x00500000'.
    Returns hex-encoded bytes in 'data'."""
    return _send({"cmd": "read_memory", "addr": addr, "size": size})


@mcp.tool()
def write_memory(addr: str, data: str) -> dict:
    """Write bytes to the emulator's memory bus.
    `addr` is a hex string like '0x00500000'.
    `data` is a hex string like 'DEADBEEF' (pairs of hex digits, no spaces)."""
    return _send({"cmd": "write_memory", "addr": addr, "data": data})


@mcp.tool()
def emu_run() -> dict:
    """Start the emulator running freely (equivalent to pressing F9 / Resume)."""
    return _send({"cmd": "emu_run"})


@mcp.tool()
def emu_stop() -> dict:
    """Pause the emulator (equivalent to pressing F9 while running)."""
    return _send({"cmd": "emu_stop"})


@mcp.tool()
def emu_step(count: int = 1) -> dict:
    """Single-step the emulator `count` instructions (1–1 000 000).
    The emulator must be stopped (not running) to step."""
    return _send({"cmd": "emu_step", "count": count})


@mcp.tool()
def set_breakpoint(addr: str, label: str = "") -> dict:
    """Add a breakpoint at `addr` (hex string like '0x00074E4').
    Optional `label` is shown in the Breakpoints debug window."""
    return _send({"cmd": "set_breakpoint", "addr": addr, "label": label})


@mcp.tool()
def clear_breakpoint(addr: str) -> dict:
    """Remove all breakpoints at `addr` (hex string like '0x00074E4')."""
    return _send({"cmd": "clear_breakpoint", "addr": addr})


@mcp.tool()
def enable_breakpoint(addr: str) -> dict:
    """Re-enable a previously disabled breakpoint at `addr` without removing it."""
    return _send({"cmd": "enable_breakpoint", "addr": addr})


@mcp.tool()
def disable_breakpoint(addr: str) -> dict:
    """Disable a breakpoint at `addr` (keeps it in the list but won't trigger)."""
    return _send({"cmd": "disable_breakpoint", "addr": addr})


@mcp.tool()
def clear_all_breakpoints() -> dict:
    """Remove every breakpoint from the list."""
    return _send({"cmd": "clear_all_breakpoints"})


@mcp.tool()
def list_breakpoints() -> dict:
    """List all active breakpoints with their addresses, labels, and enabled state."""
    return _send({"cmd": "list_breakpoints"})


@mcp.tool()
def set_break_on_unknown_cop(enable: bool = True) -> dict:
    """Enable or disable breaking on the first unknown COP command.
    When enabled, the emulator stops as soon as it executes a COP opcode
    that has no handler — use wait_for_stop() after emu_run() to catch it.
    The response includes the triggering cmd and IP once stopped."""
    return _send({"cmd": "set_break_on_unknown_cop", "enable": 1 if enable else 0})


@mcp.tool()
def get_cop_diagnostics() -> dict:
    """Return COP coprocessor diagnostics:
    - Activity counters: writes, reads, transforms (0x14802929), matrix_reads (0x02800505)
    - unknown_cmds: total unknown command executions; unknown_unique: distinct opcodes
    - break_on_unknown / unknown_triggered / trigger_cmd / trigger_ip
    - unknown_log: list of {cmd, first_ip, count} for every distinct unknown opcode seen
      this session, sorted by insertion order (first encountered first).
    Use this to find unimplemented COP commands and their call sites."""
    return _send({"cmd": "get_cop_diagnostics"})


@mcp.tool()
def wait_for_stop(timeout_ms: int = 30000) -> dict:
    """Block until the emulator stops running (breakpoint hit, halt, or manual pause),
    or until `timeout_ms` elapses (max 300 000 ms / 5 min).
    Returns: stopped (bool), reason ('breakpoint'|'halted'|'stopped'|'timeout'),
    ip (hex string), elapsed_ms.
    Typical use: call emu_run(), set a breakpoint, then call wait_for_stop()."""
    return _send({"cmd": "wait_for_stop", "timeout_ms": timeout_ms})


# ---- Object viewer ----------------------------------------------------------
#
# The debug object viewer draws one model on its own, offscreen, from a camera
# these tools place, and writes PNGs. It needs a WINDOWED emulator: --headless
# has no renderer, and these tools say so rather than hanging.


@mcp.tool()
def objview_status() -> dict:
    """Report the object viewer's whole state and whether it can be used yet.

    Key fields: `ready` (the game has built its 3D state — see objview_wait_ready),
    `gpu_readback` (this build can take screenshots at all), `window` (the
    viewer's panel is open in the UI), `models` (entries in the model table),
    `captures` (models the board drew in the last frame), plus every setting
    objview_set takes and the last decode's `tris`, `lines`, `bmin`/`bmax`,
    `dist`, `target`, `eye`, `last_ok` and `last_error`."""
    return _send({"cmd": "objview_status"})


@mcp.tool()
def objview_wait_ready(timeout_ms: int = 60000) -> dict:
    """Block until the game has initialised the 3D state the viewer needs.

    The model table is ROM and readable from the moment a set loads, but the
    texture sheets and the face palette are filled by the game's own boot code.
    A model decoded before then has the right shape with no texels and no
    colours, which reads as an artifact and is not one. In STF this lands as
    attract mode starts. Call emu_run() first; returns ok=False on timeout."""
    return _send({"cmd": "objview_wait_ready", "timeout_ms": timeout_ms})


@mcp.tool()
def objview_list(first: int = 0, count: int = 64, nonempty_only: bool = True) -> dict:
    """List how many triangles each model-table entry decodes to, over a range.

    Most of the table is empty in any given game, and an empty entry looks
    exactly like a broken one from a screenshot — use this to find the models
    worth looking at. `count` is capped at 4096 per call."""
    return _send({"cmd": "objview_list", "first": first, "count": count,
                  "nonempty_only": 1 if nonempty_only else 0})


@mcp.tool()
def objview_set(
    active: Optional[bool] = None,
    window: Optional[bool] = None,
    model: Optional[int] = None,
    capture: Optional[int] = None,
    use_capture_matrix: Optional[bool] = None,
    pos_x: Optional[float] = None,
    pos_y: Optional[float] = None,
    pos_z: Optional[float] = None,
    rot_x: Optional[float] = None,
    rot_y: Optional[float] = None,
    rot_z: Optional[float] = None,
    scale: Optional[float] = None,
    yaw: Optional[float] = None,
    pitch: Optional[float] = None,
    dist: Optional[float] = None,
    fov: Optional[float] = None,
    autofit: Optional[bool] = None,
    fit_margin: Optional[float] = None,
    target_x: Optional[float] = None,
    target_y: Optional[float] = None,
    target_z: Optional[float] = None,
    width: Optional[int] = None,
    height: Optional[int] = None,
    bg_r: Optional[float] = None,
    bg_g: Optional[float] = None,
    bg_b: Optional[float] = None,
    wireframe: Optional[bool] = None,
    textured: Optional[bool] = None,
    cull: Optional[int] = None,
    settle_ms: int = 1500,
) -> dict:
    """Select the object, place it in 3D space and aim the camera. Every
    argument is optional; an omitted one keeps its current value.

    OBJECT — `model` picks a model-table index; `capture` picks one of the
    models the board drew this frame (see get_geo_captures) and, with
    `use_capture_matrix`, places it exactly as the board did, in which case
    pos/rot/scale are not applied. Setting either one selects that source.

    PLACEMENT — `pos_*` in world units, `rot_*` in degrees (Rz then Ry then Rx),
    `scale` uniform.

    CAMERA — an orbit about a target: `yaw` and `pitch` in degrees, `dist` in
    world units, `fov` vertical degrees. At yaw 0 / pitch 0 the camera stands on
    +Z looking down -Z; a model's own facing is whatever the ROM gave it.
    `autofit` (on by default) centres the orbit on the model and pulls back far
    enough to hold its bounding sphere, `fit_margin` loosening or tightening
    that. Naming `dist` or any `target_*` turns autofit off, since leaving it on
    would overwrite what you just set.

    IMAGE — `width`/`height` (32..2048), `bg_*` the clear colour 0..1,
    `wireframe` overlays the decoder's edges, `textured` off draws flat face
    colour only (which separates a texturing artifact from a geometry one),
    `cull` is 0 none / 1 CW front / 2 CCW front.

    UI — `window` opens or closes the viewer's panel in the emulator's window,
    so whoever is at the machine sees the object being inspected. `active`
    switches the viewer on.

    The call waits up to `settle_ms` for one render pass, so the reply carries
    this object's triangle count, bounds and auto-fit distance rather than the
    previous object's. ok=False means the object did not draw; `last_error`
    says why."""
    cmd: dict = {"cmd": "objview_set", "settle_ms": settle_ms}
    for name, value in (
        ("active", active), ("window", window), ("model", model),
        ("capture", capture), ("use_capture_matrix", use_capture_matrix),
        ("pos_x", pos_x), ("pos_y", pos_y), ("pos_z", pos_z),
        ("rot_x", rot_x), ("rot_y", rot_y), ("rot_z", rot_z), ("scale", scale),
        ("yaw", yaw), ("pitch", pitch), ("dist", dist), ("fov", fov),
        ("autofit", autofit), ("fit_margin", fit_margin),
        ("target_x", target_x), ("target_y", target_y), ("target_z", target_z),
        ("width", width), ("height", height),
        ("bg_r", bg_r), ("bg_g", bg_g), ("bg_b", bg_b),
        ("wireframe", wireframe), ("textured", textured), ("cull", cull),
    ):
        if value is None:
            continue
        cmd[name] = int(value) if isinstance(value, bool) else value
    return _send(cmd)


@mcp.tool()
def objview_shot(
    path: str,
    six: bool = False,
    count: int = 1,
    yaw0: Optional[float] = None,
    pitch0: Optional[float] = None,
    yaw_step: Optional[float] = None,
    pitch_step: Optional[float] = None,
    model: Optional[int] = None,
    capture: Optional[int] = None,
    use_capture_matrix: Optional[bool] = None,
    width: Optional[int] = None,
    height: Optional[int] = None,
    wireframe: Optional[bool] = None,
    textured: Optional[bool] = None,
    cull: Optional[int] = None,
    fov: Optional[float] = None,
    autofit: Optional[bool] = None,
    fit_margin: Optional[float] = None,
    dist: Optional[float] = None,
    timeout_ms: int = 30000,
) -> dict:
    """Render the object from one or more angles and write a PNG per angle.

    `path` MUST BE ABSOLUTE: it is resolved by the emulator process, whose
    working directory is not yours. A single shot writes exactly that file; for
    several it is the stem and the shots land at <stem>-000.png, <stem>-001.png
    and so on.

    ANGLES, in the order they are checked:
      six=True          six camera stations: +Z, +X, -Z, -X, +Y (looking down)
                        and -Y (looking up)
      count=N           a turntable from yaw0 in steps of yaw_step; with no
                        step named the shots spread evenly over a full turn
      neither           one shot at the viewer's current yaw and pitch
    N is capped at 64, and a whole batch renders inside one host frame.

    Any setting objview_set takes may be passed here too, so one call can
    select the object, place it and shoot it.

    Each shot reports `coverage` (the fraction of the image that is not the
    background) and `box` (the pixel rectangle the object drew into) — enough
    to tell an off-screen or hair-thin result from a well-framed one without
    opening the file. Coverage 0 with autofit on usually means the model
    decoded to nothing; check `tris` in the reply."""
    cmd: dict = {"cmd": "objview_shot", "path": path, "count": count,
                 "timeout_ms": timeout_ms}
    if six:
        cmd["six"] = 1
    for name, value in (
        ("yaw0", yaw0), ("pitch0", pitch0),
        ("yaw_step", yaw_step), ("pitch_step", pitch_step),
        ("model", model), ("capture", capture),
        ("use_capture_matrix", use_capture_matrix),
        ("width", width), ("height", height),
        ("wireframe", wireframe), ("textured", textured), ("cull", cull),
        ("fov", fov), ("autofit", autofit), ("fit_margin", fit_margin),
        ("dist", dist),
    ):
        if value is None:
            continue
        cmd[name] = int(value) if isinstance(value, bool) else value
    return _send(cmd)


@mcp.tool()
def get_geo_captures() -> dict:
    """List the models the board drew in the last frame, with the matrix, the
    position and the clip window each was given. `idx` is what objview_set's
    `capture` takes — the route from "that object on screen looks wrong" to
    inspecting it on its own."""
    return _send({"cmd": "get_geo_captures"})


@mcp.tool()
def wait_frames(count: int = 1, timeout_ms: int = 10000) -> dict:
    """Block until `count` more game frames have completed, or the timeout.
    Use it to let the game reach the scene that draws the object you want."""
    return _send({"cmd": "wait_frames", "count": count, "timeout_ms": timeout_ms})


@mcp.tool()
def set_input(held: str = "0x0") -> dict:
    """Set the held input mask (hex string, the 0x500700 bit layout) that the
    game's input read serves. e.g. '0x1000' holds P1 DOWN, '0x0' releases."""
    return _send({"cmd": "set_input", "held": held})


# ---- Entry point -----------------------------------------------------------

if __name__ == "__main__":
    if _args.launch:
        _launch_emulator()
    mcp.run()
