#!/usr/bin/env python3
"""grade-sound-synth.py -- hold the voices in C (board/sound_synth.h) against
the SCSP's slots, input by input: both under the sound driver in C.

The synth is meant to be bit-exact with the chip (which is MAME's, scsp.h), so
this grades by identity, not by ear. Each input runs through tests/snd_replay
twice, SND_HLE=1 (the driver plays the chip) and SND_HLE=1 SND_SYNTH=1 (the
driver plays the synth), and everything the two write must be the same bytes:

  wav      the audio, 44.1 kHz stereo
  bin      the driver's register writes and reads (what it saw of the voices)
  ram      the driver's RAM, dumped every frame
  regs     the slot and common registers, dumped every frame

On a difference it says where the audio first parts and by how much. With
--exact both runs also take SCSP_EXACT=1 (the reverb every sample, scsp.h "A
quiet DSP"); without it both skip the quiet reverb, and the synth must skip
the same samples.

Usage:
  python tools/grade-sound-synth.py <snd_replay> <out-dir> <input-prefix>... [--seconds S] [--exact]
  inputs: tools/snd_stimuli.py's (bgm, sfx, sys, fuzz) or a MAME capture
  $ROMDIR as snd_replay wants it (sfight.zip and schamp.zip)

numpy only. Nothing it writes belongs in the repo.
"""
import argparse
import os
import subprocess
import sys

import numpy as np

PARTS = (('wav', '.wav'), ('bin', '.bin'), ('ram', '.ram.bin'), ('regs', '.regs.bin'))


def run(replay, inp, out, seconds, synth, exact):
    env = dict(os.environ, SND_HLE='1', SCSP_EXACT='1' if exact else '0')
    if synth:
        env['SND_SYNTH'] = '1'
    else:
        env.pop('SND_SYNTH', None)
    r = subprocess.run([replay, inp, out, str(seconds)], env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'{replay} failed on {inp}:\n{r.stdout}{r.stderr}')
    if 'driver not known' in r.stdout:
        sys.exit('the driver in C did not take this program ROM: ' + r.stdout)
    if synth and 'sound synth:' not in r.stdout:
        sys.exit('snd_replay did not run the synth (built before sound_synth.h?)')


def audio_diff(a, b):
    """Where two WAVs' samples first part, and the largest difference."""
    x = np.fromfile(a, dtype='<i2')[22:].astype(np.int32)
    y = np.fromfile(b, dtype='<i2')[22:].astype(np.int32)
    if len(x) != len(y):
        return f'lengths {len(x) // 2} and {len(y) // 2} samples'
    d = np.nonzero(x != y)[0]
    return f'first at sample {d[0] // 2} ({d[0] // 2 / 44100:.3f} s), max |diff| {np.abs(x - y).max()}, {len(d)} values'


def grade(replay, outdir, inp, seconds, exact):
    name = os.path.basename(inp)
    chip, syn = os.path.join(outdir, name + '_chip'), os.path.join(outdir, name + '_synth')
    run(replay, inp, chip, seconds, False, exact)
    run(replay, inp, syn, seconds, True, exact)
    bad = []
    for part, ext in PARTS:
        with open(chip + ext, 'rb') as f1, open(syn + ext, 'rb') as f2:
            if f1.read() != f2.read():
                bad.append(part)
    line = f'{name:8} ' + ' '.join(f'{p}:{"DIFF" if p in bad else "same"}' for p, _ in PARTS)
    if 'wav' in bad:
        line += '\n         audio: ' + audio_diff(chip + '.wav', syn + '.wav')
    print(line)
    return not bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('replay')
    ap.add_argument('outdir')
    ap.add_argument('inputs', nargs='+')
    ap.add_argument('--seconds', type=float, default=30)
    ap.add_argument('--exact', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    ok = all([grade(a.replay, a.outdir, i, a.seconds, a.exact) for i in a.inputs])
    print('PASS: the synth is the chip, to the bit' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
