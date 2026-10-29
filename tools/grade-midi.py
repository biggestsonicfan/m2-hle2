#!/usr/bin/env python3
"""grade-midi.py -- hold the sound board's music against ValleyBell's M2MidiDec.

M2MidiDec (github.com/ValleyBell/MidiConverters, M2MidiDec.c) reads the sound
program ROM's own sequence data and writes each song as a MIDI file, plus an SF2
of the sample ROMs. It is a second reading of the same driver data, made without
running the 68000, so it is a check on the board that shares no code with it.
Its MIDIs and SF2 are an approximation of the SCSP, though, and three of their
departures have to be taken out before the audio can be compared at all (see
tools/README.md, "Music against M2MidiDec"):

  stim  <dir>                      one snd_replay input per song: the boot
                                   commands, then AE 10 xx at 3.0 s
  fix   <mid-dir> <out-dir>        the MIDIs at the board's tempo, with MIDI
                                   channel 10 moved off the percussion channel
  grade <mid-dir> <board-dir> [--wav <render-dir>] [--secs 88] [songs...]
                                   notes, tempo, pitch, lengths and levels of
                                   <board-dir>/songXX (snd_replay) against
                                   <mid-dir>/SONG_XX.MID; with --wav, the
                                   spectrum of <board-dir>/songXX.wav against
                                   <render-dir>/SONG_XX.wav
  pitch <mid-dir> <board-dir> <sf2> [--secs 88] [songs...]
                                   each note's pitch on the board against the
                                   one the SF2's root key and the MIDI's bend
                                   give it, in cents

The whole run, from a sfight set (epr-19021.31 is the sound program):

  M2MidiDec mus epr-19021.31                                  # SONG_00..18.MID
  M2MidiDec -o stf.sf2 sf2 epr-19021.31 mpr-19022.32 mpr-19023.33 mpr-19024.34 mpr-19025.35
  python tools/grade-midi.py stim stim
  for s in 00 01 ... 18; do snd_replay stim/song$s board/song$s 93; done
  python tools/grade-midi.py fix mid midfix
  echo "interp 1" > lin.cfg     # the SCSP interpolates linearly
  for s in ...; do fluidsynth -ni -f lin.cfg -g 0.05 -R 0 -C 0 -r 44100 \\
      -F render/SONG_$s.wav stf.sf2 midfix/SONG_$s.MID; done
  python tools/grade-midi.py grade midfix board --wav render
  python tools/grade-midi.py pitch midfix board stf.sf2

numpy only.
"""
import collections
import math
import os
import struct
import sys
import wave

import numpy as np

CLK = 11289600.0                    # 68000 clock periods a second
RATE = 44100
TIMER_B = 50.0146                   # samples per sequencer tick (the board's timer B, = MAME's)
DRUM_SWAP = (9, 12)                 # STF's MIDI channel 10 is melodic: move it to 13


# ---------------------------------------------------------------- MIDI files

def _vlq(d, p):
    v = 0
    while True:
        b = d[p]; p += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            return v, p


def _put_vlq(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F)); v >>= 7
    return bytes(reversed(out))


def midi_parse(path):
    """-> (division, [track: [(delta, status, data bytes)]]), running status expanded."""
    d = open(path, 'rb').read()
    assert d[:4] == b'MThd'
    ntrk, div = struct.unpack('>xxHH', d[8:14])
    p, tracks = 14, []
    for _ in range(ntrk):
        assert d[p:p + 4] == b'MTrk'
        n = struct.unpack('>I', d[p + 4:p + 8])[0]
        q, end, run, ev = p + 8, p + 8 + n, 0, []
        while q < end:
            dt, q = _vlq(d, q)
            st = d[q]
            if st & 0x80:
                q += 1
            else:
                st = run
            if st == 0xFF:
                typ = d[q]; ln, q2 = _vlq(d, q + 1)
                ev.append((dt, st, bytes([typ]) + d[q2:q2 + ln])); q = q2 + ln
            elif st in (0xF0, 0xF7):
                ln, q2 = _vlq(d, q)
                ev.append((dt, st, d[q2:q2 + ln])); q = q2 + ln
            else:
                run = st
                k = 1 if st & 0xF0 in (0xC0, 0xD0) else 2
                ev.append((dt, st, d[q:q + k])); q += k
        tracks.append(ev)
        p = end
    return div, tracks


