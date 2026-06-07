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


# ---- Entry point -----------------------------------------------------------

if __name__ == "__main__":
    if _args.launch:
        _launch_emulator()
    mcp.run()
