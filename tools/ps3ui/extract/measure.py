"""Pixel measurements of the lobby sprites (run after sprdump.py). Prints run-length profiles.
usage: python measure.py name[:col|row spec] ...   e.g.  python measure.py hw_pla_ovr_cnr
"""
import sys, os
import numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))


def load(n):
    for s in ('n_cmn', 'n_stf', 'n_info'):
        p = os.path.join(HERE, s, 'spr', n if n.startswith('n_') else '%s_%s' % (s, n)) + '.png'
        if os.path.exists(p):
            return np.array(Image.open(p)).astype(int)
    raise FileNotFoundError(n)


def hx(p):
    return '%02X%02X%02X/%02X' % tuple(int(round(v)) for v in p)


def runs(seq, tol=0):
    out = []; start = 0
    for i in range(1, len(seq) + 1):
        if i == len(seq) or np.abs(seq[i] - seq[start]).max() > tol:
            out.append((start, i - 1, seq[start])); start = i
    return out


def fmt_runs(seq, tol=0):
    return ' '.join(('%d' % a if a == b else '%d-%d' % (a, b)) + ':' + hx(v) for a, b, v in runs(seq, tol))


def profile(n, tol=0):
    a = load(n); h, w = a.shape[:2]
    print('== %s %dx%d' % (n, w, h))
    al = a[..., 3]
    ys, xs = np.nonzero(al > 0)
    if len(ys):
        print('  alpha>0 bbox x %d..%d y %d..%d' % (xs.min(), xs.max(), ys.min(), ys.max()))
    for x in sorted(set([0, w // 4, w // 2, w - 1 - w // 4, w - 1])):
        print('  col x=%d (top->bottom): %s' % (x, fmt_runs(a[:, x], tol)))
    for y in sorted(set([0, h // 4, h // 2, h - 1 - h // 4, h - 1])):
        print('  row y=%d (left->right): %s' % (y, fmt_runs(a[y, :], tol)))


def glow(n, core_alpha=250, maxr=30):
    """Glow falloff: mean RGBA of pixels at Euclidean distance r outside the solid core (alpha >= core_alpha),
    and of the core at depth r inside it."""
    from scipy import ndimage
    a = load(n).astype(float); al = a[..., 3]
    core = al >= core_alpha
    print('== glow %s core(alpha>=%d) pixels=%d' % (n, core_alpha, core.sum()))
    ys, xs = np.nonzero(core)
    if len(ys):
        print('  core bbox x %d..%d y %d..%d' % (xs.min(), xs.max(), ys.min(), ys.max()))
    lab, cnt = ndimage.label(core)
    for i, o in enumerate(ndimage.find_objects(lab)):
        if (lab[o] == i + 1).sum() > 20:
            print('  component x %d..%d y %d..%d  px=%d' % (o[1].start, o[1].stop - 1, o[0].start, o[0].stop - 1, (lab == i + 1).sum()))
    d = ndimage.distance_transform_edt(~core)
    for r in range(1, maxr):
        m = (d > r - 0.5) & (d <= r + 0.5)
        if m.sum():
            print('  out r=%2d n=%5d rgb=%s alpha=%5.1f' % (r, m.sum(), hx(np.r_[a[m][:, :3].mean(0), 255])[:6], al[m].mean()))
    di = ndimage.distance_transform_edt(core)
    for r in range(1, 12):
        m = (di > r - 0.5) & (di <= r + 0.5)
        if m.sum():
            print('  in  r=%2d n=%5d rgba=%s' % (r, m.sum(), hx(np.r_[a[m][:, :3].mean(0), al[m].mean()])))


def fitglow(n, core_alpha=250):
    """Fit glow alpha outside the core as k * gaussian_blur(core_mask, sigma) (least squares over the halo)."""
    from scipy import ndimage
    a = load(n).astype(float); al = a[..., 3]; core = al >= core_alpha
    pad = 40
    cm = np.pad(core.astype(float), pad); alp = np.pad(al, pad)
    halo = np.pad(~core, pad, constant_values=True) & (alp > 0) | (np.pad(np.ones_like(al, bool), pad, constant_values=False) & ~np.pad(core, pad))
    best = None
    for s in np.arange(2, 20.01, 0.25):
        b = ndimage.gaussian_filter(cm, s)
        m = halo
        k = (b[m] * alp[m]).sum() / max((b[m] ** 2).sum(), 1e-9)
        err = np.sqrt(((k * b[m] - alp[m]) ** 2).mean())
        if best is None or err < best[0]:
            best = (err, s, k)
    print('== fitglow %s: alpha_out ~= %.1f * gauss(core, sigma=%.2f px)  rms err %.2f alpha units' % (n, best[2], best[1], best[0]))
    return best


def fillrows(n, depth=3, core_alpha=250):
    """Median colour of the letter fill per row (pixels at least `depth` px inside the core)."""
    from scipy import ndimage
    a = load(n); al = a[..., 3]; core = al >= core_alpha
    di = ndimage.distance_transform_edt(core)
    print('== fill rows %s (>= %d px inside)' % (n, depth))
    rows = []
    for y in range(a.shape[0]):
        m = di[y] >= depth
        if m.sum():
            rows.append(np.median(a[y][m], 0))
        else:
            rows.append(None)
    last = None
    for y, r in enumerate(rows):
        if r is not None:
            print('  y=%3d %s' % (y, hx(r)))


if __name__ == '__main__':
    tol = 0
    for arg in sys.argv[1:]:
        if arg.startswith('--tol='):
            tol = int(arg[6:]); continue
        if arg.startswith('glow:'):
            glow(arg[5:]); continue
        profile(arg, tol)