def midi_write(path, div, tracks):
    out = [b'MThd', struct.pack('>IHHH', 6, 0 if len(tracks) == 1 else 1, len(tracks), div)]
    for ev in tracks:
        body = bytearray()
        for dt, st, data in ev:
            body += _put_vlq(dt)
            if st == 0xFF:
                body += bytes([0xFF, data[0]]) + _put_vlq(len(data) - 1) + data[1:]
            elif st in (0xF0, 0xF7):
                body += bytes([st]) + _put_vlq(len(data)) + data
            else:
                body += bytes([st]) + data
        out += [b'MTrk', struct.pack('>I', len(body)), bytes(body)]
    open(path, 'wb').write(b''.join(out))


def _channel_state(ch, kind, data):
    """The (field, value) a program change, volume, expression or bend sets, else None."""
    if kind == 0xC0:
        return 'prog', data[0]
    if kind == 0xB0 and data[0] in (7, 11):
        return ('vol' if data[0] == 7 else 'expr'), data[1]
    if kind == 0xE0:
        return 'bend', (data[0] | data[1] << 7) - 8192
    return None


def midi_notes(path):
    """Note-ons in seconds, with the channel state each one starts under and its length."""
    div, tracks = midi_parse(path)
    evs = []
    for ev in tracks:
        tick = 0
        for i, (dt, st, data) in enumerate(ev):
            tick += dt
            evs.append((tick, i, st, data))
    evs.sort(key=lambda e: (e[0], e[1]))
    tempo, t, last = 500000, 0.0, 0
    state = [dict(prog=0, vol=100, expr=127, bend=0) for _ in range(16)]
    sounding, out = {}, []
    for tick, _, st, data in evs:
        t += (tick - last) * tempo / div / 1e6; last = tick
        if st == 0xFF:
            if data[0] == 0x51:
                tempo = int.from_bytes(data[1:4], 'big')
            continue
        ch, kind = st & 0x0F, st & 0xF0
        cs = _channel_state(ch, kind, data)
        if cs:
            state[ch][cs[0]] = cs[1]
        elif kind in (0x80, 0x90):
            key = (ch, data[0])
            if key in sounding:            # a note-off, or a new note-on on a sounding key
                n = out[sounding.pop(key)]; n['dur'] = t - n['t']
            if kind == 0x90 and data[1]:
                sounding[key] = len(out)
                out.append(dict(t=t, ch=ch, note=data[0], vel=data[1], dur=math.inf, **state[ch]))
    return out


# ---------------------------------------------------------------- the SF2

def _riff(d, p, end):
    while p < end:
        n = struct.unpack_from('<I', d, p + 4)[0]
        yield d[p:p + 4], p + 8, n
        p += 8 + n + (n & 1)


def _recs(blob, fmt):
    k = struct.calcsize(fmt)
    return [struct.unpack_from(fmt, blob, i) for i in range(0, len(blob) - k + 1, k)]


def _gens(bags, gens, b):
    return {op: amt for op, amt in gens[bags[b][0]:bags[b + 1][0]]}


