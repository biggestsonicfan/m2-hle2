#!/usr/bin/env python3
"""Composite an AET composition at a frame, from the JSON written by aetdump.py.

python aetrender.py OUT.png [--boxes] [--size WxH] LAYERSPEC [LAYERSPEC ...]
  LAYERSPEC = set:scene:comp@frame[+dx,dy]   (comp = name or C<index>; drawn in the order given,
              later specs on top).  e.g.  n_cmn:n_cmn_base:bg@100  n_cmn:n_cmn_online:z_base@40
--boxes     outline the source-less placeholder videos (p_*, head_tit_*) in magenta with their names
--size      output size (default 1920x1080, the scenes' native size; output is resampled)

Model (After Effects, y down, degrees clockwise):
  layer matrix  = parent_chain * T(position) * R(rotation) * S(scale) * T(-anchor)
  a comp layer's children run at local time (t - start) * time_scale + offset;
  a layer is live for start <= t < end and only if flags bit0 (video active) is set;
  layers draw from last to first (index 0 is the top);
  opacity multiplies down the nesting; blend 3 = normal (premultiplied over), 5 = add.
  3D layers (only the ring-line effect uses them) are projected orthographically.
"""
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

from aetdump import eval_fcurve

# extract.py's layout: <work>/aet (this JSON), <work>/out (the psarc), <work>/sprites.
HERE = os.environ.get('PS3UI_AET', os.path.dirname(os.path.abspath(__file__)))
SPR = os.path.join(HERE, '..', 'out', 'sprite')
ALT_SPR = os.path.join(HERE, '..', 'sprites')   # sprdump.py's decoded PNGs, if present

_json = {}
_tex = {}
_sprdb = None


def load(set_name):
    if set_name not in _json:
        _json[set_name] = json.load(open(os.path.join(HERE, set_name + '.json')))
    return _json[set_name]


def sprdb():
    global _sprdb
    if _sprdb is None:
        _sprdb = {s['name']: s for s in json.load(open(os.path.join(HERE, 'sprdb.json')))}
    return _sprdb


def texture(set_name, idx):
    key = (set_name, idx)
    if key not in _tex:
        tname = sprdb()[set_name]['textures'][idx]['name'].lower()
        p = os.path.join(SPR, set_name + '.farc.d', 'tex', tname + '.dds')
        _tex[key] = Image.open(p).convert('RGBA')
    return _tex[key]


def sprite_image(set_name, index, margin=1):
    s = sprdb()[set_name]['sprites'][index]
    tex = texture(set_name, s['texture'])
    x, y, w, h = [int(round(v)) for v in s['rect']]
    return tex.crop((x - margin, y - margin, x + w + margin, y + h + margin)), w, h


def mat(a, b, c, d, e, f):
    return np.array([[a, b, c], [d, e, f], [0, 0, 1.0]])


def layer_matrix(vd, t):
    ev = lambda k: eval_fcurve(vd[k], t)
    ax, ay, px, py = ev('anchor_x'), ev('anchor_y'), ev('position_x'), ev('position_y')
    rz, sx, sy = math.radians(ev('rotation')), ev('scale_x'), ev('scale_y')
    A = mat(1, 0, -ax, 0, 1, -ay)
    S = mat(sx, 0, 0, 0, sy, 0)
    c, s = math.cos(rz), math.sin(rz)
    R = mat(c, -s, 0, s, c, 0)
    if '3d' in vd:
        d3 = vd['3d']
        e3 = lambda k: math.radians(eval_fcurve(d3[k], t))
        def rx(a):
            return np.array([[1, 0, 0], [0, math.cos(a), -math.sin(a)], [0, math.sin(a), math.cos(a)]])
        def ry(a):
            return np.array([[math.cos(a), 0, math.sin(a)], [0, 1, 0], [-math.sin(a), 0, math.cos(a)]])
        def rzz(a):
            return np.array([[math.cos(a), -math.sin(a), 0], [math.sin(a), math.cos(a), 0], [0, 0, 1]])
        M3 = (rx(e3('direction_x')) @ ry(e3('direction_y')) @ rzz(e3('direction_z')) @
              rx(e3('rotation_x')) @ ry(e3('rotation_y')) @ rzz(rz))
        R = mat(M3[0, 0], M3[0, 1], 0, M3[1, 0], M3[1, 1], 0)
    T = mat(1, 0, px, 0, 1, py)
    return T @ R @ S @ A


