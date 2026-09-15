"""Run sfight from power-on under MAME and capture the sound board: MIDI bytes in,
every SCSP register write, the register reads the 68000 driver acts on, the
interrupt levels it takes, per-frame work RAM and SCSP registers (see
snd-capture.lua), and MAME's own audio output as a WAV.

Run: claude_mame/mcp_server/.venv/Scripts/python.exe snd_capture.py <outprefix> <frames>
  MAME_ROMPATH: a directory holding only the zips (sfight, schamp, segabill).
Writes <outprefix>.bin/.ram.bin/.regs.bin/.json and <outprefix>.wav.
"""
import asyncio
import os
import sys
import time

CLAUDE_MAME = r"C:\Users\bigge\source\repos\ai\claude_mame"
sys.path.insert(0, os.path.join(CLAUDE_MAME, "mcp_server"))
os.environ.setdefault("MAME_EXE_NAME", "mame.exe")
from mame_client import MameBridge  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
LUA = os.path.join(HERE, "snd-capture.lua").replace("\\", "/")
ROMPATH = os.environ.get("MAME_ROMPATH", os.path.join(HERE, "mameroms"))

OUT = os.path.abspath(sys.argv[1]).replace("\\", "/")
FRAMES = int(sys.argv[2])


async def main():
    b = MameBridge()

    async def ev(expr, timeout=120):
        r = await b.call("eval", {"expr": expr}, timeout=timeout)
        if not r.get("ok"):
            raise RuntimeError(f"lua failed: {expr[:80]} -> {r}")
        return r.get("result")

    try:
        print("launching MAME", flush=True)
        await b.launch_mame("sfight", extra_args=[
            "-rompath", ROMPATH, "-sound", "none", "-nothrottle",
            "-window", "-nomax", "-wavwrite", OUT + ".wav",
        ])
        print(await ev(f'dofile("{LUA}")'), flush=True)
        await ev(f"(function() _G.SNDCAP.want = {FRAMES}; return 'ok' end)()")
        print("start:", await ev(f'_G.SNDCAP.start("{OUT}")'), flush=True)
        await b.call("continue")
        t0 = time.time()
        last = -1
        while True:
            await asyncio.sleep(2)
            st = await ev("_G.SNDCAP.state")
            marks = int(await ev("#_G.SNDCAP.marks"))
            nrec = int(await ev("_G.SNDCAP.n"))
            if marks // 120 != last:
                last = marks // 120
                print(f"  t={time.time() - t0:5.0f}s frames={marks} records={nrec} state={st}", flush=True)
            if st == "error":
                raise RuntimeError(await ev("_G.SNDCAP.err"))
            if st == "captured":
                break
            if time.time() - t0 > 7200:
                raise RuntimeError("timed out")
        await b.call("pause")
        print(await ev("_G.SNDCAP.write()", timeout=900), flush=True)
        # a clean exit, so MAME finalises the WAV header
        try:
            await b.call("eval", {"expr": "manager.machine:exit()"}, timeout=10)
        except Exception:
            pass
        for _ in range(120):
            if not b.is_mame_running():
                break
            await asyncio.sleep(0.5)
    finally:
        await b.shutdown_mame()


asyncio.run(main())