def sf2_roots(path):
    """{program: [(lo key, hi key, root key)]} of bank 0. M2MidiDec writes every
    sample at 44.1 kHz with no tuning generators, so the root key is the whole pitch."""
    d = open(path, 'rb').read()
    pd = {}
    for cid, p, n in _riff(d, 12, len(d)):
        if cid == b'LIST' and d[p:p + 4] == b'pdta':
            pd = {c.decode(): d[q:q + m] for c, q, m in _riff(d, p + 4, p + n)}
    phdr, inst = _recs(pd['phdr'], '<20sHHHIII'), _recs(pd['inst'], '<20sH')
    pbag, ibag = _recs(pd['pbag'], '<HH'), _recs(pd['ibag'], '<HH')
    pgen, igen = _recs(pd['pgen'], '<Hh'), _recs(pd['igen'], '<Hh')
    out = {}
    for i, h in enumerate(phdr[:-1]):
        ins = [_gens(pbag, pgen, b).get(41) for b in range(h[3], phdr[i + 1][3])]
        ins = [x for x in ins if x is not None]
        if h[2] != 0 or not ins:
            continue
        zs = [_gens(ibag, igen, b) for b in range(inst[ins[0]][1], inst[ins[0] + 1][1])]
        out[h[1]] = [(z[43] & 0xFF, (z[43] >> 8) & 0xFF, z[58]) for z in zs if 43 in z and 58 in z]
    return out


# ---------------------------------------------------------------- the board

