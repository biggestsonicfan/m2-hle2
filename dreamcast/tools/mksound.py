#!/usr/bin/env python3
"""
mksound.py -- the Dreamcast port's sound disc file (Pinboard #342).

Reads the console release's CRI ADX2 bank (StF - PS3/sound: stf_all.acb with
its stf_all.awb) and writes STF.AFS, a CRI AFS archive of plain ADX files, as
Sega's own Dreamcast games shipped their music:

  entry 0       CUES.BIN: the sound codes the i960 sends, each with its
                category and the AFS entry it plays (see below)
  entry 1..n    one ADX per cue the code table names, in that order

The code table is Sega's: the console DLL's (stf-pxd-w64-d3d12_retail.dll,
0x180126a70, 123 entries), which looks up the code sound_request_special
(0x3F268) takes and plays the cue of that name. Category 5 is music, one at a
time; 2 is effects and voices. CLAUDE.md, "Sound board", has the rest.

The ADX are CRI's standard format (4-bit, 18-byte frames, 500 Hz highpass
coefficients, header version 3 with the loop), encoded by ffmpeg's adpcm_adx.
Music is 44.1 kHz stereo (the HCA is 48 kHz; the AICA's output rate saves the
SH-4 a resampler), effects stay 22,050 Hz mono. Loop points come from each
HCA's 'loop' chunk: start ls*1024 + start_delay, end (le+1)*1024 - end_padding,
both less the encoder delay, which is trimmed off the front.

CUES.BIN (big-endian, like the ADX headers):
  'STFC', u32 count, then count x { u32 code, u8 category, u8 0, u16 entry }
  entry 0xFFFF: the code is in Sega's table with no cue (it plays nothing).
  category 0: a stop code. The DLL stops eight looping cues by name, each on a
  code of its own (0xAE14xx, FUN_180004b80's switch); entry is the cue's ADX.
  Codes are sorted. 0xA00001-3 (stop) and 0xA003xx (fade) are the player's.

Needs ffmpeg with the hca decoder and adx encoder, and numpy.
Usage: mksound.py <StF - PS3/sound dir> <out STF.AFS>
"""
import os
import struct
import subprocess
import sys

import numpy as np

# The DLL's stop codes (FUN_180004b80): code, the looping cue it stops.
STOP_MAP = """
AE1401 plane_2a   AE1404 ufo_6b     AE1405 computer_1  AE1413 computer_3
AE1417 US04_15_01 AE141D beam_6c    AE141F ele_3       AE1421 torocco_2
"""

