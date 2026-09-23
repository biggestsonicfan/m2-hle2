#!/usr/bin/env python3
"""Decoder for the PS3 Sonic the Fighters (NPUB30927) AET / aetdb / sprdb / sprite.bin files.

Big-endian, 32-bit offsets, all offsets absolute from the start of each file.
Usage: python aetdump.py [psarc_out_dir] [out_dir]
  defaults: ../out  and  .  (relative to this script)
Writes <set>.json for every auth2d/<set>.farc.d/aet.bin, plus aetdb.json and sprdb.json.

Layouts (all u32 BE unless noted):

aetdb.bin
  +0  set_count  +4 sets_off
  set (20 B): id, name_off, file_off, sprite_set_id, scenes_hdr_off
  scenes_hdr (8 B): count, entries_off ; entry (8 B): scene_id, name_off

sprdb.bin
  +0  set_count  +4 sets_off
  set (20 B): id, name_off, file_off, sprites_hdr_off, textures_hdr_off
  *_hdr (8 B): count, entries_off ; entry (8 B): id, name_off
  The position of a sprite in its set's list IS its index in sprite.bin.
  Texture ids carry 0x10000 (texture flag) in the high half.

sprite.bin (per set)
  +0 texture_count  +4 sprite_count  +8 sprite_table_off -> sprite_count u32 offsets
  sprite (48 B): tex_index, unk(=9, resolution mode?), 0, 0,
                 f32 u0,v0,u1,v1 (normalised), f32 x,y,w,h (texels)
  Texture i = i-th entry of texture.farc (MERGE_*.dds, standard little-endian DDS).

aet.bin (per set)
  +0 scene_count  +4 scene_ptrs_off -> scene_count u32 scene offsets
  scene (52 B): f32 start, f32 end, f32 fps, u32 bg_color(RGBx), u32 width, u32 height,
                camera_off, comp_count, comps_off, video_count, videos_off, audio_count, audios_off
                (NO name field: names come from aetdb, same order)
  comp (8 B): layer_count, layers_off.  The LAST comp is the root.
  layer (52 B): name_off, name_id, f32 start, f32 end, f32 offset, f32 time_scale,
                u16 flags, u8 quality, u8 type(0 none,1 video,2 audio,3 comp),
                item_off (video / comp entry / audio), parent_layer_off,
                marker_count, markers_off, video_data_off, audio_data_off
  marker (12 B): f32 frame, name_off, name_id
  video (20 B): u8 r,g,b,pad, u16 width, u16 height, f32 frames_per_source, source_count, sources_off
  source (20 B): u32 sprite_ref (hi16 = sprite_set_id*2, lo16 = sprdb sprite id), u32 0, u32 0, f32 1.0, f32 1.0
  video_data (72 B): u8 blend_mode, u8 flags, u8 track_matte, u8 pad,
                8 fcurves (8 B each: count, off): anchor x,y, position x,y, rotation, scale x,y, opacity,
                u32 video3d_off
  fcurve: count==0 -> 0 ; count==1 -> off points at 1 f32 value ;
          count>1 -> off points at count f32 frames, then count (value, tangent) f32 pairs.
  video3d (64 B, if present): 8 fcurves: anchor z, position z, direction x,y,z, rotation x,y, scale z
"""
import json
import os
import struct
import sys

BLEND_MODES = {0: 'copy', 1: 'behind', 2: 'in_front', 3: 'normal', 4: 'dissolve', 5: 'add',
               6: 'multiply', 7: 'screen', 8: 'overlay', 9: 'soft_light', 10: 'hard_light',
               11: 'darken', 12: 'lighten', 13: 'classic_difference', 14: 'hue', 15: 'saturation',
               16: 'color', 17: 'luminosity', 18: 'stencil_alpha', 19: 'stencil_luma',
               20: 'silhouette_alpha', 21: 'silhouette_luma', 22: 'luminescent_premul',
               23: 'alpha_add', 24: 'classic_color_dodge', 25: 'classic_color_burn',
               26: 'exclusion', 27: 'difference', 28: 'color_dodge', 29: 'color_burn',
               30: 'linear_dodge', 31: 'linear_burn', 32: 'linear_light', 33: 'vivid_light',
               34: 'pin_light', 35: 'hard_mix', 36: 'lighter_color', 37: 'darker_color',
               38: 'subtract', 39: 'divide'}
