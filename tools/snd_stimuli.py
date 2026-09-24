#!/usr/bin/env python3
"""snd_stimuli.py -- synthetic sound-board captures for snd_replay.

The MAME attract capture carries 300 MIDI bytes: a few music changes and the
attract fight's effects. These are four more inputs in the same record format
(tag 1 records, 0x9C0000, the 68000 clock period each byte arrives on), so
snd_replay can hold two builds to the same bits over far more of the driver:

  bgm   every music code 0x00-0x2F in turn, 2.5 s apiece, with effects between
  sfx   an effects storm on one track: bursts of up to 12 commands at once
  sys   every AE 14 code, and three random commands on each status A0-AF
  fuzz  random bytes in runs of 1-6, which the driver must survive

Each opens with the boot commands the game sends (A0 00 01, A0 00 03, A0 03 60)
and runs about two minutes. The byte streams are fixed by the seeds, so any two
runs of this script write the same files.

Usage: python tools/snd_stimuli.py <out-dir>
   then snd_replay <out-dir>/bgm <out-prefix> 120   (and sfx, sys, fuzz)
"""
import os
import random
import struct
import sys

CLK = 11289600                      # 68000 clock periods a second


def write(out, name, cmds):
    cmds.sort(key=lambda c: c[0])
    with open(os.path.join(out, name + '.bin'), 'wb') as f:
        for t, bs in cmds:
            tc = int(t * CLK)
            for k, b in enumerate(bs):   # a byte every 0.33 ms, as the UART sends them
                f.write(struct.pack('<4I', (1 << 24) | 0x9C0000, b | 0xFF0000, tc + k * 3700, 0))


def main(out):
    os.makedirs(out, exist_ok=True)
    boot = [(0.19, [0xA0, 0, 1]), (1.0, [0xA0, 0, 3]), (1.0, [0xA0, 3, 0x60]), (2.0, [0xA0, 0, 1])]

    r = random.Random(1)
    c = list(boot); t = 3.0
    for bgm in range(0x00, 0x30):
        c.append((t, [0xAE, 0x10, bgm]))
        u = t + 0.3
        while u < t + 2.4:
            c.append((u, [0xAE, 0x11, r.randrange(0x80)])); u += r.uniform(0.05, 0.6)
        t += 2.5
    write(out, 'bgm', c)

    r = random.Random(2)
    c = list(boot) + [(2.5, [0xAE, 0x10, 0x10])]; t = 3.0
    while t < 118:
        n = r.choice([1, 1, 1, 2, 4, 8, 12])
        for _ in range(n):
            c.append((t, [0xAE, 0x11, r.randrange(0x80)]))
        t += r.uniform(0.005, 0.4)
    write(out, 'sfx', c)

    r = random.Random(3)
    c = list(boot); t = 3.0
    for x in range(0x80):
        c.append((t, [0xAE, 0x14, x])); t += 0.35
        c.append((t, [0xAE, 0x10, r.randrange(0x30)])); t += 0.2
    for a in range(0xA0, 0xB0):
        for _ in range(3):
            c.append((t, [a, r.randrange(0x80), r.randrange(0x80)])); t += 0.25
    write(out, 'sys', c)

    r = random.Random(4)
    c = list(boot) + [(2.5, [0xAE, 0x10, 0x05])]; t = 3.0
    while t < 118:
        c.append((t, [r.randrange(256) for _ in range(r.randrange(1, 7))])); t += r.uniform(0.01, 0.5)
    write(out, 'fuzz', c)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
