#!/usr/bin/env python3
"""Grade the PS3-style UI against the PS3's own.

The references are the PS3 port's decoded sprites and a capture of the real
screen, which stay outside the repo (see README.md for how to make them).

  python tools/ps3ui/grade.py sprites --ours <dir> --ref <dir>
      each painted sprite against the PS3's: mean and max error, alpha-weighted,
      worst first.

  python tools/ps3ui/grade.py screen --ours <png> --ref <png> [--out <dir>]
      a rendered screen against a capture: our render is scaled to the
      capture's size (RPCS3 captures are the 1280x720 frame, scaled) and
      diffed. Writes side.png (ours | theirs) and diff.png (|ours - theirs| x4).

Needs Pillow and numpy.
"""
import argparse, os, sys
import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert('RGBA')).astype(np.float64)


def premul(a):
    return np.concatenate([a[..., :3] * a[..., 3:4] / 255.0, a[..., 3:4]], axis=-1)


def grade_sprites(ours, ref):
    rows = []
    for name in sorted(os.listdir(ours)):
        rp = os.path.join(ref, name)
        if not os.path.exists(rp):
            continue
        a, b = load(os.path.join(ours, name)), load(rp)
        if a.shape != b.shape:
            rows.append((1e9, name, 'size %s vs %s' % (a.shape[:2], b.shape[:2])))
            continue
        d = np.abs(premul(a) - premul(b))
        if d.size == 0:
            continue
        rows.append((d.mean(), name, 'mean %5.2f  p99 %6.1f  max %6.1f' % (d.mean(), np.percentile(d, 99), d.max())))
    rows.sort(reverse=True)
    for m, name, text in rows:
        print('%-44s %s' % (name, text))
    if rows:
        print('sprites: %d, mean of means %.2f' % (len(rows), np.mean([r[0] for r in rows if r[0] < 1e9])))


def fit_capture(a, b):
    """Scale our frame onto the capture. A window capture is rarely a clean
    scale of the frame (a border line, a row or two cropped), so search the
    placement: x/y scale and offset, by mean error on a downsampled image."""
    B = np.asarray(b).astype(np.float64)
    best = None
    W, H = b.size
    for sx in np.arange(0.990, 1.0101, 0.0025):
        for sy in np.arange(0.990, 1.0101, 0.0025):
            w, h = int(round(W * sx)), int(round(H * sy))
            r = np.asarray(a.resize((w, h), Image.BILINEAR)).astype(np.float64)
            for dx in range(-3, 4):
                for dy in range(-3, 4):
                    x0, y0 = max(0, dx), max(0, dy)
                    x1, y1 = min(W, w + dx), min(H, h + dy)
                    if x1 - x0 < W * 0.9 or y1 - y0 < H * 0.9:
                        continue
                    d = np.abs(r[y0 - dy:y1 - dy:4, x0 - dx:x1 - dx:4] - B[y0:y1:4, x0:x1:4]).mean()
                    if best is None or d < best[0]:
                        best = (d, sx, sy, dx, dy)
    d, sx, sy, dx, dy = best
    print('placement: scale %.4f x %.4f, offset %+d,%+d' % (sx, sy, dx, dy))
    w, h = int(round(W * sx)), int(round(H * sy))
    canvas = Image.new('RGB', b.size)
    canvas.paste(a.resize((w, h), Image.BILINEAR), (dx, dy))
    return canvas


def grade_screen(ours, ref, out):
    b = Image.open(ref).convert('RGB')
    a = fit_capture(Image.open(ours).convert('RGB'), b)
    A, B = np.asarray(a).astype(np.float64), np.asarray(b).astype(np.float64)
    # the capture's own frame edge (a window border) is not ours to grade
    A, B = A[3:-3, 3:-3], B[3:-3, 3:-3]
    d = np.abs(A - B)
    print('screen: mean %.2f  p90 %.1f  p99 %.1f  max %.0f' % (d.mean(), np.percentile(d, 90), np.percentile(d, 99), d.max()))
    # error by horizontal band of the frame, to point at the element that is off
    h = B.shape[0]
    for i in range(10):
        y0, y1 = h * i // 10, h * (i + 1) // 10
        print('  rows %3d-%3d  mean %.2f' % (y0, y1, d[y0:y1].mean()))
    if out:
        os.makedirs(out, exist_ok=True)
        side = Image.new('RGB', (b.size[0] * 2, b.size[1]))
        side.paste(a, (0, 0))
        side.paste(b, (b.size[0], 0))
        side.save(os.path.join(out, 'side.png'))
        Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(os.path.join(out, 'diff.png'))
        Image.fromarray(A.astype(np.uint8)).save(os.path.join(out, 'ours_fit.png'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('what', choices=['sprites', 'screen'])
    ap.add_argument('--ours', required=True)
    ap.add_argument('--ref', required=True)
    ap.add_argument('--out')
    a = ap.parse_args()
    if a.what == 'sprites':
        grade_sprites(a.ours, a.ref)
    else:
        grade_screen(a.ours, a.ref, a.out)


if __name__ == '__main__':
    sys.exit(main())