LAYER_TYPES = {0: 'none', 1: 'video', 2: 'audio', 3: 'composition'}
LAYER_FLAGS = ['video_active', 'audio_active', 'effects_active', 'motion_blur', 'frame_blending',
               'locked', 'shy', 'collapse', 'auto_orient_rotation', 'adjustment_layer',
               'time_remapping', 'layer_is_3d', 'look_at_camera', 'look_at_poi', 'solo',
               'markers_locked']
QUALITY = {0: 'none', 1: 'wireframe', 2: 'draft', 3: 'best'}
TRANSFORM = ['anchor_x', 'anchor_y', 'position_x', 'position_y', 'rotation', 'scale_x', 'scale_y',
             'opacity']
TRANSFORM3D = ['anchor_z', 'position_z', 'direction_x', 'direction_y', 'direction_z',
               'rotation_x', 'rotation_y', 'scale_z']


class Buf:
    def __init__(self, data):
        self.d = data

    def u32(self, o):
        return struct.unpack_from('>I', self.d, o)[0]

    def f32(self, o):
        return struct.unpack_from('>f', self.d, o)[0]

    def s(self, o):
        if o == 0:
            return None
        return self.d[o:self.d.index(b'\0', o)].decode('ascii', 'replace')


def r(v):
    """Round a float for JSON without losing the f32 value."""
    return float('%.7g' % v)


# ---------------------------------------------------------------- databases
def read_aetdb(path):
    b = Buf(open(path, 'rb').read())
    n, o = b.u32(0), b.u32(4)
    sets = []
    for i in range(n):
        e = o + 20 * i
        sid, nm, fn, spr, sh = (b.u32(e + 4 * k) for k in range(5))
        cnt, eo = b.u32(sh), b.u32(sh + 4)
        scenes = [{'id': b.u32(eo + 8 * j), 'name': b.s(b.u32(eo + 8 * j + 4))} for j in range(cnt)]
        sets.append({'id': sid, 'name': b.s(nm), 'file': b.s(fn), 'sprite_set_id': spr,
                     'scenes': scenes})
    return sets


def read_sprdb(path):
    b = Buf(open(path, 'rb').read())
    n, o = b.u32(0), b.u32(4)
    sets = []
    for i in range(n):
        e = o + 20 * i
        sid, nm, fn, sh, th = (b.u32(e + 4 * k) for k in range(5))

        def lst(h):
            c, eo = b.u32(h), b.u32(h + 4)
            return [{'index': j, 'id': b.u32(eo + 8 * j), 'name': b.s(b.u32(eo + 8 * j + 4))}
                    for j in range(c)]
        sets.append({'id': sid, 'name': b.s(nm), 'file': b.s(fn), 'sprites': lst(sh),
                     'textures': lst(th)})
    return sets


def read_sprite_bin(path):
    b = Buf(open(path, 'rb').read())
    ntex, nspr, to = b.u32(0), b.u32(4), b.u32(8)
    out = []
    for j in range(nspr):
        o = b.u32(to + 4 * j)
        tex, unk, z0, z1 = (b.u32(o + 4 * k) for k in range(4))
        f = [b.f32(o + 16 + 4 * k) for k in range(8)]
        out.append({'index': j, 'texture': tex, 'unk': unk, 'uv': [r(x) for x in f[:4]],
                    'rect': [r(x) for x in f[4:]]})
    return {'texture_count': ntex, 'sprites': out}


