"""Decode the PS3 Sonic the Fighters (NPUB30927) sprite database, sprite sets and textures.

Formats (all big-endian except the DDS payloads, which are ordinary little-endian DX9 DDS):

sprdb.bin
  u32 setCount, u32 setTableOff
  set[i] (20 bytes): u32 setId, u32 nameOff, u32 fileNameOff, u32 sprListPtr, u32 texListPtr
    *sprListPtr -> (u32 count, u32 off) -> count x (u32 spriteId, u32 nameOff)     sorted by name
    *texListPtr -> (u32 count, u32 off) -> count x (u32 0x10000|texId, u32 nameOff) sorted by name
  The list order (by name) is the sprite / texture index inside the set.

<set>.farc  (FArc) -> sprite.bin + texture.farc
sprite.bin
  u32 texCount, u32 sprCount, u32 sprOffTableOff (=0xC), u32 sprOff[sprCount]
  sprite (0x30 bytes): u32 texIndex, u32 resMode (9 = 1280x720 on every sprite here),
                       u32 0, u32 0, f32 u0, v0, u1, v1, f32 x, y, w, h   (pixels, top-left origin)
texture.farc (FArc) -> <name>.dds, ordered by name == texIndex. DXT5 or A8R8G8B8, one mip, linear.
"""
import struct, os, sys, json, zlib
import numpy as np
from PIL import Image

# extract.py sets these: where the unpacked psarc is, and where to write.
HERE = os.environ.get('PS3UI_SPRITES', os.path.dirname(os.path.abspath(__file__)))
ROOT = os.environ.get('PS3UI_OUT', os.path.join(HERE, '..', 'out'))
SPR = os.path.join(ROOT, 'sprite')


def farc_entries(d):
    m = d[:4]; hs = struct.unpack('>I', d[4:8])[0]; p = 12
    out = []
    while p < 8 + hs:
        e = d.index(b'\0', p); n = d[p:e].decode(); p = e + 1
        if m == b'FArc':
            off, sz = struct.unpack('>II', d[p:p + 8]); p += 8; out.append((n, d[off:off + sz]))
        elif m == b'FArC':
            off, csz, sz = struct.unpack('>III', d[p:p + 12]); p += 12
            out.append((n, zlib.decompress(d[off:off + csz], 31)))
        else:
            raise ValueError(m)
    return out


def _565(c):
    r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.int32)


