#!/usr/bin/env python3
"""hud_ocr.py -- read the Dreamcast build's stats HUD back out of a video
(Pinboard #511).

    python3 -I hud_ocr.py VIDEO --log out.log --jsonl out.jsonl [--csv out.csv]

The HUD (main_dc.c, dc_prof.h with HUD=prof) is the BIOS font, 12x24 cells,
20 rows, redrawn every 2 s. In a 1280x720 capture of Flycast's 640x480 picture
a cell is 12.8 x 25.6 px from (321.9, 87.0) (--grid to move it). Each cell is
matched against a glyph atlas (hud_font.png beside this script, learned from
a capture by --learn); every 2 s window is read in all its frames and the
characters voted. Rows are then parsed against the HUD's own formats, numbers
taking only digits, symbol names snapped to the program's identifiers.

The screen shows 53 of a row's up to 84 characters: a field that runs into
the right edge may be cut, and is named in the record's "edge" list.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ATLAS = os.path.join(HERE, "hud_font.png")
# the atlas's characters, in its order (the first is the space)
CHARS = " $%()+,-./0123456789:<>ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz|"
CW, CH, PAD = 16, 32, 3      # a cell resampled; neighbours' pixels kept each side
NCOL = 53                    # columns visible (the 53rd three-quarters)
EDGE_W = 12                  # of the 53rd column's 16, those on screen
ROWS = [0, 1, 2, 6, 7, 8, 9, 10, 16, 17, 18, 19]
GRID = (321.9, 87.0, 12.8, 25.6)   # x0, y0 of cell (0, 0), pitch x, y; in 1280x720
CROP = (320, 80, 720, 540)         # x, y, w, h read from the video


# ---- cells and glyphs ------------------------------------------------------

def frames(video, fps):
    x, y, w, h = CROP
    cmd = ["ffmpeg", "-loglevel", "error", "-i", video, "-vf",
           f"fps={fps},scale=1280:720,crop={w}:{h}:{x}:{y},format=gray", "-f", "rawvideo", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    n = 0
    while True:
        b = p.stdout.read(w * h)
        if len(b) < w * h:
            break
        yield n / float(fps), np.frombuffer(b, np.uint8).reshape(h, w)
        n += 1
    p.wait()


def cells(frame, rows, grid=GRID):
    """{row: array (NCOL, CH, CW + 2*PAD)}, column c of the row at index c - 1."""
    x0, y0, px, py = grid
    x0 -= CROP[0]
    y0 -= CROP[1]
    im = Image.fromarray(frame)
    n = NCOL + 2
    out = {}
    s = CW / px
    for r in rows:
        y = y0 + py * r
        a = np.asarray(im.resize((n * CW, CH), Image.BILINEAR, box=(x0, y, x0 + px * n, y + py)),
                       dtype=np.float32)
        a = np.pad(a, ((0, 0), (PAD, PAD)))
        out[r] = np.stack([a[:, c * CW:c * CW + CW + 2 * PAD] for c in range(1, NCOL + 1)])
    return out


def load_atlas(path=ATLAS):
    a = np.asarray(Image.open(path).convert("L"), dtype=np.float32)
    n = a.shape[1] // CW
    keys = [CHARS[i] for i in range(n)]
    M = np.stack([a[:, i * CW:(i + 1) * CW] for i in range(n)])
    have = [i for i in range(n) if M[i].any() or keys[i] == " "]
    return [keys[i] for i in have], M[have]


def classify_row(C, keys, M):
    """The row's text and each cell's distance to its glyph."""
    txt, dist, bests = [], [], []
    for c in range(NCOL):
        w = EDGE_W if c == NCOL - 1 else CW
        best = None
        for dx in range(-2, 3):
            win = C[c][:, PAD + dx:PAD + dx + w]
            d = ((M[:, :, :w] - win) ** 2).mean((1, 2))
            best = d if best is None else np.minimum(best, d)
        k = int(best.argmin())
        ch = keys[k]
        if ch in " .,":   # a dot is too small to match: tell these three by where the ink is
            core = C[c][:, PAD:PAD + CW]
            if core[2:19, 3:13].max() < 40:
                ch = " " if core[21:29, 2:12].max() < 40 else "," if core[28:32, 2:12].max() >= 60 else "."
        txt.append(ch)
        dist.append(float(best[k]))
        bests.append(best)
    # A letter inside a number is a digit misread: none of the HUD's words has a
    # digit beside a letter but "i960" and "3d", so take the nearest digit there.
    digs = [i for i, ch in enumerate(keys) if ch.isdigit()]
    s = "".join(txt)
    for c, ch in enumerate(txt):
        if not ch.isalpha() or ch == "K":   # "89K" is the arena size
            continue
        l = s[c - 1] if c else " "
        r = s[c + 1] if c + 1 < len(s) else " "
        word = s[max(0, c - 3):c + 3]
        if "i960" in word or "3d" in word:
            continue
        if (l.isdigit() and (r.isdigit() or r in " )%K/+")) or (r.isdigit() and l.isdigit()):
            k = digs[int(bests[c][digs].argmin())]
            txt[c], dist[c] = keys[k], float(bests[c][k])
    return "".join(txt), dist


# ---- the HUD's formats -----------------------------------------------------
# A row is its tokens: a literal word, or a field: (name, kind[, prefix, suffix]).
# kinds: n unsigned, i signed, f one decimal, s a word, y a symbol name.

def F(name, kind="n", pre="", suf=""):
    return (name, kind, pre, suf)


FORMATS = {
    0: ["frame", F("frame"), F("fps", "f"), "fps", "(shown", F("shown_fps", "f", "", ")"), "slice",
        F("slice_ms"), "ms", "3d", ("3d", "n+n", "", ""), "ms"],
    1: ["ld", F("loads"), F("load_ms", "n", "("), "ms)", "flt", F("refills"), "|", "ev", F("evictions"),
        "pin", F("pinned"), "wr", F("rom_writes"), "err", F("read_errors")],
    2: ["profile", F("profile", "s"), F("gems", "s", "", ","), "cache", F("cache_kb"), "KB,", "heap",
        F("heap_kb"), "KB"],
    6: ["stall", "d$", F("stall_d_pct", "n", "", "%"), "i$", F("stall_i_pct", "n", "", "%"), "|", "pvr",
        "rnd", F("pvr_rnd_ms", "f"), "reg", F("pvr_reg_ms", "f"), "ms", "vbl", F("vbl"), "|", "smp",
        F("samples")],
    7: [x for g in ("aot", "gem", "hok", "960", "cop", "geo", "drw") for x in (g, F("g_" + g))],
    8: [x for g in ("til", "snd", "dsc", "kos", "lib", "bio", "oth") for x in (g, F("g_" + g))],
    9: [F("top1", "y"), F("top1_pct"), "|", F("top2", "y"), F("top2_pct"), "|", F("top3", "y"), F("top3_pct")],
    10: [F("top4", "y"), F("top4_pct"), "|", F("top5", "y"), F("top5_pct"), "|", F("top6", "y"), F("top6_pct")],
    16: ["i960", F("i960_ms"), "ms", F("cop_ms", "n", "(cop "), "/slice,", F("blk_pct", "n", "", "%"),
         "blk", F("aot_pct", "n", "", "%"), "aot,", F("steps"), "steps"],
    17: ["tl", F("tiles_ms"), "sc", F("scan_ms"), "so", F("sort_ms"), "|", "mesh", F("meshes"), "b",
         F("mesh_builds"), "h", F("mesh_hits"), "c", ("mesh_cl", "n/n", "", ""), ("arena_kb", "n", "", "K")],
    18: ["tris", F("tris"), "runs", F("runs"), "full", F("full"), "|", "tex", F("tex"), "new", F("tex_new"),
         "drop", F("tex_drop"), "fail", F("tex_fail"), "pk", ("pk", "n/n", "", ""), "rd", F("pk_reads"),
         F("pk_read_ms", "n", "", "ms")],
    19: ["snd", F("snd", "s"), "codes", F("codes"), "unk", F("unknown"), "bgm", F("bgm", "i"), "ring",
         F("ring_kb"), "KB", "under", F("underruns")],
}
# "(cop 3)" is two tokens on screen
FORMATS[16][3:4] = ["(cop", F("cop_ms", "n", "", ")")]

DIG = {"l": "1", "|": "1", "I": "1", "i": "1", "O": "0", "o": "0", "D": "0", "S": "5", "s": "5",
       "B": "8", "Z": "2", "z": "2", "g": "9", "q": "9", "b": "6", "G": "6", "T": "7"}
NUM = r"[0-9" + re.escape("".join(DIG)) + "]+"
KIND_RE = {"n": f"({NUM})", "i": f"(-?{NUM})", "f": f"({NUM})\\.({NUM})", "s": r"(\S+)", "y": r"(\S+)",
           "n+n": f"({NUM})\\+({NUM})", "n/n": f"({NUM})/({NUM})"}


def num(s):
    return int("".join(DIG.get(c, c) for c in s))


def fuzzy_eq(a, b):
    """A literal word read with at most one character wrong (two for long words)."""
    if len(a) != len(b):
        return False
    return sum(x != y for x, y in zip(a, b)) <= (1 if len(b) < 6 else 2)


def parse_row(row, text, symbols):
    """(fields, cut field names, ok). The text is cut at the 53rd column: a
    token touching it may be partial, and the format's tail may be missing."""
    fmt = FORMATS[row]
    cut = len(text) >= NCOL and text[NCOL - 1] != " "
    toks = text.rstrip().split()
    fields, edge = {}, []
    if not toks:
        return fields, edge, False
    for i, (tok, spec) in enumerate(zip(toks, fmt)):
        last = cut and i == len(toks) - 1
        if isinstance(spec, str):
            if not fuzzy_eq(tok, spec) and not (last and spec.startswith(tok[:-1])):
                return fields, edge, False
            continue
        name, kind, pre, suf = spec
        m = re.fullmatch(re.escape(pre) + KIND_RE[kind] + re.escape(suf), tok)
        if not m and last:   # cut before its suffix
            m = re.fullmatch(re.escape(pre) + KIND_RE[kind], tok)
        if not m:
            if last:
                edge.append(name)
                continue
            return fields, edge, False
        g = m.groups()
        if kind == "n" or kind == "i":
            v = num(g[0].lstrip("-")) * (-1 if g[0].startswith("-") else 1)
        elif kind == "f":
            v = float(f"{num(g[0])}.{num(g[1])}")
        elif kind in ("n+n", "n/n"):
            v = [num(g[0]), num(g[1])]
        elif kind == "y" or (kind == "s" and name == "profile"):
            v = snap(g[0], symbols)
        else:
            v = g[0]
        if row == 2 and name == "gems":
            v = v == "+gems"
        fields[name] = v
        if last:
            edge.append(name)
    return fields, edge, True


