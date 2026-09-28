"""The sound board's clockwork, read off a capture (MAME's or snd_replay's).

  python tools/mame/snd_timing.py <prefix> [seconds]

From <prefix>.bin (tools/mame/snd-capture.lua's format): each timer's reload
value and its period fire to fire (consecutive reload writes, in samples), the
clocks from the interrupt to that reload, and how long the i960's MIDI bytes
take to raise the 68000's MIDI interrupt. These are the numbers the sound board
is held to MAME by, beside snd_compare.py: current MAME has timer A at 505.44
samples and timer B at 50.014 over 90 s of attract, and a byte on the serial
line 313 us; each was 0.2% off before the 68000's wait states, exception time
and the serial line were put in (tools/README.md, "The sound board").
"""
import sys, numpy as np
CLK = 11289600.0
a = np.fromfile(sys.argv[1] + '.bin', dtype='<u4'); a = a[:len(a)//4*4].reshape(-1, 4)
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 1e9
tag, off, data, t = a[:,0] >> 24, a[:,0] & 0xffffff, a[:,1], a[:,2].astype(np.int64)
keep = t / CLK < secs
tag, off, data, t, pc = tag[keep], off[keep], data[keep], t[keep], a[keep, 3]
print(f'{sys.argv[1]}: {len(t)} records, {t[-1]/CLK:.1f} s')
# interrupt levels from the last SCILV writes
lv = [0, 0, 0]
for i in np.nonzero((tag == 2) & (off >= 0x424) & (off <= 0x428))[0][:6]:
    lv[(off[i] - 0x424) // 2] = int(data[i]) & 0xffff
def level(bit):
    return ((lv[0] >> bit) & 1) | (((lv[1] >> bit) & 1) << 1) | (((lv[2] >> bit) & 1) << 2)
lta, ltb, lmidi = level(6), level(7), level(3)
print(f'levels: timer A {lta}, timer B/C {ltb}, MIDI {lmidi}')
irq = np.nonzero(tag == 4)[0]
for L in range(1, 8):
    n = int(np.sum(off[irq] == L))
    if n: print(f'  irqs level {L}: {n}')
for name, reg in (('timer A', 0x418), ('timer B', 0x41a), ('timer C', 0x41c)):
    w = np.nonzero((tag == 2) & (off == reg))[0]
    if len(w) < 3: continue
    d = np.diff(t[w])
    d = d[(d > 0) & (d < d.max() if len(d) > 2 else True)]
    vals = data[w] & 0xffff
    print(f'{name}: {len(w)} writes, reload {sorted(set(int(v) for v in vals[-20:]))}, period mean {d.mean()/256:.4f} samples (median {np.median(d)/256:.4f}), min {d.min()/256:.3f} max {d.max()/256:.3f}')
    # latency from the interrupt taken to the reload write
    L = lta if reg == 0x418 else ltb
    ir = irq[off[irq] == L]
    if len(ir):
        it = t[ir]
        k = np.searchsorted(it, t[w], side='right') - 1
        ok = k >= 0
        lat = t[w][ok] - it[k[ok]]
        lat = lat[lat < 20000]
        print(f'  irq->reload: mean {lat.mean():.1f} clocks, median {np.median(lat):.0f}, min {lat.min()}, max {lat.max()}, n {len(lat)}')
# MIDI: i960 byte -> MIDI interrupt
mb = np.nonzero((tag == 1) & (off == 0x9c0000))[0]
im = irq[off[irq] == lmidi]
if len(mb) and len(im):
    it = t[im]
    k = np.searchsorted(it, t[mb], side='left')
    ok = k < len(it)
    d = it[k[ok]] - t[mb][ok]
    print(f'MIDI: {len(mb)} bytes, {len(im)} MIDI irqs; byte->irq delay median {np.median(d):.0f} clocks ({np.median(d)/CLK*1e6:.0f} us), min {d.min()}, 10%% {np.percentile(d,10):.0f}, 90%% {np.percentile(d,90):.0f}')
    gaps = np.diff(t[mb]); gaps = gaps[gaps < 100000]
    if len(gaps): print(f'  byte spacing (<100k clocks): median {np.median(gaps):.0f}, min {gaps.min()}')
# MIDI reads (0x404) with pc, first few
r = np.nonzero((tag == 3) & (off == 0x404))[0]
print(f'MIDI reads recorded (value changes): {len(r)}')