# ---------------------------------------------------------------- aet
class AetReader:
    def __init__(self, path, sprite_names):
        self.b = Buf(open(path, 'rb').read())
        self.sprite_names = sprite_names  # id -> (set name, index, name)

    def fcurve(self, o):
        b = self.b
        c, p = b.u32(o), b.u32(o + 4)
        if c == 0:
            return {'value': 0.0}
        if c == 1:
            return {'value': r(b.f32(p))}
        frames = [b.f32(p + 4 * k) for k in range(c)]
        vp = p + 4 * c
        keys = [{'frame': r(frames[k]), 'value': r(b.f32(vp + 8 * k)),
                 'tangent': r(b.f32(vp + 8 * k + 4))} for k in range(c)]
        return {'keys': keys}

    def video_data(self, o):
        b = self.b
        d = b.d
        vd = {'blend_mode_raw': d[o], 'blend_mode': BLEND_MODES.get(d[o], '?%d' % d[o]),
              'transfer_flags': d[o + 1], 'track_matte': d[o + 2], 'pad': d[o + 3]}
        for k, nm in enumerate(TRANSFORM):
            vd[nm] = self.fcurve(o + 4 + 8 * k)
        v3 = b.u32(o + 68)
        if v3:
            vd['3d'] = {nm: self.fcurve(v3 + 8 * k) for k, nm in enumerate(TRANSFORM3D)}
        return vd

    def video(self, o):
        b = self.b
        d = b.d
        n, so = b.u32(o + 12), b.u32(o + 16)
        srcs = []
        for j in range(n):
            e = so + 20 * j
            ref = b.u32(e)
            sid = ref & 0xFFFF
            info = self.sprite_names.get(sid)
            srcs.append({'sprite_ref': '0x%08X' % ref, 'sprite_set_id': (ref >> 16) >> 1,
                         'sprite_id': sid,
                         'sprite_set': info[0] if info else None,
                         'sprite_index': info[1] if info else None,
                         'sprite': info[2] if info else None,
                         'extra': [b.u32(e + 4), b.u32(e + 8), r(b.f32(e + 12)), r(b.f32(e + 16))]})
        return {'offset': o, 'color': '#%02X%02X%02X' % (d[o], d[o + 1], d[o + 2]),
                'width': struct.unpack_from('>H', d, o + 4)[0],
                'height': struct.unpack_from('>H', d, o + 6)[0],
                'frames_per_source': r(b.f32(o + 8)), 'sources': srcs}

    def scene(self, o, name, sid):
        b = self.b
        sc = {'id': sid, 'name': name, 'offset': o, 'start_frame': r(b.f32(o)),
              'end_frame': r(b.f32(o + 4)), 'fps': r(b.f32(o + 8)),
              'background_color': '#%06X' % (b.u32(o + 12) >> 8),
              'width': b.u32(o + 16), 'height': b.u32(o + 20), 'camera_offset': b.u32(o + 24)}
        ncomp, co = b.u32(o + 28), b.u32(o + 32)
        nvid, vo = b.u32(o + 36), b.u32(o + 40)
        naud, ao = b.u32(o + 44), b.u32(o + 48)
        videos = [self.video(vo + 20 * j) for j in range(nvid)]
        vid_index = {v['offset']: j for j, v in enumerate(videos)}
        comp_off = [co + 8 * j for j in range(ncomp)]
        comp_index = {c: j for j, c in enumerate(comp_off)}
        audios = [{'offset': ao + 4 * j, 'sound_id': b.u32(ao + 4 * j)} for j in range(naud)]
        aud_index = {a['offset']: j for j, a in enumerate(audios)}
        # layer offsets -> (comp, idx) for parent resolution
        layer_loc = {}
        for ci, c in enumerate(comp_off):
            n, lo = b.u32(c), b.u32(c + 4)
            for li in range(n):
                layer_loc[lo + 52 * li] = (ci, li)
        comps = []
        comp_names = {}
        for ci, c in enumerate(comp_off):
            n, lo = b.u32(c), b.u32(c + 4)
            layers = []
            for li in range(n):
                lo_ = lo + 52 * li
                L = self.layer(lo_, vid_index, comp_index, aud_index, layer_loc)
                layers.append(L)
                if L['type'] == 'composition':
                    comp_names.setdefault(L['composition'], []).append(L['name'])
            comps.append({'index': ci, 'offset': c, 'layers': layers})
        for ci, cmp in enumerate(comps):
            cmp['root'] = ci == ncomp - 1
            nm = comp_names.get(ci, [])
            cmp['name'] = '__root__' if cmp['root'] else (nm[0] if nm else None)
            cmp['referenced_as'] = sorted(set(nm))
        sc['compositions'] = comps
        sc['videos'] = videos
        sc['audios'] = audios
        return sc

    def layer(self, o, vid_index, comp_index, aud_index, layer_loc):
        b = self.b
        d = b.d
        flags = struct.unpack_from('>H', d, o + 24)[0]
        q, t = d[o + 26], d[o + 27]
        item, parent = b.u32(o + 28), b.u32(o + 32)
        nm, mo = b.u32(o + 36), b.u32(o + 40)
        vdo, ado = b.u32(o + 44), b.u32(o + 48)
        L = {'offset': o, 'name': b.s(b.u32(o)), 'name_id': b.u32(o + 4),
             'start_frame': r(b.f32(o + 8)), 'end_frame': r(b.f32(o + 12)),
             'offset_frame': r(b.f32(o + 16)), 'time_scale': r(b.f32(o + 20)),
             'flags_raw': flags, 'flags': [f for k, f in enumerate(LAYER_FLAGS) if flags >> k & 1],
             'quality': QUALITY.get(q, q), 'type': LAYER_TYPES.get(t, t)}
        if t == 1:
            L['video'] = vid_index.get(item, item)
        elif t == 3:
            L['composition'] = comp_index.get(item, item)
        elif t == 2:
            L['audio'] = aud_index.get(item, item)
        elif item:
            L['item_offset'] = item
        L['parent'] = list(layer_loc[parent]) if parent in layer_loc else (parent or None)
        L['markers'] = [{'frame': r(b.f32(mo + 12 * k)), 'name': b.s(b.u32(mo + 12 * k + 4)),
                         'name_id': b.u32(mo + 12 * k + 8)} for k in range(nm)]
        if vdo:
            L['video_data'] = self.video_data(vdo)
        if ado:
            L['audio_data'] = {nm_: self.fcurve(ado + 8 * k) for k, nm_ in
                               enumerate(['volume_l', 'volume_r', 'pan_l', 'pan_r'])}
        return L

    def read(self, scene_meta):
        b = self.b
        n, po = b.u32(0), b.u32(4)
        scenes = []
        for i in range(n):
            meta = scene_meta[i] if i < len(scene_meta) else {'name': None, 'id': None}
            scenes.append(self.scene(b.u32(po + 4 * i), meta['name'], meta['id']))
        return scenes


