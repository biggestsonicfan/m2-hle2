#!/usr/bin/env python3
"""hud_read.py -- read the HUD=prof panel back out of screenshots or a video
of the console (Pinboard #518).

    python3 -I hud_read.py CAPTURE.mp4 --jsonl windows.jsonl --lv frames.csv
    python3 -I hud_read.py shot1.png shot2.png

The panel (main_dc.c hud_line, dc_pvr.h) is tools/hud_font5x7.py's font in
8x16 cells on the 640x480 picture, text from column 1, a line a row:

    TAG [nnnn] key=value key=value ... XX

XX is the CRC-8 of all before its space: a line is kept only when its check
holds, so a cell misread anywhere drops the line rather than a wrong number.
The templates are drawn from hud_font5x7.py, the same table the disc's font
is generated from.

The picture is found by itself: a 4:3 picture the capture's full height,
centred (Flycast's window, a capture card's 640x480 or 720x480), then nudged
until lines' checks hold; --geom X0,Y0,SCALE sets it (the picture's top left
in the capture, capture pixels a picture pixel). A line that fails its check
in one frame is voted over the row's last frames (the window lines stand for
2 s).

Out:
  stdout   the last good reading of each line, and a count of lines read.
  --jsonl  a record per 2-s window: {"n": .., "t": seconds into the capture,
           "WN": {"f": 3841, "fps": 29.9, ...}, "CP": {...}, ...}; the ID line
           and the bench lines (B0-B3) as records of their own.
  --lv     a CSV row per capture frame whose LV line was read: the capture
           frame and time, then f (board frame), d (frames drawn), v
           (vblanks), t (ms since boot), dt (ms since the last frame drawn).
           A capture shows a drawn frame as long as the PVR holds it, so
           d's steps and repeats say what the screen really showed.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter, deque

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hud_font5x7 as font  # noqa: E402

CW, CH = font.CELL_W, font.CELL_H
ROWS, COLS = 30, 80
CHARS = [chr(c) for c in range(32, 127)]
TPL = np.array([np.array(font.cell(c), dtype=np.float32).ravel() for c in CHARS])   # (95, 128)
TPL_SQ = (TPL * TPL).sum(1)
LINE = re.compile(r"^([A-Z][A-Z0-9]) (.*) ([0-9A-F]{2})$")
WINDOW_TAGS = {"WN", "FT", "CP", "PG", "LD", "RD", "DR", "MS", "TX", "SN", "HW", "PV", "G0", "G1", "S0", "S1", "SC"}


def read_cells(pic):
    """pic: 480x640 float 0..1. The rows' text, 80 characters each (column 0 included)."""
    cells = pic.reshape(ROWS, CH, COLS, CW).transpose(0, 2, 1, 3).reshape(ROWS * COLS, CH * CW)
    d = (cells * cells).sum(1)[:, None] - 2 * cells @ TPL.T + TPL_SQ[None, :]
    best = d.argmin(1).reshape(ROWS, COLS)
    return ["".join(CHARS[i] for i in row) for row in best]


def check(text):
    """The line in a row's text (from column 1), if its check holds: (tag, body) or None."""
    s = text[1:]
    # the band ends where the text does; past it is the game's picture
    for k in range(len(s), 5, -1):
        m = LINE.match(s[:k])
        if m and font.crc8(s[:k - 3]) == int(m.group(3), 16):
            return m.group(1), m.group(2)
    return None


def picture(gray, geom):
    """The capture's picture as 640x480, 0..1, black and white."""
    x0, y0, sc = geom
    box = (x0, y0, x0 + 640 * sc, y0 + 480 * sc)
    im = Image.fromarray(gray).resize((640, 480), Image.BOX if sc > 1 else Image.BILINEAR, box=box)
    a = np.asarray(im, dtype=np.float32) / 255.0
    return (a > 0.5).astype(np.float32)


def good_lines(gray, geom):
    rows = read_cells(picture(gray, geom))
    return rows, sum(1 for r in rows if check(r))


def find_geom(gray):
    """The picture's place: the full-height 4:3 guess, then the nearby offsets
    and scales, keeping the one most lines check out in."""
    h, w = gray.shape
    sc0 = h / 480.0
    guess = ((w - 640 * sc0) / 2, 0.0, sc0)
    best = (good_lines(gray, guess)[1], guess)
    if best[0] >= 3:
        return best
    for sc in (sc0, w / 640.0, 1.0, 1.5, 2.0, 2.25):
        if 640 * sc > w + 1 or 480 * sc > h + 1:
            continue
        cx, cy = (w - 640 * sc) / 2, (h - 480 * sc) / 2
        for dy in np.arange(-8, 8.01, 0.5) * sc:
            for dx in np.arange(-4, 4.01, 1) * sc:
                g = (cx + dx, max(0.0, cy + dy), sc)
                n = good_lines(gray, g)[1]
                if n > best[0]:
                    best = (n, g)
    return best