# Sega's code table (console DLL 0x180126a70): code, category, cue name.
CUE_MAP = """
AE1000 5 bgm00   AE1001 5 bgm01   AE1002 5 bgm02   AE1003 5 bgm03
AE1004 5 bgm04   AE1005 5 bgm05   AE1006 5 bgm06   AE1007 5 bgm07
AE1008 5 bgm08   AE1009 5 bgm09   AE100A 5 bgm0a   AE100B 5 bgm0b
AE100C 5 bgm0c   AE100D 5 bgm0d   AE100F 5 bgm0f   AE1010 5 bgm10
AE1011 5 bgm11   AE1012 5 bgm12   AE1013 5 bgm13   AE1014 5 bgm14_noloop
AE1015 5 bgm15   AE1016 5 bgm16   AE1017 5 bgm17   AE1018 5 bgm18
AE1019 5 -       AE101A 5 -       AE101B 5 -       AE101C 5 -
AE101D 5 -       AE101E 5 -       AE101F 5 -       AE1020 5 bgm14
AE1100 2 jump_1  AE1101 2 jump_2  AE1102 2 ring_2  AE1103 2 spin_1
AE1104 2 spin_2  AE1108 2 fall_2  AE1109 2 fall_6  AE110A 2 fall_9
AE110F 2 punch_a AE1110 2 punch_b AE1111 2 punch_c AE1113 2 punch_e
AE1114 2 punch_f AE1117 2 punch_j AE1118 2 punch_k AE1119 2 punch_m
AE111B 2 punch_o AE111C 2 punch_p AE1124 2 plane_2a AE1126 2 rope_3
AE1128 2 up_2    AE1129 2 up_3    AE112C 2 cane_2d AE112E 2 cork_1a
AE112F 2 cork_1b AE1130 2 cork_6a AE1131 2 cork_6b AE1132 2 cork_6c
AE1133 2 fence_1 AE1134 2 fence_2 AE1136 2 g_punch_1 AE1137 2 g_punch_2
AE1138 2 land_1  AE113B 2 ufo_6b  AE113C 2 ufo_esc_1 AE113D 2 computer_1
AE113F 2 plane_2 AE1145 2 gum_2   AE1147 2 gum_2c  AE114A 2 pico_2a
AE114E 2 pico_2e AE1154 2 pinball_b AE1155 2 pinball_c AE115B 2 pinball_j
AE1168 2 wall_hit_1 AE1169 2 wall_hit_2 AE116A 2 wall_hit_3 AE116C 2 wall_hit_5
AE116E 2 computer_3 AE1172 2 swing_c AE1176 2 swing_g AE1178 2 clash_1
AE117A 2 knock_3 AE117B 2 knock_9 AE1221 2 WB01_26_02 AE1225 2 WB01_28_02
AE122A 2 WB02_15_01 AE122C 2 WB02_24_01 AE122E 2 WB02_25_02 AE122F 2 WB02_29_05
AE1231 2 WB02_67_01 AE1232 2 WB02_70_03 AE1236 2 WB04_62_01 AE1237 2 WB04_62_02
AE1239 2 WB04_63_01 AE123B 2 WB04_81_01 AE123C 2 WB04_82_04 AE1243 2 WB04_92_03
AE1245 2 WB05_51_03 AE124B 2 US02_79_02 AE124F 2 US03_59_04 AE1250 2 US04_11_01
AE1255 2 US04_15_01 AE1302 2 gong_4  AE130B 2 ring_5a AE1318 2 bound_1a
AE1326 2 beam_6c AE1328 2 foot_1  AE1329 2 foot_2  AE132A 2 foot_3
AE132B 2 foot_4  AE132C 2 foot_5  AE133D 2 bomb_1  AE1341 2 ele_1
AE1343 2 ele_3   AE1344 2 ele_4   AE1346 2 shatter_1 AE1347 2 shatter_3
AE1348 2 torocco_2 AE1358 2 laugh0b AE1359 2 bowling_hit
"""

BGM_RATE = 44100
SECTOR = 2048


# ---- CRI @UTF tables (ACB) ----------------------------------------------------

def utf_table(b, off=0):
    assert b[off:off + 4] == b'@UTF', 'not an @UTF table at %#x' % off
    base = off + 8
    rows_off, str_off, data_off, _name, ncol, rowlen, nrows = \
        struct.unpack_from('>IIIIHHI', b, base)

    def string(o):
        s = base + str_off + o
        return b[s:b.index(b'\0', s)].decode('utf8', 'replace')

    def value(p, ty):
        fmt = {0: '>B', 1: '>b', 2: '>H', 3: '>h', 4: '>I', 5: '>i',
               6: '>Q', 7: '>q', 8: '>f'}.get(ty)
        if fmt:
            return struct.unpack_from(fmt, b, p)[0], p + struct.calcsize(fmt)
        if ty == 0xA:
            return string(struct.unpack_from('>I', b, p)[0]), p + 4
        if ty == 0xB:
            o, n = struct.unpack_from('>II', b, p)
            return (base + data_off + o, n), p + 8
        raise ValueError('@UTF column type %#x' % ty)

    cols, p = [], base + 24
    for _ in range(ncol):
        flags = b[p]
        name = string(struct.unpack_from('>I', b, p + 1)[0])
        p += 5
        const = None
        if flags & 0xF0 == 0x30:
            const, p = value(p, flags & 0x0F)
        cols.append((name, flags & 0xF0, flags & 0x0F, const))
    rows = []
    for r in range(nrows):
        p, row = base + rows_off + r * rowlen, {}
        for name, storage, ty, const in cols:
            if storage == 0x50:
                row[name], p = value(p, ty)
            else:
                row[name] = const
        rows.append(row)
    return rows


