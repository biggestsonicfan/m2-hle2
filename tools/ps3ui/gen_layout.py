#!/usr/bin/env python3
"""Generate src/ui/ps3ui_layout.h: the lobby's AET layouts as C tables.

Input is the JSON that aetdump.py (the AET decoder, kept outside the repo with
the PS3 data it reads) writes for the n_cmn set of the PS3 port's
rom.psarc. Only the compositions the lobby plays are kept, and only their
numbers: which layers, their timing, keyframes, blend modes and which sprite
each shows, by name. No pixel data comes through here; the sprites are painted
by code in ps3ui_sprites.h, keyed by the enum this script writes.

  python tools/ps3ui/gen_layout.py <path>/n_cmn.json <path>/sprites.json <path>/fontmap.json

sprites.json (from the sprite decoder) gives each sprite's size, the size its
painter paints it at. fontmap.json gives, for ASCII, where each character's
ink starts in its cell and how wide it is in the PS3's two faces: the text
layout's numbers (the PS3 advances by ink width + 4), not its glyphs.
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# scene -> compositions the lobby plays (children are pulled in).
WANT = {
    'n_cmn_base': ['bg', 'sousa_win', 'win_block_anm3x3', 'cursor_cmn01_46', 'loading', 'cmn_win_b_01',
                   'choice_win_06', 'choice_win_07', 'choice_win_08', 'cmn_win_l_01', 'cmn_win_l_02',
                   'cmn_win_l_03', 'pause_win_ss', 'pause_win_s', 'pause_win_m', 'pause_win_l', 'pause_win_ll',
                   'head_index', 'winbase_2', 'cmn_win_s_01', 'cmn_win_s_02', 'cmn_win_s_03',
                   'winbase_3', 'cmn_win_m_01', 'cmn_win_m_02', 'cmn_win_m_03',
                   'choice_win_02', 'choice_win_03', 'choice_win_04', 'choice_win_05'],
    'n_cmn_online': None,     # all of it
    'n_cmn_screen': ['connect_win', 'loading_win', 'controls_win_ps3', 'tips_info_win',
                     'vs_end_menu', 'offend_1pbase', 'offend_2pbase'],
}
CURVES = ['anchor_x', 'anchor_y', 'position_x', 'position_y', 'rotation', 'scale_x', 'scale_y', 'opacity']
DEFAULT = {'scale_x': 1.0, 'scale_y': 1.0, 'opacity': 1.0}


def cname(s):
    return re.sub(r'[^A-Za-z0-9]', '_', s)


def f(v):
    r = repr(float(v))
    return (r if ('.' in r or 'e' in r) else r + '.0') + 'f'


def comp_name(scene, ci):
    for c in scene['compositions']:
        for l in c['layers']:
            if l['type'] == 'composition' and l['composition'] == ci:
                return l['name']
    return '__root__' if ci == len(scene['compositions']) - 1 else None


def main():
    src = sys.argv[1]
    d = json.load(open(src))
    size = {s['name']: (int(s['rect'][2]), int(s['rect'][3])) for s in json.load(open(sys.argv[2]))}
    fontmap = {f['id']: {c['code']: c for c in f['chars']} for f in json.load(open(sys.argv[3], encoding='utf-8'))}
    scenes = {s['name']: s for s in d['scenes']}
    sprites = {}
    out = []

    def spr_id(name):
        if name not in sprites:
            sprites[name] = len(sprites)
        return sprites[name]

    body = []
    for sname, roots in WANT.items():
        sc = scenes[sname]
        names = {i: comp_name(sc, i) for i in range(len(sc['compositions']))}
        if roots is None:
            keep = set(range(len(sc['compositions'])))
        else:
            keep = set()
            want = [i for i, n in names.items() if n in roots]
            missing = set(roots) - {names[i] for i in want}
            if missing:
                raise SystemExit(f'{sname}: no composition {sorted(missing)}')
            while want:
                i = want.pop()
                if i in keep:
                    continue
                keep.add(i)
                for l in sc['compositions'][i]['layers']:
                    if l['type'] == 'composition':
                        want.append(l['composition'])
        order = sorted(keep)
        remap = {ci: n for n, ci in enumerate(order)}
        comps, layers, keys, srcs = [], [], [], []
        for ci in order:
            c = sc['compositions'][ci]
            comps.append((names[ci], len(layers), len(c['layers'])))
            for l in c['layers']:
                vd = l.get('video_data') or {}
                cur = []
                for cn in CURVES:
                    cv = vd.get(cn)
                    if cv is None:
                        cur.append((0, 0, DEFAULT.get(cn, 0.0)))
                    elif 'keys' in cv and len(cv['keys']) > 0:
                        cur.append((len(cv['keys']), len(keys), 0.0))
                        keys.extend((k['frame'], k['value'], k.get('tangent', 0.0)) for k in cv['keys'])
                    else:
                        cur.append((0, 0, cv.get('value', DEFAULT.get(cn, 0.0))))
                typ = {'video': 1, 'composition': 3}.get(l['type'], 0)
                item, nsrc, w, h, fps = 0, 0, 0, 0, 0.0
                if typ == 1:
                    v = sc['videos'][l['video']]
                    item, nsrc = len(srcs), len(v['sources'])
                    w, h, fps = v['width'], v['height'], v.get('frames_per_source', 0.0)
                    for s in v['sources']:
                        srcs.append(spr_id(s['sprite']))
                elif typ == 3:
                    item = remap[l['composition']]
                layers.append(dict(name=l['name'], typ=typ, blend=vd.get('blend_mode_raw', 3),
                                   active=1 if (l.get('flags_raw', 0) & 1) else 0, nsrc=nsrc,
                                   start=l['start_frame'], end=l['end_frame'], off=l['offset_frame'],
                                   ts=l['time_scale'], item=item, w=w, h=h, fps=fps, cur=cur))
        p = 'ps3ui_' + cname(sname)
        # windows: root layers carrying the open / idle / close markers
        wins = []
        kept = {names[ci] for ci in order}
        for l in sc['compositions'][-1]['layers']:
            m = {k['name']: k['frame'] for k in l['markers']}
            if l['name'] not in kept:
                continue
            if 'sta_s' in m:
                wins.append((l['name'], m['sta_s'], m['sta_e'], m['neu_s'], m['neu_e'], m['end_s'], m['end_e']))
            elif 'loop_s' in m:
                wins.append((l['name'], -1.0, -1.0, m['loop_s'], m['loop_e'], -1.0, -1.0))
        body.append(f'static const ps3ui_window_def_t {p}_wins[{max(1, len(wins))}] = {{')
        for w in wins:
            body.append('    {"%s",%s},' % (w[0], ','.join(f(v) for v in w[1:])))
        if not wins:
            body.append('    {0}')
        body.append('};')
        body.append(f'/* {sname}: {len(comps)} compositions, {len(layers)} layers, {len(keys)} keys */')
        body.append(f'static const ps3ui_key_t {p}_keys[{max(1, len(keys))}] = {{')
        for i in range(0, len(keys), 3):
            body.append('    ' + ' '.join('{%s,%s,%s},' % (f(a), f(b), f(c)) for a, b, c in keys[i:i + 3]))
        if not keys:
            body.append('    {0}')
        body.append('};')
        body.append(f'static const uint16_t {p}_srcs[{max(1, len(srcs))}] = {{')
        for i in range(0, len(srcs), 16):
            body.append('    ' + ','.join(str(s) for s in srcs[i:i + 16]) + ',')
        if not srcs:
            body.append('    0')
        body.append('};')
        body.append(f'static const ps3ui_layer_t {p}_layers[{len(layers)}] = {{')
        for L in layers:
            cs = ','.join('{%d,%d,%s}' % (n, first, f(v)) for n, first, v in L['cur'])
            body.append('    {"%s",%d,%d,%d,%d,%s,%s,%s,%s,%d,%d,%d,%s,{%s}},' % (
                L['name'], L['typ'], L['blend'], L['active'], L['nsrc'], f(L['start']), f(L['end']),
                f(L['off']), f(L['ts']), L['item'], L['w'], L['h'], f(L['fps']), cs))
        body.append('};')
        body.append(f'static const ps3ui_comp_t {p}_comps[{len(comps)}] = {{')
        for n, first, cnt in comps:
            body.append('    {%s,%d,%d},' % ('"%s"' % n if n else 'NULL', first, cnt))
        body.append('};')
        body.append(f'static const ps3ui_scene_t {p} = {{ {p}_comps, {p}_layers, {p}_keys, {p}_srcs, {len(comps)}, {p}_wins, {len(wins)} }};')
        body.append('')

    out.append('/* Generated by tools/ps3ui/gen_layout.py -- do not edit.')
    out.append(' *')
    out.append(' * The PS3 lobby\'s layouts: layer timing, keyframes, blend modes and which')
    out.append(' * sprite each layer shows, by id. The sprites themselves are painted by')
    out.append(' * code in ps3ui_sprites.h. */')
    out.append('#ifndef PS3UI_LAYOUT_H')
    out.append('#define PS3UI_LAYOUT_H')
    out.append('')
    out.append('#include "ps3ui.h"')
    out.append('')
    out.append('enum {')
    for n, i in sorted(sprites.items(), key=lambda x: x[1]):
        out.append(f'    PS3UI_SPR_{cname(n[6:] if n.startswith("n_cmn_") else n).upper()} = {i},')
    out.append(f'    PS3UI_SPR_COUNT = {len(sprites)}')
    out.append('};')
    out.append('')
    out.append('static const struct { const char *name; uint16_t w, h; } ps3ui_sprite_info[PS3UI_SPR_COUNT] = {')
    for n, i in sorted(sprites.items(), key=lambda x: x[1]):
        w, h = size[n]
        out.append(f'    {{"{n}", {w}, {h}}},')
    out.append('};')
    out.append('')
    out.append('/* PS3 text metrics for ASCII 32..126: ink left and ink width in the cell,')
    out.append(' * [0] = font 1 (46 px cell, cap 37), [1] = font 2 (62x56 cell, cap 40). */')
    out.append('static const uint8_t ps3ui_ps3_metrics[2][95][2] = {')
    for fid in (1, 2):
        row = []
        for cp in range(32, 127):
            c = fontmap[fid].get(cp)
            row.append('{%d,%d}' % ((c['ink_left'], c['ink_width']) if c else (0, 0)))
        out.append('    {' + ','.join(row) + '},')
    out.append('};')
    out.append('')
    out.extend(body)
    out.append('#endif')
    path = os.path.join(ROOT, 'src', 'ui', 'ps3ui_layout.h')
    with open(path, 'w', newline='\n') as fp:
        fp.write('\n'.join(out) + '\n')
    print(path, len(sprites), 'sprites', os.path.getsize(path), 'bytes')


if __name__ == '__main__':
    main()