def tree_symbols(root):
    """The identifiers of the program's C, as the HUD shows them (16 characters)."""
    out = set()
    for sub in ("dreamcast", "src"):
        for d, _, fs in os.walk(os.path.join(root, sub)):
            for f in fs:
                if f.endswith((".c", ".h")):
                    with open(os.path.join(d, f), errors="replace") as fh:
                        out.update(w[:16] for w in re.findall(r"\b[A-Za-z_][A-Za-z0-9_]{2,}\b", fh.read()))
    return out


def snap(name, symbols):
    """The program's identifier (cut to 16, as the HUD does) nearest the
    read name, if one is within two edits and alone there."""
    if not symbols or name in symbols:
        return name
    best, bd, tie = None, 3, False
    for s in symbols:
        if abs(len(s) - len(name)) > 2:
            continue
        d = edit(name, s, bd)
        if d < bd:
            best, bd, tie = s, d, False
        elif d == bd:
            tie = True
    return best if best and not tie else name


def edit(a, b, cap):
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        if min(cur) >= cap:
            return cap
        prev = cur
    return prev[-1]


def render(row, fields):
    """The row's text as main_dc.c prints it, from parsed fields (for --learn)."""
    out = []
    for spec in FORMATS[row]:
        if isinstance(spec, str):
            out.append(spec)
            continue
        name, kind, pre, suf = spec
        if name not in fields:
            return None
        v = fields[name]
        if kind == "f":
            s = f"{v:.1f}"
        elif kind == "n+n":
            s = f"{v[0]}+{v[1]}"
        elif kind == "n/n":
            s = f"{v[0]}/{v[1]}"
        elif name == "gems":
            s = "+gems" if v else ""
        else:
            s = str(v)
        out.append(pre + s + suf)
    return " ".join(out)