# ---------------------------------------------------------------- evaluation helpers
def eval_fcurve(fc, frame):
    """DIVA/MML Hermite: tangents are value-per-frame slopes."""
    if 'value' in fc:
        return fc['value']
    k = fc['keys']
    if frame <= k[0]['frame']:
        return k[0]['value']
    if frame >= k[-1]['frame']:
        return k[-1]['value']
    for a, c in zip(k, k[1:]):
        if a['frame'] <= frame < c['frame']:
            f1, f2 = a['frame'], c['frame']
            p1, p2, t1, t2 = a['value'], c['value'], a['tangent'], c['tangent']
            t = (frame - f1) / (f2 - f1)
            t_1 = t - 1.0
            return (t_1 * t1 + t * t2) * t_1 * (frame - f1) + (t * 2.0 - 3.0) * t * t * (p1 - p2) + p1
    return k[-1]['value']


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', 'out')
    dst = sys.argv[2] if len(sys.argv) > 2 else here
    aetdb = read_aetdb(os.path.join(src, 'auth2d', 'aetdb.bin'))
    sprdb = read_sprdb(os.path.join(src, 'sprite', 'sprdb.bin'))
    for s in sprdb:
        sb = os.path.join(src, 'sprite', s['name'] + '.farc.d', 'sprite.bin')
        if os.path.exists(sb):
            bin_ = read_sprite_bin(sb)
            s['texture_count_bin'] = bin_['texture_count']
            for spr, rec in zip(s['sprites'], bin_['sprites']):
                spr.update({k: rec[k] for k in ('texture', 'unk', 'uv', 'rect')})
            if len(bin_['sprites']) != len(s['sprites']):
                print('WARN sprite count mismatch', s['name'])
    json.dump(aetdb, open(os.path.join(dst, 'aetdb.json'), 'w'), indent=1)
    json.dump(sprdb, open(os.path.join(dst, 'sprdb.json'), 'w'), indent=1)
    names = {}
    for s in sprdb:
        for spr in s['sprites']:
            names[spr['id']] = (s['name'], spr['index'], spr['name'])
    for s in aetdb:
        p = os.path.join(src, 'auth2d', s['name'] + '.farc.d', 'aet.bin')
        if not os.path.exists(p):
            continue
        scenes = AetReader(p, names).read(s['scenes'])
        spr_set = next((x['name'] for x in sprdb if x['id'] == s['sprite_set_id']), None)
        out = {'set': s['name'], 'id': s['id'], 'sprite_set_id': s['sprite_set_id'],
               'sprite_set': spr_set, 'scenes': scenes}
        json.dump(out, open(os.path.join(dst, s['name'] + '.json'), 'w'), indent=1)
        print(s['name'], [(sc['name'], len(sc['compositions']), len(sc['videos']),
                           sc['width'], sc['height']) for sc in scenes])


if __name__ == '__main__':
    main()
