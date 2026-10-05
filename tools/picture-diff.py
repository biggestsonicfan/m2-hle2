#!/usr/bin/env python3
"""picture-diff.py -- two sets of pictures of the same frames, side by side and
measured (Pinboard #486).

  tools/picture-diff.py DIR [--out OUTDIR] [--a m] [--b d] [--frame 640x480+72+48]

DIR holds pairs named <a><frame>.png and <b><frame>.png: what
dc-lockstep.py --boot --shots leaves (m = MAME's screen, d = the Dreamcast's as
RetroArch saw it). A picture that is not 496x384 is the Dreamcast's whole
640x480 frame at whatever size RetroArch saved it; --frame says where the
board's 496x384 sits in it, and that part is cut out and box-filtered down to
496x384 before anything is measured. The default, auto, tries every place
within 8 pixels of dc_pvr.h's (72, 48) on the first pair that is not black and
keeps the one nearest the first set: Flycast shows the frame 4 pixels right of
where the PVR puts it.

For each frame it prints:
  mad     the mean absolute difference over every channel of every pixel (0-255)
  off%    the share of pixels where some channel differs by more than 32
  hist    the intersection of the two pictures' colour histograms (8 levels a
          channel, 512 bins): 1.000 is the same colours in the same amounts,
          wherever they are; a low value is a colour or a whole layer missing
  rows    the board rows (of 384, in 8-row bands) where off% is over 10%

and OUTDIR gets <frame>.png for each pair (a | b | the difference, amplified 4x)
and sheet.png, every pair in one picture, a row each.
"""
import argparse, os, re, sys
from PIL import Image, ImageChops

W, H = 496, 384


def board(path, frame):
    im = Image.open(path).convert('RGB')
    if im.size == (W, H):
        return im
    fw, fh, x0, y0 = frame
    sx, sy = im.width / fw, im.height / fh
    box = (round(x0 * sx), round(y0 * sy), round((x0 + W) * sx), round((y0 + H) * sy))
    return im.crop(box).resize((W, H), Image.BOX)


def hist(im):
    q = im.point(lambda v: v >> 5)
    r, g, b = q.split()
    h = [0] * 512
    for rv, gv, bv in zip(r.get_flattened_data(), g.get_flattened_data(), b.get_flattened_data()):
        h[rv * 64 + gv * 8 + bv] += 1
    return h


def measure(a, b):
    d = ImageChops.difference(a, b)
    dr, dg, db = d.split()
    worst = ImageChops.lighter(ImageChops.lighter(dr, dg), db)
    px = list(worst.get_flattened_data())
    mad = sum(sum(c) for c in zip(dr.get_flattened_data(), dg.get_flattened_data(), db.get_flattened_data())) / (3 * W * H)
    off = [v > 32 for v in px]
    ha, hb = hist(a), hist(b)
    inter = sum(min(x, y) for x, y in zip(ha, hb)) / (W * H)
    bands = []
    for y in range(0, H, 8):
        n = sum(off[y * W:(y + 8) * W])
        if n > 0.10 * 8 * W:
            bands.append(y)
    return mad, 100.0 * sum(off) / len(off), inter, bands, d


def spans(bands):
    out, s = [], None
    for y in bands + [None]:
        if s is not None and (y is None or y != p + 8):
            out.append('%d-%d' % (s, p + 7))
            s = None
        if y is not None and s is None:
            s = y
        p = y
    return ' '.join(out) or '-'


def register(frames, path, a):
    """The place in the 640x480 frame where the second set's board is nearest
    the first's, on the first pair whose first picture is not black."""
    for fr in frames:
        ia = Image.open(path(a.a, fr)).convert('RGB')
        if ia.convert('L').getextrema()[1] > 32:
            break
    else:
        return (640, 480, 72, 48)
    best = None
    for y in range(40, 57):
        for x in range(64, 81):
            f = (640, 480, x, y)
            ib = board(path(a.b, fr), f)
            d = ImageChops.difference(ia, ib).convert('L')
            s = sum(d.histogram()[k] * k for k in range(256))
            if best is None or s < best[0]:
                best = (s, f)
    return best[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dir')
    ap.add_argument('--out')
    ap.add_argument('--a', default='m', help='prefix of the first set (default m, MAME)')
    ap.add_argument('--b', default='d', help='prefix of the second set (default d, the Dreamcast)')
    ap.add_argument('--frame', default='auto',
                    help="WxH+X+Y: where the board's 496x384 sits in a bigger picture (dc_pvr.h DC_X0/DC_Y0)")
    ap.add_argument('--fps', type=float, default=60.0, help='to print each frame as seconds too')
    a = ap.parse_args()
    pat = re.compile(re.escape(a.a) + r'(\d+)\.png$')
    frames = sorted(int(pat.match(f).group(1)) for f in os.listdir(a.dir) if pat.match(f))
    path = lambda pre, fr: os.path.join(a.dir, '%s%05d.png' % (pre, fr))
    frames = [fr for fr in frames if os.path.exists(path(a.b, fr))]
    if a.frame == 'auto':
        frame = register(frames, path, a)
        print('the board sits at +%d+%d of the 640x480 frame' % frame[2:])
    else:
        m = re.match(r'(\d+)x(\d+)\+(\d+)\+(\d+)$', a.frame)
        if not m:
            ap.error('--frame WxH+X+Y or auto')
        frame = tuple(int(v) for v in m.groups())
    out = a.out or os.path.join(a.dir, 'diff')
    os.makedirs(out, exist_ok=True)
    rows = []
    print('%7s %6s %6s %6s %6s  %s' % ('frame', 's', 'mad', 'off%', 'hist', 'rows over 10% off'))
    for fr in frames:
        ia, ib = board(path(a.a, fr), frame), board(path(a.b, fr), frame)
        mad, offp, inter, bands, d = measure(ia, ib)
        print('%7d %6.1f %6.1f %6.1f %6.3f  %s' % (fr, fr / a.fps, mad, offp, inter, spans(bands)), flush=True)
        trio = Image.new('RGB', (3 * W, H))
        trio.paste(ia, (0, 0))
        trio.paste(ib, (W, 0))
        trio.paste(d.point(lambda v: min(255, 4 * v)), (2 * W, 0))
        trio.save(os.path.join(out, '%05d.png' % fr))
        rows.append(trio)
    if not rows:
        print('no pairs in %s' % a.dir)
        return 1
    scale = 3
    sheet = Image.new('RGB', (3 * W // scale, len(rows) * H // scale))
    for i, t in enumerate(rows):
        sheet.paste(t.resize((3 * W // scale, H // scale), Image.BOX), (0, i * H // scale))
    sheet.save(os.path.join(out, 'sheet.png'))
    print('%d pairs; side by side in %s' % (len(rows), out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