class Renderer:
    def __init__(self, W=1920, H=1080, boxes=False):
        self.W, self.H = W, H
        self.rgb = np.zeros((H, W, 3), np.float32)
        self.a = np.zeros((H, W), np.float32)
        self.boxes = boxes
        self.labels = []

    def comp(self, set_name, scene, ci, t, M=np.eye(3), opacity=1.0, depth=0):
        comp = scene['compositions'][ci]
        layers = comp['layers']
        mats = {}

        def lm(li):
            if li in mats:
                return mats[li]
            L = layers[li]
            m = layer_matrix(L['video_data'], t) if L.get('video_data') else np.eye(3)
            if isinstance(L['parent'], list) and L['parent'][0] == ci:
                m = lm(L['parent'][1]) @ m
            mats[li] = m
            return m

        for li in range(len(layers) - 1, -1, -1):
            L = layers[li]
            if not (L['start_frame'] <= t < L['end_frame']):
                continue
            if not (L['flags_raw'] & 1):
                continue
            vd = L.get('video_data')
            if vd is None:
                continue
            op = opacity * eval_fcurve(vd['opacity'], t)
            Ml = M @ lm(li)
            if L['type'] == 'composition':
                lt = (t - L['start_frame']) * L['time_scale'] + L['offset_frame']
                self.comp(set_name, scene, L['composition'], lt, Ml, op, depth + 1)
            elif L['type'] == 'video':
                v = scene['videos'][L['video']]
                if not v['sources']:
                    if self.boxes:
                        self.labels.append((L['name'], Ml, v['width'], v['height']))
                    continue
                if op <= 0:
                    continue
                lt = (t - L['start_frame']) * L['time_scale'] + L['offset_frame']
                fps = v['frames_per_source'] or 1.0
                si = min(len(v['sources']) - 1, max(0, int(lt / fps)))
                src = v['sources'][si]
                self.draw(src['sprite_set'], src['sprite_index'], Ml, op, vd['blend_mode'])

    def draw(self, spr_set, index, M, op, blend, margin=1):
        img, w, h = sprite_image(spr_set, index, margin)
        corners = M @ np.array([[0, w, w, 0], [0, 0, h, h], [1, 1, 1, 1.0]])
        x0 = max(0, int(math.floor(corners[0].min())) - 1)
        y0 = max(0, int(math.floor(corners[1].min())) - 1)
        x1 = min(self.W, int(math.ceil(corners[0].max())) + 1)
        y1 = min(self.H, int(math.ceil(corners[1].max())) + 1)
        if x1 <= x0 or y1 <= y0:
            return
        try:
            Mi = np.linalg.inv(M)
        except np.linalg.LinAlgError:
            return
        # output pixel (i, j) -> canvas (x0 + i, y0 + j) -> layer -> crop (+margin)
        O = Mi @ mat(1, 0, x0, 0, 1, y0)
        data = (O[0, 0], O[0, 1], O[0, 2] + margin, O[1, 0], O[1, 1], O[1, 2] + margin)
        size = (x1 - x0, y1 - y0)
        pm = img.convert('RGBa')
        warped = np.asarray(pm.transform(size, Image.AFFINE, data, Image.BILINEAR), np.float32) / 255
        # geometric coverage of the sprite quad (1-canvas-pixel antialias), computed from the
        # layer-space position of every canvas pixel centre; texels outside the rect are still
        # sampled, as a GPU does at the quad edge
        jj, ii = np.mgrid[0:size[1], 0:size[0]].astype(np.float32)
        cx, cy = ii + x0 + 0.5, jj + y0 + 0.5
        u = Mi[0, 0] * cx + Mi[0, 1] * cy + Mi[0, 2]
        v = Mi[1, 0] * cx + Mi[1, 1] * cy + Mi[1, 2]
        pu = max(1e-6, math.hypot(M[0, 0], M[1, 0]))  # canvas px per layer unit along u
        pv = max(1e-6, math.hypot(M[0, 1], M[1, 1]))
        cov = (np.clip(np.minimum(u, w - u) * pu + 0.5, 0, 1) *
               np.clip(np.minimum(v, h - v) * pv + 0.5, 0, 1))
        k = (cov * op)[..., None]
        src = warped[..., :3] * k
        sa = warped[..., 3] * k[..., 0]
        dst = self.rgb[y0:y1, x0:x1]
        da = self.a[y0:y1, x0:x1]
        if blend == 'add':
            dst += src
            da += sa
        else:
            dst *= (1 - sa)[..., None]
            dst += src
            da *= (1 - sa)
            da += sa
        np.clip(dst, 0, 1, out=dst)
        np.clip(da, 0, 1, out=da)

    def image(self, bg=(0, 0, 0)):
        rgb = self.rgb + np.array(bg, np.float32)[None, None] / 255 * (1 - self.a)[..., None]
        im = Image.fromarray((np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGB')
        if self.boxes and self.labels:
            dr = ImageDraw.Draw(im)
            for name, M, w, h in self.labels:
                c = M @ np.array([[0, w, w, 0], [0, 0, h, h], [1, 1, 1, 1.0]])
                pts = [(float(c[0, k]), float(c[1, k])) for k in range(4)]
                dr.polygon(pts, outline=(255, 0, 255))
                dr.text((pts[0][0] + 2, pts[0][1] + 1), name, fill=(255, 0, 255))
        return im


def find_comp(scene, spec):
    if spec.startswith('C') and spec[1:].isdigit():
        return int(spec[1:])
    if spec == '__root__':
        return len(scene['compositions']) - 1
    for c in scene['compositions']:
        if c['name'] == spec:
            return c['index']
    raise KeyError(spec)


def render(specs, out, boxes=False, size=None):
    R = Renderer(boxes=boxes)
    for spec in specs:
        off = (0.0, 0.0)
        if '+' in spec:
            spec, o = spec.split('+')
            off = tuple(float(x) for x in o.split(','))
        body, fr = spec.split('@')
        set_name, scene_name, comp = body.split(':')
        j = load(set_name)
        scene = [s for s in j['scenes'] if s['name'] == scene_name][0]
        R.comp(set_name, scene, find_comp(scene, comp), float(fr), mat(1, 0, off[0], 0, 1, off[1]))
    im = R.image()
    if size:
        im = im.resize(size, Image.LANCZOS)
    im.save(out)
    return im


if __name__ == '__main__':
    args = sys.argv[1:]
    boxes = '--boxes' in args
    size = None
    if '--size' in args:
        i = args.index('--size')
        size = tuple(int(x) for x in args[i + 1].split('x'))
        del args[i:i + 2]
    args = [a for a in args if a != '--boxes']
    render(args[1:], args[0], boxes, size)
