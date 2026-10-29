#!/usr/bin/env python3
"""m2emulator 1.1a's default control layouts as Markdown (Pinboard #1347).

Runs M2emuControls.java over EMULATOR.EXE in a headless Ghidra and prints, per
layout, the games that use it and each input's port, mask and default key.
docs/m2emulator-controls.md is this output plus the parity notes.

  python3 tools/m2emu-controls/m2emu_controls.py --exe <m2emulator>/EMULATOR.EXE \\
      --ghidra ~/build/tools/ghidra --work /tmp/<task>/ghidra > out.md
  python3 tools/m2emu-controls/m2emu_controls.py --json controls.json   # reuse a run

--work keeps the Ghidra project (the first run analyses the EXE, a few minutes);
pass the same one again to skip the analysis.
"""
import argparse, hashlib, json, os, subprocess, sys

EXE_SHA256 = '2abe14866db96f4853b58bfaebc95d803c931a0e5ae33e95758c6b39520bccbc'  # 1.1a
HERE = os.path.dirname(os.path.abspath(__file__))

# DirectInput scan codes (DIK_*) the layouts use; 0x100+ is the first joystick.
DIK = {1: 'Esc', 14: 'Backspace', 15: 'Tab', 28: 'Enter', 29: 'LCtrl', 42: 'LShift',
       54: 'RShift', 56: 'LAlt', 57: 'Space', 156: 'Pad Enter', 157: 'RCtrl', 184: 'RAlt',
       199: 'Home', 200: 'Up', 201: 'PgUp', 203: 'Left', 205: 'Right', 207: 'End',
       208: 'Down', 209: 'PgDn', 210: 'Insert', 211: 'Delete'}
for i, c in enumerate('1234567890'): DIK[2 + i] = c
for row, first in (('QWERTYUIOP', 16), ('ASDFGHJKL', 30), ('ZXCVBNM', 44)):
    for i, c in enumerate(row): DIK[first + i] = c
for i in range(10): DIK[59 + i] = f'F{i + 1}'
DIK[87], DIK[88] = 'F11', 'F12'
for k, n in {71: 7, 72: 8, 73: 9, 75: 4, 76: 5, 77: 6, 79: 1, 80: 2, 81: 3, 82: 0}.items():
    DIK[k] = f'Pad {n}'
TYPE = {0: 'emulator', 1: 'button', 3: 'button', 5: 'button', 8: 'axis'}


def key_name(k):
    if k == 0xffffffff: return '-'
    if k >= 0xff000000: return f'joy axis {k & 0xff}'
    if 0x100 <= k < 0x200:
        j = k - 0x100
        return 'joy ' + (['Left', 'Right', 'Up', 'Down'][j] if j < 4 else f'button {j // 16}')
    return DIK.get(k, f'DIK 0x{k:02x}')


def run_ghidra(a):
    exe = os.path.abspath(a.exe)
    sha = hashlib.sha256(open(exe, 'rb').read()).hexdigest()
    if sha != EXE_SHA256:
        sys.exit(f'{exe}: sha256 {sha}, not m2emulator 1.1a\'s; the addresses would be wrong')
    os.makedirs(a.work, exist_ok=True)
    out = os.path.join(a.work, 'controls.json')
    head = [os.path.join(a.ghidra, 'support', 'analyzeHeadless'), a.work, 'm2emu']
    have = os.path.exists(os.path.join(a.work, 'm2emu.gpr'))
    head += ['-process', os.path.basename(exe), '-noanalysis'] if have else ['-import', exe]
    head += ['-scriptPath', HERE, '-postScript', 'M2emuControls.java', out]
    with open(os.path.join(a.work, 'headless.log'), 'w') as log:
        subprocess.run(head, stdout=log, stderr=subprocess.STDOUT, check=True)
    return json.load(open(out))


def markdown(d):
    games = {}
    for g in d['games']: games.setdefault(g['list'], []).append(g['name'])
    out = []
    for obj, lst in d['lists'].items():
        named = [e for e in lst['entries'] if e['name'] and e['type'] != 0]
        out.append(f"### {lst['class']} ({len(games.get(obj, []))} games)\n")
        out.append('Games: ' + ', '.join(sorted(games.get(obj, []))) + '\n' if obj in games else 'Games: none\n')
        out.append('| Input | Port | Mask | Key |\n|---|---|---|---|')
        for e in named:
            out.append(f"| {e['name']} | {e['port']} | 0x{e['mask']:02X} | {key_name(e['key'])} |")
        out.append('')
    return '\n'.join(out)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--exe'); p.add_argument('--ghidra'); p.add_argument('--work')
    p.add_argument('--json', help='a controls.json from an earlier run')
    a = p.parse_args()
    if a.json: d = json.load(open(a.json))
    elif a.exe and a.ghidra and a.work: d = run_ghidra(a)
    else: p.error('--json, or --exe, --ghidra and --work')
    print(markdown(d))


if __name__ == '__main__':
    main()
