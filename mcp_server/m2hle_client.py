"""Reusable m2-hle bridge: launches the emulator and exposes send().

Usage:
    bridge = M2HleBridge()
    bridge.launch()          # kills any existing instance, starts fresh
    bridge.send({"cmd": "emu_run"})
    ...
    bridge.shutdown()

Default paths assume the script is run from mcp_server/ inside the repo.
"""

import json
import os
import socket
import subprocess
import time

DEFAULT_EXE = os.path.join(os.path.dirname(__file__),
                           "..", "build", "Release", "m2hle.exe")
DEFAULT_ROM = os.path.join(os.path.dirname(__file__),
                           "..", "build", "Release", "sfight.zip")
DEFAULT_PORT = 7172


class M2HleBridge:
    def __init__(self, exe=DEFAULT_EXE, rom=DEFAULT_ROM, port=DEFAULT_PORT):
        self.exe  = os.path.abspath(exe)
        self.rom  = os.path.abspath(rom)
        self.port = port
        self._proc = None
        self._sock = None
        self._buf  = b""

    # ── lifecycle ─────────────────────────────────────────────────────────────

    def launch(self, run=True, wait_secs=30):
        """Kill any existing m2hle, start a fresh one, wait until emu is ready."""
        self._kill_existing()
        cmd = [self.exe, "--mcp", "--mcp-port", str(self.port),
               "--rom", self.rom]
        if run:
            cmd.append("--run")
        self._proc = subprocess.Popen(cmd, cwd=os.path.dirname(self.exe))

        # Phase 1: wait for the TCP port to open.
        deadline = time.monotonic() + wait_secs
        while time.monotonic() < deadline:
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=1)
                s.close()
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise RuntimeError(f"m2hle bridge did not open on port {self.port} "
                               f"within {wait_secs}s")

        self._sock = socket.create_connection(("127.0.0.1", self.port), timeout=30)
        self._buf  = b""

        # Phase 2: poll until the emu thread has initialized (IP becomes non-zero).
        # The ROM loader runs on the sokol init thread after the port opens; this
        # ensures we don't send commands that lock the mutex before it is ready.
        while time.monotonic() < deadline:
            try:
                st = self.send({"cmd": "get_status"})
                if st.get("ip", "0x00000000") != "0x00000000":
                    break
            except OSError:
                pass
            time.sleep(0.2)
        else:
            raise RuntimeError("m2hle emu thread did not start within timeout")

    def shutdown(self):
        """Close socket and terminate emulator."""
        if self._sock:
            try: self._sock.close()
            except OSError: pass
            self._sock = None
        if self._proc:
            self._proc.terminate()
            try: self._proc.wait(timeout=3)
            except subprocess.TimeoutExpired: self._proc.kill()
            self._proc = None

    # ── communication ─────────────────────────────────────────────────────────

    def send(self, cmd: dict) -> dict:
        msg = json.dumps(cmd) + "\n"
        self._sock.sendall(msg.encode())
        while b"\n" not in self._buf:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise EOFError("m2hle bridge connection closed")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return json.loads(line)

    def wait_for_breakpoint(self, timeout_ms=30000) -> dict:
        """Resume execution and block until a breakpoint fires (or timeout)."""
        self.send({"cmd": "emu_run"})
        return self.send({"cmd": "wait_for_stop", "timeout_ms": timeout_ms})

    # ── helpers ───────────────────────────────────────────────────────────────

    def _kill_existing(self):
        """Best-effort: kill any running m2hle process."""
        try:
            subprocess.run(
                ["taskkill", "/F", "/IM", "m2hle.exe"],
                capture_output=True, timeout=3
            )
            time.sleep(0.4)
        except Exception:
            pass