def afs2_entries(b, base=0):
    """AFS2 (the AWB): {id: (start, end)} as absolute offsets into b."""
    assert b[base:base + 4] == b'AFS2'
    off_size, id_size = b[base + 5], b[base + 6]
    n, align = struct.unpack_from('<IH', b, base + 8)
    p, ids, offs = base + 16, [], []
    for _ in range(n):
        ids.append(int.from_bytes(b[p:p + id_size], 'little'))
        p += id_size
    for _ in range(n + 1):
        offs.append(int.from_bytes(b[p:p + off_size], 'little'))
        p += off_size
    return {ids[i]: (base + (offs[i] + align - 1) // align * align, base + offs[i + 1])
            for i in range(n)}


def load_bank(sound_dir):
    """cue name -> (HCA bytes, streamed)."""
    acb = open(os.path.join(sound_dir, 'stf_all.acb'), 'rb').read()
    awb = open(os.path.join(sound_dir, 'stf_all.awb'), 'rb').read()
    head = utf_table(acb)[0]
    tab = {t: utf_table(acb, head[t][0]) for t in
           ('CueNameTable', 'CueTable', 'SynthTable', 'WaveformTable')}
    mem = afs2_entries(acb, head['AwbFile'][0])
    stream = afs2_entries(awb)

    def waves(synth):
        o, n = tab['SynthTable'][synth]['ReferenceItems']
        out = []
        for k in range(0, n, 4):
            kind, idx = struct.unpack_from('>HH', acb, o + k)
            if kind == 1:
                out.append(tab['WaveformTable'][idx])
            elif kind == 2:
                out += waves(idx)
        return out

    bank = {}
    for row in tab['CueNameTable']:
        cue = tab['CueTable'][row['CueIndex']]
        assert cue['ReferenceType'] == 2, row['CueName']
        w = waves(cue['ReferenceIndex'])
        assert len(w) == 1, row['CueName']
        w = w[0]
        assert w['EncodeType'] == 2, row['CueName']     # HCA
        src, table = (awb, stream) if w['Streaming'] else (acb, mem)
        s, e = table[w['Id']]
        bank[row['CueName']] = src[s:e]
    return bank


# ---- HCA -> PCM ---------------------------------------------------------------

def hca_info(h):
    hsz = struct.unpack_from('>H', h, 6)[0]
    p, info = 8, {'loop': None}
    while p < hsz:
        tag = bytes(c & 0x7F for c in h[p:p + 4])
        if tag == b'fmt\0':
            info['ch'] = h[p + 4]
            info['rate'] = int.from_bytes(h[p + 5:p + 8], 'big')
            info['blocks'], info['delay'], info['pad'] = struct.unpack_from('>IHH', h, p + 8)
            p += 16
        elif tag == b'loop':
            info['loop'] = struct.unpack_from('>IIHH', h, p + 4)
            p += 16
        elif tag in (b'comp', b'loop'):
            p += 16
        elif tag == b'dec\0':
            p += 12
        elif tag in (b'ath\0', b'ciph'):
            if tag == b'ciph':
                assert struct.unpack_from('>H', h, p + 4)[0] == 0, 'encrypted HCA'
            p += 6
        elif tag in (b'rva\0', b'vbr\0'):
            p += 8
        else:
            break
    return info


def ffmpeg(args, data):
    return subprocess.run(['ffmpeg', '-v', 'error'] + args, input=data,
                          stdout=subprocess.PIPE, check=True).stdout


def hca_pcm(h):
    """(int16 array [n, ch], rate, loop (start, end) or None, in samples)."""
    info = hca_info(h)
    ch = info['ch']
    pcm = np.frombuffer(ffmpeg(['-f', 'hca', '-i', '-', '-f', 's16le', '-'], h),
                        np.int16).reshape(-1, ch)
    d = info['delay']               # ffmpeg keeps the encoder delay; drop it
    if info['loop']:
        ls, le, sdelay, epad = info['loop']
        loop = (ls * 1024 + sdelay - d, (le + 1) * 1024 - epad - d)
        end = loop[1]
    else:
        loop = None
        end = info['blocks'] * 1024 - info['pad'] - d
    return pcm[d:d + end], info['rate'], loop


def resample(pcm, rate, loop, to):
    """Resample, with the loop seam continued so the filter sees the wrap."""
    if rate == to:
        return pcm, loop
    ch = pcm.shape[1]
    src = pcm
    if loop:
        src = np.concatenate([pcm[:loop[1]], pcm[loop[0]:loop[0] + 8192]])
    out = np.frombuffer(ffmpeg(['-f', 's16le', '-ar', str(rate), '-ac', str(ch), '-i', '-',
                                '-af', 'aresample=%d' % to, '-f', 's16le', '-'],
                               src.tobytes()), np.int16).reshape(-1, ch)
    scale = to / rate
    if loop:
        loop = (round(loop[0] * scale), round(loop[1] * scale))
        return out[:loop[1]], loop
    return out[:round(len(pcm) * scale)], None


# ---- PCM -> ADX ---------------------------------------------------------------

ADX_DATA = 0x200                    # header + "(c)CRI", then the frames


def adx_file(pcm, rate, loop):
    ch, n = pcm.shape[1], pcm.shape[0]
    enc = ffmpeg(['-f', 's16le', '-ar', str(rate), '-ac', str(ch), '-i', '-',
                  '-c:a', 'adpcm_adx', '-f', 'adx', '-'], pcm.tobytes())
    assert enc[0:2] == b'\x80\x00'
    frames = enc[struct.unpack_from('>H', enc, 2)[0] + 4:]
    nframes = (n + 31) // 32
    frames = frames[:nframes * 18 * ch]
    assert len(frames) == nframes * 18 * ch, 'short ADX from ffmpeg'

    hdr = bytearray(ADX_DATA)
    struct.pack_into('>HHBBBBIIHBB', hdr, 0, 0x8000, ADX_DATA - 4, 3, 18, 4, ch,
                     rate, n, 500, 3, 0)
    if loop:
        ls, le = loop
        struct.pack_into('>HHIIIII', hdr, 0x14, 0, 1, 1,
                         ls, ADX_DATA + ls // 32 * 18 * ch,
                         le, ADX_DATA + (le + 31) // 32 * 18 * ch)
    hdr[ADX_DATA - 6:ADX_DATA] = b'(c)CRI'
    return bytes(hdr) + frames


# ---- AFS ----------------------------------------------------------------------

def afs_file(entries, names):
    """CRI AFS: 'AFS\\0', count, (offset, size) each, then the name table's
    (offset, size); entries and the name table start on a 2048-byte sector."""
    n = len(entries)
    head = 8 + 8 * n + 8
    pos = (head + SECTOR - 1) // SECTOR * SECTOR
    toc, body = [], bytearray()
    for e in entries:
        toc.append((pos, len(e)))
        body += e + bytes(-len(e) % SECTOR)
        pos += len(e) + (-len(e) % SECTOR)
    names_tab = b''.join(nm.encode()[:31].ljust(32, b'\0') + bytes(12) +
                         struct.pack('<I', len(e)) for nm, e in zip(names, entries))
    out = bytearray(b'AFS\0' + struct.pack('<I', n))
    for o, s in toc:
        out += struct.pack('<II', o, s)
    out += struct.pack('<II', pos, len(names_tab))
    out += bytes((head + SECTOR - 1) // SECTOR * SECTOR - len(out))
    return bytes(out + body + names_tab)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    bank = load_bank(sys.argv[1])
    codes = [(int(c, 16), int(cat), nm) for c, cat, nm in
             zip(*[iter(CUE_MAP.split())] * 3)]

    order = []                      # cue names in AFS order (entry 1 on)
    for _, _, nm in codes:
        if nm != '-' and nm not in order:
            order.append(nm)
    entries, total = [], 0
    for i, nm in enumerate(order):
        pcm, rate, loop = hca_pcm(bank[nm])
        if pcm.shape[1] == 2 and rate != BGM_RATE:
            pcm, loop = resample(pcm, rate, loop, BGM_RATE)
            rate = BGM_RATE
        adx = adx_file(pcm, rate, loop)
        entries.append(adx)
        total += len(adx)
        print('%3d %-14s %5d Hz %d ch %8d samples %s %7d bytes' %
              (i + 1, nm, rate, pcm.shape[1], len(pcm),
               'loop %d-%d' % loop if loop else 'once', len(adx)), flush=True)

    codes += [(int(c, 16), 0, nm) for c, nm in zip(*[iter(STOP_MAP.split())] * 2)]
    codes.sort()
    cues = bytearray(b'STFC' + struct.pack('>I', len(codes)))
    for code, cat, nm in codes:
        cues += struct.pack('>IBBH', code, cat, 0,
                            order.index(nm) + 1 if nm != '-' else 0xFFFF)
    with open(sys.argv[2], 'wb') as f:
        f.write(afs_file([bytes(cues)] + entries,
                         ['CUES.BIN'] + [nm.upper() + '.ADX' for nm in order]))
    print('%s: %d cues, %d ADX, %.1f MB' % (sys.argv[2], len(codes), len(order),
                                            os.path.getsize(sys.argv[2]) / 1e6))


if __name__ == '__main__':
    main()
