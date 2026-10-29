"""How far a sound-board run is from the exact chip's: the margin of the quiet DSP.

  python tools/scsp-ab.py <exact.wav> <other.wav> [seconds]

Both from tests/snd_replay.c (or any 16-bit stereo WAV of the same length and
rate, sample-aligned: the board is deterministic, so the two runs line up at
sample 0). Prints, over the whole run or its first `seconds`:
  - samples that differ, and the longest run of them
  - the largest difference in LSB (16-bit), and its time
  - RMS of the difference, the RMS of the exact signal, the SNR in dB
  - a histogram of |diff|
  - a spectral difference: over 4096-point Hann frames (hop 2048), the mean
    power spectrum of the exact run, of the other run and of their difference,
    per octave band in dBFS (a full-scale sine is 0 dBFS), with the band's
    SNR and the shift of the other run's level against the exact one; the
    loudest bin of the difference's spectrum and the frame it peaks in
The quiet DSP (scsp.h, "A quiet DSP", Pinboard #1681) is held to a few LSB
here; the exact chip (--scsp-exact / SCSP_EXACT=1) is held to MAME elsewhere.
"""
import struct
import sys

import numpy as np


def read_wav(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE', path
    pos, rate, ch, bits = 12, 0, 0, 0
    while pos + 8 <= len(data):
        tag, n = data[pos:pos + 4], struct.unpack_from('<I', data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + n]
        if tag == b'fmt ':
            ch, rate, bits = struct.unpack_from('<H', body, 2)[0], struct.unpack_from('<I', body, 4)[0], struct.unpack_from('<H', body, 14)[0]
        elif tag == b'data':
            assert bits == 16, bits
            return rate, np.frombuffer(body[:len(body) // (2 * ch) * 2 * ch], dtype='<i2').reshape(-1, ch)
        pos += 8 + n + (n & 1)
    raise SystemExit('no data chunk in ' + path)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    rate_a, a = read_wav(sys.argv[1])
    rate_b, b = read_wav(sys.argv[2])
    assert rate_a == rate_b, (rate_a, rate_b)
    n = min(len(a), len(b))
    if len(sys.argv) > 3:
        n = min(n, int(float(sys.argv[3]) * rate_a))
    if len(a) != len(b):
        print(f'lengths differ: {len(a)} vs {len(b)} frames, comparing the first {n}')
    a, b = a[:n].astype(np.int32), b[:n].astype(np.int32)
    d = b - a
    ad = np.abs(d).max(axis=1)
    diff = ad > 0
    nd = int(diff.sum())
    print(f'{n} frames at {rate_a} Hz ({n / rate_a:.2f} s): {nd} differ ({100.0 * nd / max(n, 1):.3f}%)')
    if nd:
        edges = np.flatnonzero(np.diff(np.concatenate(([0], diff.astype(np.int8), [0]))))
        runs = edges[1::2] - edges[0::2]
        k = int(runs.argmax())
        print(f'longest differing run {int(runs.max())} frames at {edges[2 * k] / rate_a:.3f} s; {len(runs)} runs')
        i = int(ad.argmax())
        print(f'max |diff| {int(ad[i])} LSB at {i / rate_a:.3f} s (exact {a[i].tolist()}, other {b[i].tolist()})')
    rms_d = float(np.sqrt(np.mean(d.astype(np.float64) ** 2)))
    rms_a = float(np.sqrt(np.mean(a.astype(np.float64) ** 2)))
    snr = 20 * np.log10(rms_a / rms_d) if rms_d > 0 else float('inf')
    print(f'RMS diff {rms_d:.4f} LSB, RMS exact {rms_a:.1f} LSB, SNR {snr:.1f} dB')
    hist = np.bincount(np.minimum(ad, 16), minlength=17)
    print('|diff| histogram: ' + ', '.join(f'{v}:{int(c)}' for v, c in enumerate(hist) if c) + ' (16 = 16 or more)')
    spectral(a.mean(axis=1), b.mean(axis=1), rate_a)
    return 0


def db(p):
    return 10 * np.log10(np.maximum(p, 1e-30))


def spectral(a, b, rate, n=4096, hop=2048):
    """Mean power spectra of a (exact), b (other) and b - a, per octave band."""
    w = np.hanning(n)
    scale = 1.0 / (32768.0 * w.sum() / 2)          # a full-scale sine peaks at 1.0
    frames = (len(a) - n) // hop + 1
    if frames < 1:
        print('too short for a spectrum')
        return
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    pa = np.zeros(n // 2 + 1)
    pb = np.zeros_like(pa)
    pd = np.zeros_like(pa)
    peak, peak_t = -1.0, 0.0
    d = (b - a).astype(np.float64)
    for i in range(frames):
        sl = slice(i * hop, i * hop + n)
        xa = np.abs(np.fft.rfft(a[sl] * w)) * scale
        xb = np.abs(np.fft.rfft(b[sl] * w)) * scale
        xd = np.abs(np.fft.rfft(d[sl] * w)) * scale
        pa += xa * xa
        pb += xb * xb
        pd += xd * xd
        m = float((xd * xd).sum())
        if m > peak:
            peak, peak_t = m, i * hop / rate
    pa /= frames
    pb /= frames
    pd /= frames
    print(f'spectrum over {frames} frames of {n} ({n / rate * 1000:.0f} ms, Hann, hop {hop}); levels in dBFS:')
    print(f'{"band (Hz)":>14} {"exact":>7} {"other":>7} {"diff":>7} {"SNR":>6} {"shift":>6}')
    lo = 31.25
    while lo < rate / 2:
        hi = min(lo * 2, rate / 2)
        m = (freqs >= lo) & (freqs < hi)
        ea, eb, ed = pa[m].sum(), pb[m].sum(), pd[m].sum()
        snr = db(ea) - db(ed) if ed > 0 else float('inf')
        shift = db(eb) - db(ea)
        print(f'{lo:>6.0f}-{hi:<7.0f} {db(ea):>7.1f} {db(eb):>7.1f} {db(ed):>7.1f} {snr:>6.1f} {shift:>+6.2f}')
        lo = hi
    ea, eb, ed = pa.sum(), pb.sum(), pd.sum()
    print(f'{"all":>14} {db(ea):>7.1f} {db(eb):>7.1f} {db(ed):>7.1f} {db(ea) - db(ed) if ed > 0 else float("inf"):>6.1f} {db(eb) - db(ea):>+6.2f}')
    k = int(pd.argmax())
    print(f'loudest bin of the difference: {freqs[k]:.0f} Hz at {db(pd[k]):.1f} dBFS (exact there {db(pa[k]):.1f} dBFS)')
    print(f'the frame the difference peaks in: {peak_t:.2f} s at {db(peak):.1f} dBFS')
    loud = pa > 10 ** (-90 / 10)
    if loud.any():
        dev = np.abs(db(pb[loud]) - db(pa[loud]))
        j = int(dev.argmax())
        print(f'largest level shift of any bin above -90 dBFS: {dev[j]:.3f} dB at {freqs[loud][j]:.0f} Hz')


if __name__ == '__main__':
    sys.exit(main())