# ---- windows ---------------------------------------------------------------

def windows(reads):
    """Split the frames' reads into the HUD's 2 s windows: a window starts
    where three or more rows change their text at once."""
    starts = [0]
    for i in range(1, len(reads)):
        ch = sum(reads[i][r][0] != reads[i - 1][r][0] for r in ROWS)
        if ch >= 3 and i - starts[-1] > 2:
            starts.append(i)
    starts.append(len(reads))
    return [(starts[k], starts[k + 1]) for k in range(len(starts) - 1)]


def vote(texts):
    return "".join(Counter(col).most_common(1)[0][0] for col in zip(*texts))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video")
    ap.add_argument("--fps", default="10", help="frames read a second of video (10)")
    ap.add_argument("--log", help="the HUD's lines, a block per window")
    ap.add_argument("--jsonl", help="a record per window, every field parsed")
    ap.add_argument("--csv", help="the same records as CSV")
    ap.add_argument("--symbols", help="file of the program's identifiers, one a line (snaps rows 9-10); "
                    "default: every identifier in this checkout's dreamcast/ and src/")
    ap.add_argument("--atlas", default=ATLAS)
    ap.add_argument("--learn", help="write an atlas learned from this video's well-parsed rows")
    ap.add_argument("--grid", help="x0,y0,px,py of the cell grid in 1280x720 (default %s)" % (GRID,))
    a = ap.parse_args()
    grid = tuple(float(v) for v in a.grid.split(",")) if a.grid else GRID
    if a.symbols:
        symbols = {s.strip()[:16] for s in open(a.symbols) if s.strip()}
    else:
        symbols = tree_symbols(os.path.join(HERE, "..", ".."))
    keys, M = load_atlas(a.atlas)

    reads, times, keep = [], [], []
    for t, fr in frames(a.video, a.fps):
        C = cells(fr, ROWS, grid)
        reads.append({r: classify_row(C[r], keys, M) for r in ROWS})
        times.append(t)
        if a.learn:
            keep.append(C)
    print(f"{len(reads)} frames read", file=sys.stderr)

    recs = []
    for s, e in windows(reads):
        # the window's first and last frames may hold the old and new text half drawn
        span = list(range(s + 1, e - 1)) if e - s > 4 else list(range(s, e))
        rec = {"t": round(times[s], 2), "t_end": round(times[e - 1], 2), "frames_read": len(span),
               "lines": {}, "fields": {}, "edge": [], "unparsed": []}
        for r in ROWS:
            txt = vote([reads[i][r][0] for i in span]).rstrip()
            rec["lines"][r] = txt
            if not txt:
                continue
            f, edge, ok = parse_row(r, txt, symbols)
            rec["fields"].update(f)
            rec["edge"] += edge
            if not ok:
                rec["unparsed"].append(r)
        rec["span"] = span
        recs.append(rec)

    if a.learn:
        learn(a.learn, recs, keep)

    if a.log:
        with open(a.log, "w") as o:
            o.write(f"# {os.path.basename(a.video)}: Dreamcast HUD read back by hud_ocr.py, a block per 2 s window\n")
            o.write("# t= video seconds; the row number, then the row as on screen (53 columns)\n")
            for rec in recs:
                o.write(f"\nt={rec['t']:.1f}\n")
                for r, txt in rec["lines"].items():
                    if txt:
                        o.write(f"{r:2d} {txt}\n")
    if a.jsonl:
        with open(a.jsonl, "w") as o:
            for rec in recs:
                d = {"t": rec["t"], "t_end": rec["t_end"], "frames_read": rec["frames_read"]}
                d.update(rec["fields"])
                if rec["edge"]:
                    d["edge"] = rec["edge"]
                if rec["unparsed"]:
                    d["unparsed"] = {str(r): rec["lines"][r] for r in rec["unparsed"]}
                o.write(json.dumps(d) + "\n")
    if a.csv:
        import csv
        cols = ["t", "t_end", "frames_read"]
        for rec in recs:
            for k, v in rec["fields"].items():
                ks = [k + "_a", k + "_b"] if isinstance(v, list) else [k]
                for kk in ks:
                    if kk not in cols:
                        cols.append(kk)
        cols += ["edge", "unparsed"]
        with open(a.csv, "w", newline="") as o:
            w = csv.writer(o)
            w.writerow(cols)
            for rec in recs:
                d = {"t": rec["t"], "t_end": rec["t_end"], "frames_read": rec["frames_read"],
                     "edge": " ".join(rec["edge"]), "unparsed": " ".join(map(str, rec["unparsed"]))}
                for k, v in rec["fields"].items():
                    if isinstance(v, list):
                        d[k + "_a"], d[k + "_b"] = v
                    else:
                        d[k] = v
                w.writerow([d.get(c, "") for c in cols])
    bad = sum(1 for r in recs if r["unparsed"])
    print(f"{len(recs)} windows, {bad} with a row not parsed", file=sys.stderr)


