#!/usr/bin/env python3
"""Decode your own copy of the PS3 release's menu data, for the grader and the
layout generator. Nothing this writes goes into the repo.

  python tools/ps3ui/extract/extract.py <NPUB30927>/USRDIR/rom.psarc <workdir>

  <workdir>/out      rom.psarc unpacked, every FArc beside it as <name>.farc.d/
  <workdir>/aet      the AET layouts as JSON (aetdump.py); aetrender.py composites them
  <workdir>/sprites  sprites.json, fontmap.json, strings, and each sprite as a PNG
                     (sprdump.py) -- the references tools/ps3ui/grade.py compares with

Then:
  python tools/ps3ui/gen_layout.py <workdir>/aet/n_cmn.json <workdir>/sprites/sprites.json          <workdir>/sprites/fontmap.json
Needs numpy and Pillow.
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def run(script, *args, env=None):
    e = dict(os.environ)
    e.update(env or {})
    subprocess.check_call([sys.executable, os.path.join(HERE, script), *args], env=e)


def main():
    psarc, work = sys.argv[1], os.path.abspath(sys.argv[2])
    out, aet, spr = (os.path.join(work, d) for d in ('out', 'aet', 'sprites'))
    for d in (out, aet, spr):
        os.makedirs(d, exist_ok=True)
    run('unpsarc.py', psarc, out)
    farcs = [os.path.join(dp, f) for dp, _, fs in os.walk(out) for f in fs if f.endswith('.farc')]
    run('farc.py', *farcs)
    run('aetdump.py', out, aet)
    run('sprdump.py', env={'PS3UI_OUT': out, 'PS3UI_SPRITES': spr})
    print('done:', work)


if __name__ == '__main__':
    main()