def value(v):
    v = v.rstrip("%K")
    for f in (int, float):
        try:
            return f(v)
        except ValueError:
            pass
    return v


def fields(tag, body):
    toks = body.split()
    out = {}
    if tag in WINDOW_TAGS and toks and toks[0].isdigit():
        out["n"] = int(toks[0])
        toks = toks[1:]
    for t in toks:
        k, eq, v = t.partition("=")
        if eq:
            out[k] = value(v)
    return out


def frames(paths, args):
    """(index, seconds, gray uint8 array) of each capture frame."""
    if all(p.lower().endswith((".png", ".jpg", ".jpeg", ".bmp")) for p in paths):
        for i, p in enumerate(paths):
            yield i, 0.0, np.asarray(Image.open(p).convert("L"))
        return
    if len(paths) != 1:
        sys.exit("one video, or images")
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                            "stream=width,height,r_frame_rate", "-of", "json", paths[0]],
                           capture_output=True, text=True, check=True)
    st = json.loads(probe.stdout)["streams"][0]
    w, h = st["width"], st["height"]
    num, den = (int(x) for x in st["r_frame_rate"].split("/"))
    fps = num / den if den else 30.0
    cmd = ["ffmpeg", "-v", "error"]
    if args.start:
        cmd += ["-ss", str(args.start)]
    cmd += ["-i", paths[0]]
    if args.seconds:
        cmd += ["-t", str(args.seconds)]
    cmd += ["-f", "rawvideo", "-pix_fmt", "gray", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    n = w * h
    i = 0
    try:
        while True:
            buf = p.stdout.read(n)
            if len(buf) < n:
                break
            if i % args.every == 0:
                yield i, (args.start or 0) + i / fps, np.frombuffer(buf, np.uint8).reshape(h, w)
            i += 1
    finally:
        p.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", nargs="+", help="a video, or screenshots")
    ap.add_argument("--geom", help="X0,Y0,SCALE: the picture's top left in the capture, and its scale")
    ap.add_argument("--jsonl", help="a record per 2-s window")
    ap.add_argument("--lv", help="a CSV row per frame's LV line")
    ap.add_argument("--every", type=int, default=1, help="read every Nth frame of a video")
    ap.add_argument("--start", type=float, default=0, help="seconds into the video")
    ap.add_argument("--seconds", type=float, default=0, help="this long")
    ap.add_argument("--raw", action="store_true", help="print every row as read, check or not")
    args = ap.parse_args()

    geom = tuple(float(x) for x in args.geom.split(",")) if args.geom else None
    last = {}                         # tag -> its last good body
    windows = {}                      # n -> record
    once = {}                         # ID, B0-B3, AO, JT
    hist = [deque(maxlen=8) for _ in range(ROWS)]
    lv_out = open(args.lv, "w") if args.lv else None
    if lv_out:
        lv_out.write("frame,sec,f,d,v,t,dt\n")
    n_frames = n_lines = n_voted = 0
    for idx, sec, gray in frames(args.capture, args):
        n_frames += 1
        if geom is None:
            n, g = find_geom(gray)
            if n == 0:
                continue
            geom = g
            print("picture at x %.1f y %.1f, scale %.3f" % geom, file=sys.stderr)
        rows = read_cells(picture(gray, geom))
        for r, text in enumerate(rows):
            if args.raw:
                print("%3d %2d |%s|" % (idx, r, text.rstrip()))
            got = check(text)
            hist[r].append(text)
            if not got and len(hist[r]) >= 3:   # a column's most common character over the row's last frames
                voted = "".join(Counter(col).most_common(1)[0][0] for col in zip(*hist[r]))
                got = check(voted)
                n_voted += got is not None
            if not got:
                continue
            n_lines += 1
            tag, body = got
            last[tag] = body
            f = fields(tag, body)
            if tag == "LV" and lv_out:
                lv_out.write("%d,%.3f,%s\n" % (idx, sec, ",".join(str(f.get(k, "")) for k in ("f", "d", "v", "t", "dt"))))
            elif "n" in f:
                rec = windows.setdefault(f["n"], {"n": f["n"], "t": round(sec, 3)})
                rec[tag] = {k: v for k, v in f.items() if k != "n"}
            elif tag != "LV":
                once[tag] = f
    if lv_out:
        lv_out.close()
    if args.jsonl:
        with open(args.jsonl, "w") as o:
            for tag in sorted(once):
                o.write(json.dumps({"tag": tag, **once[tag]}) + "\n")
            for n in sorted(windows):
                o.write(json.dumps(windows[n]) + "\n")
    for tag in sorted(last):
        print("%s %s" % (tag, last[tag]))
    print("%d frames, %d lines whose check held (%d by vote), %d windows"
          % (n_frames, n_lines, n_voted, len(windows)), file=sys.stderr)


if __name__ == "__main__":
    main()