def learn(path, recs, keep):
    """Average each character's cells over rows whose parse re-renders to
    within two characters of what was read, and write them as the atlas."""
    acc = {}
    for rec in recs:
        if rec["frames_read"] < 4:
            continue
        for r, txt in rec["lines"].items():
            if r in rec["unparsed"] or not txt or r in (9, 10):
                continue
            f, edge, ok = parse_row(r, txt, set())
            if edge:
                continue
            want = render(r, f)
            if want is None:
                continue
            want = want[:NCOL].ljust(NCOL)
            got = txt.ljust(NCOL)
            if len(want.rstrip()) != len(got.rstrip()) or sum(x != y for x, y in zip(want, got)) > 2:
                continue
            for i in rec["span"]:
                for c, ch in enumerate(want[:NCOL - 1]):
                    acc.setdefault(ch, []).append(keep[i][r][c][:, PAD:PAD + CW])
        for r in (9, 10):   # symbol rows: take them as read (names hold most letters)
            txt = rec["lines"].get(r, "")
            if r in rec["unparsed"] or not txt:
                continue
            for i in rec["span"]:
                for c, ch in enumerate(txt.ljust(NCOL)[:NCOL - 1]):
                    acc.setdefault(ch, []).append(keep[i][r][c][:, PAD:PAD + CW])
    old_keys, old_M = load_atlas()
    out = np.zeros((CH, CW * len(CHARS)), np.float32)
    for i, ch in enumerate(CHARS):
        if ch in acc and len(acc[ch]) >= 3:
            out[:, i * CW:(i + 1) * CW] = np.mean(acc[ch], 0)
        elif ch in old_keys:
            out[:, i * CW:(i + 1) * CW] = old_M[old_keys.index(ch)]
    Image.fromarray(out.clip(0, 255).astype(np.uint8)).save(path)
    print(f"atlas: {sorted(k for k in acc if len(acc[k]) >= 3)}", file=sys.stderr)


if __name__ == "__main__":
    main()