def board_capture(prefix, song):
    """Key-ons (time, slot, pitch in cents, TL) and how long each was held, from a
    snd_replay capture; times from the byte that completes AE 10 <song>."""
    a = np.fromfile(prefix + '.bin', dtype='<u4')
    a = a[:len(a) // 4 * 4].reshape(-1, 4)
    tag, off = a[:, 0] >> 24, a[:, 0] & 0xFFFFFF
    mi = np.nonzero((tag == 1) & (off == 0x9C0000))[0]
    data = bytes(int(a[i, 1]) & 0xFF for i in mi)
    k = data.find(bytes([0xAE, 0x10, song]))
    if k < 0:
        raise SystemExit(f'{prefix}: AE 10 {song:02X} is not in the MIDI stream')
    t0 = a[mi[k + 2], 2] / CLK
    reg = np.zeros((32, 0x20), dtype=np.int64)
    keyed, on = [None] * 32, []
    t = a[:, 2] / CLK
    for i in np.nonzero((tag == 2) & (off < 0x400))[0]:
        o, d = int(off[i]), int(a[i, 1])
        s, r = o >> 5, o & 0x1E
        reg[s, r] = (reg[s, r] & ~(d >> 16)) | (d & 0xFFFF & (d >> 16))
        if r or not reg[s, 0] & 0x1000:
            continue
        tt = float(t[i]) - t0
        for ss in range(32):
            w0 = int(reg[ss, 0])
            if (w0 >> 11) & 1 and keyed[ss] is None:
                pw = int(reg[ss, 0x10])
                oc = (pw >> 11) & 0xF
                oc = oc - 16 if oc & 8 else oc
                keyed[ss] = len(on)
                on.append(dict(t=tt, slot=ss, cents=1200 * oc + 1200 * math.log2(1 + (pw & 0x3FF) / 1024),
                               tl=int(reg[ss, 0xC]) & 0xFF, held=math.inf))
            elif not (w0 >> 11) & 1 and keyed[ss] is not None:
                e = on[keyed[ss]]; e['held'] = tt - e['t']; keyed[ss] = None
        reg[s, 0] &= ~0x1000
    return [e for e in on if e['t'] >= 0], t0


# ---------------------------------------------------------------- lining up

def onsets(ts):
    return np.unique(np.round(np.asarray(ts, dtype=float), 3))


def fit(mt, bt, tol=0.012):
    """board = r * midi + off, both starting on their first note; r from the whole song."""
    best = None
    for r in np.arange(0.94, 1.06, 0.0001):
        off = bt[0] - mt[0] * r
        s = mt * r + off
        s = s[s < bt[-1] + tol]
        i = np.clip(np.searchsorted(bt, s), 1, len(bt) - 1)
        err = np.minimum(abs(bt[i] - s), abs(bt[i - 1] - s))
        sc = np.mean(err < tol)
        if best is None or sc > best[0]:
            best = (sc, r, off, np.median(err[err < tol]) if (err < tol).any() else 0.0)
    return best[1], best[2], best[0], best[3]


def clean_pairs(mn, on, r, off, secs, win=0.03):
    """Moments where one MIDI note and one key-on stand alone: the only pairing that
    unisons and chords cannot confuse."""
    xm = np.array([n['t'] for n in mn]) * r + off
    bt = np.array([e['t'] for e in on])
    out = []
    for i, n in enumerate(mn):
        if xm[i] > secs or np.sum(abs(xm - xm[i]) < win) != 1:
            continue
        k = np.nonzero(abs(bt - xm[i]) < win)[0]
        if len(k) == 1:
            out.append((n, on[k[0]]))
    return out


def pitch_agreement(mn, on, r, off, secs, tol=0.012):
    """Each MIDI key (channel, program, note) keeps one sample and one root on the
    board, so cents - 100 * note is a constant per key; a note agrees when a key-on
    at its time sits on that constant."""
    xm = np.array([n['t'] for n in mn]) * r + off
    bt = np.array([e['t'] for e in on]); bc = np.array([e['cents'] for e in on])
    hist, cand = collections.defaultdict(collections.Counter), []
    for i, n in enumerate(mn):
        if xm[i] > secs:
            break
        k = np.nonzero(abs(bt - xm[i]) < tol)[0]
        key = (n['ch'], n['prog'], n['note'])
        d = [int(round(bc[j] - 100 * n['note'])) for j in k]
        cand.append((key, d))
        for v in set(d):
            hist[key][v] += 1
    good = sum(1 for key, d in cand if hist[key] and any(abs(v - hist[key].most_common(1)[0][0]) <= 3 for v in d))
    return good / max(len(cand), 1)


# ---------------------------------------------------------------- audio

BANDS = [(20, 80), (80, 250), (250, 1000), (1000, 4000), (4000, 8000), (8000, 16000), (16000, 22050)]


def wav_mono(path):
    w = wave.open(path, 'rb')
    n, ch, sw, sr = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
    assert sw == 2, path
    x = np.frombuffer(w.readframes(n), dtype='<i2').astype(float).reshape(-1, ch).mean(1)
    return x / 32768, sr


def stft(x, sr, times, n=4096):
    w, fr = np.hanning(n), []
    for t in times:
        i = int(t * sr) - n // 2
        seg = np.zeros(n)
        a, b = max(i, 0), min(i + n, len(x))
        if a < b:
            seg[a - i:b - i] = x[a:b]
        fr.append(np.abs(np.fft.rfft(seg * w)))
    return np.array(fr), np.fft.rfftfreq(n, 1 / sr)


def audio(board_wav, render_wav, t0, r, off, secs):
    bx, bsr = wav_mono(board_wav)
    rx, rsr = wav_mono(render_wav)
    bx = bx - bx.mean()
    tb = np.arange(0.2, secs, 1024 / RATE)                 # board time after AE 10 xx
    Sb, f = stft(bx, bsr, t0 + tb)
    Sr, _ = stft(rx, rsr, (tb - off) / r)                   # the render's own clock
    Eb = np.array([(Sb[:, (f >= a) & (f < b)] ** 2).sum(1) for a, b in BANDS]).T
    Er = np.array([(Sr[:, (f >= a) & (f < b)] ** 2).sum(1) for a, b in BANDS]).T
    band = 10 * np.log10(Eb.mean(0) / Eb.mean(0).sum()) - 10 * np.log10(Er.mean(0) / Er.mean(0).sum())
    env = np.corrcoef(np.sqrt(Eb.sum(1)), np.sqrt(Er.sum(1)))[0, 1]
    m = (f > 55) & (f < 5000)
    pc = (np.round(12 * np.log2(f[m] / 440)) % 12).astype(int)
    Cb = np.array([(Sb[:, m][:, pc == k] ** 2).sum(1) for k in range(12)]).T
    Cr = np.array([(Sr[:, m][:, pc == k] ** 2).sum(1) for k in range(12)]).T
    ok = (Cb.std(1) > 0) & (Cr.std(1) > 0)
    chroma = np.mean([np.corrcoef(Cb[i], Cr[i])[0, 1] for i in np.nonzero(ok)[0]])
    rms = 20 * math.log10(math.sqrt((Eb.sum(1)).mean()) / math.sqrt((Er.sum(1)).mean()))
    return band, env, chroma, rms


# ---------------------------------------------------------------- commands

def cmd_stim(out):
    os.makedirs(out, exist_ok=True)
    boot = [(0.19, [0xA0, 0, 1]), (1.0, [0xA0, 0, 3]), (1.0, [0xA0, 3, 0x60]), (2.0, [0xA0, 0, 1])]
    for s in range(0x19):
        with open(os.path.join(out, f'song{s:02X}.bin'), 'wb') as f:
            for t, bs in boot + [(3.0, [0xAE, 0x10, s])]:
                for k, b in enumerate(bs):   # the snd_stimuli.py record: a byte every 0.33 ms
                    f.write(struct.pack('<4I', (1 << 24) | 0x9C0000, b | 0xFF0000, int(t * CLK) + k * 3700, 0))
    print(f'{out}: song00..song18.bin')


def cmd_fix(src, dst):
    os.makedirs(dst, exist_ok=True)
    for name in sorted(os.listdir(src)):
        if not name.upper().endswith('.MID'):
            continue
        div, tracks = midi_parse(os.path.join(src, name))
        tempo = round(TIMER_B / RATE * 1e6 * div)       # one tick of the board's sequencer per MIDI tick
        fixed = []
        for ev in tracks:
            new = []
            for dt, st, data in ev:
                if st == 0xFF and data[0] == 0x51:
                    data = bytes([0x51]) + tempo.to_bytes(3, 'big')
                elif st < 0xF0 and (st & 0x0F) == DRUM_SWAP[0]:
                    st = (st & 0xF0) | DRUM_SWAP[1]
                new.append((dt, st, data))
            fixed.append(new)
        midi_write(os.path.join(dst, name), div, fixed)
    print(f'{dst}: tempo {TIMER_B / RATE * 1e3:.4f} ms a tick, MIDI channel {DRUM_SWAP[0] + 1} -> {DRUM_SWAP[1] + 1}')


def note_levels(pairs):
    """(channel, program, board - MIDI dB) per paired note. The driver's TL is
    0.375 dB a step; the decoder wrote velocity and volume on a 40 log10 curve,
    so the MIDI says the same attenuation in dB."""
    return [(n['ch'], n['prog'], e['tl'] * 0.375 - 40 * math.log10(127 / n['vel'])
             - 40 * math.log10(127 / max(n['vol'], 1)) - 40 * math.log10(127 / max(n['expr'], 1)))
            for n, e in pairs]


def note_lengths(pairs, r):
    """(channel, held within 20 ms of the MIDI length) per paired note that has both."""
    return [(n['ch'], abs(e['held'] - n['dur'] * r) < 0.02) for n, e in pairs
            if math.isfinite(n['dur']) and math.isfinite(e['held'])]


def print_audio(s, board_dir, wav_dir, t0, r, off, span):
    rw = os.path.join(wav_dir, f'SONG_{s:02X}.wav')
    if os.path.exists(rw):
        band, env, chroma, rms = audio(os.path.join(board_dir, f'song{s:02X}.wav'), rw, t0, r, off, span)
        print(f'  | {env:.3f} {chroma:.3f}  ' + ' '.join(f'{v:+5.1f}' for v in band), end='')


def notes_in_span(mn, on, r, off, span):
    """How many MIDI notes (on the board's clock) and board key-ons fall
    before span."""
    nm = sum(1 for n in mn if n['t'] * r + off < span)
    nb = sum(1 for e in on if e['t'] < span)
    return nm, nb


def grade_song(s, mid_dir, board_dir, wav_dir, secs):
    """One song's row. Returns its (song, channel, length ok) and (song, channel,
    program, dB) lists, or None when either side has no notes."""
    mn = midi_notes(os.path.join(mid_dir, f'SONG_{s:02X}.MID'))
    on, t0 = board_capture(os.path.join(board_dir, f'song{s:02X}'), s)
    if not mn or not on:
        return None
    r, off, sc, med = fit(onsets([n['t'] for n in mn]), onsets([e['t'] for e in on]))
    # M2MidiDec plays a song's loop twice (-l); the board loops for ever
    span = min(secs, on[-1]['t'] + 0.5, mn[-1]['t'] * r + off + 0.5)
    nm, nb = notes_in_span(mn, on, r, off, span)
    p = pitch_agreement(mn, on, r, off, span)
    pairs = clean_pairs(mn, on, r, off, span)
    lens = [(s, ch, ok) for ch, ok in note_lengths(pairs, r)]
    lok = np.mean([ok for *_, ok in lens]) if lens else float('nan')
    lv = note_levels(pairs)
    sd = np.std([v[2] for v in lv]) if lv else float('nan')
    print(f'{s:02X}  {nm:6d} {nb:6d}  {r:.4f}  {sc * 100:5.1f}%  {med * 1000:5.2f}  {p * 100:5.1f}%  '
          f'{lok * 100:5.1f}%  {len(lv):5d}  {sd:4.1f}', end='')
    if wav_dir:
        print_audio(s, board_dir, wav_dir, t0, r, off, span)
    print(flush=True)
    return lens, [(s,) + v for v in lv]


def print_lengths(lens_all):
    bad = collections.Counter((s, ch) for s, ch, ok in lens_all if not ok)
    print(f'\nnote lengths within 20 ms: {np.mean([ok for *_, ok in lens_all]) * 100:.1f}% of '
          f'{len(lens_all)} lone notes; the misses by (song, MIDI channel 1-16): '
          + ', '.join(f'{s:02X}/{ch + 1}: {n}' for (s, ch), n in bad.most_common(8)))


def print_levels(lvl_all):
    med = np.median([v[3] for v in lvl_all])
    g = collections.defaultdict(list)
    for s, ch, prog, d in lvl_all:
        g[(s, ch, prog)].append(d - med)
    rows = sorted(((np.median(v), k, len(v)) for k, v in g.items() if len(v) >= 5), key=lambda x: -abs(x[0]))
    print(f'levels: board TL = MIDI + {med:.1f} dB (median over {len(lvl_all)} lone notes); '
          f'{sum(1 for x in rows if abs(x[0]) < 1)} of {len(rows)} (song, channel, program) groups within 1 dB. '
          'Furthest: ' + ', '.join(f'{k[0]:02X}/{k[1] + 1}/{k[2]} {v:+.1f}' for v, k, n in rows[:6]))


def cmd_grade(mid_dir, board_dir, wav_dir, secs, songs):
    print('song  notes midi/board  rate    onsets  err ms  pitch   lengths  levels  sd dB', end='')
    if wav_dir:
        print('  | env   chroma  band dB board-render ' + ' '.join(f'{a}-{b}' for a, b in BANDS), end='')
    print()
    lens_all, lvl_all = [], []
    for s in songs:
        got = grade_song(s, mid_dir, board_dir, wav_dir, secs)
        if got:
            lens_all += got[0]
            lvl_all += got[1]
    if lens_all:
        print_lengths(lens_all)
    if lvl_all:
        print_levels(lvl_all)


def pitch_rows(mn, on, r, off, span, roots):
    """(note, expected cents, board - expected) per note, each paired with the
    unused key-on within 12 ms nearest its pitch; None when there is none."""
    bt, used, rows = np.array([e['t'] for e in on]), set(), []
    for n in mn:
        x = n['t'] * r + off
        if x >= span:
            break
        z = [root for lo, hi, root in roots.get(n['prog'], []) if lo <= n['note'] <= hi]
        if not z:
            rows.append((n, None, None)); continue
        exp = 100 * (n['note'] - z[0]) + 200 * n['bend'] / 8192     # GM's 2-semitone bend
        k = [j for j in np.nonzero(abs(bt - x) < 0.012)[0] if j not in used]
        if not k:
            rows.append((n, exp, None)); continue
        j = min(k, key=lambda j: abs(on[j]['cents'] - exp)); used.add(j)
        rows.append((n, exp, on[j]['cents'] - exp))
    return rows


def pitch_report(rows):
    d = np.array([x[2] for s, x in rows if x[2] is not None])
    print(f'\n{len(rows)} notes, {len(d)} paired with a key-on, '
          f'{sum(1 for s, x in rows if x[1] is None)} with no SF2 zone; of the paired, '
          + ', '.join(f'{np.mean(abs(d) <= t) * 100:.2f}% within {t} c' for t in (1, 5, 50)))
    g = collections.defaultdict(list)
    for s, (n, e, dd) in rows:
        if dd is not None:
            g[(s, n['ch'], n['prog'])].append(dd)
    bad = sorted(((np.mean(abs(np.array(v)) > 5), k, len(v), np.median(v)) for k, v in g.items()),
                 key=lambda x: -x[0] * x[2])
    print('(song, MIDI channel 1-16, program) groups with notes off by more than 5 c: '
          + (', '.join(f'{k[0]:02X}/{k[1] + 1}/{k[2]} {f * n:.0f} of {n} (median {m:+.0f} c)'
                       for f, k, n, m in bad[:8] if f > 0) or 'none'))


def cmd_pitch(mid_dir, board_dir, sf2, secs, songs):
    roots = sf2_roots(sf2)
    print('song  notes  paired  within 1 c  within 5 c  median c')
    rows = []
    for s in songs:
        mn = midi_notes(os.path.join(mid_dir, f'SONG_{s:02X}.MID'))
        on, t0 = board_capture(os.path.join(board_dir, f'song{s:02X}'), s)
        if not mn or not on:
            continue
        r, off, sc, med = fit(onsets([n['t'] for n in mn]), onsets([e['t'] for e in on]))
        span = min(secs, on[-1]['t'] + 0.5, mn[-1]['t'] * r + off + 0.5)
        rs = pitch_rows(mn, on, r, off, span, roots)
        rows += [(s, x) for x in rs]
        d = np.array([x[2] for x in rs if x[2] is not None] or [math.nan])
        print(f'{s:02X}  {len(rs):6d}  {len(d):6d}  {np.mean(abs(d) <= 1) * 100:9.2f}%  '
              f'{np.mean(abs(d) <= 5) * 100:9.2f}%  {np.median(d):+7.1f}', flush=True)
    pitch_report(rows)


def _opts(rest):
    secs, songs = 88.0, []
    while rest:
        a = rest.pop(0)
        if a == '--secs':
            secs = float(rest.pop(0))
        else:
            songs.append(int(a, 16))
    return secs, songs or range(0x19)


def main(argv):
    if len(argv) >= 2 and argv[0] == 'stim':
        return cmd_stim(argv[1])
    if len(argv) >= 3 and argv[0] == 'fix':
        return cmd_fix(argv[1], argv[2])
    if len(argv) >= 4 and argv[0] == 'pitch':
        return cmd_pitch(argv[1], argv[2], argv[3], *_opts(argv[4:]))
    if len(argv) >= 3 and argv[0] == 'grade':
        rest, wav_dir, secs, songs = argv[3:], None, 88.0, []
        while rest:
            a = rest.pop(0)
            if a == '--wav':
                wav_dir = rest.pop(0)
            elif a == '--secs':
                secs = float(rest.pop(0))
            else:
                songs.append(int(a, 16))
        return cmd_grade(argv[1], argv[2], wav_dir, secs, songs or range(0x19))
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]) or 0)
