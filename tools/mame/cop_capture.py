"""Run sfight's attract mode under MAME and capture the coprocessor firmware's
side of its FIFOs, matrix snapshots, per-frame probes, both fighters' TGP slots
and the unit-matrix cache (see cop-capture.lua). tests/cop_replay.c replays it.

Run: claude_mame/mcp_server/.venv/Scripts/python.exe cop_capture.py <outprefix> <from> <frames> <probes>
  <probes>: "hexaddr:size,..." — node -e "import('./tools/lib/dl.mjs').then(m=>console.log(m.probeListSpec(m.SCENE_PROBES)))"
  MAME_ROMPATH: a directory holding only the zips (sfight, schamp, segabill).
  MAME has to run -nodrc: the capture reads the SHARC's PC in a read tap.
"""
import asyncio
import json
import os
import sys
import time

CLAUDE_MAME = r"C:\Users\bigge\source\repos\ai\claude_mame"
sys.path.insert(0, os.path.join(CLAUDE_MAME, "mcp_server"))
os.environ.setdefault("MAME_EXE_NAME", "mame.exe")
from mame_client import MameBridge  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
LUA = os.path.join(HERE, "cop-capture.lua").replace("\\", "/")
ROMPATH = os.environ.get("MAME_ROMPATH", os.path.join(HERE, "mameroms"))

OUT = os.path.abspath(sys.argv[1]).replace("\\", "/")
FROM = int(sys.argv[2])
FRAMES = int(sys.argv[3])
PROBES = sys.argv[4]


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
            "-rompath", ROMPATH, "-nodrc", "-sound", "none", "-nothrottle",
            "-window", "-nomax",
        ])
        print(await ev(f'dofile("{LUA}")'), flush=True)
        print("probes:", await ev(f'_G.COPCAP.set_probes("{PROBES}")'), flush=True)
        await ev(f"(function() _G.COPCAP.from = {FROM}; _G.COPCAP.want = {FRAMES}; return 'ok' end)()")
        await ev("_G.COPCAP.attach()")
        await b.call("continue")
        t0 = time.time()
        last = -1
        while True:
            await asyncio.sleep(2)
            st = await ev("_G.COPCAP.state")
            fc = int(await ev("_G.COPCAP.frame_counter()"))
            marks = int(await ev("#_G.COPCAP.marks")); nrec = int(await ev("_G.COPCAP.n"))
            if fc // 300 != last:
                last = fc // 300
                print(f"  t={time.time() - t0:5.0f}s frame_counter={fc} state={st} marks={marks} records={nrec}", flush=True)
            if st == "error":
                raise RuntimeError(await ev("_G.COPCAP.err"))
            if st == "captured":
                break
            if time.time() - t0 > 7200:
                raise RuntimeError("timed out")
        await b.call("pause")
        print(await ev(f'_G.COPCAP.write("{OUT}")', timeout=600), flush=True)
    finally:
        await b.shutdown_mame()


asyncio.run(main())