def decode_dxt5(buf, w, h):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    a = np.frombuffer(buf, np.uint8, bw * bh * 16).reshape(bh, bw, 16)
    a0 = a[..., 0].astype(np.int32); a1 = a[..., 1].astype(np.int32)
    abits = np.zeros((bh, bw), np.uint64)
    for i in range(6):
        abits |= a[..., 2 + i].astype(np.uint64) << np.uint64(8 * i)
    aidx = np.stack([((abits >> np.uint64(3 * k)) & np.uint64(7)).astype(np.int32) for k in range(16)], -1)
    pal = np.zeros((bh, bw, 8), np.int32)
    pal[..., 0] = a0; pal[..., 1] = a1
    big = a0 > a1
    for k in range(1, 7):
        pal[..., k + 1] = np.where(big, ((7 - k) * a0 + k * a1) // 7, pal[..., k + 1])
    for k in range(1, 5):
        pal[..., k + 1] = np.where(~big, ((5 - k) * a0 + k * a1) // 5, pal[..., k + 1])
    pal[..., 6] = np.where(~big, 0, pal[..., 6]); pal[..., 7] = np.where(~big, 255, pal[..., 7])
    alpha = np.take_along_axis(pal, aidx, -1)
    c0 = a[..., 8].astype(np.int32) | (a[..., 9].astype(np.int32) << 8)
    c1 = a[..., 10].astype(np.int32) | (a[..., 11].astype(np.int32) << 8)
    bits = (a[..., 12].astype(np.int64) | (a[..., 13].astype(np.int64) << 8) |
            (a[..., 14].astype(np.int64) << 16) | (a[..., 15].astype(np.int64) << 24))
    C0 = _565(c0); C1 = _565(c1)
    cp = np.stack([C0, C1, (2 * C0 + C1) // 3, (C0 + 2 * C1) // 3], 2)  # DXT5 always 4-colour
    cidx = np.stack([((bits >> (2 * k)) & 3).astype(np.int32) for k in range(16)], -1)
    col = np.take_along_axis(cp, cidx[..., None].repeat(3, -1), 2)
    rgba = np.concatenate([col, alpha[..., None]], -1).astype(np.uint8)  # bh,bw,16,4
    img = rgba.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w]


def decode_dds(d):
    h, w = struct.unpack('<II', d[12:20])
    pf_flags, fourcc, bpp, rm, gm, bm, am = struct.unpack('<I4sIIIII', d[80:108])
    body = d[128:]
    if pf_flags & 4 and fourcc == b'DXT5':
        return decode_dxt5(body, w, h), 'DXT5'
    if pf_flags & 0x40 and bpp == 32 and (rm, gm, bm, am) == (0xff0000, 0xff00, 0xff, 0xff000000):
        p = np.frombuffer(body, np.uint8, w * h * 4).reshape(h, w, 4)
        return p[..., [2, 1, 0, 3]].copy(), 'A8R8G8B8'
    raise ValueError((pf_flags, fourcc, bpp))


def cstr(d, o):
    return d[o:d.index(b'\0', o)].decode('ascii')


def read_sprdb():
    d = open(os.path.join(SPR, 'sprdb.bin'), 'rb').read()
    n, off = struct.unpack('>II', d[:8]); sets = {}
    for i in range(n):
        sid, no, fo, sp, tp = struct.unpack('>5I', d[off + i * 20:off + i * 20 + 20])
        sc, so = struct.unpack('>II', d[sp:sp + 8]); tc, to = struct.unpack('>II', d[tp:tp + 8])
        spr = [(struct.unpack('>I', d[so + j * 8:so + j * 8 + 4])[0], cstr(d, struct.unpack('>I', d[so + j * 8 + 4:so + j * 8 + 8])[0])) for j in range(sc)]
        tex = [(struct.unpack('>I', d[to + j * 8:to + j * 8 + 4])[0], cstr(d, struct.unpack('>I', d[to + j * 8 + 4:to + j * 8 + 8])[0])) for j in range(tc)]
        sets[cstr(d, no)] = dict(id=sid, file=cstr(d, fo), sprites=spr, textures=tex)
    return sets


def main():
    sets = read_sprdb()
    allspr = []
    for name in ['n_cmn', 'n_info', 'n_stf', 'n_advstf', 'n_fnt']:
        farc = os.path.join(SPR, name + '.farc')
        ents = dict(farc_entries(open(farc, 'rb').read()))
        sb = ents['sprite.bin']
        texs = farc_entries(ents['texture.farc'])
        od = os.path.join(HERE, name); os.makedirs(os.path.join(od, 'spr'), exist_ok=True)
        imgs = []
        for i, (tn, td) in enumerate(texs):
            img, fmt = decode_dds(td)
            Image.fromarray(img, 'RGBA').save(os.path.join(od, 'tex_%d.png' % i))
            imgs.append(img)
            print(name, i, tn, fmt, img.shape[1], img.shape[0])
        ntex, nspr, toff = struct.unpack('>III', sb[:12])
        db = sets[name]
        assert ntex == len(texs) == len(db['textures']) and nspr == len(db['sprites'])
        for j in range(nspr):
            o = struct.unpack('>I', sb[toff + 4 * j:toff + 4 * j + 4])[0]
            ti, mode, z0, z1 = struct.unpack('>4I', sb[o:o + 16])
            u0, v0, u1, v1, x, y, w, h = struct.unpack('>8f', sb[o + 16:o + 48])
            sid, sname = db['sprites'][j]
            img = imgs[ti]
            H, W = img.shape[:2]
            xi, yi, wi, hi = int(round(x)), int(round(y)), int(round(w)), int(round(h))
            crop = img[yi:yi + hi, xi:xi + wi]
            Image.fromarray(crop, 'RGBA').save(os.path.join(od, 'spr', sname + '.png'))
            allspr.append(dict(set=name, set_id=db['id'], index=j, id=sid, name=sname, texture=ti,
                               texture_name=db['textures'][ti][1], texture_size=[W, H],
                               rect=[x, y, w, h], uv=[u0, v0, u1, v1], res_mode=mode,
                               uv_check=[round(u0 * W, 2), round(v0 * H, 2), round(u1 * W, 2), round(v1 * H, 2)]))
    json.dump(allspr, open(os.path.join(HERE, 'sprites.json'), 'w'), indent=1)
    json.dump({k: v for k, v in sets.items()}, open(os.path.join(HERE, 'sprdb.json'), 'w'), indent=1)


def dump_strings():
    """string_array.farc: FArc of string_array_{en,jp}.bin. Each .bin: BE u32 offset table (its length = first
    offset / 4; 0 = no string), then NUL-terminated UTF-8. Button glyphs are Private Use codepoints U+E000.."""
    ents = farc_entries(open(os.path.join(ROOT, 'string_array.farc'), 'rb').read())
    out = {}
    for n, d in ents:
        lang = n.split('_')[-1].split('.')[0]
        first = struct.unpack('>I', d[:4])[0]
        offs = struct.unpack('>%dI' % (first // 4), d[:first])
        out[lang] = [None if o == 0 else d[o:d.index(b'\0', o)].decode('utf-8') for o in offs]
    json.dump(out, open(os.path.join(HERE, 'strings.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=0)
    with open(os.path.join(HERE, 'strings.txt'), 'w', encoding='utf-8') as f:
        for i in range(max(len(v) for v in out.values())):
            for lang in sorted(out):
                v = out[lang][i] if i < len(out[lang]) else None
                if v is not None:
                    f.write('%d\t%s\t%s\n' % (i, lang, v.replace('\n', '\\n')))
    return out


def read_fontmap():
    """fontmap.bin (FMH3, LITTLE-endian unlike everything else here):
      char[4] 'FMH3', u32 0, u32 fontCount, u32 fontTableOff, u32 fontOff[fontCount]
      font: u32 id, u8 glyphW, u8 glyphH, u8 cellW, u8 cellH, u8 unk[4] (03 01 02 00 in both), u32 0,
            u32 columns, u32 charCount, u32 charsOff (absolute), u32 0
      char (8 bytes): u16 code (UTF-16), u8 halfwidth, u8 0, u8 col, u8 row, u8 inkLeft, u8 inkWidth
      glyph cell = (col*cellW, row*cellH, glyphW, glyphH); ink spans [inkLeft, inkLeft+inkWidth) inside it."""
    d = open(os.path.join(ROOT, 'fontmap.farc.d', 'fontmap.bin'), 'rb').read()
    assert d[:4] == b'FMH3'
    _, n, tab = struct.unpack('<III', d[4:16])
    fonts = []
    for i in range(n):
        o = struct.unpack('<I', d[tab + 4 * i:tab + 4 * i + 4])[0]
        fid, gw, gh, cw, ch = struct.unpack('<I4B', d[o:o + 8])
        unk = list(d[o + 8:o + 12])
        cols, cnt, co = struct.unpack('<III', d[o + 16:o + 28])
        chars = []
        for j in range(cnt):
            code, hw, z, col, row, ofs, w = struct.unpack('<HBBBBBB', d[co + 8 * j:co + 8 * j + 8])
            chars.append(dict(code=code, char=chr(code), halfwidth=hw, col=col, row=row, ink_left=ofs, ink_width=w,
                              cell=[col * cw, row * ch, gw, gh]))
        fonts.append(dict(id=fid, glyph_w=gw, glyph_h=gh, cell_w=cw, cell_h=ch, unk=unk, columns=cols, count=cnt, chars=chars))
    return fonts


FONT_SPRITE = {1: (2, 'n_fnt46_0'), 2: (1, 'asc_fnt_62x56')}  # fontmap id -> n_fnt texture / sprite


def dump_fonts():
    from PIL import ImageDraw
    fonts = read_fontmap()
    od = os.path.join(HERE, 'fonts'); os.makedirs(od, exist_ok=True)
    for f in fonts:
        ti, sname = FONT_SPRITE[f['id']]
        f['sprite'] = 'n_fnt/' + sname; f['texture'] = 'n_fnt/tex_%d.png' % ti
        atlas = np.array(Image.open(os.path.join(HERE, 'n_fnt', 'tex_%d.png' % ti)))
        for c in f['chars']:
            x, y, w, h = c['cell']
            a = atlas[y:y + h, x:x + w, 3]
            rows = np.nonzero(a.max(1) >= 128)[0]; cols = np.nonzero(a.max(0) >= 128)[0]
            c['ink_bbox_a128'] = None if len(rows) == 0 else [int(cols[0]), int(rows[0]), int(cols[-1]) + 1, int(rows[-1]) + 1]
        # glyph sheet: ASCII + Latin-1 + a sample of the rest, 2x scale for the small ones
        ascii_chars = [c for c in f['chars'] if c['code'] < 0x3000]
        sc = 2 if f['glyph_h'] < 50 else 1
        per = 16; cw = f['glyph_w'] * sc + 6; ch = f['glyph_h'] * sc + 18
        rows = (len(ascii_chars) + per - 1) // per
        sheet = Image.new('RGBA', (per * cw, rows * ch), (24, 24, 40, 255)); dr = ImageDraw.Draw(sheet)
        for k, c in enumerate(ascii_chars):
            x, y, w, h = c['cell']
            g = Image.fromarray(atlas[y:y + h, x:x + w].copy(), 'RGBA').resize((w * sc, h * sc), Image.NEAREST)
            px, py = (k % per) * cw + 3, (k // per) * ch + 14
            dr.rectangle([px - 1, py - 1, px + w * sc, py + h * sc], outline=(70, 70, 110, 255))
            sheet.alpha_composite(g, (px, py))
            l, r = c['ink_left'] * sc, (c['ink_left'] + c['ink_width']) * sc
            dr.line([px + l, py + h * sc, px + r - 1, py + h * sc], fill=(255, 80, 80, 255))
            dr.text((px, py - 13), 'U+%04X %d' % (c['code'], c['ink_width']), fill=(255, 220, 0, 255))
        sheet.save(os.path.join(od, 'font%d_%s_sheet.png' % (f['id'], sname)))
        bg = Image.new('RGBA', (atlas.shape[1], atlas.shape[0]), (0, 0, 0, 255))
        bg.alpha_composite(Image.fromarray(atlas, 'RGBA')); bg.convert('RGB').save(os.path.join(od, 'font%d_%s_atlas_on_black.png' % (f['id'], sname)))
    json.dump(fonts, open(os.path.join(HERE, 'fontmap.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=0)
    # the 10x16 debug font has no fontmap entry: plain 16x16 grid of ASCII 0..255 in n_fnt tex_0 (160x256)


if __name__ == '__main__':
    main()
    dump_strings()
    dump_fonts()
